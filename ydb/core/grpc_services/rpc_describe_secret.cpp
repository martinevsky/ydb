#include "service_secret.h"
#include "rpc_scheme_base.h"

#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/security/iam_delegation/lifecycle/client.h>
#include <ydb/core/ydb_convert/ydb_convert.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/public/api/protos/ydb_secret.pb.h>
#include <util/generic/scope.h>

namespace NKikimr::NGRpcService {

using namespace NActors;
namespace P = NKikimrIamDelegation;

using TEvDescribeSecretRequest = TGrpcRequestOperationCall<::Ydb::Secret::DescribeSecretRequest,
    ::Ydb::Secret::DescribeSecretResponse>;

class TDescribeSecretRPC : public TRpcSchemeRequestActor<TDescribeSecretRPC, TEvDescribeSecretRequest> {
    using TBase = TRpcSchemeRequestActor<TDescribeSecretRPC, TEvDescribeSecretRequest>;
    using TTabletEvents = NIamDelegation::TEvIamDelegationTablet;

public:
    using TBase::TBase;

    void Bootstrap() {
        TBase::Bootstrap(ActorContext());
        auto navigateRequest = std::make_unique<TEvTxUserProxy::TEvNavigate>();
        SetAuthToken(navigateRequest, *Request_);
        SetDatabase(navigateRequest.get(), *Request_);
        navigateRequest->Record.MutableDescribePath()->SetPath(
            Request_->NormalizePath(GetProtoRequest()->path()));
        Send(MakeTxProxyID(), navigateRequest.release());
        Become(&TThis::StateDescribeScheme);
    }

private:
    enum class EBinding { Current, Pending };

