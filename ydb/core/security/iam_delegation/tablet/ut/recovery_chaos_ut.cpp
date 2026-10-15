#include "requests.h"

#include <util/generic/map.h>
#include <util/generic/maybe.h>
#include <util/generic/set.h>
#include <util/generic/yexception.h>
#include <util/string/cast.h>

#include <algorithm>
#include <array>
#include <random>

namespace NKikimr::NIamDelegation::NTests {
namespace {

// This oracle tracks application-level roles, not tablet revisions or claims.
// A held remote setup remains unresolved regardless of elapsed virtual time.
enum class ERole { Current, Pending, UnresolvedRetired, Revocable, Cancelled, Revoked };

enum class ERevokeProgress { Failing, PendingReported, UncertainAfterProgress };
enum class EExpectedFailure { Absent, SubjectNotDelegatable };

struct TQueueWindow {
    TInstant Earliest;
    TInstant Latest;
};

struct TExpectedDelegation {
    NProto::TIamBinding Binding;
    ERole Role;
    EExpectedFailure Failure = EExpectedFailure::Absent;
    ui32 ConsecutiveFailures = 0;
    ERevokeProgress Progress = ERevokeProgress::Failing;
    TString RevokeOperationId;
    TMaybe<TQueueWindow> Queued;
    // First observation is accepted only inside the independently recorded
    // transition window; every subsequent operation must retain that instant.
    TMaybe<ui64> ObservedQueuedAt;
};

struct TRecoveryModel {
    TMap<TString, TExpectedDelegation> Delegations;
    TSet<TString> Incarnations;
    NProto::TSecretIdentity Identity;
    TString Current;
    TString Pending;

    void Check(TTestContext& ctx) {
        const auto secret = GetSecret(ctx, Identity);
        UNIT_ASSERT_VALUES_EQUAL(secret.GetState(), NProto::SECRET_LIVE);
        UNIT_ASSERT_VALUES_EQUAL(secret.GetCurrentOperationId(), Current);
        UNIT_ASSERT_VALUES_EQUAL(secret.GetPendingOperationId(), Pending);

        TSet<TString> databases;
        TSet<TString> inventory;
        auto databaseRequest = ListDatabasesRequest(2);
        for (;;) {
            const auto page = ctx.Call(databaseRequest);
            for (const auto& database : page.GetDatabases()) {
                const auto& incarnation = database.GetIdentity().GetIncarnation();
                UNIT_ASSERT(databases.insert(incarnation).second);
                auto inventoryRequest = ListRequest(incarnation, 2);
                for (;;) {
                    const auto records = ctx.Call(inventoryRequest);
                    for (const auto& record : records.GetDelegations()) {
                        UNIT_ASSERT(inventory.insert(record.GetOperationId()).second);
                    }
                    if (records.GetNextAfterOperationId().empty()) {
                        break;
                    }
                    UNIT_ASSERT_VALUES_EQUAL(records.DelegationsSize(), 2);
                    inventoryRequest.MutableListInventory()->SetAfterOperationId(records.GetNextAfterOperationId());
                    inventoryRequest.MutableListInventory()->SetInventoryRevision(records.GetDatabase().GetInventoryRevision());
                }
            }
            if (page.GetNextAfterIncarnation().empty()) {
                break;
            }
            UNIT_ASSERT_VALUES_EQUAL(page.DatabasesSize(), 2);
            databaseRequest.MutableListDatabases()->SetAfterIncarnation(page.GetNextAfterIncarnation());
        }
        UNIT_ASSERT(databases == Incarnations);
        TSet<TString> expectedInventory;
        for (auto& [id, expected] : Delegations) {
            const auto actual = GetDelegation(ctx, id);
            AssertBinding(actual, expected.Binding);
            UNIT_ASSERT_VALUES_EQUAL(actual.GetRevokeConsecutiveFailures(), expected.ConsecutiveFailures);
            UNIT_ASSERT_VALUES_EQUAL(actual.GetRevokeOperationId(), expected.RevokeOperationId);
            if (expected.Failure == EExpectedFailure::SubjectNotDelegatable) {
                UNIT_ASSERT(actual.HasSetupFailure());
                UNIT_ASSERT_VALUES_EQUAL(actual.GetSetupFailure().GetStatus(), Ydb::StatusIds::UNAUTHORIZED);
                UNIT_ASSERT_VALUES_EQUAL(actual.GetSetupFailure().ReasonsSize(), 1);
                UNIT_ASSERT_VALUES_EQUAL(actual.GetSetupFailure().GetReasons(0), NProto::SETUP_FAILURE_SUBJECT_NOT_DELEGATABLE);
            } else {
                UNIT_ASSERT(!actual.HasSetupFailure());
            }
            UNIT_ASSERT_VALUES_EQUAL(actual.HasCleanupQueuedAtUs(), expected.Queued.Defined());
            if (expected.Queued) {
                const auto when = actual.GetCleanupQueuedAtUs();
                UNIT_ASSERT(when >= expected.Queued->Earliest.MicroSeconds());
                UNIT_ASSERT(when <= expected.Queued->Latest.MicroSeconds());
                if (expected.ObservedQueuedAt) {
                    UNIT_ASSERT_VALUES_EQUAL(when, *expected.ObservedQueuedAt);
                } else {
                    expected.ObservedQueuedAt = when;
                }
            }
            if (expected.Role != ERole::Cancelled && expected.Role != ERole::Revoked) {
                expectedInventory.insert(id);
            }
            switch (expected.Role) {
                case ERole::Current:
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetState(), NProto::ACTIVE);
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetSetupState(), NProto::SETUP_SUCCEEDED);
                    break;
                case ERole::Pending:
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetState(), NProto::PENDING);
                    break;
                case ERole::UnresolvedRetired:
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetState(), NProto::WAITING_SETUP);
                    UNIT_ASSERT(actual.GetSetupState() == NProto::SETUP_STARTED
                        || actual.GetSetupState() == NProto::SETUP_UNKNOWN);
                    break;
                case ERole::Revocable:
                    UNIT_ASSERT(actual.GetState() == NProto::REVOCATION_PENDING || actual.GetState() == NProto::REVOKING);
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetSetupState(), NProto::SETUP_SUCCEEDED);
                    break;
                case ERole::Cancelled:
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetState(), NProto::CANCELLED);
                    break;
                case ERole::Revoked:
                    UNIT_ASSERT_VALUES_EQUAL(actual.GetState(), NProto::REVOKED);
                    break;
            }
        }
        UNIT_ASSERT(inventory == expectedInventory);
    }

    void CheckClaim(const NProto::TDelegation& claimed) const {
        const auto expected = Delegations.find(claimed.GetOperationId());
        UNIT_ASSERT(expected != Delegations.end());
        UNIT_ASSERT(expected->second.Role == ERole::Revocable);
        UNIT_ASSERT_VALUES_EQUAL(claimed.GetSetupState(), NProto::SETUP_SUCCEEDED);
        AssertBinding(claimed, expected->second.Binding);
    }
};

