#include "service_cms.h"
#include "rpc_deferrable.h"

#include <ydb/core/base/auth.h>
#include <ydb/core/base/path.h>
#include <ydb/core/security/iam_delegation/lifecycle/client.h>
#include <ydb/public/api/protos/ydb_cms.pb.h>

#include <library/cpp/string_utils/base64/base64.h>

namespace NKikimr::NGRpcService {
namespace {

using namespace NActors;
namespace P = NKikimrIamDelegation;
using TTabletEvents = NIamDelegation::TEvIamDelegationTablet;
using TRequest = TGrpcRequestOperationCall<Ydb::Cms::ListDatabaseIamDelegationsRequest,
    Ydb::Cms::ListDatabaseIamDelegationsResponse>;

class TListDatabaseIamDelegationsRPC final
    : public TRpcOperationRequestActor<TListDatabaseIamDelegationsRPC, TRequest> {
    using TBase = TRpcOperationRequestActor<TListDatabaseIamDelegationsRPC, TRequest>;

public:
    using TBase::TBase;

    void Bootstrap(const TActorContext& ctx) {
        TBase::Bootstrap(ctx);
        // The retained inventory is a cluster administration API: a tenant's
        // existence and ACLs cannot authorize access after that tenant is gone.
        if (!IsAdministrator(AppData(), Request_->GetInternalToken().Get())) {
            return Fail(Ydb::StatusIds::UNAUTHORIZED, "User is not a cluster administrator", ctx);
        }
        const auto& request = *GetProtoRequest();
        Path = Request_->NormalizePath(request.path());
        if (request.path().empty() || Path.empty() || Path.size() > 4096 || Path.front() != '/' || CanonizePath(Path) != Path) {
            return Fail(Ydb::StatusIds::BAD_REQUEST, "A canonical absolute database path is required", ctx);
        }
        for (const auto& part : NKikimr::SplitPath(Path)) {
            if (IsPathPartContainsOnlyDots(part)) {
                return Fail(Ydb::StatusIds::BAD_REQUEST, "Database path contains a dot-only component", ctx);
            }
        }
        PageSize = request.page_size() ? request.page_size() : 100;
        Incarnation = request.database_incarnation();
        if (PageSize > 1000 || Incarnation.size() > 256) {
            return Fail(Ydb::StatusIds::BAD_REQUEST, "Page size must be at most 1000 and database incarnation at most 256 bytes", ctx);
        }
        if (!request.page_token().empty() && !ReadToken(request.page_token())) {
            return Fail(Ydb::StatusIds::BAD_REQUEST, "Invalid inventory page token or mismatched database identity", ctx);
        }
        TabletId = AppData()->IamConfig.GetDelegationTabletId();
        if (!TabletId) {
            return Fail(Ydb::StatusIds::UNAVAILABLE, "IAM delegation tablet is not configured", ctx);
        }
        Become(&TThis::StateWork);
        Schedule(TDuration::Seconds(30), new TEvents::TEvWakeup(InventoryDeadlineTag));
        if (Incarnation.empty()) {
            ReadRegistry(ctx);
        } else {
            ReadInventory();
        }
    }

private:
    bool ReadToken(const TString& token) {
        if (token.size() > 16384 || !token.StartsWith("v1.")) {
            return false;
        }
        P::TResponse cursor;
        try {
            if (!cursor.ParseFromString(Base64StrictDecode(TStringBuf(token).Skip(3)))) {
                return false;
            }
        } catch (const yexception&) {
            return false;
        }
        const auto& database = cursor.GetDatabase();
        const auto& identity = database.GetIdentity();
        if (cursor.GetStatus() != P::SUCCESS || identity.GetPath() != Path
            || identity.GetIncarnation().empty() || identity.GetIncarnation().size() > 256
            || (!Incarnation.empty() && Incarnation != identity.GetIncarnation())
            || !database.GetInventoryRevision() || cursor.GetNextAfterOperationId().empty()
            || cursor.GetNextAfterOperationId().size() > 256) {
            return false;
        }
        Incarnation = identity.GetIncarnation();
        InventoryRevision = database.GetInventoryRevision();
        AfterOperationId = cursor.GetNextAfterOperationId();
        return true;
    }

    TString MakeToken(const P::TResponse& response) const {
        P::TResponse cursor;
        cursor.SetStatus(P::SUCCESS);
        auto* database = cursor.MutableDatabase();
        database->MutableIdentity()->SetIncarnation(Incarnation);
        database->MutableIdentity()->SetPath(Path);
        database->SetInventoryRevision(response.GetDatabase().GetInventoryRevision());
        cursor.SetNextAfterOperationId(response.GetNextAfterOperationId());
        return "v1." + Base64Encode(cursor.SerializeAsString());
    }

    void SendRequest(THolder<TTabletEvents::TEvRequest> request) {
        TabletRequest = Register(NIamDelegation::CreateIamDelegationTabletRequestActor(TabletId));
        Send(TabletRequest, request.Release(), 0, ++Cookie);
    }

    void ReadRegistry(const TActorContext& ctx) {
        if (++RegistryPages > MaxRegistryPages) {
            return Fail(Ydb::StatusIds::UNAVAILABLE,
                "Database registry scan limit reached; specify database_incarnation", ctx);
        }
        ReadingRegistry = true;
        auto request = MakeHolder<TTabletEvents::TEvRequest>();
        request->Record.MutableListDatabases()->SetPageSize(1000);
        request->Record.MutableListDatabases()->SetAfterIncarnation(AfterIncarnation);
        SendRequest(std::move(request));
    }

    void ReadInventory() {
        ReadingRegistry = false;
        auto request = MakeHolder<TTabletEvents::TEvRequest>();
        auto* inventory = request->Record.MutableListInventory();
        inventory->SetDatabaseIncarnation(Incarnation);
        inventory->SetPageSize(PageSize);
        inventory->SetAfterOperationId(AfterOperationId);
        if (InventoryRevision) {
            inventory->SetInventoryRevision(InventoryRevision);
        }
        SendRequest(std::move(request));
    }

    void Handle(TTabletEvents::TEvResponse::TPtr& ev, const TActorContext& ctx) {
        if (ev->Cookie != Cookie || ev->Sender != TabletRequest) {
            return;
        }
        TabletRequest = {};
        const auto& response = ev->Get()->Record;
        switch (response.GetStatus()) {
            case P::SUCCESS:
                break;
            case P::SNAPSHOT_EXPIRED:
                return Fail(Ydb::StatusIds::PRECONDITION_FAILED,
                    "Inventory snapshot expired; restart enumeration without a page token", ctx);
            case P::NOT_FOUND:
                return Fail(Ydb::StatusIds::NOT_FOUND, "Database incarnation is not registered", ctx);
            case P::INVALID_ARGUMENT:
                return Fail(Ydb::StatusIds::BAD_REQUEST, "Invalid inventory request", ctx);
            default:
                return Fail(Ydb::StatusIds::UNAVAILABLE, "Cannot read IAM delegation inventory", ctx);
        }
        if (ReadingRegistry) {
            return HandleRegistry(response, ctx);
        }
        const auto& database = response.GetDatabase();
        if (!response.HasDatabase() || database.GetIdentity().GetIncarnation() != Incarnation
            || database.GetIdentity().GetPath() != Path) {
            return Fail(Ydb::StatusIds::BAD_REQUEST, "Database incarnation does not match the requested path", ctx);
        }
        if ((!database.GetInventoryRevision() && response.DelegationsSize())
            || (InventoryRevision && InventoryRevision != database.GetInventoryRevision())
            || response.DelegationsSize() > PageSize
            || (!response.GetNextAfterOperationId().empty()
                && (response.DelegationsSize() != PageSize
                    || response.GetNextAfterOperationId() <= AfterOperationId
                    || response.GetNextAfterOperationId() != response.GetDelegations(PageSize - 1).GetOperationId()))) {
            return Fail(Ydb::StatusIds::UNAVAILABLE, "Inconsistent inventory page", ctx);
        }
        Ydb::Cms::ListDatabaseIamDelegationsResult result;
        result.set_database_incarnation(Incarnation);
        result.set_inventory_revision(database.GetInventoryRevision());
        TString previousOperationId = AfterOperationId;
        for (const auto& delegation : response.GetDelegations()) {
            if (delegation.GetDatabaseIncarnation() != Incarnation
                || delegation.GetOperationId() <= previousOperationId || !delegation.HasBinding()) {
                return Fail(Ydb::StatusIds::UNAVAILABLE, "Inconsistent inventory identity or ordering", ctx);
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
        }
        if (!response.GetNextAfterOperationId().empty()) {
            result.set_next_page_token(MakeToken(response));
        }
        ReplyWithResult(Ydb::StatusIds::SUCCESS, result, ctx);
    }

    void HandleRegistry(const P::TResponse& response, const TActorContext& ctx) {
        for (const auto& database : response.GetDatabases()) {
            const auto& identity = database.GetIdentity();
            if (identity.GetPath() == Path) {
                if (!Incarnation.empty() && Incarnation != identity.GetIncarnation()) {
                    return Fail(Ydb::StatusIds::BAD_REQUEST,
                        "Multiple retained database incarnations match this path; specify database_incarnation", ctx);
                }
                Incarnation = identity.GetIncarnation();
            }
        }
        if (!response.GetNextAfterIncarnation().empty()) {
            if (response.GetNextAfterIncarnation() <= AfterIncarnation) {
                return Fail(Ydb::StatusIds::UNAVAILABLE, "Database registry cursor did not advance", ctx);
            }
            AfterIncarnation = response.GetNextAfterIncarnation();
            return ReadRegistry(ctx);
        }
        if (Incarnation.empty()) {
            return Fail(Ydb::StatusIds::NOT_FOUND, "Database path is not registered in the IAM delegation inventory", ctx);
        }
        ReadInventory();
    }

    void Fail(Ydb::StatusIds::StatusCode status, const TString& message, const TActorContext& ctx) {
        Request_->RaiseIssue(NYql::TIssue(message));
        Reply(status, ctx);
    }

    void PassAway() override {
        if (TabletRequest) {
            Send(TabletRequest, new TEvents::TEvPoison);
        }
        TBase::PassAway();
    }

    STATEFN(StateWork) {
        if (ev->GetTypeRewrite() == TEvents::TEvWakeup::EventType
            && ev->Get<TEvents::TEvWakeup>()->Tag == InventoryDeadlineTag) {
            return Fail(Ydb::StatusIds::UNAVAILABLE, "IAM delegation inventory deadline exceeded", ActorContext());
        }
        switch (ev->GetTypeRewrite()) {
            HFunc(TTabletEvents::TEvResponse, Handle);
        default:
            return TBase::StateFuncBase(ev);
        }
    }

    static constexpr ui64 InventoryDeadlineTag = 73;
    static constexpr ui32 MaxRegistryPages = 100;
    TActorId TabletRequest;
    ui64 TabletId = 0;
    ui64 Cookie = 0;
    ui64 InventoryRevision = 0;
    ui32 PageSize = 0;
    ui32 RegistryPages = 0;
    bool ReadingRegistry = false;
    TString Path;
    TString Incarnation;
    TString AfterIncarnation;
    TString AfterOperationId;
};

} // namespace

void DoListDatabaseIamDelegationsRequest(std::unique_ptr<IRequestOpCtx> request, const IFacilityProvider& facility) {
    facility.RegisterActor(new TListDatabaseIamDelegationsRPC(request.release()));
}

} // namespace NKikimr::NGRpcService
