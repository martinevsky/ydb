#include "worker.h"
#include "operations.h"
#include "schema.h"
#include <ydb/core/security/iam_delegation/policy/retry.h>
#include <ydb/core/util/backoff.h>
#include <ydb/core/security/iam_delegation/tablet/public/setup_failure.h>

#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/base/path.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/library/actors/async/sleep.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <util/generic/guid.h>

namespace NKikimr::NIamDelegation {
namespace {
using namespace NActors;
namespace P = NKikimrIamDelegation;

// TxProxy's scheme proposal replies do not echo the request cookie. Keep their
// lifetime isolated and forward exactly one result to the coroutine's cookie.
class TSchemaRequest final : public TActorBootstrapped<TSchemaRequest> {
public:
    explicit TSchemaRequest(TActorId proxy = MakeTxProxyID()) : Proxy(proxy) {}
    void Bootstrap() { Become(&TThis::StateWork); }
private:
    void Handle(TEvTxUserProxy::TEvProposeTransaction::TPtr& ev) {
        ReplyTo = ev->Sender;
        Cookie = ev->Cookie;
        Send(Proxy, ev->Release().Release(), IEventHandle::FlagTrackDelivery);
        Schedule(TDuration::Seconds(30), new TEvents::TEvWakeup);
    }
    void Handle(TEvTxUserProxy::TEvProposeTransactionStatus::TPtr& ev) {
        Send(ReplyTo, ev->Release().Release(), 0, Cookie);
        PassAway();
    }
    void Unavailable() {
        Send(ReplyTo, new TEvTxUserProxy::TEvProposeTransactionStatus(
            TEvTxUserProxy::TResultStatus::ProxyShardNotAvailable), 0, Cookie);
        PassAway();
    }
    STRICT_STFUNC(StateWork,
        hFunc(TEvTxUserProxy::TEvProposeTransaction, Handle);
        hFunc(TEvTxUserProxy::TEvProposeTransactionStatus, Handle);
        cFunc(TEvents::TEvUndelivered::EventType, Unavailable);
        cFunc(TEvents::TEvWakeup::EventType, Unavailable);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )
    const TActorId Proxy;
    TActorId ReplyTo;
    ui64 Cookie = 0;
};

class TRecoveryWorker final : public TActorBootstrapped<TRecoveryWorker> {
public:
    explicit TRecoveryWorker(ui64 tabletId)
        : TabletId(tabletId)
        , WorkerId(CreateGuidAsString())
    {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        TString afterDatabase;
        THashMap<TString, TString> inventoryCursors;
        TBackoff scanPacing(TRetrySettings::InitialDelay, TRetrySettings::InitialDelay);
        for (;;) {
            // Pace before taking a lease; the remote wait never consumes the
            // delay intended for another record or another worker's scan.
            co_await AsyncSleepFor(scanPacing.Next());
            co_await Revoke();
            co_await RecoverInventory(afterDatabase, inventoryCursors);
        }
    }

private:
    async<void> Revoke() {
        P::TRequest request;
        auto* claim = request.MutableClaimRevocations();
        claim->SetWorkerId(WorkerId);
        claim->SetLimit(1); // never hold a batch of leases while waiting on IAM
        claim->SetLeaseUs(TDuration::Minutes(1).MicroSeconds());
        const auto claimed = co_await CallIamDelegationTablet(TabletId, std::move(request));
        if (claimed.GetStatus() != P::SUCCESS) {
            co_return;
        }
        for (const auto& delegation : claimed.GetDelegations()) {
            TDelegationOperationResult result;
            if (delegation.GetRevokeOperationId()) {
                result = co_await CallIamOperation(new TEvIamDelegation::TEvPollDelegationOperation(delegation.GetRevokeOperationId()));
            } else {
                result = co_await CallIamOperation(new TEvIamDelegation::TEvSubmitDelegationOperation(
                    EDelegationOperationKind::Revoke, ToIamBinding(delegation.GetBinding()), TString(), delegation.GetOperationId()));
            }
            P::TRequest finish;
            auto* ack = finish.MutableFinishRevocation();
            ack->SetOperationId(delegation.GetOperationId());
            *ack->MutableClaim() = delegation.GetClaim();
            ack->SetOutcome(P::REVOKE_RETRY);
            // Retain the old wire delay for rolling upgrades. New tablets own
            // the durable streak and compute the delay from RetryReason.
            ack->SetRetryAfterUs(TRetrySettings::InitialDelay.MicroSeconds());
            switch (result.Outcome) {
                case EDelegationOperationOutcome::Succeeded:
                    ack->SetOutcome(P::REVOKE_SUCCEEDED);
                    break;
                case EDelegationOperationOutcome::Accepted:
                    ack->SetRetryReason(P::REVOKE_RETRY_PENDING);
                    break;
                case EDelegationOperationOutcome::Failed:
                    ack->SetRetryReason(P::REVOKE_RETRY_FAILURE);
                    // Only a terminal remote operation permits a fresh revoke.
                    ack->SetClearRevokeOperationId(true);
                    break;
                case EDelegationOperationOutcome::Unknown:
                    ack->SetRetryReason(P::REVOKE_RETRY_FAILURE);
                    break;
            }
            if (result.Outcome != EDelegationOperationOutcome::Failed && result.OperationId) {
                ack->SetRevokeOperationId(result.OperationId);
            }
            // The tablet rejects late workers by generation, attempt and deadline.
            co_await CallIamDelegationTablet(TabletId, std::move(finish));
        }
    }