void RunRecoveryChaos(ui64 seed) {
    std::mt19937_64 rng(seed);
    TTestContext ctx;
    TRecoveryModel model;
    CreateActive(ctx);
    model.Identity = SecretIdentity();
    model.Current = "create-1";
    model.Incarnations.insert("database-incarnation-1");
    model.Delegations.emplace("create-1", TExpectedDelegation{Binding(), ERole::Current});

    for (ui32 round = 0; round < 6; ++round) {
        const auto label = ToString(round);
        const TString abandonedId = "abandoned-" + label;
        auto abandonedBinding = Binding("referrer-" + abandonedId);
        // Cleanup must keep original identities even when later requests use
        // another service/microservice/resource and reference policy.
        abandonedBinding.SetServiceId("service-" + label);
        abandonedBinding.SetMicroserviceId("microservice-" + label);
        abandonedBinding.SetCloudId("cloud-" + label);
        abandonedBinding.SetReferencePolicy(round % 2 ? NProto::WITH_REFERENCES : NProto::WITHOUT_REFERENCES);
        auto abandoned = ctx.Call(StageAlterRequest(GetSecret(ctx, model.Identity), abandonedId,
            abandonedBinding)).GetDelegation();
        abandoned = ctx.Call(StartRequest(abandoned)).GetDelegation();
        if (rng() % 2) {
            abandoned = ctx.Call(SetupResultRequest(abandoned, NProto::SETUP_UNKNOWN)).GetDelegation();
        }
        model.Pending = abandonedId;
        model.Delegations.emplace(abandonedId, TExpectedDelegation{abandonedBinding, ERole::Pending});
        model.Check(ctx);

        const auto retire = RetirePendingRequest(GetSecret(ctx, model.Identity), abandoned);
        const auto crash = static_cast<ECrashPoint>(round % 3);
        ctx.CrashCall(retire, crash);
        if (crash != ECrashPoint::BeforeCommit) {
            model.Pending.clear();
            model.Delegations.at(abandonedId).Role = ERole::UnresolvedRetired;
        }
        model.Check(ctx);
        ctx.Call(retire); // retry a lost durable response as well as an uncommitted request
        model.Pending.clear();
        model.Delegations.at(abandonedId).Role = ERole::UnresolvedRetired;
        ctx.AdvanceTime(TDuration::Days(1));
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
        model.Check(ctx);

        const TString replacementId = "replacement-" + label;
        const auto replacementBinding = Binding("referrer-" + replacementId);
        const auto staged = ctx.Call(StageAlterRequest(GetSecret(ctx, model.Identity), replacementId,
            replacementBinding)).GetDelegation();
        const auto ready = CompleteSetup(ctx, staged).GetDelegation();
        model.Pending = replacementId;
        model.Delegations.emplace(replacementId, TExpectedDelegation{replacementBinding, ERole::Pending});
        const auto lateOutcome = rng() % 2 ? NProto::SETUP_SUCCEEDED : NProto::SETUP_FAILED;
        auto lateResult = SetupResultRequest(GetDelegation(ctx, abandonedId), lateOutcome);
        if (lateOutcome == NProto::SETUP_FAILED) {
            auto* failure = lateResult.MutableSetSetupResult()->MutableSetupFailure();
            failure->SetStatus(Ydb::StatusIds::UNAUTHORIZED);
            failure->AddReasons(NProto::SETUP_FAILURE_SUBJECT_NOT_DELEGATABLE);
        }
        const auto oldCurrent = model.Current;
        std::array<ui32, 4> actions{0, 1, 2, 3};
        std::shuffle(actions.begin(), actions.end(), rng);
        for (const auto action : actions) {
            switch (action) {
                case 0: {
                    const auto before = ctx.GetRuntime().GetCurrentTime();
                    ctx.Call(lateResult);
                    auto& expected = model.Delegations.at(abandonedId);
                    expected.Role = lateOutcome == NProto::SETUP_SUCCEEDED
                        ? ERole::Revocable : ERole::Cancelled;
                    if (lateOutcome == NProto::SETUP_SUCCEEDED) {
                        expected.Queued = TQueueWindow{before, ctx.GetRuntime().GetCurrentTime()};
                    } else {
                        expected.Failure = EExpectedFailure::SubjectNotDelegatable;
                    }
                    // Duplicate terminal replies cannot retire current or clear
                    // a newer pending operation even after their CAS is stale.
                    ctx.Call(lateResult);
                    break;
                }
                case 1: {
                    const auto promote = PromoteRequest(ready, GetSecret(ctx, model.Identity).GetRevision());
                    const auto before = ctx.GetRuntime().GetCurrentTime();
                    if (rng() % 2) {
                        ctx.CrashCall(promote, ECrashPoint::ResponseLost);
                    }
                    ctx.Call(promote);
                    model.Current = replacementId;
                    model.Pending.clear();
                    model.Delegations.at(oldCurrent).Role = ERole::Revocable;
                    model.Delegations.at(oldCurrent).Queued = TQueueWindow{before, ctx.GetRuntime().GetCurrentTime()};
                    model.Delegations.at(replacementId).Role = ERole::Current;
                    break;
                }
                case 2:
                    ctx.Call(retire);
                    break;
                case 3:
                    ctx.Reboot();
                    break;
            }
            model.Check(ctx);
        }

        if (round % 2) {
            const auto before = ctx.GetRuntime().GetCurrentTime();
            ctx.Call(DropRequest(GetSecret(ctx, model.Identity)));
            model.Delegations.at(model.Current).Role = ERole::Revocable;
            model.Delegations.at(model.Current).Queued = TQueueWindow{before, ctx.GetRuntime().GetCurrentTime()};
            const auto oldIdentity = model.Identity;
            const TString incarnation = "incarnation-reused-" + label;
            // Identical database path and full secret PathId in a new database
            // incarnation must never alias the old identity.
            ctx.Call(RegisterRequest(DatabaseIdentity(incarnation, "/Root/database")));
            model.Incarnations.insert(incarnation);
            model.Identity = SecretIdentity(42, incarnation);
            const TString createId = "recreated-" + label;
            const auto binding = Binding("referrer-" + createId);
            const auto intent = ctx.Call(StageCreateRequest(createId, binding,
                "/Root/database/secret", incarnation, 100 + round)).GetDelegation();
            const auto bound = ctx.Call(BindRequest(intent, model.Identity)).GetDelegation();
            const auto setup = CompleteSetup(ctx, bound).GetDelegation();
            ctx.Call(PromoteRequest(setup, GetSecret(ctx, model.Identity).GetRevision()));
            model.Current = createId;
            model.Delegations.emplace(createId, TExpectedDelegation{binding, ERole::Current});
            ctx.Call(retire);
            ctx.Call(lateResult);
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx, oldIdentity).GetState(), NProto::SECRET_DROPPED);
            model.Check(ctx);
        }

        const auto firstClaims = ctx.Call(ClaimRequest("first-worker", 2));
        UNIT_ASSERT(firstClaims.DelegationsSize() > 0);
        for (const auto& record : firstClaims.GetDelegations()) {
            model.CheckClaim(record);
        }
        if (rng() % 2) {
            ctx.Reboot();
        }
        ctx.AdvanceTime(TDuration::Seconds(11));
        for (const auto& record : firstClaims.GetDelegations()) {
            ctx.Call(FinishRequest(record), NProto::STALE_CLAIM);
        }
        for (ui32 batch = 0;; ++batch) {
            UNIT_ASSERT_C(batch < 60, "The fixed recovery schedule did not drain cleanup");
            // Past both the maximum policy delay (including jitter) and lease.
            ctx.AdvanceTime(TDuration::Seconds(80));
            const auto claims = ctx.Call(ClaimRequest("recovery-worker", 2));
            if (!claims.DelegationsSize()) {
                break;
            }
            for (const auto& record : claims.GetDelegations()) {
                model.CheckClaim(record);
                auto& expected = model.Delegations.at(record.GetOperationId());
                auto next = expected;
                auto ack = FinishRequest(record, NProto::REVOKE_RETRY);
                if (expected.Progress == ERevokeProgress::Failing && expected.ConsecutiveFailures < 2 + round % 3) {
                    ack.MutableFinishRevocation()->SetRetryReason(NProto::REVOKE_RETRY_FAILURE);
                    ack.MutableFinishRevocation()->ClearRevokeOperationId();
                    ++next.ConsecutiveFailures;
                } else if (expected.Progress == ERevokeProgress::Failing) {
                    ack.MutableFinishRevocation()->SetRetryReason(NProto::REVOKE_RETRY_PENDING);
                    ack.MutableFinishRevocation()->SetRevokeOperationId("remote-cleanup-" + record.GetOperationId());
                    next.RevokeOperationId = ack.GetFinishRevocation().GetRevokeOperationId();
                    next.ConsecutiveFailures = 0;
                    next.Progress = ERevokeProgress::PendingReported;
                } else if (expected.Progress == ERevokeProgress::PendingReported) {
                    ack.MutableFinishRevocation()->SetRetryReason(NProto::REVOKE_RETRY_FAILURE);
                    ack.MutableFinishRevocation()->ClearRevokeOperationId();
                    next.ConsecutiveFailures = 1;
                    next.Progress = ERevokeProgress::UncertainAfterProgress;
                } else {
                    ack = FinishRequest(record);
                    ack.MutableFinishRevocation()->SetRevokeOperationId(expected.RevokeOperationId);
                    next.Role = ERole::Revoked;
                    // Completion removes outstanding work. Its old-shaped ACK
                    // does not erase the last durable failure history.
                }
                const auto before = ctx.GetRuntime().GetCurrentTime();
                if (rng() % 3 == 0) {
                    const auto point = static_cast<ECrashPoint>(rng() % 3);
                    ctx.CrashCall(ack, point);
                    if (point != ECrashPoint::BeforeCommit) {
                        expected = std::move(next);
                    }
                    ctx.Call(ack, NProto::STALE_CLAIM);
                    // Every remaining claim in this batch belongs to the old
                    // generation. Never let a dead owner finish another row.
                    for (const auto& old : claims.GetDelegations()) {
                        ctx.Call(FinishRequest(old), NProto::STALE_CLAIM);
                    }
                    break;
                }
                const auto saved = ctx.Call(ack).GetDelegation();
                expected = std::move(next);
                if (expected.Role == ERole::Revocable) {
                    UNIT_ASSERT(saved.GetDueAtUs() >= (before + TDuration::Seconds(1)).MicroSeconds());
                    UNIT_ASSERT(saved.GetDueAtUs() < (ctx.GetRuntime().GetCurrentTime() + TDuration::Seconds(75)).MicroSeconds());
                }
                UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ack).GetDelegation().SerializeAsString(), saved.SerializeAsString());
            }
            model.Check(ctx);
        }
        for (const auto& [_, delegation] : model.Delegations) {
            UNIT_ASSERT(delegation.Role != ERole::Revocable);
        }
        ctx.Reboot();
        model.Check(ctx);
    }
}

void CheckRecoverySeed(ui64 seed) {
    try {
        RunRecoveryChaos(seed);
    } catch (...) {
        ythrow yexception() << "IAM recovery chaos seed " << seed << ": " << CurrentExceptionMessage();
    }
}

} // namespace

Y_UNIT_TEST_SUITE(IamDelegationRecoveryChaos) {
    Y_UNIT_TEST(Seed1) {
        CheckRecoverySeed(1);
    }

    Y_UNIT_TEST(Seed330) {
        CheckRecoverySeed(330);
    }

    Y_UNIT_TEST(Seed20261013) {
        CheckRecoverySeed(20261013);
    }
}

} // namespace NKikimr::NIamDelegation::NTests
