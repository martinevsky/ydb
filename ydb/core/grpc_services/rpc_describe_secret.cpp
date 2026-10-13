#include "service_secret.h"
#include "rpc_scheme_base.h"

#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/security/iam_delegation_lifecycle/client.h>
#include <ydb/core/ydb_convert/ydb_convert.h>
#include <ydb/public/api/protos/ydb_secret.pb.h>

namespace NKikimr::NGRpcService {

using namespace NActors;

using TEvDescribeSecretRequest = TGrpcRequestOperationCall<::Ydb::Secret::DescribeSecretRequest,
    ::Ydb::Secret::DescribeSecretResponse>;

class TDescribeSecretRPC : public TRpcSchemeRequestActor<TDescribeSecretRPC, TEvDescribeSecretRequest> {
    using TBase = TRpcSchemeRequestActor<TDescribeSecretRPC, TEvDescribeSecretRequest>;

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
        Become(&TDescribeSecretRPC::StateDescribeScheme);
    }

private:
    enum class EReading { Secret, Current, Pending };

    STATEFN(StateDescribeScheme) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult, Handle);
        default:
            return TBase::StateWork(ev);
        }
    }

    STATEFN(StateReadDelegation) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NIamDelegation::TEvIamDelegationTablet::TEvResponse, Handle);
        default:
            return TBase::StateWork(ev);
        }
    }

    void Handle(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->GetRecord();
        const auto& pathDescription = record.GetPathDescription();

        if (record.HasReason()) {
            Request_->RaiseIssue(NYql::TIssue(record.GetReason()));
        }

        switch (record.GetStatus()) {
            case NKikimrScheme::StatusSuccess: {
                if (pathDescription.GetSelf().GetPathType() != NKikimrSchemeOp::EPathTypeSecret) {
                    Request_->RaiseIssue(NYql::TIssue(
                        TStringBuilder() << "Unexpected path type: " << pathDescription.GetSelf().GetPathType()));
                    return Reply(Ydb::StatusIds::SCHEME_ERROR, ctx);
                }
                if (!pathDescription.HasSecretDescription()) {
                    Request_->RaiseIssue(NYql::TIssue("Secret description is missing"));
                    return Reply(Ydb::StatusIds::SCHEME_ERROR, ctx);
                }

                ConvertDirectoryEntry(pathDescription.GetSelf(), Result.mutable_self(), true);
                Result.set_version(pathDescription.GetSecretDescription().GetVersion());
                if (!pathDescription.GetSecretDescription().HasIamDelegation()) {
                    return ReplyWithResult(Ydb::StatusIds::SUCCESS, Result, ctx);
                }

                Reference = pathDescription.GetSecretDescription().GetIamDelegation();
                Identity.SetDatabaseIncarnation(Reference.GetDatabaseIncarnation());
                Identity.SetPathOwnerId(pathDescription.GetSelf().GetSchemeshardId());
                Identity.SetPathLocalId(pathDescription.GetSelf().GetPathId());
                if (!Reference.GetTabletId() || Reference.GetDatabaseIncarnation().empty()
                    || Reference.GetCreateOperationId().empty() || !Identity.GetPathOwnerId()
                    || !Identity.GetPathLocalId()) {
                    return Fail(Ydb::StatusIds::SCHEME_ERROR, "IAM secret reference is incomplete", ctx);
                }
                Become(&TDescribeSecretRPC::StateReadDelegation);
                return ReadSnapshot(ctx);
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

    bool SameIdentity(const NKikimrIamDelegation::TSecretIdentity& identity) const {
        return identity.GetDatabaseIncarnation() == Identity.GetDatabaseIncarnation()
            && identity.GetPathOwnerId() == Identity.GetPathOwnerId()
            && identity.GetPathLocalId() == Identity.GetPathLocalId();
    }

    bool SameSnapshot(const NKikimrIamDelegation::TSecretRecord& secret) const {
        return secret.GetRevision() == Snapshot.GetRevision()
            && secret.GetCurrentOperationId() == Snapshot.GetCurrentOperationId()
            && secret.GetPendingOperationId() == Snapshot.GetPendingOperationId();
    }

    void SendRequest(THolder<NIamDelegation::TEvIamDelegationTablet::TEvRequest> request) {
        TabletRequest = Register(NIamDelegation::CreateIamDelegationTabletRequestActor(Reference.GetTabletId()));
        Send(TabletRequest, request.Release(), 0, ++RequestCookie);
    }

    void ReadSnapshot(const TActorContext& ctx) {
        if (++SnapshotAttempts > MaxSnapshotAttempts) {
            return Fail(Ydb::StatusIds::UNAVAILABLE, "IAM secret changed while being described; retry the request", ctx);
        }
        Result.clear_iam_delegation();
        Result.clear_pending_iam_delegation();
        Reading = EReading::Secret;
        auto request = MakeHolder<NIamDelegation::TEvIamDelegationTablet::TEvRequest>();
        *request->Record.MutableGetSecret()->MutableSecret() = Identity;
        SendRequest(std::move(request));
    }

    void ReadBinding(EReading reading, const TString& operationId) {
        Reading = reading;
        auto request = MakeHolder<NIamDelegation::TEvIamDelegationTablet::TEvRequest>();
        request->Record.MutableGetDelegation()->SetOperationId(operationId);
        SendRequest(std::move(request));
    }

    void ReadPendingOrReply(const TActorContext& ctx) {
        if (!Snapshot.GetPendingOperationId().empty()) {
            return ReadBinding(EReading::Pending, Snapshot.GetPendingOperationId());
        }
        ReplyWithResult(Ydb::StatusIds::SUCCESS, Result, ctx);
    }

    void Handle(NIamDelegation::TEvIamDelegationTablet::TEvResponse::TPtr& ev, const TActorContext& ctx) {
        if (ev->Cookie != RequestCookie || ev->Sender != TabletRequest) {
            return;
        }
        TabletRequest = {};
        const auto& record = ev->Get()->Record;
        if (record.GetStatus() != NKikimrIamDelegation::SUCCESS) {
            return Fail(record.GetStatus() == NKikimrIamDelegation::NOT_FOUND
                ? Ydb::StatusIds::SCHEME_ERROR : Ydb::StatusIds::UNAVAILABLE,
                "Cannot read IAM secret lifecycle state", ctx);
        }
        const auto& secret = record.GetSecret();
        if (!record.HasSecret() || !SameIdentity(secret.GetIdentity())
            || secret.GetCreateOperationId() != Reference.GetCreateOperationId()
            || secret.GetState() != NKikimrIamDelegation::SECRET_LIVE) {
            return Fail(Ydb::StatusIds::SCHEME_ERROR, "IAM secret lifecycle identity no longer matches the schema object", ctx);
        }
        if (Reading == EReading::Secret) {
            Snapshot = secret;
            if (!Snapshot.GetCurrentOperationId().empty()) {
                return ReadBinding(EReading::Current, Snapshot.GetCurrentOperationId());
            }
            return ReadPendingOrReply(ctx);
        }

        // GetDelegation reads its associated Secret in the same tablet transaction.
        // Restart all reads if promotion, retirement or another ALTER intervened.
        if (!SameSnapshot(secret)) {
            return ReadSnapshot(ctx);
        }
        const auto& delegation = record.GetDelegation();
        const auto& operationId = Reading == EReading::Current
            ? Snapshot.GetCurrentOperationId() : Snapshot.GetPendingOperationId();
        if (!record.HasDelegation() || delegation.GetOperationId() != operationId
            || !SameIdentity(delegation.GetSecret())
            || delegation.GetDatabaseIncarnation() != Identity.GetDatabaseIncarnation()
            || !delegation.HasBinding()
            || (Reading == EReading::Current && (delegation.GetState() != NKikimrIamDelegation::ACTIVE
                || delegation.GetSetupState() != NKikimrIamDelegation::SETUP_SUCCEEDED))
            || (Reading == EReading::Pending && delegation.GetState() != NKikimrIamDelegation::PENDING)) {
            return Fail(Ydb::StatusIds::SCHEME_ERROR, "IAM secret binding does not match its lifecycle snapshot", ctx);
        }
        auto* binding = Reading == EReading::Current
            ? Result.mutable_iam_delegation() : Result.mutable_pending_iam_delegation();
        binding->set_service_account_id(delegation.GetBinding().GetServiceAccountId());
        binding->set_cloud_id(delegation.GetBinding().GetCloudId());
        binding->set_referrer_id(delegation.GetBinding().GetReferrerId());
        if (Reading == EReading::Current) {
            return ReadPendingOrReply(ctx);
        }
        ReplyWithResult(Ydb::StatusIds::SUCCESS, Result, ctx);
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

    static constexpr ui32 MaxSnapshotAttempts = 3;
    EReading Reading = EReading::Secret;
    ui32 SnapshotAttempts = 0;
    ui64 RequestCookie = 0;
    TActorId TabletRequest;
    NKikimrSchemeOp::TIamDelegationSecretReference Reference;
    NKikimrIamDelegation::TSecretIdentity Identity;
    NKikimrIamDelegation::TSecretRecord Snapshot;
    Ydb::Secret::DescribeSecretResult Result;
};

void DoDescribeSecretRequest(std::unique_ptr<IRequestOpCtx> p, const IFacilityProvider& f) {
    f.RegisterActor(new TDescribeSecretRPC(p.release()));
}

}