    async<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr> DescribeRequest(TString path) {
        auto request = MakeHolder<TEvTxUserProxy::TEvNavigate>();
        request->Record.MutableDescribePath()->SetPath(path);
        co_return co_await ActorRequest<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>(MakeTxProxyID(), request.Release());
    }

    async<void> ResumeSetup(P::TDelegation delegation) {
        P::TRequest start;
        start.MutableStartSetup()->SetOperationId(delegation.GetOperationId());
        start.MutableStartSetup()->SetExpectedRevision(delegation.GetRevision());
        const auto started = co_await CallIamDelegationTablet(TabletId, std::move(start));
        if (started.GetStatus() != P::SUCCESS) {
            co_return; // another worker or the statement may have won the dispatch permission
        }
        delegation = started.GetDelegation();
        const auto result = co_await CallIamOperation(new TEvIamDelegation::TEvSubmitDelegationOperation(
            EDelegationOperationKind::Setup, ToIamBinding(delegation.GetBinding()),
            delegation.GetSubjectId(), delegation.GetOperationId()));
        // DROP can change the revision while IAM is running. Retry persisting the
        // result against fresh durable state, never the external mutation itself.
        for (ui32 attempt = 0; attempt < 3; ++attempt) {
            P::TRequest update;
            auto* setup = update.MutableSetSetupResult();
            setup->SetOperationId(delegation.GetOperationId());
            setup->SetExpectedRevision(delegation.GetRevision());
            setup->SetOutcome(result.Outcome == EDelegationOperationOutcome::Succeeded ? P::SETUP_SUCCEEDED
                : result.Outcome == EDelegationOperationOutcome::Failed ? P::SETUP_FAILED : P::SETUP_UNKNOWN);
            setup->SetSetupOperationId(result.OperationId);
            if (result.Outcome == EDelegationOperationOutcome::Failed) {
                *setup->MutableSetupFailure() = MakeSetupFailure(result);
            }
            const auto saved = co_await CallIamDelegationTablet(TabletId, std::move(update));
            if (saved.GetStatus() == P::SUCCESS) {
                co_return;
            }
            P::TRequest read;
            read.MutableGetDelegation()->SetOperationId(delegation.GetOperationId());
            const auto current = co_await CallIamDelegationTablet(TabletId, std::move(read));
            if (current.GetStatus() != P::SUCCESS) {
                co_return;
            }
            delegation = current.GetDelegation();
            if (delegation.GetSetupState() == P::SETUP_SUCCEEDED || delegation.GetSetupState() == P::SETUP_FAILED) {
                co_return;
            }
        }
    }

