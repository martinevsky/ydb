#include "schemeshard_iam_delegation.h"
#include "schemeshard_impl.h"

#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::FLAT_TX_SCHEMESHARD

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

TVector<NKikimrSchemeOp::TIamDelegation> NamedIamDelegations(const NKikimrSchemeOp::TSecretDescription& secret) {
    TVector<NKikimrSchemeOp::TIamDelegation> result;
    if (secret.HasIamDelegation() && !secret.GetIamDelegation().GetReferrerId().empty()) {
        result.push_back(secret.GetIamDelegation());
    }
    if (secret.HasPendingIamDelegation() && !secret.GetPendingIamDelegation().GetReferrerId().empty()) {
        result.push_back(secret.GetPendingIamDelegation());
    }
    return result;
}

namespace {

// One revocation. A delegation service call is answered with TEvRevokeDelegationResult, with TEvUndelivered
// when the service is not running on the node (the feature flag is off or IAM is not configured), or not at
// all; every outcome but success is retried after a growing delay. Seq tells stale replies and timers from
// the current ones: every send and every scheduled retry gets a new value.
class TIamDelegationRevoker: public TActorBootstrapped<TIamDelegationRevoker> {
    static constexpr TDuration RequestTimeout = TDuration::Minutes(3); // the service retries and polls the IAM operation inside
    static constexpr TDuration MinRetryDelay = TDuration::Seconds(10);
    static constexpr TDuration MaxRetryDelay = TDuration::Minutes(5);

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::SCHEMESHARD_IAM_DELEGATION_REVOKER;
    }

    TIamDelegationRevoker(const TActorId& schemeShard, const TIamDelegationRevocation& revocation)
        : SchemeShard(schemeShard)
        , Revocation(revocation)
    {
    }

    void Bootstrap() {
        Become(&TIamDelegationRevoker::StateWork);
        SendRequest();
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(NIamDelegation::TEvIamDelegation::TEvRevokeDelegationResult, Handle);
            hFunc(TEvents::TEvUndelivered, Handle);
            hFunc(TEvents::TEvWakeup, Handle);
            sFunc(TEvents::TEvPoison, PassAway);
        }
    }

private:
    void SendRequest() {
        InFlight = true;
        ++Seq;
        NIamDelegation::TDelegationSpec spec;
        spec.ServiceAccountId = Revocation.ServiceAccountId;
        spec.CloudId = Revocation.CloudId;
        spec.ReferrerId = Revocation.ReferrerId;
        YDB_LOG_INFO("Revoking an IAM delegation", {"revocation", Revocation.ToString()}, {"attempt", Failures + 1});
        // the service of this node, under the id the KQP proxy registers it with
        Send(NIamDelegation::MakeIamDelegationServiceId(),
            new NIamDelegation::TEvIamDelegation::TEvRevokeDelegation(std::move(spec)), IEventHandle::FlagTrackDelivery, Seq);
        Schedule(RequestTimeout, new TEvents::TEvWakeup(Seq));
    }

    void Handle(NIamDelegation::TEvIamDelegation::TEvRevokeDelegationResult::TPtr& ev) {
        if (ev->Cookie != Seq) {
            return; // a reply to a request already given up on
        }
        const auto& result = ev->Get()->Result;
        // NOT_FOUND: the delegation was never set up, or was revoked before (the service maps IAM's "absent" to it)
        if (result.IsSuccess() || result.Status == Ydb::StatusIds::NOT_FOUND) {
            YDB_LOG_NOTICE("IAM delegation revoked", {"revocation", Revocation.ToString()}, {"status", result.Status});
            Send(SchemeShard, new TEvPrivate::TEvIamDelegationRevoked(Revocation.ReferrerId));
            return PassAway();
        }
        Retry(TStringBuilder() << Ydb::StatusIds::StatusCode_Name(result.Status) << ": " << result.Issues.ToOneLineString());
    }

    void Handle(TEvents::TEvUndelivered::TPtr& ev) {
        if (ev->Cookie != Seq) {
            return;
        }
        Retry("the IAM delegation service is not running on this node");
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev) {
        if (ev->Get()->Tag != Seq) {
            return;
        }
        if (InFlight) {
            return Retry("timeout");
        }
        SendRequest();
    }

    void Retry(const TString& reason) {
        InFlight = false;
        ++Failures;
        const TDuration delay = Min(MaxRetryDelay, MinRetryDelay * (1ull << Min<ui32>(Failures - 1, 5)));
        YDB_LOG_WARN("Cannot revoke an IAM delegation, retrying",
            {"revocation", Revocation.ToString()}, {"reason", reason}, {"failures", Failures}, {"delay", delay});
        ++Seq;
        Schedule(delay, new TEvents::TEvWakeup(Seq));
    }

