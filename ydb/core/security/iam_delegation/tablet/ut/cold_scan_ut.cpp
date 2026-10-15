#include "requests.h"

#include <ydb/core/base/localdb.h>
#include <ydb/core/tablet_flat/shared_cache_events.h>
#include <ydb/core/security/iam_delegation/tablet/schema.h>

#include <util/generic/set.h>
#include <util/string/cast.h>

namespace NKikimr::NIamDelegation::NTests {
namespace {

void EnableSmallCompactions(TTestContext& ctx, std::initializer_list<ui32> tables) {
    auto event = MakeHolder<TEvTablet::TEvLocalSchemeTx>();
    for (const ui32 table : tables) {
        auto* delta = event->Record.MutableSchemeChanges()->AddDelta();
        delta->SetDeltaType(NTabletFlatScheme::TAlterRecord::SetCompactionPolicy);
        delta->SetTableId(table);
        NLocalDb::TCompactionPolicy policy;
        policy.InMemSizeToSnapshot = 16 * 1024;
        policy.InMemStepsToSnapshot = 1;
        policy.InMemForceStepsToSnapshot = 16;
        policy.InMemForceSizeToSnapshot = 32 * 1024;
        policy.MinDataPageSize = 1024;
        policy.Serialize(*delta->MutableCompactionPolicy());
    }
    auto& runtime = ctx.GetRuntime();
    const auto edge = runtime.AllocateEdgeActor();
    runtime.SendToPipe(TTestContext::TabletId, edge, event.Release(), 0, GetPipeConfigWithRetries());
    const auto response = runtime.GrabEdgeEvent<TEvTablet::TEvLocalSchemeTxResponse>(edge);
    UNIT_ASSERT_VALUES_EQUAL(response->Get()->Record.GetStatus(), NKikimrProto::OK);
}

NProto::TIamBinding WideBinding(const TString& operation) {
    auto binding = Binding("referrer-" + operation);
    binding.SetCloudId(TString(200, 'c'));
    binding.SetServiceAccountId(TString(200, 'a'));
    binding.SetServiceId(TString(200, 's'));
    binding.SetMicroserviceId(TString(200, 'm'));
    binding.SetResourceType(TString(200, 'r'));
    return binding;
}

class TPageReadEvidence {
public:
    explicit TPageReadEvidence(TTestContext& ctx)
        : Compactions(ctx.GetRuntime().AddObserver<NSharedCache::TEvSaveCompactedPages>([this](auto& event) {
            if (event->Get()->PageCollection->Label().TabletID() == TTestContext::TabletId) {
                ++CompactedCollections;
            }
        }))
        , Reads(ctx.GetRuntime().AddObserver<NSharedCache::TEvRequest>([this](auto& event) {
            if (event->Get()->PageCollection->Label().TabletID() == TTestContext::TabletId
                && event->Cookie == static_cast<ui64>(NSharedCache::ERequestTypeCookie::Transaction)) {
                ++TransactionFetches;
            }
        }))
    {}