    async<void> DropFailedCreateShell(const P::TDelegation& delegation,
        const NKikimrSchemeOp::TDirEntry& self, const TString& databasePath)
    {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetDatabaseName(databasePath);
        auto* scheme = request->Record.MutableTransaction()->MutableModifyScheme();
        scheme->SetWorkingDir(TString(ExtractParent(delegation.GetSecretPath())));
        scheme->SetOperationType(NKikimrSchemeOp::ESchemeOpDropSecret);
        scheme->MutableDrop()->SetName(TString(ExtractBase(delegation.GetSecretPath())));
        scheme->MutableDrop()->SetId(self.GetPathId());
        auto* condition = scheme->AddApplyIf();
        condition->SetPathOwnerId(self.GetSchemeshardId());
        condition->SetPathId(self.GetPathId());
        condition->SetPathVersion(self.GetPathVersion());
        co_await ActorRequest<TEvTxUserProxy::TEvProposeTransactionStatus>(
            Register(new TSchemaRequest), request.Release());
        // Accepted is not completion, and transport loss is not rejection. The
        // next sweep retains the marker until authoritative schema absence.
    }

    async<IEventHandle::TPtr> AllocateSchemaServices() {
        co_return co_await ActorRequest<IEventHandle>(MakeTxProxyID(), new TEvTxUserProxy::TEvAllocateTxId,
            IEventHandle::FlagTrackDelivery);
    }

    async<void> RecoverUnbound(const P::TDelegation& delegation) {
        const auto observed = co_await ObserveIamDelegationSchemaCreate(delegation, TabletId);
        if (observed.State == ESchemaCreateState::Retire) {
            P::TRequest retire;
            retire.MutableDropSecret()->SetOperationId(delegation.GetOperationId());
            retire.MutableDropSecret()->SetExpectedRevision(delegation.GetRevision());
            co_await CallIamDelegationTablet(TabletId, std::move(retire));
        } else if (observed.State == ESchemaCreateState::Created) {
            const auto& self = observed.Schema.GetPathDescription().GetSelf();
            P::TRequest bind;
            auto* command = bind.MutableBindSecret();
            command->SetOperationId(delegation.GetOperationId());
            command->SetExpectedRevision(delegation.GetRevision());
            auto* identity = command->MutableSecret();
            identity->SetDatabaseIncarnation(delegation.GetDatabaseIncarnation());
            identity->SetPathOwnerId(self.GetSchemeshardId());
            identity->SetPathLocalId(self.GetPathId());
            co_await CallIamDelegationTablet(TabletId, std::move(bind));
        } else if (observed.State == ESchemaCreateState::Replay) {
            auto allocated = co_await WithTimeout(TDuration::Seconds(30), &TThis::AllocateSchemaServices, this);
            if (!allocated || (*allocated)->GetTypeRewrite() != TEvTxUserProxy::TEvAllocateTxIdResult::EventType) {
                co_return;
            }
            auto allocation = THolder<TEvTxUserProxy::TEvAllocateTxIdResult>(
                (*allocated)->Release<TEvTxUserProxy::TEvAllocateTxIdResult>().Release());
            // The fresh allocation supplies services/monitoring only. Never
            // substitute its transaction ID for the durable original CREATE ID.
            const auto proxy = Register(CreateIamDelegationSchemaProxy(delegation.GetCreateTxId(), std::move(allocation)));
            co_await ActorRequest<TEvTxUserProxy::TEvProposeTransactionStatus>(
                Register(new TSchemaRequest(proxy)), MakeIamDelegationSchemaCreate(delegation, TabletId).Release());
            // Acceptance is not completion; all reply/transport outcomes are
            // reconciled by the next authoritative parent-then-child observation.
        }
    }

