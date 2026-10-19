#include "requests.h"
#include <ydb/core/security/iam_delegation/tablet/public/setup_failure.h>

#include <util/folder/tempdir.h>

namespace NKikimr::NIamDelegation::NTests {
namespace {

enum class EStorageOpenMode { Format, Reopen };

NFake::TStorage DiskStorage(const TTempDir& directory, EStorageOpenMode mode) {
    NFake::TStorage storage;
    storage.UseDisk = true;
    storage.FormatDisk = mode == EStorageOpenMode::Format;
    storage.DiskPath = directory.Name() + "/";
    // Exercise the real PDisk/VDisk path, with the same file across runtimes.
    storage.DiskSize = 8ull << 30;
    storage.SectorSize = 4096;
    storage.ChunkSize = 32ull << 20;
    return storage;
}

} // namespace

Y_UNIT_TEST_SUITE(IamDelegationStorageReopen) {
    Y_UNIT_TEST(InventorySigningKeySurvivesWholeRuntimeReplacement) {
        TTempDir directory;
        NProto::TRequest continuation;
        NProto::TResponse expected;
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
            CreateActive(ctx);
            ctx.Call(StageAlterRequest(GetSecret(ctx), "replacement", Binding("replacement-referrer")));
            const auto first = ctx.Call(AuthenticatedListRequest());
            UNIT_ASSERT(!first.GetNextPageToken().empty());
            continuation = AuthenticatedListRequest(1, first.GetNextPageToken());
            expected = ctx.Call(continuation);
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(continuation).SerializeAsString(), expected.SerializeAsString());
        }
    }



    Y_UNIT_TEST(RevocationFailureHistorySurvivesWholeRuntimeReplacement) {
        TTempDir directory;
        NProto::TDelegation saved;
        NProto::TRequest oldAck;
        NProto::TResponse inventory;
        TInstant clock;
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
            CreateActive(ctx);
            ctx.Call(DropRequest(GetSecret(ctx)));
            const auto claimed = ctx.Call(ClaimRequest()).GetDelegations(0);
            oldAck = FinishRequest(claimed, NProto::REVOKE_RETRY);
            oldAck.MutableFinishRevocation()->SetRetryReason(NProto::REVOKE_RETRY_FAILURE);
            saved = ctx.Call(oldAck).GetDelegation();
            UNIT_ASSERT_VALUES_EQUAL(saved.GetRevokeConsecutiveFailures(), 1);
            UNIT_ASSERT(saved.HasCleanupQueuedAtUs());
            inventory = ctx.Call(ListRequest());
            clock = ctx.GetRuntime().GetCurrentTime();
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            ctx.GetRuntime().UpdateCurrentTime(Max(clock, ctx.GetRuntime().GetCurrentTime()));
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").SerializeAsString(), saved.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), inventory.SerializeAsString());
            ctx.Call(oldAck, NProto::STALE_CLAIM);
            ctx.GetRuntime().UpdateCurrentTime(Max(ctx.GetRuntime().GetCurrentTime(),
                TInstant::MicroSeconds(saved.GetDueAtUs())));
            const auto claimed = ctx.Call(ClaimRequest("replacement-runtime", 1)).GetDelegations(0);
            auto ack = FinishRequest(claimed, NProto::REVOKE_RETRY);
            ack.MutableFinishRevocation()->SetRetryReason(NProto::REVOKE_RETRY_FAILURE);
            const auto retried = ctx.Call(ack).GetDelegation();
            UNIT_ASSERT_VALUES_EQUAL(retried.GetRevokeConsecutiveFailures(), 2);
            UNIT_ASSERT_VALUES_EQUAL(retried.GetCleanupQueuedAtUs(), saved.GetCleanupQueuedAtUs());
        }
    }


    Y_UNIT_TEST(AuthenticationFailureKindsSurviveWholeRuntimeDiskReopen) {
        for (auto reason : {NProto::SETUP_FAILURE_AUTHENTICATION_FAILED, NProto::SETUP_FAILURE_PERMISSION_DENIED}) {
            TTempDir directory;
            NProto::TRequest request;
            NProto::TDelegation saved;
            {
                TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
                ctx.Call(RegisterRequest(DatabaseIdentity()));
                auto pending = ctx.Call(StageCreateRequest()).GetDelegation();
                pending = ctx.Call(BindRequest(pending)).GetDelegation();
                pending = ctx.Call(StartRequest(pending)).GetDelegation();
                request = SetupResultRequest(pending, NProto::SETUP_FAILED);
                auto* failure = request.MutableSetSetupResult()->MutableSetupFailure();
                failure->SetStatus(Ydb::StatusIds::UNAUTHORIZED);
                failure->AddReasons(reason);
                saved = ctx.Call(request).GetDelegation();
            }
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            const auto restored = GetDelegation(ctx, "create-1");
            UNIT_ASSERT_VALUES_EQUAL(restored.SerializeAsString(), saved.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(restored.GetSetupFailure().ReasonsSize(), 1);
            UNIT_ASSERT_VALUES_EQUAL(restored.GetSetupFailure().GetReasons(0), reason);
            const auto message = DescribeSetupFailure(restored.GetSetupFailure());
            UNIT_ASSERT_STRING_CONTAINS(message, reason == NProto::SETUP_FAILURE_AUTHENTICATION_FAILED
                ? "authentication failed" : "permission denied");
            if (reason == NProto::SETUP_FAILURE_PERMISSION_DENIED) {
                UNIT_ASSERT_STRING_CONTAINS(message, "iam.serviceAccounts.user");
                UNIT_ASSERT_STRING_CONTAINS(message, "internal.service-control.delegator");
            }
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().SerializeAsString(), saved.SerializeAsString());
        }
    }

    Y_UNIT_TEST(SetupFailureDetailsSurviveWholeRuntimeReplacement) {
        TTempDir directory;
        NProto::TRequest request;
        NProto::TResponse saved;
        NProto::TDelegation active;
        TInstant clock;
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
            CreateActive(ctx);
            active = GetDelegation(ctx, "create-1");
            auto pending = ctx.Call(StageAlterRequest(GetSecret(ctx), "refused", Binding("refused-referrer"))).GetDelegation();
            pending = ctx.Call(StartRequest(pending)).GetDelegation();
            request = SetupResultRequest(pending, NProto::SETUP_FAILED, "refused-operation");
            auto* failure = request.MutableSetSetupResult()->MutableSetupFailure();
            failure->SetStatus(Ydb::StatusIds::UNAUTHORIZED);
            failure->AddReasons(NProto::SETUP_FAILURE_SUBJECT_NOT_DELEGATABLE);
            saved = ctx.Call(request);
            UNIT_ASSERT_VALUES_EQUAL(saved.GetDelegation().GetSetupFailure().SerializeAsString(), failure->SerializeAsString());
            clock = ctx.GetRuntime().GetCurrentTime();
        }
        for (ui32 reopen = 0; reopen < 2; ++reopen) {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            ctx.GetRuntime().UpdateCurrentTime(Max(clock, ctx.GetRuntime().GetCurrentTime()));
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "refused").SerializeAsString(), saved.GetDelegation().SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), saved.GetSecret().SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").SerializeAsString(), active.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().SerializeAsString(), saved.GetDelegation().SerializeAsString());
            auto conflict = request;
            conflict.MutableSetSetupResult()->MutableSetupFailure()->SetStatus(Ydb::StatusIds::BAD_REQUEST);
            ctx.Call(conflict, NProto::CONFLICT);
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
            clock = ctx.GetRuntime().GetCurrentTime();
        }
    }

    Y_UNIT_TEST(RetiredPendingAndItsReplayReceiptSurviveWholeRuntimeReplacement) {
        TTempDir directory;
        NProto::TRequest retirement;
        NProto::TSecretRecord live;
        NProto::TResponse inventory;
        TInstant clock;
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
            CreateActive(ctx);
            auto unknown = ctx.Call(StageAlterRequest(GetSecret(ctx), "unknown",
                Binding("unknown-referrer"))).GetDelegation();
            unknown = ctx.Call(StartRequest(unknown)).GetDelegation();
            unknown = ctx.Call(SetupResultRequest(unknown, NProto::SETUP_UNKNOWN)).GetDelegation();
            retirement = RetirePendingRequest(GetSecret(ctx), unknown);
            ctx.Call(retirement);
            ctx.Call(StageAlterRequest(GetSecret(ctx), "newer", Binding("newer-referrer")));
            live = GetSecret(ctx);
            inventory = ctx.Call(ListRequest());
            clock = ctx.GetRuntime().GetCurrentTime();
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            ctx.GetRuntime().UpdateCurrentTime(Max(clock, ctx.GetRuntime().GetCurrentTime()));
            const auto databases = ctx.Call(ListDatabasesRequest());
            UNIT_ASSERT_VALUES_EQUAL(databases.DatabasesSize(), 1);
            UNIT_ASSERT_VALUES_EQUAL(databases.GetDatabases(0).GetIdentity().GetIncarnation(), "database-incarnation-1");
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), inventory.SerializeAsString());
            ctx.Call(retirement);
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), live.SerializeAsString());
            ctx.AdvanceTime(TDuration::Days(1));
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
            ctx.Call(SetupResultRequest(GetDelegation(ctx, "unknown"), NProto::SETUP_SUCCEEDED));
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), live.SerializeAsString());
            clock = ctx.GetRuntime().GetCurrentTime();
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            ctx.GetRuntime().UpdateCurrentTime(Max(clock, ctx.GetRuntime().GetCurrentTime()));
            ctx.Call(retirement);
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), live.SerializeAsString());
            const auto claims = ctx.Call(ClaimRequest());
            UNIT_ASSERT_VALUES_EQUAL(claims.DelegationsSize(), 1);
            UNIT_ASSERT_VALUES_EQUAL(claims.GetDelegations(0).GetOperationId(), "unknown");
            ctx.Call(FinishRequest(claims.GetDelegations(0)));
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 2);
        }
    }

    Y_UNIT_TEST(DatabaseAndUnboundIntentSurviveWholeRuntimeReplacement) {
        TTempDir directory;
        NProto::TResponse inventory;
        NProto::TDelegation intent;
        ui32 generation = 0;
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
            ctx.Call(RegisterRequest(DatabaseIdentity()));
            intent = ctx.Call(StageCreateRequest()).GetDelegation();
            inventory = ctx.Call(ListRequest());
            generation = ctx.Generation();
        }
        UNIT_ASSERT((directory.Path() / "pdisk_1.dat").IsFile());
        for (ui32 restart = 0; restart < 3; ++restart) {
            // Nothing from the previous runtime survives except the disk file.
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            UNIT_ASSERT(ctx.Generation() > generation);
            generation = ctx.Generation();
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").SerializeAsString(), intent.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), inventory.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(StageCreateRequest()).GetDelegation().SerializeAsString(), intent.SerializeAsString());
            ctx.Call(StageCreateRequest("duplicate-name", Binding("unused-referrer")), NProto::CONFLICT);
            ctx.Call(StageCreateRequest("duplicate-referrer", Binding(), "/Root/database/other"), NProto::CONFLICT);
        }
    }

    Y_UNIT_TEST(MixedLifecycleAndCleanupSurviveWholeRuntimeReplacement) {
        TTempDir directory;
        NProto::TDelegation claimed;
        NProto::TDelegation unknown;
        NProto::TSecretRecord secret;
        NProto::TResponse inventory;
        NProto::TDelegation cancelled;
        TInstant clock;
        ui32 generation = 0;
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Format));
            CreateActive(ctx);
            const auto replacement = ctx.Call(StageAlterRequest(GetSecret(ctx), "replacement",
                Binding("replacement-referrer"))).GetDelegation();
            const auto ready = CompleteSetup(ctx, replacement).GetDelegation();
            ctx.Call(PromoteRequest(ready, GetSecret(ctx).GetRevision()));
            const auto claims = ctx.Call(ClaimRequest());
            UNIT_ASSERT_VALUES_EQUAL(claims.DelegationsSize(), 1);
            claimed = claims.GetDelegations(0);
            const auto pending = ctx.Call(StageAlterRequest(GetSecret(ctx), "unknown",
                Binding("unknown-referrer"))).GetDelegation();
            const auto started = ctx.Call(StartRequest(pending)).GetDelegation();
            unknown = ctx.Call(SetupResultRequest(started, NProto::SETUP_UNKNOWN, "uncertain-operation")).GetDelegation();
            const auto unbound = ctx.Call(StageCreateRequest("cancelled", Binding("cancelled-referrer"),
                "/Root/database/cancelled")).GetDelegation();
            cancelled = ctx.Call(DropRequest(unbound)).GetDelegation();
            secret = GetSecret(ctx);
            inventory = ctx.Call(ListRequest());
            UNIT_ASSERT_VALUES_EQUAL(inventory.DelegationsSize(), 3);
            clock = ctx.GetRuntime().GetCurrentTime();
            generation = ctx.Generation();
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            ctx.GetRuntime().UpdateCurrentTime(Max(clock, ctx.GetRuntime().GetCurrentTime()));
            UNIT_ASSERT(ctx.Generation() > generation);
            generation = ctx.Generation();
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).SerializeAsString(), secret.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "create-1").SerializeAsString(), claimed.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "unknown").SerializeAsString(), unknown.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "cancelled").SerializeAsString(), cancelled.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), inventory.SerializeAsString());
            ctx.Call(FinishRequest(claimed), NProto::STALE_CLAIM);
            ctx.AdvanceTime(TDuration::Seconds(11));
            const auto claims = ctx.Call(ClaimRequest("recovered-worker"));
            UNIT_ASSERT_VALUES_EQUAL(claims.DelegationsSize(), 1);
            claimed = claims.GetDelegations(0);
            AssertBinding(claimed, Binding());
            ctx.Call(FinishRequest(claimed, NProto::REVOKE_RETRY, TDuration::Seconds(30)));
            ctx.Call(DropRequest(GetSecret(ctx)));
            const auto retiredUnknown = GetDelegation(ctx, "unknown");
            UNIT_ASSERT_VALUES_EQUAL(retiredUnknown.GetState(), NProto::WAITING_SETUP);
            ctx.Call(SetupResultRequest(retiredUnknown, NProto::SETUP_SUCCEEDED, "uncertain-operation"));
            inventory = ctx.Call(ListRequest());
            clock = ctx.GetRuntime().GetCurrentTime();
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            ctx.GetRuntime().UpdateCurrentTime(Max(clock, ctx.GetRuntime().GetCurrentTime()));
            UNIT_ASSERT(ctx.Generation() > generation);
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), inventory.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx).GetState(), NProto::SECRET_DROPPED);
            const auto due = ctx.Call(ClaimRequest("third-runtime"));
            UNIT_ASSERT_VALUES_EQUAL(due.DelegationsSize(), 2);
            for (const auto& record : due.GetDelegations()) {
                UNIT_ASSERT(record.GetOperationId() == "replacement" || record.GetOperationId() == "unknown");
                ctx.Call(FinishRequest(record));
            }
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
            ctx.AdvanceTime(TDuration::Seconds(31));
            const auto retry = ctx.Call(ClaimRequest());
            UNIT_ASSERT_VALUES_EQUAL(retry.DelegationsSize(), 1);
            UNIT_ASSERT_VALUES_EQUAL(retry.GetDelegations(0).GetOperationId(), "create-1");
            ctx.Call(FinishRequest(retry.GetDelegations(0)));
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 0);
        }
        {
            TTestContext ctx(DiskStorage(directory, EStorageOpenMode::Reopen));
            for (const TString operation : {"create-1", "replacement", "unknown"}) {
                UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, operation).GetState(), NProto::REVOKED);
            }
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, "cancelled").GetState(), NProto::CANCELLED);
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), 0);
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
            ctx.Call(StageCreateRequest("reuse-referrer", Binding(), "/Root/database/new"), NProto::CONFLICT);
        }
    }
}

} // namespace NKikimr::NIamDelegation::NTests