    ui64 CompactedCollections = 0;
    ui64 TransactionFetches = 0;

private:
    TTestActorRuntime::TEventObserverHolder Compactions;
    TTestActorRuntime::TEventObserverHolder Reads;
};

} // namespace

Y_UNIT_TEST_SUITE(IamDelegationColdScans) {
    Y_UNIT_TEST(AuthenticatedPaginationRetriesColdPagesWithoutPreloadingInventory) {
        TTestContext ctx;
        TPageReadEvidence evidence(ctx);
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        EnableSmallCompactions(ctx, {TSchema::Inventory::TableId});
        constexpr ui32 count = 257;
        TSet<TString> expected;
        for (ui32 index = 0; index < count; ++index) {
            const TString operation = "operation-" + ToString(index);
            ctx.Call(StageCreateRequest(operation, WideBinding(operation),
                "/Root/database/secret-" + ToString(index)));
            expected.insert(operation);
        }
        UNIT_ASSERT_C(evidence.CompactedCollections > 0, "The test must read persisted parts, not only the memtable");
        ctx.Reboot();
        evidence.TransactionFetches = 0;

        auto request = AuthenticatedListRequest(64);
        TSet<TString> actual;
        ui64 revision = 0;
        ui32 pages = 0;
        ui64 firstPageFetches = 0;
        TString previous;
        while (true) {
            const auto response = ctx.Call(request);
            UNIT_ASSERT(++pages <= 5);
            if (pages == 1) {
                firstPageFetches = evidence.TransactionFetches;
            }
            if (!revision) {
                revision = response.GetDatabase().GetInventoryRevision();
            }
            UNIT_ASSERT_VALUES_EQUAL(response.GetDatabase().GetInventoryRevision(), revision);
            for (const auto& record : response.GetDelegations()) {
                UNIT_ASSERT(actual.insert(record.GetOperationId()).second);
                UNIT_ASSERT(previous.empty() || previous < record.GetOperationId());
                previous = record.GetOperationId();
                AssertBinding(record, WideBinding(record.GetOperationId()));
            }
            if (response.GetNextPageToken().empty()) {
                UNIT_ASSERT_VALUES_EQUAL(response.DelegationsSize(), 1);
                break;
            }
            UNIT_ASSERT_VALUES_EQUAL(response.DelegationsSize(), 64);
            UNIT_ASSERT_VALUES_EQUAL(response.GetPaginationCapability(), NProto::PAGINATION_CAPABILITY_AUTHENTICATED);
            request = AuthenticatedListRequest(64, response.GetNextPageToken());
        }
        UNIT_ASSERT_VALUES_EQUAL(pages, 5);
        UNIT_ASSERT(actual == expected);
        UNIT_ASSERT_C(evidence.TransactionFetches > firstPageFetches,
            "The first bounded page must not preload the entire inventory");
        UNIT_ASSERT_C(evidence.TransactionFetches > 1, "Cold transaction page reads must actually exercise Execute retries");
    }


    Y_UNIT_TEST(DisabledAdmissionReplaysColdCommittedIntentsWithoutMutation) {
        TTestContext ctx;
        TPageReadEvidence evidence(ctx);
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        EnableSmallCompactions(ctx, {TSchema::Delegations::TableId});
        TVector<std::pair<NProto::TRequest, NProto::TDelegation>> committed;
        for (ui32 index = 0; index < 40; ++index) {
            const TString operation = "operation-" + ToString(index);
            auto request = StageCreateRequest(operation, WideBinding(operation),
                "/Root/database/secret-" + ToString(index));
            auto record = ctx.Call(request).GetDelegation();
            committed.emplace_back(std::move(request), std::move(record));
        }
        UNIT_ASSERT(evidence.CompactedCollections > 0);
        ctx.Reboot();
        ctx.GetRuntime().GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(false);
        evidence.TransactionFetches = 0;
        for (const auto& [request, record] : committed) {
            UNIT_ASSERT_VALUES_EQUAL(ctx.Call(request).GetDelegation().SerializeAsString(), record.SerializeAsString());
        }
        UNIT_ASSERT_C(evidence.TransactionFetches > 1, "Saved Stage replay must actually retry cold page reads");
        auto fresh = StageCreateRequest("fresh", WideBinding("fresh"), "/Root/database/fresh");
        ctx.Call(fresh, NProto::PRECONDITION_FAILED);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), committed.size());
    }

    Y_UNIT_TEST(DatabaseEnumerationRetriesColdPagesWithoutPreloadingRegistry) {
        TTestContext ctx;
        TPageReadEvidence evidence(ctx);
        EnableSmallCompactions(ctx, {TSchema::Databases::TableId});
        TSet<TString> expected;
        for (ui32 index = 0; index < 257; ++index) {
            const TString incarnation = "incarnation-" + ToString(index);
            ctx.Call(RegisterRequest(DatabaseIdentity(incarnation,
                "/Root/" + TString(200, 'p') + ToString(index), TString(200, 'd'))));
            expected.insert(incarnation);
        }
        UNIT_ASSERT(evidence.CompactedCollections > 0);
        ctx.Reboot();
        evidence.TransactionFetches = 0;

        auto request = ListDatabasesRequest(64);
        TSet<TString> actual;
        ui32 pages = 0;
        ui64 firstPageFetches = 0;
        TString previous;
        for (;;) {
            const auto response = ctx.Call(request);
            UNIT_ASSERT(++pages <= 5);
            if (pages == 1) {
                firstPageFetches = evidence.TransactionFetches;
            }
            for (const auto& record : response.GetDatabases()) {
                const auto& incarnation = record.GetIdentity().GetIncarnation();
                UNIT_ASSERT(actual.insert(incarnation).second);
                UNIT_ASSERT(previous.empty() || previous < incarnation);
                previous = incarnation;
                UNIT_ASSERT_VALUES_EQUAL(record.GetIdentity().GetDatabaseId(), TString(200, 'd'));
            }
            if (response.GetNextAfterIncarnation().empty()) {
                UNIT_ASSERT_VALUES_EQUAL(response.DatabasesSize(), 1);
                break;
            }
            UNIT_ASSERT_VALUES_EQUAL(response.DatabasesSize(), 64);
            UNIT_ASSERT_VALUES_EQUAL(response.GetNextAfterIncarnation(), previous);
            request.MutableListDatabases()->SetAfterIncarnation(previous);
        }
        UNIT_ASSERT(actual == expected);
        UNIT_ASSERT_VALUES_EQUAL(pages, 5);
        UNIT_ASSERT_C(evidence.TransactionFetches > firstPageFetches,
            "A bounded database page must not preload the entire registry");
        UNIT_ASSERT_C(evidence.TransactionFetches > 1, "Database enumeration must retry cold transaction reads");
    }

    Y_UNIT_TEST(PaginationRetriesColdPagesWithoutDuplicatesOrShortNonfinalPages) {
        TTestContext ctx;
        TPageReadEvidence evidence(ctx);
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        EnableSmallCompactions(ctx, {TSchema::Inventory::TableId});
        constexpr ui32 count = 257;
        TSet<TString> expected;
        for (ui32 index = 0; index < count; ++index) {
            const TString operation = "operation-" + ToString(index);
            ctx.Call(StageCreateRequest(operation, WideBinding(operation),
                "/Root/database/secret-" + ToString(index)));
            expected.insert(operation);
        }
        UNIT_ASSERT_C(evidence.CompactedCollections > 0, "The test must read persisted parts, not only the memtable");
        ctx.Reboot();
        evidence.TransactionFetches = 0;

        auto request = ListRequest("database-incarnation-1", 64);
        TSet<TString> actual;
        ui64 revision = 0;
        ui32 pages = 0;
        ui64 firstPageFetches = 0;
        TString previous;
        while (true) {
            const auto response = ctx.Call(request);
            UNIT_ASSERT(++pages <= 5);
            if (pages == 1) {
                firstPageFetches = evidence.TransactionFetches;
            }
            if (!revision) {
                revision = response.GetDatabase().GetInventoryRevision();
            }
            UNIT_ASSERT_VALUES_EQUAL(response.GetDatabase().GetInventoryRevision(), revision);
            for (const auto& record : response.GetDelegations()) {
                UNIT_ASSERT(actual.insert(record.GetOperationId()).second);
                UNIT_ASSERT(previous.empty() || previous < record.GetOperationId());
                previous = record.GetOperationId();
                AssertBinding(record, WideBinding(record.GetOperationId()));
            }
            if (response.GetNextAfterOperationId().empty()) {
                UNIT_ASSERT_VALUES_EQUAL(response.DelegationsSize(), 1);
                break;
            }
            UNIT_ASSERT_VALUES_EQUAL(response.DelegationsSize(), 64);
            UNIT_ASSERT_VALUES_EQUAL(response.GetNextAfterOperationId(), previous);
            request.MutableListInventory()->SetAfterOperationId(previous);
            request.MutableListInventory()->SetInventoryRevision(revision);
        }
        UNIT_ASSERT_VALUES_EQUAL(pages, 5);
        UNIT_ASSERT(actual == expected);
        UNIT_ASSERT_C(evidence.TransactionFetches > firstPageFetches,
            "The first bounded page must not preload the entire inventory");
        UNIT_ASSERT_C(evidence.TransactionFetches > 1, "Cold transaction page reads must actually exercise Execute retries");
    }

    Y_UNIT_TEST(ClaimBatchRetriesColdReadsWithoutDuplicateClaimsOrPartialWrites) {
        TTestContext ctx;
        TPageReadEvidence evidence(ctx);
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        EnableSmallCompactions(ctx, {TSchema::Delegations::TableId, TSchema::Revocations::TableId});
        constexpr ui32 count = 40;
        TSet<TString> expected;
        for (ui32 index = 0; index < count; ++index) {
            const TString operation = "operation-" + ToString(index);
            const auto staged = ctx.Call(StageCreateRequest(operation, WideBinding(operation),
                "/Root/database/secret-" + ToString(index))).GetDelegation();
            const auto identity = SecretIdentity(index + 1);
            const auto bound = ctx.Call(BindRequest(staged, identity)).GetDelegation();
            const auto ready = CompleteSetup(ctx, bound).GetDelegation();
            ctx.Call(PromoteRequest(ready, GetSecret(ctx, identity).GetRevision()));
            ctx.Call(DropRequest(GetSecret(ctx, identity)));
            expected.insert(operation);
        }
        UNIT_ASSERT(evidence.CompactedCollections > 0);
        ctx.Reboot();
        evidence.TransactionFetches = 0;
        TSet<TString> actual;
        for (ui32 batch = 0; batch < 3; ++batch) {
            const auto response = ctx.Call(ClaimRequest("worker-" + ToString(batch), 16));
            if (batch == 0) {
                UNIT_ASSERT_C(evidence.TransactionFetches > 1, "The first claim must fault before verification reads warm its pages");
            }
            UNIT_ASSERT_VALUES_EQUAL(response.DelegationsSize(), batch == 2 ? 8 : 16);
            for (const auto& record : response.GetDelegations()) {
                UNIT_ASSERT(actual.insert(record.GetOperationId()).second);
                UNIT_ASSERT_VALUES_EQUAL(record.GetState(), NProto::REVOKING);
                UNIT_ASSERT_VALUES_EQUAL(record.GetClaim().GetAttempt(), 1);
                AssertBinding(record, WideBinding(record.GetOperationId()));
                UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, record.GetOperationId()).SerializeAsString(), record.SerializeAsString());
            }
        }
        UNIT_ASSERT(actual == expected);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ClaimRequest()).DelegationsSize(), 0);
        UNIT_ASSERT_C(evidence.TransactionFetches > 1, "Claim reads must actually fault into persisted parts");
        ctx.Reboot();
        for (const auto& operation : expected) {
            const auto record = GetDelegation(ctx, operation);
            UNIT_ASSERT_VALUES_EQUAL(record.GetState(), NProto::REVOKING);
            UNIT_ASSERT_VALUES_EQUAL(record.GetClaim().GetAttempt(), 1);
        }
    }
    Y_UNIT_TEST(PromotionAndDropRetryColdReadsWithoutPartialLifecycleTransitions) {
        TTestContext ctx;
        TPageReadEvidence evidence(ctx);
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        EnableSmallCompactions(ctx, {TSchema::Secrets::TableId, TSchema::Delegations::TableId,
            TSchema::Inventory::TableId, TSchema::Revocations::TableId});
        struct TPair {
            NProto::TSecretIdentity Identity;
            NProto::TDelegation Old;
            NProto::TDelegation Ready;
            NProto::TRequest Promote;
            NProto::TRequest Drop;
            TString UnknownId;
        };
        TVector<TPair> pairs;
        constexpr ui32 count = 20;
        for (ui32 index = 0; index < count; ++index) {
            const TString suffix = ToString(index);
            TPair pair;
            pair.Identity = SecretIdentity(100 + index);
            const auto oldId = "old-" + suffix;
            auto old = ctx.Call(StageCreateRequest(oldId, WideBinding(oldId),
                "/Root/database/secret-" + suffix, "database-incarnation-1", 100 + index)).GetDelegation();
            old = ctx.Call(BindRequest(old, pair.Identity)).GetDelegation();
            old = CompleteSetup(ctx, old).GetDelegation();
            pair.Old = ctx.Call(PromoteRequest(old, GetSecret(ctx, pair.Identity).GetRevision())).GetDelegation();
            const auto nextId = "next-" + suffix;
            auto next = ctx.Call(StageAlterRequest(GetSecret(ctx, pair.Identity), nextId,
                WideBinding(nextId))).GetDelegation();
            pair.Ready = CompleteSetup(ctx, next).GetDelegation();
            pair.Promote = PromoteRequest(pair.Ready, GetSecret(ctx, pair.Identity).GetRevision());
            pair.UnknownId = "unknown-" + suffix;
            pairs.push_back(std::move(pair));
        }
        UNIT_ASSERT(evidence.CompactedCollections > 0);
        ctx.Reboot();
        evidence.TransactionFetches = 0;
        for (ui32 index = 0; index < count; ++index) {
            auto& pair = pairs[index];
            const auto promoted = ctx.Call(pair.Promote);
            if (!index) {
                UNIT_ASSERT_C(evidence.TransactionFetches > 1,
                    "Promotion must retry actual cold reads before assertions warm the records");
            }
            UNIT_ASSERT_VALUES_EQUAL(promoted.GetDelegation().GetState(), NProto::ACTIVE);
            UNIT_ASSERT_VALUES_EQUAL(promoted.GetDelegation().GetRevision(), pair.Ready.GetRevision() + 1);
            UNIT_ASSERT_VALUES_EQUAL(promoted.GetSecret().GetCurrentOperationId(), pair.Ready.GetOperationId());
            UNIT_ASSERT(promoted.GetSecret().GetPendingOperationId().empty());
            const auto retired = GetDelegation(ctx, pair.Old.GetOperationId());
            UNIT_ASSERT_VALUES_EQUAL(retired.GetState(), NProto::REVOCATION_PENDING);
            UNIT_ASSERT_VALUES_EQUAL(retired.GetRevision(), pair.Old.GetRevision() + 1);
            AssertBinding(retired, pair.Old.GetBinding());
            ctx.Call(pair.Promote); // a stale CAS receipt cannot enqueue or promote twice
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx, pair.Identity).SerializeAsString(), promoted.GetSecret().SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, retired.GetOperationId()).SerializeAsString(), retired.SerializeAsString());
            auto unknown = ctx.Call(StageAlterRequest(promoted.GetSecret(), pair.UnknownId,
                WideBinding(pair.UnknownId))).GetDelegation();
            unknown = ctx.Call(StartRequest(unknown)).GetDelegation();
            ctx.Call(SetupResultRequest(unknown, NProto::SETUP_UNKNOWN, "iam-" + pair.UnknownId));
            pair.Drop = DropRequest(GetSecret(ctx, pair.Identity));
        }
        ctx.Reboot();
        evidence.TransactionFetches = 0;
        for (ui32 index = 0; index < count; ++index) {
            const auto& pair = pairs[index];
            const auto dropped = ctx.Call(pair.Drop);
            if (!index) {
                UNIT_ASSERT_C(evidence.TransactionFetches > 1,
                    "Drop must retry cold active/pending reads before verification");
            }
            UNIT_ASSERT_VALUES_EQUAL(dropped.GetSecret().GetState(), NProto::SECRET_DROPPED);
            UNIT_ASSERT(dropped.GetSecret().GetCurrentOperationId().empty());
            UNIT_ASSERT(dropped.GetSecret().GetPendingOperationId().empty());
            const auto current = GetDelegation(ctx, pair.Ready.GetOperationId());
            const auto unknown = GetDelegation(ctx, pair.UnknownId);
            UNIT_ASSERT_VALUES_EQUAL(current.GetState(), NProto::REVOCATION_PENDING);
            UNIT_ASSERT_VALUES_EQUAL(unknown.GetState(), NProto::WAITING_SETUP);
            UNIT_ASSERT_VALUES_EQUAL(unknown.GetSetupState(), NProto::SETUP_UNKNOWN);
            ctx.Call(pair.Drop);
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(ctx, pair.Identity).SerializeAsString(), dropped.GetSecret().SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, pair.Ready.GetOperationId()).SerializeAsString(), current.SerializeAsString());
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, pair.UnknownId).SerializeAsString(), unknown.SerializeAsString());
        }
        ctx.Reboot();
        const auto claims = ctx.Call(ClaimRequest("after-cold-transitions", 100));
        UNIT_ASSERT_VALUES_EQUAL(claims.DelegationsSize(), count * 2);
        TSet<TString> claimed;
        for (const auto& delegation : claims.GetDelegations()) {
            UNIT_ASSERT(claimed.insert(delegation.GetOperationId()).second);
            UNIT_ASSERT_VALUES_EQUAL(delegation.GetSetupState(), NProto::SETUP_SUCCEEDED);
            UNIT_ASSERT_VALUES_EQUAL(delegation.GetClaim().GetAttempt(), 1);
        }
        for (const auto& pair : pairs) {
            UNIT_ASSERT(claimed.contains(pair.Old.GetOperationId()));
            UNIT_ASSERT(claimed.contains(pair.Ready.GetOperationId()));
            UNIT_ASSERT(!claimed.contains(pair.UnknownId));
            UNIT_ASSERT_VALUES_EQUAL(GetDelegation(ctx, pair.UnknownId).GetState(), NProto::WAITING_SETUP);
        }
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).DelegationsSize(), count * 3);
    }

}

} // namespace NKikimr::NIamDelegation::NTests