    async<void> Reconcile(P::TDelegation delegation, const TString& databasePath) {
        if (delegation.GetState() == P::REVOKING || delegation.GetState() == P::REVOCATION_PENDING) {
            co_return;
        }
        // A known operation is recoverable without retaining the user's token or
        // resubmitting Setup. Unknown-ID records remain visible in inventory.
        if ((delegation.GetSetupState() == P::SETUP_STARTED || delegation.GetSetupState() == P::SETUP_UNKNOWN)
            && delegation.GetSetupOperationId()) {
            const auto result = co_await CallIamOperation(new TEvIamDelegation::TEvPollDelegationOperation(delegation.GetSetupOperationId()));
            if (result.Outcome == EDelegationOperationOutcome::Succeeded || result.Outcome == EDelegationOperationOutcome::Failed) {
                P::TRequest update;
                auto* setup = update.MutableSetSetupResult();
                setup->SetOperationId(delegation.GetOperationId());
                setup->SetExpectedRevision(delegation.GetRevision());
                setup->SetOutcome(result.Outcome == EDelegationOperationOutcome::Succeeded ? P::SETUP_SUCCEEDED : P::SETUP_FAILED);
                setup->SetSetupOperationId(delegation.GetSetupOperationId());
                if (result.Outcome == EDelegationOperationOutcome::Failed) {
                    *setup->MutableSetupFailure() = MakeSetupFailure(result);
                }
                auto saved = co_await CallIamDelegationTablet(TabletId, std::move(update));
                if (saved.GetStatus() != P::SUCCESS) {
                    co_return;
                }
                delegation = saved.GetDelegation();
            }
        }
        P::TResponse current;
        if (delegation.HasSecret()) {
            P::TRequest read;
            *read.MutableGetSecret()->MutableSecret() = delegation.GetSecret();
            current = co_await CallIamDelegationTablet(TabletId, std::move(read));
            if (current.GetStatus() != P::SUCCESS || current.GetSecret().GetState() != P::SECRET_LIVE) {
                co_return;
            }
        } else if (delegation.GetState() != P::PENDING) {
            co_return;
        }
        if (!delegation.HasSecret() && delegation.HasSchemaCreate()) {
            co_await RecoverUnbound(delegation);
            co_return;
        }
        const auto described = co_await WithTimeout(TDuration::Seconds(30), &TThis::DescribeRequest, this, delegation.GetSecretPath());
        if (!described) {
            co_return;
        }
        const auto& record = (*described)->Get()->GetRecord();
        if (record.GetStatus() != NKikimrScheme::StatusSuccess
            && record.GetStatus() != NKikimrScheme::StatusPathDoesNotExist) {
            co_return; // transport/access errors never prove deletion
        }
        const auto& path = record.GetPathDescription();
        const auto& self = path.GetSelf();
        const auto& reference = path.GetSecretDescription().GetIamDelegation();
        bool matches = record.GetStatus() == NKikimrScheme::StatusSuccess
            && self.GetPathType() == NKikimrSchemeOp::EPathTypeSecret
            && path.GetSecretDescription().HasIamDelegation()
            && reference.GetTabletId() == TabletId
            && reference.GetDatabaseIncarnation() == delegation.GetDatabaseIncarnation()
            && self.GetCreateTxId() == delegation.GetCreateTxId();
        if (!delegation.HasSecret()) {
            // Do not cancel an unbound intent on absence: CREATE may still be
            // queued in SchemeShard. It has no dispatch permission yet.
            if (!matches || !self.GetCreateFinished() || reference.GetCreateOperationId() != delegation.GetOperationId()) {
                co_return;
            }
            P::TRequest bind;
            auto* command = bind.MutableBindSecret();
            command->SetOperationId(delegation.GetOperationId());
            command->SetExpectedRevision(delegation.GetRevision());
            auto* identity = command->MutableSecret();
            identity->SetDatabaseIncarnation(delegation.GetDatabaseIncarnation());
            identity->SetPathOwnerId(self.GetSchemeshardId());
            identity->SetPathLocalId(self.GetPathId());
            co_await CallIamDelegationTablet(TabletId, std::move(bind));
            co_return;
        }
        matches = matches && self.GetSchemeshardId() == delegation.GetSecret().GetPathOwnerId()
            && self.GetPathId() == delegation.GetSecret().GetPathLocalId();
        matches = matches && current.GetSecret().GetCreateOperationId() == reference.GetCreateOperationId();
        if (!matches) {
            P::TRequest drop;
            *drop.MutableDropSecret()->MutableSecret() = delegation.GetSecret();
            drop.MutableDropSecret()->SetExpectedRevision(current.GetSecret().GetRevision());
            co_await CallIamDelegationTablet(TabletId, std::move(drop));
        } else if (self.GetCreateFinished() && delegation.GetState() == P::CANCELLED
            && delegation.GetSetupState() == P::SETUP_FAILED
            && current.GetSecret().GetCreateOperationId() == delegation.GetOperationId()
            && current.GetSecret().GetCurrentOperationId().empty()
            && current.GetSecret().GetPendingOperationId().empty()
            && path.GetSecretDescription().GetVersion() == 0) {
            // CREATE's initial schema version is zero. Every ALTER/OR REPLACE
            // authorizes in schema and increments it before staging a binding.
            // Preserve an intervening replacement; a concurrent one is fenced
            // by the exact path version and owner on this narrow internal DROP.
            co_await DropFailedCreateShell(delegation, self, databasePath);
        } else if (self.GetCreateFinished() && delegation.GetSubjectId()
            && delegation.GetSetupState() == P::SETUP_NOT_STARTED && delegation.GetState() == P::PENDING
            && current.GetSecret().GetPendingOperationId() == delegation.GetOperationId()) {
            co_await ResumeSetup(delegation);
        } else if (self.GetCreateFinished()
            && delegation.GetSetupState() == P::SETUP_SUCCEEDED && delegation.GetState() == P::PENDING
            && current.GetSecret().GetPendingOperationId() == delegation.GetOperationId()) {
            P::TRequest promote;
            auto* command = promote.MutablePromote();
            command->SetOperationId(delegation.GetOperationId());
            command->SetExpectedRevision(delegation.GetRevision());
            command->SetExpectedSecretRevision(current.GetSecret().GetRevision());
            co_await CallIamDelegationTablet(TabletId, std::move(promote));
        }
    }

