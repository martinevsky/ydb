#include "service_cms.h"
#include "rpc_deferrable.h"

#include <ydb/core/base/auth.h>
#include <ydb/core/base/path.h>
#include <ydb/core/security/iam_delegation/lifecycle/client.h>
#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/public/api/protos/ydb_cms.pb.h>

#include <util/generic/scope.h>

#include <optional>

namespace NKikimr::NGRpcService {
namespace {

using namespace NActors;
namespace P = NKikimrIamDelegation;
using TTabletEvents = NIamDelegation::TEvIamDelegationTablet;
using TRequest = TGrpcRequestOperationCall<Ydb::Cms::ListDatabaseIamDelegationsRequest,
    Ydb::Cms::ListDatabaseIamDelegationsResponse>;

struct TInventoryReply {
    Ydb::StatusIds::StatusCode Status = Ydb::StatusIds::SUCCESS;
    TString Message;
    Ydb::Cms::ListDatabaseIamDelegationsResult Result;
};

TInventoryReply Failure(Ydb::StatusIds::StatusCode status, TString message) {
    return {status, std::move(message), {}};
}

std::optional<TInventoryReply> TabletError(P::EStatus status) {
    switch (status) {
        case P::SUCCESS:
            return std::nullopt;
        case P::SNAPSHOT_EXPIRED:
            return Failure(Ydb::StatusIds::PRECONDITION_FAILED,
                "Inventory snapshot expired; restart enumeration without a page token");
        case P::NOT_FOUND:
            return Failure(Ydb::StatusIds::NOT_FOUND, "Database incarnation is not registered");
        case P::INVALID_ARGUMENT:
            return Failure(Ydb::StatusIds::BAD_REQUEST, "Invalid inventory request");
        default:
            return Failure(Ydb::StatusIds::UNAVAILABLE, "Cannot read IAM delegation inventory");
    }
}

async<P::TResponse> ReadTablet(ui64 tabletId, P::TRequest request) {
    const auto client = TActivationContext::Register(NIamDelegation::CreateIamDelegationTabletRequestActor(tabletId));
    auto* actorSystem = TActivationContext::AsActorContext().ActorSystem();
    Y_DEFER { actorSystem->Send(client, new TEvents::TEvPoison); };
    auto event = MakeHolder<TTabletEvents::TEvRequest>();
    event->Record.Swap(&request);
    const auto cookie = AllocateWaitCookie();
    TActivationContext::Send(new IEventHandle(client, TActivationContext::AsActorContext().SelfID,
        event.Release(), 0, cookie));
    for (;;) {
        auto reply = co_await ActorWaitForEvent<TTabletEvents::TEvResponse>(cookie);
        if (reply->Sender == client) {
            co_return std::move(reply->Get()->Record);
        }
    }
}

TInventoryReply ProjectInventory(const P::TResponse& response, const TString& path,
    const TString& requestedIncarnation, ui32 pageSize)
{
    if (response.GetPaginationCapability() != P::PAGINATION_CAPABILITY_AUTHENTICATED) {
        return Failure(Ydb::StatusIds::UNAVAILABLE,
            "IAM delegation tablet must be upgraded to authenticated inventory pagination");
    }
    const auto& database = response.GetDatabase();
    const auto& incarnation = database.GetIdentity().GetIncarnation();
    if (!response.HasDatabase() || incarnation.empty() || incarnation.size() > 256
        || (!requestedIncarnation.empty() && incarnation != requestedIncarnation)
        || database.GetIdentity().GetPath() != path) {
        return Failure(Ydb::StatusIds::BAD_REQUEST, "Database incarnation does not match the requested path");
    }
    const auto& nextToken = response.GetNextPageToken();
    const auto& nextCursor = response.GetNextAfterOperationId();
    if ((!database.GetInventoryRevision() && response.DelegationsSize())
        || response.DelegationsSize() > pageSize
        || nextToken.empty() != nextCursor.empty()
        || (!nextToken.empty() && (!nextToken.StartsWith("v2.") || nextToken.size() > 16384
            || response.DelegationsSize() != pageSize
            || nextCursor != response.GetDelegations(pageSize - 1).GetOperationId()))) {
        return Failure(Ydb::StatusIds::UNAVAILABLE, "Inconsistent inventory page");
    }
    TInventoryReply reply;
    auto& result = reply.Result;
    result.set_database_incarnation(incarnation);
    result.set_inventory_revision(database.GetInventoryRevision());
    TString previousOperationId;
    for (const auto& delegation : response.GetDelegations()) {
        if (delegation.GetDatabaseIncarnation() != incarnation
            || delegation.GetOperationId() <= previousOperationId
            || delegation.GetOperationId().size() > 256 || !delegation.HasBinding()) {
            return Failure(Ydb::StatusIds::UNAVAILABLE, "Inconsistent inventory identity or ordering");
        }
        previousOperationId = delegation.GetOperationId();
        auto* item = result.add_delegations();
        const auto& binding = delegation.GetBinding();
        item->set_operation_id(delegation.GetOperationId());
        item->set_service_account_id(binding.GetServiceAccountId());
        item->set_cloud_id(binding.GetCloudId());
        item->set_resource_type(binding.GetResourceType());
        item->set_referrer_id(binding.GetReferrerId());
        item->set_referrer_type(binding.GetReferrerType());
        item->set_service_id(binding.GetServiceId());
        item->set_microservice_id(binding.GetMicroserviceId());
        item->set_reference_policy(static_cast<Ydb::Cms::DatabaseIamDelegation::ReferencePolicy>(binding.GetReferencePolicy()));
        item->set_state(static_cast<Ydb::Cms::DatabaseIamDelegation::State>(delegation.GetState()));
        item->set_setup_state(static_cast<Ydb::Cms::DatabaseIamDelegation::SetupState>(delegation.GetSetupState()));
        item->set_secret_path(delegation.GetSecretPath());
        item->set_revision(delegation.GetRevision());
        item->set_setup_operation_id(delegation.GetSetupOperationId());
        item->set_revoke_operation_id(delegation.GetRevokeOperationId());
        if (delegation.HasStagedAtUs()) {
            item->set_staged_at_us(delegation.GetStagedAtUs());
        }
        if (delegation.HasCleanupQueuedAtUs()) {
            item->set_cleanup_queued_at_us(delegation.GetCleanupQueuedAtUs());
        }
        if (delegation.HasRevokeConsecutiveFailures()) {
            item->set_revoke_consecutive_failures(delegation.GetRevokeConsecutiveFailures());
        }
    }
    result.set_next_page_token(nextToken);
    return reply;
}

class TListDatabaseIamDelegationsRPC final
    : public TRpcOperationRequestActor<TListDatabaseIamDelegationsRPC, TRequest> {
    using TBase = TRpcOperationRequestActor<TListDatabaseIamDelegationsRPC, TRequest>;

public:
    using TBase::TBase;

    void Bootstrap() {
        TBase::Bootstrap(ActorContext());
        Become(&TThis::StateWork);
        auto reply = co_await WithTimeout(TDuration::Seconds(30), &TThis::Run, this);
        if (!reply) {
            reply = Failure(Ydb::StatusIds::UNAVAILABLE, "IAM delegation inventory deadline exceeded");
        }
        if (reply->Status == Ydb::StatusIds::SUCCESS) {
            ReplyWithResult(reply->Status, reply->Result, ActorContext());
        } else {
            Request_->RaiseIssue(NYql::TIssue(reply->Message));
            Reply(reply->Status, ActorContext());
        }
    }

private:
    async<TInventoryReply> Run() {
        // Retained inventory is a cluster administration API, even after schema deletion.
        if (!IsAdministrator(AppData(), Request_->GetInternalToken().Get())) {
            co_return Failure(Ydb::StatusIds::UNAUTHORIZED, "User is not a cluster administrator");
        }
        const auto& request = *GetProtoRequest();
        const TString path = Request_->NormalizePath(request.path());
        if (request.path().empty() || path.empty() || path.size() > 4096
            || path.front() != '/' || CanonizePath(path) != path) {
            co_return Failure(Ydb::StatusIds::BAD_REQUEST, "A canonical absolute database path is required");
        }
        for (const auto& part : NKikimr::SplitPath(path)) {
            if (IsPathPartContainsOnlyDots(part)) {
                co_return Failure(Ydb::StatusIds::BAD_REQUEST, "Database path contains a dot-only component");
            }
        }
        const ui32 pageSize = request.page_size() ? request.page_size() : 100;
        TString incarnation = request.database_incarnation();
        if (pageSize > 1000 || incarnation.size() > 256) {
            co_return Failure(Ydb::StatusIds::BAD_REQUEST,
                "Page size must be at most 1000 and database incarnation at most 256 bytes");
        }
        const TString token = request.page_token();
        if (token.StartsWith("v1.")) {
            co_return Failure(Ydb::StatusIds::BAD_REQUEST,
                "Unsigned inventory page tokens are no longer accepted; restart enumeration without a page token");
        }
        if (!token.empty() && (token.size() > 16384 || token.size() <= 3 || !token.StartsWith("v2."))) {
            co_return Failure(Ydb::StatusIds::BAD_REQUEST, "Invalid inventory page token");
        }
        const auto tabletId = AppData()->IamConfig.GetDelegationTabletId();
        if (!tabletId) {
            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "IAM delegation tablet is not configured");
        }
        if (incarnation.empty() && token.empty()) {
            TString afterIncarnation;
            for (ui32 page = 0; ; ++page) {
                if (page == 100) {
                    co_return Failure(Ydb::StatusIds::UNAVAILABLE,
                        "Database registry scan limit reached; specify database_incarnation");
                }
                P::TRequest registry;
                registry.MutableListDatabases()->SetPageSize(1000);
                registry.MutableListDatabases()->SetAfterIncarnation(afterIncarnation);
                const auto response = co_await ReadTablet(tabletId, std::move(registry));
                if (auto error = TabletError(response.GetStatus())) {
                    co_return std::move(*error);
                }
                if (response.DatabasesSize() > 1000) {
                    co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Inconsistent database registry page");
                }
                for (const auto& database : response.GetDatabases()) {
                    const auto& identity = database.GetIdentity();
                    if (identity.GetPath() == path) {
                        if (identity.GetIncarnation().empty() || identity.GetIncarnation().size() > 256) {
                            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Inconsistent database registry identity");
                        }
                        if (!incarnation.empty() && incarnation != identity.GetIncarnation()) {
                            co_return Failure(Ydb::StatusIds::BAD_REQUEST,
                                "Multiple retained database incarnations match this path; specify database_incarnation");
                        }
                        incarnation = identity.GetIncarnation();
                    }
                }
                if (response.GetNextAfterIncarnation().empty()) {
                    break;
                }
                if (response.GetNextAfterIncarnation().size() > 256
                    || response.GetNextAfterIncarnation() <= afterIncarnation) {
                    co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Database registry cursor did not advance");
                }
                afterIncarnation = response.GetNextAfterIncarnation();
            }
            if (incarnation.empty()) {
                co_return Failure(Ydb::StatusIds::NOT_FOUND,
                    "Database path is not registered in the IAM delegation inventory");
            }
        }
        P::TRequest list;
        auto* inventory = list.MutableListInventory();
        inventory->SetPaginationMode(P::PAGINATION_AUTHENTICATED);
        inventory->SetDatabasePath(path);
        inventory->SetPageSize(pageSize);
        if (!incarnation.empty()) {
            inventory->SetDatabaseIncarnation(incarnation);
        }
        if (!token.empty()) {
            inventory->SetContinuationToken(token);
        }
        const auto response = co_await ReadTablet(tabletId, std::move(list));
        if (auto error = TabletError(response.GetStatus())) {
            co_return std::move(*error);
        }
        co_return ProjectInventory(response, path, incarnation, pageSize);
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            IgnoreFunc(TTabletEvents::TEvResponse); // cancelled request's late response
            default:
                return TBase::StateFuncBase(ev);
        }
    }
};

} // namespace

void DoListDatabaseIamDelegationsRequest(std::unique_ptr<IRequestOpCtx> request, const IFacilityProvider& facility) {
    facility.RegisterActor(new TListDatabaseIamDelegationsRPC(request.release()));
}

} // namespace NKikimr::NGRpcService
