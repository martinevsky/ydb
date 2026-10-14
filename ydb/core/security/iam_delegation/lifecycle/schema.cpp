#include "schema.h"

#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/base/path.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

namespace NKikimr::NIamDelegation {
namespace {
using namespace NActors;
namespace P = NKikimrIamDelegation;

class TAllocatedSchemeProxy final : public TActorBootstrapped<TAllocatedSchemeProxy> {
public:
    TAllocatedSchemeProxy(ui64 txId, THolder<TEvTxUserProxy::TEvAllocateTxIdResult> allocation)
        : TxId(txId), Allocation(std::move(allocation)) {}
    void Bootstrap() { Become(&TThis::StateWork); }
private:
    void Handle(TEvTxUserProxy::TEvProposeTransaction::TPtr& ev) {
        TAutoPtr<TEvTxProxyReq::TEvSchemeRequest> request = new TEvTxProxyReq::TEvSchemeRequest(ev);
        Register(NTxProxy::CreateTxProxyFlatSchemeReq(Allocation->Services, TxId,
            request, Allocation->TxProxyMon));
        PassAway();
    }
    STRICT_STFUNC(StateWork,
        hFunc(TEvTxUserProxy::TEvProposeTransaction, Handle);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )
    const ui64 TxId;
    THolder<TEvTxUserProxy::TEvAllocateTxIdResult> Allocation;
};

async<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr> Describe(TString path, TString database) {
    auto request = MakeHolder<TEvTxUserProxy::TEvNavigate>();
    request->Record.SetDatabaseName(database);
    request->Record.MutableDescribePath()->SetPath(path);
    co_return co_await ActorRequest<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>(
        MakeTxProxyID(), request.Release());
}
} // namespace

THolder<TEvTxUserProxy::TEvProposeTransaction> MakeIamDelegationSchemaCreate(
    const P::TDelegation& delegation, ui64 tabletId)
{
    const auto& intent = delegation.GetSchemaCreate();
    Y_ENSURE(delegation.HasSchemaCreate() && tabletId && delegation.GetCreateTxId());
    auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
    request->Record.SetDatabaseName(intent.GetDatabasePath());
    TVector<TString> groups(intent.GetGroupSIDs().begin(), intent.GetGroupSIDs().end());
    // Construct only the verified ACL identity. No original authentication bearer.
    const NACLib::TUserToken user(intent.GetUserSID(), groups);
    request->Record.SetUserToken(user.SerializeAsString());
    auto* scheme = request->Record.MutableTransaction()->MutableModifyScheme();
    scheme->SetWorkingDir(TString(ExtractParent(delegation.GetSecretPath())));
    scheme->SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
    scheme->SetFailOnExist(true);
    scheme->SetReplaceIfExists(false);
    scheme->SetFailedOnAlreadyExists(true);
    auto* create = scheme->MutableCreateSecret();
    create->SetName(TString(ExtractBase(delegation.GetSecretPath())));
    create->SetInheritPermissions(intent.GetInheritPermissions());
    auto* reference = create->MutableIamDelegationReference();
    reference->SetTabletId(tabletId);
    reference->SetDatabaseIncarnation(delegation.GetDatabaseIncarnation());
    reference->SetCreateOperationId(delegation.GetOperationId());
    auto* domain = create->MutableExpectedDatabaseDomainKey();
    domain->SetOwnerId(intent.GetDatabaseDomainOwnerId());
    domain->SetLocalId(intent.GetDatabaseDomainLocalId());
    auto* condition = scheme->AddApplyIf();
    condition->SetPathOwnerId(intent.GetParentPathOwnerId());
    condition->SetPathId(intent.GetParentPathLocalId());
    condition->SetPathVersion(intent.GetParentPathVersion());
    if (intent.HasDiffACL()) {
        scheme->MutableModifyACL()->SetDiffACL(intent.GetDiffACL());
    }
    return request;
}

NActors::IActor* CreateIamDelegationSchemaProxy(ui64 txId,
    THolder<TEvTxUserProxy::TEvAllocateTxIdResult> allocation)
{
    return new TAllocatedSchemeProxy(txId, std::move(allocation));
}

NActors::async<TSchemaCreateObservation> ObserveIamDelegationSchemaCreate(
    const P::TDelegation& delegation, ui64 tabletId)
{
    using namespace NActors;
    TSchemaCreateObservation observation;
    if (!delegation.HasSchemaCreate()) {
        co_return observation;
    }
    const auto& intent = delegation.GetSchemaCreate();
    const auto parent = co_await WithTimeout(TDuration::Seconds(30), &Describe,
        TString(ExtractParent(delegation.GetSecretPath())), intent.GetDatabasePath());
    if (!parent) {
        co_return observation;
    }
    const auto& parentRecord = (*parent)->Get()->GetRecord();
    if (parentRecord.GetStatus() != NKikimrScheme::StatusSuccess
        && parentRecord.GetStatus() != NKikimrScheme::StatusPathDoesNotExist) {
        co_return observation;
    }
    const auto& self = parentRecord.GetPathDescription().GetSelf();
    const bool parentUnchanged = parentRecord.GetStatus() == NKikimrScheme::StatusSuccess
        && self.GetSchemeshardId() == intent.GetParentPathOwnerId()
        && self.GetPathId() == intent.GetParentPathLocalId()
        && self.GetPathVersion() == intent.GetParentPathVersion();
    const auto child = co_await WithTimeout(TDuration::Seconds(30), &Describe,
        delegation.GetSecretPath(), intent.GetDatabasePath());
    if (!child) {
        co_return observation;
    }
    observation.Schema = (*child)->Get()->GetRecord();
    if (observation.Schema.GetStatus() != NKikimrScheme::StatusSuccess
        && observation.Schema.GetStatus() != NKikimrScheme::StatusPathDoesNotExist) {
        co_return observation;
    }
    const auto& path = observation.Schema.GetPathDescription();
    const auto& secret = path.GetSelf();
    const auto& reference = path.GetSecretDescription().GetIamDelegation();
    const bool matches = observation.Schema.GetStatus() == NKikimrScheme::StatusSuccess
        && secret.GetPathType() == NKikimrSchemeOp::EPathTypeSecret
        && path.GetSecretDescription().HasIamDelegation()
        && reference.GetTabletId() == tabletId
        && reference.GetDatabaseIncarnation() == delegation.GetDatabaseIncarnation()
        && reference.GetCreateOperationId() == delegation.GetOperationId()
        && secret.GetCreateTxId() == delegation.GetCreateTxId();
    if (matches) {
        observation.State = secret.GetCreateFinished() ? ESchemaCreateState::Created : ESchemaCreateState::Creating;
    } else if (!parentUnchanged || (observation.Schema.GetStatus() == NKikimrScheme::StatusSuccess
        && secret.GetPathType() == NKikimrSchemeOp::EPathTypeSecret && secret.GetCreateFinished())) {
        // A completed competing secret occupies the name; its removal consumes
        // the saved parent version before an old CREATE could use that name.
        observation.State = ESchemaCreateState::Retire;
    } else if (observation.Schema.GetStatus() == NKikimrScheme::StatusPathDoesNotExist) {
        observation.State = ESchemaCreateState::Replay;
    }
    co_return observation;
}

} // namespace NKikimr::NIamDelegation