    async<void> RecoverInventory(TString& afterDatabase, THashMap<TString, TString>& inventoryCursors) {
        P::TRequest list;
        list.MutableListDatabases()->SetPageSize(1);
        list.MutableListDatabases()->SetAfterIncarnation(afterDatabase);
        const auto databases = co_await CallIamDelegationTablet(TabletId, std::move(list));
        if (databases.GetStatus() != P::SUCCESS) {
            co_return;
        }
        afterDatabase = databases.GetNextAfterIncarnation();
        // One record of one database per turn bounds the work between revocation
        // claims and shares progress among databases regardless of their size.
        for (const auto& database : databases.GetDatabases()) {
            const auto& incarnation = database.GetIdentity().GetIncarnation();
            P::TRequest inventory;
            auto* page = inventory.MutableListInventory();
            page->SetDatabaseIncarnation(incarnation);
            page->SetPageSize(1);
            if (const auto cursor = inventoryCursors.find(incarnation); cursor != inventoryCursors.end()) {
                page->SetAfterOperationId(cursor->second);
            }
            // This is a repeated recovery sweep, not an immutable export. Refresh
            // the revision from the registry on every turn while retaining the
            // position: our own reconciliation changes must not restart the prefix.
            // Every action still validates the record's own revision and identity.
            page->SetInventoryRevision(database.GetInventoryRevision());
            const auto records = co_await CallIamDelegationTablet(TabletId, std::move(inventory));
            if (records.GetStatus() != P::SUCCESS) {
                co_return; // concurrent mutation: retry this position on its next turn
            }
            if (records.GetNextAfterOperationId()) {
                inventoryCursors[incarnation] = records.GetNextAfterOperationId();
            } else {
                inventoryCursors.erase(incarnation);
            }
            for (const auto& delegation : records.GetDelegations()) {
                co_await Reconcile(delegation, database.GetIdentity().GetPath());
            }
        }
    }

    STRICT_STFUNC(StateWork,
        IgnoreFunc(TEvIamDelegationTablet::TEvResponse);
        IgnoreFunc(TEvTxUserProxy::TEvAllocateTxIdResult);
        IgnoreFunc(TEvIamDelegation::TEvDelegationOperationResult);
        IgnoreFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult);
        IgnoreFunc(TEvTxUserProxy::TEvProposeTransactionStatus);
        IgnoreFunc(TEvents::TEvUndelivered);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )

    const ui64 TabletId;
    const TString WorkerId;
};
} // namespace

NActors::IActor* CreateIamDelegationRecoveryWorker(ui64 tabletId) {
    return new TRecoveryWorker(tabletId);
}

} // namespace NKikimr::NIamDelegation
