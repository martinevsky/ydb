#include "requests.h"

namespace NKikimr::NIamDelegation::NTests {
namespace {

void LateOutcomePreservesNewPending(NProto::ESetupState outcome) {
    TTestContext ctx;
    CreateActive(ctx);
    auto pending = ctx.Call(StageAlterRequest(GetSecret(ctx), "unknown",
        Binding("unknown-referrer"))).GetDelegation();
    pending = ctx.Call(StartRequest(pending)).GetDelegation();
    pending = ctx.Call(SetupResultRequest(pending, NProto::SETUP_UNKNOWN)).GetDelegation();
    const auto retire = RetirePendingRequest(GetSecret(ctx), pending);
    const auto retired = ctx.Call(retire).GetDelegation();
    UNIT_ASSERT_VALUES_EQUAL(retired.GetState(), NProto::WAITING_SETUP);
    UNIT_ASSERT(GetSecret(ctx).GetPendingOperationId().empty());
    UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).GetCurrentOperationId(), "create-1");

    ctx.Reboot();
    ctx.AdvanceTime(TDuration::Days(30));
    UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
    ctx.Call(StageAlterRequest(GetSecret(ctx), "next", Binding("next-referrer")));
    const auto current = GetSecret(ctx);
    const auto resolved = ctx.Call(SetupResultRequest(retired, outcome)).GetDelegation();
    UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), current.SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").GetState(), NProto::ACTIVE);
    UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "next").GetState(), NProto::PENDING);
    UNIT_ASSERT_VALUES_EQUAL(resolved.GetState(), outcome == NProto::SETUP_SUCCEEDED
        ? NProto::REVOCATION_PENDING : NProto::CANCELLED);

    ctx.Reboot();
    const auto beforeReplay = GetDelegation(ctx, "unknown");
    const auto replay = ctx.Call(retire);
    UNIT_ASSERT_VALUES_EQUAL(replay.GetDelegation().SerializeAsString(), beforeReplay.SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), current.SerializeAsString());
    ctx.Call(PromoteRequest(resolved, current.GetRevision()), NProto::PRECONDITION_FAILED);
    const auto claims = ctx.Call(ClaimRequest());
    UNIT_ASSERT_VALUES_EQUAL(claims.DelegationsSize(), outcome == NProto::SETUP_SUCCEEDED ? 1 : 0);
    if (outcome == NProto::SETUP_SUCCEEDED) {
        UNIT_ASSERT_VALUES_EQUAL(claims.GetDelegations(0).GetOperationId(), "unknown");
        ctx.Call(FinishRequest(claims.GetDelegations(0)));
    }
    UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 2);
}

void RetireAcrossCrash(ECrashPoint point) {
    TTestContext ctx;
    CreateActive(ctx);
    auto pending = ctx.Call(StageAlterRequest(GetSecret(ctx), "pending",
        Binding("pending-referrer"))).GetDelegation();
    pending = ctx.Call(StartRequest(pending)).GetDelegation();
    const auto before = GetSecret(ctx);
    const auto request = RetirePendingRequest(before, pending);
    ctx.CrashCall(request, point);
    const auto recovered = GetSecret(ctx);
    UNIT_ASSERT_VALUES_EQUAL(recovered.GetCurrentOperationId(), "create-1");
    if (point == ECrashPoint::BeforeCommit) {
        UNIT_ASSERT_VALUES_EQUAL(recovered.SerializeAsString(), before.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "pending").GetState(), NProto::PENDING);
    } else {
        UNIT_ASSERT(recovered.GetPendingOperationId().empty());
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "pending").GetState(), NProto::WAITING_SETUP);
    }
    ctx.Call(request);
    ctx.Call(StageAlterRequest(GetSecret(ctx), "newer", Binding("newer-referrer")));
    const auto withNewer = GetSecret(ctx);
    ctx.Reboot();
    ctx.Call(request);
    UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), withNewer.SerializeAsString());
    ctx.Call(SetupResultRequest(GetDelegation(ctx, "pending"), NProto::SETUP_SUCCEEDED));
    const auto claimed = ctx.Call(ClaimRequest());
    UNIT_ASSERT_VALUES_EQUAL(claimed.DelegationsSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(claimed.GetDelegations(0).GetOperationId(), "pending");
}

} // namespace

