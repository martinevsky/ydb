#include "requests.h"

#include <util/generic/vector.h>

namespace NKikimr::NIamDelegation::NTests {

Y_UNIT_TEST_SUITE(IamDelegationInventoryTokens) {
    Y_UNIT_TEST(AuthenticatedInventoryContinuationSurvivesRestartAndNameReuse) {
        TTestContext ctx;
        CreateActive(ctx);
        ctx.Call(StageAlterRequest(GetSecret(ctx), "replacement", Binding("replacement-referrer")));
        const auto first = ctx.Call(AuthenticatedListRequest());
        UNIT_ASSERT_VALUES_EQUAL(first.GetPaginationCapability(), NProto::PAGINATION_CAPABILITY_AUTHENTICATED);
        UNIT_ASSERT_VALUES_EQUAL(first.DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(first.GetDelegations(0).GetOperationId(), "create-1");
        UNIT_ASSERT(!first.GetNextPageToken().empty());
        const auto continuation = AuthenticatedListRequest(1, first.GetNextPageToken());
        UNIT_ASSERT(continuation.GetListInventory().GetDatabaseIncarnation().empty());
        const auto second = ctx.Call(continuation);
        UNIT_ASSERT_VALUES_EQUAL(second.GetPaginationCapability(), NProto::PAGINATION_CAPABILITY_AUTHENTICATED);
        UNIT_ASSERT_VALUES_EQUAL(second.DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(second.GetDelegations(0).GetOperationId(), "replacement");
        UNIT_ASSERT(second.GetNextPageToken().empty());
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(continuation).SerializeAsString(), second.SerializeAsString());

        ctx.Call(RegisterRequest(DatabaseIdentity("recreated", "/Root/database", "new-database")));
        ctx.Call(StageCreateRequest("recreated-secret", Binding("recreated-referrer"),
            "/Root/database/secret", "recreated", 30));
        // The signed original identity wins over a later registry match. No fresh
        // discovery is needed to continue a retained incarnation after path reuse.
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(continuation).SerializeAsString(), second.SerializeAsString());
        auto wrongIncarnation = continuation;
        wrongIncarnation.MutableListInventory()->SetDatabaseIncarnation("recreated");
        ctx.Call(wrongIncarnation, NProto::INVALID_ARGUMENT);
        ctx.Call(DropRequest(GetSecret(ctx)));
        ctx.Call(continuation, NProto::SNAPSHOT_EXPIRED);
    }

    Y_UNIT_TEST(AuthenticatedInventoryRejectsTamperingAndConflictingFields) {
        TTestContext ctx;
        CreateActive(ctx);
        ctx.Call(StageAlterRequest(GetSecret(ctx), "replacement", Binding("replacement-referrer")));
        const auto first = ctx.Call(AuthenticatedListRequest());
        UNIT_ASSERT(!first.GetNextPageToken().empty());
        const auto inventory = ctx.Call(ListRequest());
        const auto token = first.GetNextPageToken();
        // Change every non-padding encoded byte independently. At least one
        // mutation falls in the payload and another in the authentication tag;
        // neither may select a different cursor or produce a valid page.
        for (size_t offset = 3; offset < token.size(); ++offset) {
            if (token[offset] == '.' || token[offset] == '=') {
                continue;
            }
            auto corrupted = token;
            corrupted[offset] = token[offset] == 'A' ? 'z' : 'A';
            ctx.Call(AuthenticatedListRequest(1, corrupted), NProto::INVALID_ARGUMENT);
        }
        for (const auto& malformed : TVector<TString>{"v1.legacy", "v2.", "v2.invalid.encoding", TString(16385, 'x')}) {
            ctx.Call(AuthenticatedListRequest(1, malformed), NProto::INVALID_ARGUMENT);
        }
        auto wrong = AuthenticatedListRequest(1, token);
        wrong.MutableListInventory()->SetDatabasePath("/Root/other");
        ctx.Call(wrong, NProto::INVALID_ARGUMENT);
        wrong = AuthenticatedListRequest(1, token);
        wrong.MutableListInventory()->SetAfterOperationId("forged-cursor");
        ctx.Call(wrong, NProto::INVALID_ARGUMENT);
        wrong = AuthenticatedListRequest(1, token);
        wrong.MutableListInventory()->SetInventoryRevision(first.GetDatabase().GetInventoryRevision() + 1);
        ctx.Call(wrong, NProto::INVALID_ARGUMENT);
        wrong = AuthenticatedListRequest(1, token);
        wrong.MutableListInventory()->SetPaginationMode(NProto::PAGINATION_STRUCTURED);
        ctx.Call(wrong, NProto::INVALID_ARGUMENT);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), inventory.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(AuthenticatedListRequest(1, token)).DelegationsSize(), 1);
    }

    Y_UNIT_TEST(AuthenticatedInventoryAdvertisesCapabilityOnEmptyAndFinalPages) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        const auto empty = ctx.Call(AuthenticatedListRequest());
        UNIT_ASSERT_VALUES_EQUAL(empty.GetPaginationCapability(), NProto::PAGINATION_CAPABILITY_AUTHENTICATED);
        UNIT_ASSERT_VALUES_EQUAL(empty.DelegationsSize(), 0);
        UNIT_ASSERT(empty.GetNextPageToken().empty());
        ctx.Call(StageCreateRequest());
        const auto final = ctx.Call(AuthenticatedListRequest());
        UNIT_ASSERT_VALUES_EQUAL(final.GetPaginationCapability(), NProto::PAGINATION_CAPABILITY_AUTHENTICATED);
        UNIT_ASSERT_VALUES_EQUAL(final.DelegationsSize(), 1);
        UNIT_ASSERT(final.GetNextPageToken().empty());
        ctx.Call(AuthenticatedListRequest(0), NProto::INVALID_ARGUMENT);
        ctx.Call(AuthenticatedListRequest(1001), NProto::INVALID_ARGUMENT);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(AuthenticatedListRequest(1000)).DelegationsSize(), 1);
        // Old trusted clients retain the separate structured-cursor protocol.
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).GetDelegations(0).SerializeAsString(),
            final.GetDelegations(0).SerializeAsString());
    }

    Y_UNIT_TEST(AuthenticatedInventoryRejectsTokenFromIndependentTabletStorage) {
        TString token;
        {
            TTestContext first;
            CreateActive(first);
            first.Call(StageAlterRequest(GetSecret(first), "replacement", Binding("replacement-referrer")));
            token = first.Call(AuthenticatedListRequest()).GetNextPageToken();
            UNIT_ASSERT(!token.empty());
        }
        {
            TTestContext second;
            CreateActive(second);
            second.Call(StageAlterRequest(GetSecret(second), "replacement", Binding("replacement-referrer")));
            // Same identities and tablet ID in an independently formatted storage
            // still cannot authenticate another tablet instance's continuation.
            second.Call(AuthenticatedListRequest(1, token), NProto::INVALID_ARGUMENT);
            UNIT_ASSERT(!second.Call(AuthenticatedListRequest()).GetNextPageToken().empty());
        }
    }

    // Unknown enum numbers are stored in protobuf unknown fields; default getters
    // must not silently turn an unsupported mode into a trusted structured request.
    Y_UNIT_TEST(UnknownPaginationModeFailsClosedWithoutChangingInventory) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto before = ctx.Call(ListRequest());
        auto request = ListRequest();
        auto* list = request.MutableListInventory();
        list->GetReflection()->MutableUnknownFields(list)->AddVarint(7, 999);
        ctx.Call(request, NProto::INVALID_ARGUMENT);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), before.SerializeAsString());
    }

    Y_UNIT_TEST(ExplicitEmptyPaginationFieldsCannotMixProtocols) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto before = ctx.Call(ListRequest());
        auto request = AuthenticatedListRequest();
        request.MutableListInventory()->SetAfterOperationId("");
        ctx.Call(request, NProto::INVALID_ARGUMENT);
        request = AuthenticatedListRequest();
        request.MutableListInventory()->SetInventoryRevision(0);
        ctx.Call(request, NProto::INVALID_ARGUMENT);
        for (const auto mode : {NProto::PAGINATION_LEGACY, NProto::PAGINATION_STRUCTURED}) {
            request = ListRequest();
            request.MutableListInventory()->SetPaginationMode(mode);
            request.MutableListInventory()->SetContinuationToken("");
            ctx.Call(request, NProto::INVALID_ARGUMENT);
            request = ListRequest();
            request.MutableListInventory()->SetPaginationMode(mode);
            request.MutableListInventory()->SetDatabasePath("");
            ctx.Call(request, NProto::INVALID_ARGUMENT);
        }
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), before.SerializeAsString());
    }

    Y_UNIT_TEST(KnownAndUnknownPaginationModesCannotBypassValidation) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto before = ctx.Call(ListRequest());
        auto request = ListRequest();
        auto* list = request.MutableListInventory();
        list->SetPaginationMode(NProto::PAGINATION_STRUCTURED);
        list->GetReflection()->MutableUnknownFields(list)->AddVarint(7, 999);
        ctx.Call(request, NProto::INVALID_ARGUMENT);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), before.SerializeAsString());
    }

    // Independent-key rejection has its own same-ID case above; this case
    // exercises dispatch to a different tablet ID.
    Y_UNIT_TEST(AuthenticatedInventoryRejectsContinuationOnDifferentTabletId) {
        TTestContext first;
        CreateActive(first);
        first.Call(StageAlterRequest(GetSecret(first), "replacement", Binding("replacement-referrer")));
        const auto token = first.Call(AuthenticatedListRequest()).GetNextPageToken();
        UNIT_ASSERT(!token.empty());
        TTestContext second({}, MakeTabletID(false, 2));
        CreateActive(second);
        second.Call(StageAlterRequest(GetSecret(second), "replacement", Binding("replacement-referrer")));
        second.Call(AuthenticatedListRequest(1, token), NProto::INVALID_ARGUMENT);
    }

    // This case establishes an integrity/validation failure even before the tablet
    // can emit an authenticated page: it must not silently ignore an opaque token
    // when all legacy routing fields would otherwise form a valid first-page read.
    Y_UNIT_TEST(InjectedUnauthenticatedContinuationCannotFallBackToFirstPage) {
        TTestContext ctx;
        CreateActive(ctx);
        const auto before = ctx.Call(ListRequest());
        for (const auto& forged : TVector<TString>{"v1.legacy", "v2.Zm9yZ2Vk.Zm9yZ2Vk", TString(16385, 'x')}) {
            auto request = AuthenticatedListRequest(1, forged);
            request.MutableListInventory()->SetDatabaseIncarnation("database-incarnation-1");
            ctx.Call(request, NProto::INVALID_ARGUMENT);
        }
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListRequest()).SerializeAsString(), before.SerializeAsString());
    }

}

} // namespace NKikimr::NIamDelegation::NTests