    STATEFN(StateDescribeScheme) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult, Handle);
        default:
            return TBase::StateWork(ev);
        }
    }

    STATEFN(StateReadDelegation) {
        switch (ev->GetTypeRewrite()) {
            IgnoreFunc(TTabletEvents::TEvResponse);
        default:
            return TBase::StateWork(ev);
        }
    }

    STFUNC(StateDying) { Y_UNUSED(ev); }

    void Handle(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->GetRecord();
        const auto& path = record.GetPathDescription();
        if (record.HasReason()) {
            Request_->RaiseIssue(NYql::TIssue(record.GetReason()));
        }
        switch (record.GetStatus()) {
            case NKikimrScheme::StatusSuccess: {
                if (path.GetSelf().GetPathType() != NKikimrSchemeOp::EPathTypeSecret) {
                    return Fail(Ydb::StatusIds::SCHEME_ERROR,
                        TStringBuilder() << "Unexpected path type: " << path.GetSelf().GetPathType());
                }
                if (!path.HasSecretDescription()) {
                    return Fail(Ydb::StatusIds::SCHEME_ERROR, "Secret description is missing");
                }
                Ydb::Secret::DescribeSecretResult result;
                ConvertDirectoryEntry(path.GetSelf(), result.mutable_self(), true);
                result.set_version(path.GetSecretDescription().GetVersion());
                if (!path.GetSecretDescription().HasIamDelegation()) {
                    return ReplyWithResult(Ydb::StatusIds::SUCCESS, result, ctx);
                }
                const auto& reference = path.GetSecretDescription().GetIamDelegation();
                P::TSecretIdentity identity;
                identity.SetDatabaseIncarnation(reference.GetDatabaseIncarnation());
                identity.SetPathOwnerId(path.GetSelf().GetSchemeshardId());
                identity.SetPathLocalId(path.GetSelf().GetPathId());
                if (!reference.GetTabletId() || reference.GetDatabaseIncarnation().empty()
                    || reference.GetCreateOperationId().empty() || !identity.GetPathOwnerId() || !identity.GetPathLocalId()) {
                    return Fail(Ydb::StatusIds::SCHEME_ERROR, "IAM secret reference is incomplete");
                }
                Become(&TThis::StateReadDelegation);
                return ReadDelegation(reference, std::move(identity), std::move(result));
            }
            case NKikimrScheme::StatusPathDoesNotExist:
            case NKikimrScheme::StatusSchemeError:
                return Reply(Ydb::StatusIds::SCHEME_ERROR, ctx);
            case NKikimrScheme::StatusAccessDenied:
                return Reply(Ydb::StatusIds::UNAUTHORIZED, ctx);
            case NKikimrScheme::StatusNotAvailable:
                return Reply(Ydb::StatusIds::UNAVAILABLE, ctx);
            default:
                return Reply(Ydb::StatusIds::GENERIC_ERROR, ctx);
        }
    }

    static bool SameIdentity(const P::TSecretIdentity& left, const P::TSecretIdentity& right) {
        return left.GetDatabaseIncarnation() == right.GetDatabaseIncarnation()
            && left.GetPathOwnerId() == right.GetPathOwnerId()
            && left.GetPathLocalId() == right.GetPathLocalId();
    }

    static bool SameSnapshot(const P::TSecretRecord& left, const P::TSecretRecord& right) {
        return left.GetRevision() == right.GetRevision()
            && left.GetCurrentOperationId() == right.GetCurrentOperationId()
            && left.GetPendingOperationId() == right.GetPendingOperationId();
    }

    // Keep the RPC's sender fence in addition to its unique per-read cookie.
    // The client owns the transport deadline; cancellation closes it immediately.
    async<P::TResponse> ReadTablet(ui64 tabletId, P::TRequest request) {
        const auto client = Register(NIamDelegation::CreateIamDelegationTabletRequestActor(tabletId));
        auto* actorSystem = TActivationContext::ActorSystem();
        Y_DEFER { actorSystem->Send(client, new TEvents::TEvPoison); };
        auto event = MakeHolder<TTabletEvents::TEvRequest>();
        event->Record.Swap(&request);
        const auto cookie = AllocateWaitCookie();
        Send(client, event.Release(), 0, cookie);
        for (;;) {
            auto reply = co_await ActorWaitForEvent<TTabletEvents::TEvResponse>(cookie);
            if (reply->Sender == client) {
                co_return std::move(reply->Get()->Record);
            }
        }
    }

    bool Validate(const P::TResponse& record, const P::TSecretIdentity& identity,
        const NKikimrSchemeOp::TIamDelegationSecretReference& reference)
    {
        if (record.GetStatus() != P::SUCCESS) {
            Fail(record.GetStatus() == P::NOT_FOUND ? Ydb::StatusIds::SCHEME_ERROR : Ydb::StatusIds::UNAVAILABLE,
                "Cannot read IAM secret lifecycle state");
            return false;
        }
        const auto& secret = record.GetSecret();
        if (!record.HasSecret() || !SameIdentity(secret.GetIdentity(), identity)
            || secret.GetCreateOperationId() != reference.GetCreateOperationId() || secret.GetState() != P::SECRET_LIVE) {
            Fail(Ydb::StatusIds::SCHEME_ERROR, "IAM secret lifecycle identity no longer matches the schema object");
            return false;
        }
        return true;
    }

    void ReadDelegation(NKikimrSchemeOp::TIamDelegationSecretReference reference,
        P::TSecretIdentity identity, Ydb::Secret::DescribeSecretResult result)
    {
        for (ui32 attempt = 0; attempt < 3; ++attempt) {
            result.clear_iam_delegation();
            result.clear_pending_iam_delegation();
            P::TRequest request;
            *request.MutableGetSecret()->MutableSecret() = identity;
            const auto first = co_await ReadTablet(reference.GetTabletId(), std::move(request));
            if (!Validate(first, identity, reference)) co_return;
            const auto snapshot = first.GetSecret();
            bool snapshotChanged = false;
            for (const auto kind : {EBinding::Current, EBinding::Pending}) {
                const auto& operationId = kind == EBinding::Current
                    ? snapshot.GetCurrentOperationId() : snapshot.GetPendingOperationId();
                if (operationId.empty()) continue;
                P::TRequest read;
                read.MutableGetDelegation()->SetOperationId(operationId);
                const auto record = co_await ReadTablet(reference.GetTabletId(), std::move(read));
                if (!Validate(record, identity, reference)) co_return;
                // Each binding and its associated secret are read in one tablet
                // transaction. A changed snapshot restarts all metadata reads.
                if (!SameSnapshot(record.GetSecret(), snapshot)) {
                    snapshotChanged = true;
                    break;
                }
                const auto& delegation = record.GetDelegation();
                if (!record.HasDelegation() || delegation.GetOperationId() != operationId
                    || !SameIdentity(delegation.GetSecret(), identity)
                    || delegation.GetDatabaseIncarnation() != identity.GetDatabaseIncarnation()
                    || !delegation.HasBinding()
                    || (kind == EBinding::Current && (delegation.GetState() != P::ACTIVE
                        || delegation.GetSetupState() != P::SETUP_SUCCEEDED))
                    || (kind == EBinding::Pending && delegation.GetState() != P::PENDING)) {
                    Fail(Ydb::StatusIds::SCHEME_ERROR, "IAM secret binding does not match its lifecycle snapshot");
                    co_return;
                }
                auto* binding = kind == EBinding::Current
                    ? result.mutable_iam_delegation() : result.mutable_pending_iam_delegation();
                binding->set_service_account_id(delegation.GetBinding().GetServiceAccountId());
                binding->set_cloud_id(delegation.GetBinding().GetCloudId());
                binding->set_referrer_id(delegation.GetBinding().GetReferrerId());
            }
            if (snapshotChanged) continue;
            ReplyWithResult(Ydb::StatusIds::SUCCESS, result, ActorContext());
            co_return;
        }
        Fail(Ydb::StatusIds::UNAVAILABLE, "IAM secret changed while being described; retry the request");
    }

    void Fail(Ydb::StatusIds::StatusCode status, const TString& message) {
        Request_->RaiseIssue(NYql::TIssue(message));
        Reply(status, ActorContext());
    }

    void PassAway() override {
        Become(&TThis::StateDying);
        TBase::PassAway();
    }
};

void DoDescribeSecretRequest(std::unique_ptr<IRequestOpCtx> p, const IFacilityProvider& f) {
    f.RegisterActor(new TDescribeSecretRPC(p.release()));
}

}