Y_UNIT_TEST_SUITE(IamDelegationRecoveryCommands) {
    Y_UNIT_TEST(SchemaCreateIntentSurvivesReplayAndReboot) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity("100:1")));
        auto request = StageCreateRequest("schema-intent", Binding(), "/Root/database/secret", "100:1");
        auto* intent = request.MutableStage()->MutableSchemaCreate();
        intent->SetDatabasePath("/Root/database");
        intent->SetUserSID("cloud-user@as");
        intent->AddGroupSIDs("group@as");
        intent->SetParentPathOwnerId(100);
        intent->SetParentPathLocalId(2);
        intent->SetParentPathVersion(7);
        intent->SetDatabaseDomainOwnerId(100);
        intent->SetDatabaseDomainLocalId(1);
        intent->SetInheritPermissions(false);
        const auto saved = ctx.Call(request).GetDelegation();
        ctx.Reboot();
        const auto restored = GetDelegation(ctx, "schema-intent");
        UNIT_ASSERT_VALUES_EQUAL(restored.GetSchemaCreate().SerializeAsString(), intent->SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().SerializeAsString(), saved.SerializeAsString());
        auto changed = request;
        changed.MutableStage()->MutableSchemaCreate()->SetParentPathVersion(8);
        ctx.Call(changed, NProto::CONFLICT);
    }

    Y_UNIT_TEST(SchemaCreateIntentRejectsIncompleteOrUnboundedIdentity) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity("100:1")));
        auto request = StageCreateRequest("schema-intent", Binding(), "/Root/database/secret", "100:1");
        auto* intent = request.MutableStage()->MutableSchemaCreate();
        intent->SetDatabasePath("/Root/database");
        intent->SetUserSID("cloud-user@as");
        intent->SetParentPathOwnerId(100);
        intent->SetParentPathLocalId(2);
        intent->SetParentPathVersion(7);
        intent->SetDatabaseDomainOwnerId(100);
        intent->SetDatabaseDomainLocalId(1);
        intent->SetInheritPermissions(false);
        for (ui32 field = 0; field < 6; ++field) {
            auto invalid = request;
            auto* value = invalid.MutableStage()->MutableSchemaCreate();
            switch (field) {
                case 0: value->SetUserSID(""); break;
                case 1: value->SetParentPathOwnerId(0); break;
                case 2: value->SetDatabaseDomainLocalId(3); break;
                case 3: value->SetDatabasePath("/Root/other"); break;
                case 4: value->AddGroupSIDs(TString(257, 'g')); break;
                case 5: value->SetDiffACL(TString(65537, 'a')); break;
            }
            ctx.Call(invalid, NProto::INVALID_ARGUMENT);
        }
        ctx.Call(request);
    }

    Y_UNIT_TEST(StageSubjectIdentitySurvivesReplayAndReboot) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        auto request = StageCreateRequest();
        request.MutableStage()->SetSubjectId("user-subject");
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().GetSubjectId(), "user-subject");
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().GetSubjectId(), "user-subject");
        request.MutableStage()->SetSubjectId("other-subject");
        ctx.Call(request, NProto::CONFLICT);
        request.MutableStage()->SetSubjectId(TString(257, 's'));
        ctx.Call(request, NProto::INVALID_ARGUMENT);
    }

    Y_UNIT_TEST(FailedCreatingShellRemainsDiscoverableUntilDrop) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        const auto staged = ctx.Call(StageCreateRequest()).GetDelegation();
        auto bound = ctx.Call(BindRequest(staged)).GetDelegation();
        bound = ctx.Call(StartRequest(bound)).GetDelegation();
        ctx.Call(SetupResultRequest(bound, NProto::SETUP_FAILED));
        ctx.Reboot();
        auto inventory = ctx.Call(ListRequest());
        UNIT_ASSERT_VALUES_EQUAL(inventory.DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(inventory.GetDelegations(0).GetState(), NProto::CANCELLED);
        UNIT_ASSERT(GetSecret(ctx).GetCurrentOperationId().empty());
        UNIT_ASSERT(GetSecret(ctx).GetPendingOperationId().empty());
        ctx.Call(DropRequest(GetSecret(ctx)));
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 0);
        ctx.Call(StageCreateRequest("recreated", Binding("recreated-referrer")));
    }

    Y_UNIT_TEST(ReplacementPromotionRemovesTerminalCreatingRecoveryMarker) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        auto creating = ctx.Call(BindRequest(ctx.Call(StageCreateRequest()).GetDelegation())).GetDelegation();
        creating = ctx.Call(StartRequest(creating)).GetDelegation();
        ctx.Call(SetupResultRequest(creating, NProto::SETUP_FAILED));
        auto next = ctx.Call(StageAlterRequest(GetSecret(ctx), "replacement", Binding("replacement-referrer"))).GetDelegation();
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 2);
        next = CompleteSetup(ctx, next).GetDelegation();
        ctx.Call(PromoteRequest(next, GetSecret(ctx).GetRevision()));
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 1);
    }

    Y_UNIT_TEST(RetiredCreatingShellSurvivesRevocationCompletion) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        auto creating = ctx.Call(BindRequest(ctx.Call(StageCreateRequest()).GetDelegation())).GetDelegation();
        creating = CompleteSetup(ctx, creating).GetDelegation();
        ctx.Call(RetirePendingRequest(GetSecret(ctx), creating));
        ctx.Call(FinishRequest(ctx.Call(ClaimRequest()).GetDelegations(0)));
        ctx.Reboot();
        const auto inventory = ctx.Call(ListRequest());
        UNIT_ASSERT_VALUES_EQUAL(inventory.DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(inventory.GetDelegations(0).GetState(), NProto::REVOKED);
        ctx.Call(DropRequest(GetSecret(ctx)));
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 0);
    }

    Y_UNIT_TEST(StageTimestampSurvivesReplayAndReboot) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto request = StageAlterRequest(GetSecret(ctx), "timestamp", Binding("timestamp-referrer"));
        const auto staged = ctx.Call(request).GetDelegation();
        UNIT_ASSERT(staged.HasStagedAtUs());
        ctx.AdvanceTime(TDuration::Minutes(11));
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().GetStagedAtUs(), staged.GetStagedAtUs());
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().GetStagedAtUs(), staged.GetStagedAtUs());
    }

    Y_UNIT_TEST(TerminalRemoteRevokeFailureCanClearOperationBeforeRetry) {
        TTestContext ctx;
        CreateActive(ctx);
        ctx.Call(DropRequest(GetSecret(ctx)));
        auto claimed = ctx.Call(ClaimRequest()).GetDelegations(0);
        ctx.Call(FinishRequest(claimed, NProto::REVOKE_RETRY));
        claimed = ctx.Call(ClaimRequest()).GetDelegations(0);
        UNIT_ASSERT_VALUES_EQUAL(claimed.GetRevokeOperationId(), "iam-revoke-1");
        auto retain = FinishRequest(claimed, NProto::REVOKE_RETRY);
        retain.MutableFinishRevocation()->ClearRevokeOperationId();
        ctx.Call(retain);
        claimed = ctx.Call(ClaimRequest()).GetDelegations(0);
        UNIT_ASSERT_VALUES_EQUAL(claimed.GetRevokeOperationId(), "iam-revoke-1");

        auto clear = FinishRequest(claimed, NProto::REVOKE_RETRY);
        clear.MutableFinishRevocation()->ClearRevokeOperationId();
        clear.MutableFinishRevocation()->SetClearRevokeOperationId(true);
        const auto cleared = ctx.Call(clear).GetDelegation();
        UNIT_ASSERT(cleared.GetRevokeOperationId().empty());
        UNIT_ASSERT_VALUES_EQUAL(cleared.GetState(), NProto::REVOCATION_PENDING);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(clear).GetDelegation().SerializeAsString(), cleared.SerializeAsString());
        auto changed = clear;
        changed.MutableFinishRevocation()->SetClearRevokeOperationId(false);
        ctx.Call(changed, NProto::CONFLICT);
        ctx.Reboot();
        UNIT_ASSERT(GetDelegation(ctx, "create-1").GetRevokeOperationId().empty());
        const auto newClaim = ctx.Call(ClaimRequest()).GetDelegations(0);
        UNIT_ASSERT(newClaim.GetRevokeOperationId().empty());
        auto finish = FinishRequest(newClaim);
        finish.MutableFinishRevocation()->SetRevokeOperationId("iam-new-revoke");
        ctx.Call(finish);
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").GetRevokeOperationId(), "iam-new-revoke");
    }

    Y_UNIT_TEST(ClearRemoteRevokeOperationRejectsConflictingFields) {
        TTestContext ctx;
        CreateActive(ctx);
        ctx.Call(DropRequest(GetSecret(ctx)));
        const auto claimed = ctx.Call(ClaimRequest()).GetDelegations(0);
        auto invalid = FinishRequest(claimed, NProto::REVOKE_RETRY);
        invalid.MutableFinishRevocation()->SetClearRevokeOperationId(true);
        ctx.Call(invalid, NProto::INVALID_ARGUMENT);
        invalid.MutableFinishRevocation()->ClearRevokeOperationId();
        invalid.MutableFinishRevocation()->SetOutcome(NProto::REVOKE_SUCCEEDED);
        ctx.Call(invalid, NProto::INVALID_ARGUMENT);
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").SerializeAsString(), claimed.SerializeAsString());
    }

    Y_UNIT_TEST(DatabaseEnumerationDiscoversKnownEmptyIncarnationsAfterReboot) {
        TTestContext ctx;
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListDatabasesRequest()).DatabasesSize(), 0);
        ctx.Call(RegisterRequest(DatabaseIdentity("c", "/Root/reused", "cloud-c")));
        ctx.Call(RegisterRequest(DatabaseIdentity("a", "/Root/reused", "cloud-a")));
        ctx.Call(RegisterRequest(DatabaseIdentity("b", "/Root/other", "")));
        ctx.Reboot();
        const auto first = ctx.Call(ListDatabasesRequest(2));
        UNIT_ASSERT_VALUES_EQUAL(first.DatabasesSize(), 2);
        UNIT_ASSERT_VALUES_EQUAL(first.GetDatabases(0).GetIdentity().GetIncarnation(), "a");
        UNIT_ASSERT_VALUES_EQUAL(first.GetDatabases(0).GetIdentity().GetDatabaseId(), "cloud-a");
        UNIT_ASSERT_VALUES_EQUAL(first.GetDatabases(1).GetIdentity().GetIncarnation(), "b");
        UNIT_ASSERT_VALUES_EQUAL(first.GetNextAfterIncarnation(), "b");
        ctx.Reboot();
        const auto second = ctx.Call(ListDatabasesRequest(2, first.GetNextAfterIncarnation()));
        UNIT_ASSERT_VALUES_EQUAL(second.DatabasesSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(second.GetDatabases(0).GetIdentity().GetIncarnation(), "c");
        UNIT_ASSERT_VALUES_EQUAL(second.GetDatabases(0).GetIdentity().GetDatabaseId(), "cloud-c");
        UNIT_ASSERT(second.GetNextAfterIncarnation().empty());
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListDatabasesRequest(2, "z")).DatabasesSize(), 0);

        // Registry scans are for recovery, not immutable snapshots: an insertion
        // behind a cursor is discovered by the worker's next full scan.
        ctx.Call(RegisterRequest(DatabaseIdentity("aa", "/Root/new")));
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListDatabasesRequest(2, "b")).DatabasesSize(), 1);
        const auto restarted = ctx.Call(ListDatabasesRequest(2));
        UNIT_ASSERT_VALUES_EQUAL(restarted.GetDatabases(1).GetIdentity().GetIncarnation(), "aa");
        ctx.Call(ListDatabasesRequest(0), NProto::INVALID_ARGUMENT);
        ctx.Call(ListDatabasesRequest(1001), NProto::INVALID_ARGUMENT);
        ctx.Call(ListDatabasesRequest(1, TString(257, 'x')), NProto::INVALID_ARGUMENT);
    }

    Y_UNIT_TEST(RetiredUnknownSuccessQueuesOnlyRetiredBinding) {
        LateOutcomePreservesNewPending(NProto::SETUP_SUCCEEDED);
    }

    Y_UNIT_TEST(RetiredUnknownFailureCannotCancelNewPending) {
        LateOutcomePreservesNewPending(NProto::SETUP_FAILED);
    }

    Y_UNIT_TEST(RetireNotStartedCancelsWithoutDispatchOrRevocation) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto pending = ctx.Call(StageAlterRequest(GetSecret(ctx), "pending",
            Binding("pending-referrer"))).GetDelegation();
        const auto retired = ctx.Call(RetirePendingRequest(GetSecret(ctx), pending)).GetDelegation();
        UNIT_ASSERT_VALUES_EQUAL(retired.GetState(), NProto::CANCELLED);
        ctx.Call(StartRequest(retired), NProto::PRECONDITION_FAILED);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).GetCurrentOperationId(), "create-1");
        ctx.Call(StageAlterRequest(GetSecret(ctx), "next", Binding("next-referrer")));
    }

    Y_UNIT_TEST(RetireSuccessfulPendingQueuesItWithoutPromotingOrRetiringCurrent) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto staged = ctx.Call(StageAlterRequest(GetSecret(ctx), "pending",
            Binding("pending-referrer"))).GetDelegation();
        const auto ready = CompleteSetup(ctx, staged).GetDelegation();
        ctx.Call(RetirePendingRequest(GetSecret(ctx), ready));
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).GetCurrentOperationId(), "create-1");
        UNIT_ASSERT(GetSecret(ctx).GetPendingOperationId().empty());
        const auto claimed = ctx.Call(ClaimRequest());
        UNIT_ASSERT_VALUES_EQUAL(claimed.DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(claimed.GetDelegations(0).GetOperationId(), "pending");
        ctx.Call(PromoteRequest(GetDelegation(ctx, "pending"), GetSecret(ctx).GetRevision()),
            NProto::PRECONDITION_FAILED);
    }

    Y_UNIT_TEST(RetireRequiresImmutableIdentityAndBothCurrentRevisions) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto pending = ctx.Call(StageAlterRequest(GetSecret(ctx), "pending",
            Binding("pending-referrer"))).GetDelegation();
        const auto secret = GetSecret(ctx);
        const auto request = RetirePendingRequest(secret, pending);
        auto invalid = request;
        invalid.MutableRetirePending()->SetExpectedSecretRevision(secret.GetRevision() - 1);
        ctx.Call(invalid, NProto::PRECONDITION_FAILED);
        invalid = request;
        invalid.MutableRetirePending()->SetExpectedRevision(pending.GetRevision() + 1);
        ctx.Call(invalid, NProto::PRECONDITION_FAILED);
        invalid = request;
        invalid.MutableRetirePending()->MutableSecret()->SetPathLocalId(999);
        ctx.Call(invalid, NProto::CONFLICT);
        invalid = request;
        invalid.MutableRetirePending()->ClearOperationId();
        ctx.Call(invalid, NProto::INVALID_ARGUMENT);
        ctx.Call(RetirePendingRequest(secret, GetDelegation(ctx, "create-1")), NProto::PRECONDITION_FAILED);
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), secret.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "pending").SerializeAsString(), pending.SerializeAsString());

        ctx.Call(request);
        invalid = request;
        invalid.MutableRetirePending()->SetExpectedRevision(pending.GetRevision() + 1);
        ctx.Call(invalid, NProto::CONFLICT);
        ctx.Call(request);
    }

    Y_UNIT_TEST(RetirementBeforeCommit) {
        RetireAcrossCrash(ECrashPoint::BeforeCommit);
    }

    Y_UNIT_TEST(RetirementAfterDurableCommit) {
        RetireAcrossCrash(ECrashPoint::AfterDurableCommitBeforeComplete);
    }

    Y_UNIT_TEST(RetirementLostResponse) {
        RetireAcrossCrash(ECrashPoint::ResponseLost);
    }
}

} // namespace NKikimr::NIamDelegation::NTests