private:
    const TActorId SchemeShard;
    const TIamDelegationRevocation Revocation;
    ui64 Seq = 0;
    ui32 Failures = 0;
    bool InFlight = false;
};

// Removes the record of a revocation IAM has accepted.
class TTxIamDelegationRevoked: public TTransactionBase<TSchemeShard> {
public:
    TTxIamDelegationRevoked(TSchemeShard* self, const TString& referrerId)
        : TTransactionBase(self)
        , ReferrerId(referrerId)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_IAM_DELEGATION_REVOKED;
    }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        Self->RunningIamDelegationRevokers.erase(ReferrerId);
        if (!Self->IamDelegationRevocations.contains(ReferrerId)) {
            return true;
        }
        NIceDb::TNiceDb db(txc.DB);
        Self->PersistIamDelegationRevocationRemove(db, ReferrerId);
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        Self->RunIamDelegationRevocations(ctx);
    }

private:
    const TString ReferrerId;
};

} // namespace

IActor* CreateIamDelegationRevoker(const TActorId& schemeShard, const TIamDelegationRevocation& revocation) {
    return new TIamDelegationRevoker(schemeShard, revocation);
}

void TSchemeShard::PersistIamDelegationRevocation(NIceDb::TNiceDb& db, const TIamDelegationRevocation& revocation) {
    Y_ABORT_UNLESS(!revocation.ReferrerId.empty());
    db.Table<Schema::IamDelegationRevocations>().Key(revocation.ReferrerId).Update(
        NIceDb::TUpdate<Schema::IamDelegationRevocations::ServiceAccountId>(revocation.ServiceAccountId),
        NIceDb::TUpdate<Schema::IamDelegationRevocations::CloudId>(revocation.CloudId),
        NIceDb::TUpdate<Schema::IamDelegationRevocations::PathId>(revocation.PathId.LocalPathId));
    IamDelegationRevocations[revocation.ReferrerId] = revocation;
}

void TSchemeShard::PersistIamDelegationRevocationRemove(NIceDb::TNiceDb& db, const TString& referrerId) {
    db.Table<Schema::IamDelegationRevocations>().Key(referrerId).Delete();
    IamDelegationRevocations.erase(referrerId);
}

ui32 TSchemeShard::PersistIamDelegationRevocations(NIceDb::TNiceDb& db, TPathId pathId,
    const NKikimrSchemeOp::TSecretDescription& before, const NKikimrSchemeOp::TSecretDescription* after)
{
    THashSet<TString> stillNamed;
    if (after) {
        for (const auto& delegation : NamedIamDelegations(*after)) {
            stillNamed.insert(delegation.GetReferrerId());
        }
    }
    ui32 count = 0;
    for (const auto& delegation : NamedIamDelegations(before)) {
        if (stillNamed.contains(delegation.GetReferrerId())) {
            continue;
        }
        TIamDelegationRevocation revocation;
        revocation.ReferrerId = delegation.GetReferrerId();
        revocation.ServiceAccountId = delegation.GetServiceAccountId();
        revocation.CloudId = delegation.GetCloudId();
        revocation.PathId = pathId;
        YDB_LOG_NOTICE("IAM delegation is no longer named by its secret, scheduling the revocation", {"revocation", revocation.ToString()});
        PersistIamDelegationRevocation(db, revocation);
        ++count;
    }
    return count;
}

void TSchemeShard::RunIamDelegationRevocations(const TActorContext& ctx) {
    for (const auto& [referrerId, revocation] : IamDelegationRevocations) {
        if (RunningIamDelegationRevokers.contains(referrerId)) {
            continue;
        }
        RunningIamDelegationRevokers[referrerId] = ctx.Register(CreateIamDelegationRevoker(SelfId(), revocation));
    }
}

void TSchemeShard::Handle(TEvPrivate::TEvRunIamDelegationRevocations::TPtr&, const TActorContext& ctx) {
    RunIamDelegationRevocations(ctx);
}

void TSchemeShard::Handle(TEvPrivate::TEvIamDelegationRevoked::TPtr& ev, const TActorContext& ctx) {
    Execute(new TTxIamDelegationRevoked(this, ev->Get()->ReferrerId), ctx);
}

} // namespace NKikimr::NSchemeShard

#undef YDB_LOG_THIS_FILE_COMPONENT
