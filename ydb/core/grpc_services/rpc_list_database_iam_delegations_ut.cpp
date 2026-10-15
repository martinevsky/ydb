#include "service_cms.h"

#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/local_rpc/local_rpc.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/security/iam_delegation/tablet/public/events.h>

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/string_utils/base64/base64.h>
#include <ydb/core/security/iam_delegation/tablet/ut/requests.h>
#include <util/generic/set.h>
#include <util/string/cast.h>

#include <deque>

namespace NKikimr::NGRpcService {
namespace {

using namespace NActors;
using namespace NKikimrIamDelegation;
using TTabletEvents = NIamDelegation::TEvIamDelegationTablet;
using TRpc = TGrpcRequestOperationCall<Ydb::Cms::ListDatabaseIamDelegationsRequest,
    Ydb::Cms::ListDatabaseIamDelegationsResponse>;

struct TFixture : IFacilityProvider {
    TTestActorRuntime Runtime;
    NThreading::TPromise<TRpc::TResponse> Promise = NThreading::NewPromise<TRpc::TResponse>();
    std::deque<TAutoPtr<IEventHandle>> Requests;
    ui32 RequestCount = 0;

    TFixture() {
        Runtime.Initialize(TAppPrepare().Unwrap());
        Runtime.GetAppData().IamConfig.SetDelegationTabletId(900);
        Runtime.GetAppData().AdministrationAllowedSIDs = {"admin"};
        Runtime.SetScheduledEventFilter([](auto&, auto&, auto, auto&) { return false; });
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TTabletEvents::TEvRequest::EventType) {
                ++RequestCount;
                Requests.push_back(ev.Release());
                return TTestActorRuntimeBase::EEventAction::DROP;
            }
            return TTestActorRuntimeBase::EEventAction::PROCESS;
        });
    }

    ~TFixture() { Runtime.SetObserverFunc(TTestActorRuntimeBase::DefaultObserverFunc); }
    ui64 GetChannelBufferSize() const override { return 0; }
    TActorId RegisterActor(IActor* actor) const override {
        return const_cast<TTestActorRuntime&>(Runtime).Register(actor);
    }

    template<class TCondition>
    void Until(TCondition condition) {
        if (!condition()) {
            TDispatchOptions options;
            options.CustomFinalCondition = condition;
            options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
            Runtime.DispatchEvents(options);
        }
        UNIT_ASSERT(condition());
    }

    void Start(const TString& incarnation = {}, const TString& pageToken = {}, const TString& user = "admin",
               ui32 pageSize = 1, const TString& path = "/Root/deleted") {
        Promise = NThreading::NewPromise<TRpc::TResponse>();
        TRpc::TRequest request;
        request.set_path(path);
        request.set_database_incarnation(incarnation);
        request.set_page_size(pageSize);
        request.set_page_token(pageToken);
        NACLib::TUserToken token(user, TVector<TString>{});
        using TCallback = NRpcService::TPromiseWrapper<TRpc::TResponse>;
        auto context = std::make_unique<NRpcService::TLocalRpcCtx<TRpc, TCallback>>(
            std::move(request), TCallback(Promise), "/Root", TMaybe<TString>(token.SerializeAsString()), Nothing(), false);
        DoListDatabaseIamDelegationsRequest(std::move(context), *this);
    }

    TAutoPtr<IEventHandle> Request() {
        Until([&] { return !Requests.empty() || Promise.GetFuture().HasValue(); });
        UNIT_ASSERT_C(!Requests.empty(), "Inventory RPC completed before its expected tablet request");
        auto result = std::move(Requests.front());
        Requests.pop_front();
        return result;
    }

    void Reply(const IEventHandle& request, const TResponse& response) {
        auto reply = MakeHolder<TTabletEvents::TEvResponse>();
        reply->Record = response;
        Runtime.Send(new IEventHandle(request.Sender, request.Recipient, reply.Release(), 0, request.Cookie));
    }

    static TResponse Inventory(ui64 revision = 7, const TString& cursor = {}) {
        TResponse result;
        result.SetStatus(SUCCESS);
        result.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        if (cursor) {
            result.SetNextPageToken("v2.tablet-owned-test-token");
        }
        auto* database = result.MutableDatabase();
        database->MutableIdentity()->SetIncarnation("old-incarnation");
        database->MutableIdentity()->SetPath("/Root/deleted");
        database->SetInventoryRevision(revision);
        result.SetNextAfterOperationId(cursor);
        auto* delegation = result.AddDelegations();
        delegation->SetOperationId(cursor.empty() ? "last-operation" : cursor);
        delegation->SetDatabaseIncarnation("old-incarnation");
        delegation->SetSecretPath("/Root/deleted/secret");
        delegation->SetState(WAITING_SETUP);
        delegation->SetSetupState(SETUP_UNKNOWN);
        delegation->SetSetupOperationId("iam-operation");
        auto* binding = delegation->MutableBinding();
        binding->SetServiceAccountId("account");
        binding->SetCloudId("cloud");
        binding->SetResourceType("resource-manager.cloud");
        binding->SetReferrerId("retained-referrer");
        binding->SetReferrerType("ydb.secret");
        binding->SetServiceId("ydb");
        binding->SetMicroserviceId("data-plane");
        binding->SetReferencePolicy(WITHOUT_REFERENCES);
        return result;
    }

    TRpc::TResponse Response() {
        Until([&] { return Promise.GetFuture().HasValue(); });
        return Promise.GetFuture().GetValue();
    }

    Ydb::Cms::ListDatabaseIamDelegationsResult Result() {
        const auto response = Response();
        UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        Ydb::Cms::ListDatabaseIamDelegationsResult result;
        UNIT_ASSERT(response.operation().result().UnpackTo(&result));
        return result;
    }
};

} // namespace

Y_UNIT_TEST_SUITE(ListDatabaseIamDelegations) {
    Y_UNIT_TEST(AuthenticatedContinuationIsOpaqueAndSkipsRegistryDiscovery) {
        TFixture f;
        f.Start("old-incarnation");
        auto request = f.Request();
        const auto& first = request->Get<TTabletEvents::TEvRequest>()->Record.GetListInventory();
        UNIT_ASSERT_VALUES_EQUAL(first.GetPaginationMode(), PAGINATION_AUTHENTICATED);
        UNIT_ASSERT_VALUES_EQUAL(first.GetDatabasePath(), "/Root/deleted");
        UNIT_ASSERT(first.GetContinuationToken().empty());
        auto response = f.Inventory(7, "first-operation");
        response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        response.SetNextPageToken("v2.tablet-owned-test-token");
        f.Reply(*request, response);
        const auto token = f.Result().next_page_token();
        UNIT_ASSERT_VALUES_EQUAL(token, "v2.tablet-owned-test-token");
        f.Start({}, token); // no explicit incarnation: routing must not rediscover a reused path
        request = f.Request();
        const auto& command = request->Get<TTabletEvents::TEvRequest>()->Record;
        UNIT_ASSERT(command.HasListInventory());
        const auto& next = command.GetListInventory();
        UNIT_ASSERT_VALUES_EQUAL(next.GetContinuationToken(), token);
        UNIT_ASSERT_VALUES_EQUAL(next.GetPaginationMode(), PAGINATION_AUTHENTICATED);
        UNIT_ASSERT_VALUES_EQUAL(next.GetDatabasePath(), "/Root/deleted");
        UNIT_ASSERT(next.GetDatabaseIncarnation().empty());
        UNIT_ASSERT(next.GetAfterOperationId().empty());
        UNIT_ASSERT(!next.HasInventoryRevision());
        response = f.Inventory();
        response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Result().database_incarnation(), "old-incarnation");
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 2);
    }

    Y_UNIT_TEST(ContinuationRequiresFreshAdministrativeAuthorization) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        auto response = f.Inventory(7, "first-operation");
        response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        response.SetNextPageToken("v2.tablet-owned-test-token");
        f.Reply(*request, response);
        const auto token = f.Result().next_page_token();
        f.Runtime.GetAppData().AdministrationAllowedSIDs = {"replacement-admin"};
        f.Start({}, token);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::UNAUTHORIZED);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
        f.Start({}, token, "replacement-admin");
        const auto authorized = f.Request();
        UNIT_ASSERT(authorized->Get<TTabletEvents::TEvRequest>()->Record.HasListInventory());
        response = f.Inventory();
        response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        f.Reply(*authorized, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Result().delegations_size(), 1);
    }

    Y_UNIT_TEST(OldTabletCannotSilentlyDowngradeEmptyOrFinalPage) {
        for (const auto size : {0, 1}) {
            TFixture f;
            f.Start("old-incarnation");
            const auto request = f.Request();
            auto response = f.Inventory();
            response.ClearPaginationCapability();
            if (!size) {
                response.ClearDelegations();
            }
            f.Reply(*request, response);
            const auto result = f.Response();
            UNIT_ASSERT_VALUES_EQUAL(result.operation().status(), Ydb::StatusIds::UNAVAILABLE);
            UNIT_ASSERT(!result.operation().has_result());
        }
    }

    Y_UNIT_TEST(UnsignedLegacyContinuationRequiresEnumerationRestart) {
        TFixture f;
        // This is a structurally valid old token, not merely malformed base64.
        auto cursor = f.Inventory(7, "first-operation");
        cursor.ClearDelegations();
        const auto legacy = "v1." + Base64Encode(cursor.SerializeAsString());
        f.Start({}, legacy);
        f.Until([&] { return f.Promise.GetFuture().HasValue() || !f.Requests.empty(); });
        UNIT_ASSERT_C(f.Promise.GetFuture().HasValue(), "Unsigned continuation reached the tablet");
        const auto result = f.Response();
        UNIT_ASSERT_VALUES_EQUAL(result.operation().status(), Ydb::StatusIds::BAD_REQUEST);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 0);
        UNIT_ASSERT(result.operation().issues_size());
    }

    Y_UNIT_TEST(TabletRejectsTamperedContinuationWithoutRpcFallback) {
        TFixture f;
        f.Start({}, "v2.corrupted-tablet-owned-token");
        const auto request = f.Request();
        const auto& command = request->Get<TTabletEvents::TEvRequest>()->Record;
        UNIT_ASSERT(command.HasListInventory());
        UNIT_ASSERT_VALUES_EQUAL(command.GetListInventory().GetContinuationToken(), "v2.corrupted-tablet-owned-token");
        TResponse response;
        response.SetStatus(INVALID_ARGUMENT);
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
    }

    Y_UNIT_TEST(AuthenticatedPageSizesRetainPublicZeroDefault) {
        for (const ui32 pageSize : {0, 1, 1000}) {
            TFixture f;
            f.Start("old-incarnation", {}, "admin", pageSize);
            const auto request = f.Request();
            const auto& list = request->Get<TTabletEvents::TEvRequest>()->Record.GetListInventory();
            UNIT_ASSERT_VALUES_EQUAL(list.GetPageSize(), pageSize ? pageSize : 100);
            UNIT_ASSERT_VALUES_EQUAL(list.GetPaginationMode(), PAGINATION_AUTHENTICATED);
            auto response = f.Inventory();
            response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
            f.Reply(*request, response);
            UNIT_ASSERT_VALUES_EQUAL(f.Result().delegations_size(), 1);
        }
        TFixture f;
        f.Start("old-incarnation", {}, "admin", 1001);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 0);
    }

    Y_UNIT_TEST(InventoryExportsDurableCleanupHistoryWithoutInventingLegacyAge) {
        TFixture f;
        f.Start("old-incarnation");
        auto request = f.Request();
        auto response = f.Inventory();
        response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        auto* record = response.MutableDelegations(0);
        record->SetState(REVOCATION_PENDING);
        record->SetSetupState(SETUP_SUCCEEDED);
        record->SetStagedAtUs(1000000);
        record->SetCleanupQueuedAtUs(3000000);
        record->SetRevokeConsecutiveFailures(9);
        record->SetDueAtUs(9000000); // due time is not cleanup age
        f.Reply(*request, response);
        const auto exported = f.Result().delegations(0);
        UNIT_ASSERT(exported.has_staged_at_us());
        UNIT_ASSERT_VALUES_EQUAL(exported.staged_at_us(), 1000000);
        UNIT_ASSERT(exported.has_cleanup_queued_at_us());
        UNIT_ASSERT_VALUES_EQUAL(exported.cleanup_queued_at_us(), 3000000);
        UNIT_ASSERT(exported.has_revoke_consecutive_failures());
        UNIT_ASSERT_VALUES_EQUAL(exported.revoke_consecutive_failures(), 9);
        f.Start("old-incarnation");
        request = f.Request();
        response = f.Inventory();
        response.SetPaginationCapability(PAGINATION_CAPABILITY_AUTHENTICATED);
        f.Reply(*request, response);
        const auto legacy = f.Result().delegations(0);
        UNIT_ASSERT(!legacy.has_staged_at_us());
        UNIT_ASSERT(!legacy.has_cleanup_queued_at_us());
        UNIT_ASSERT(!legacy.has_revoke_consecutive_failures());
        UNIT_ASSERT(legacy.state() == Ydb::Cms::DatabaseIamDelegation::WAITING_SETUP);
        UNIT_ASSERT(legacy.setup_state() == Ydb::Cms::DatabaseIamDelegation::SETUP_UNKNOWN);
        UNIT_ASSERT_VALUES_EQUAL(legacy.setup_operation_id(), "iam-operation");
    }

    Y_UNIT_TEST(RealAuthenticatedPagesPreserveEverySameAccountReferrer) {
        using namespace NIamDelegation::NTests;
        TTestContext tablet;
        tablet.Call(RegisterRequest(DatabaseIdentity()));
        TSet<TString> expected;
        for (ui32 index = 0; index < 5; ++index) {
            const auto id = "operation-" + ToString(index);
            tablet.Call(StageCreateRequest(id, Binding("referrer-" + ToString(index)),
                "/Root/database/secret-" + ToString(index)));
            expected.insert(id);
        }
        TFixture f;
        f.Runtime.GetAppData().IamConfig.SetDelegationTabletId(TTestContext::TabletId);
        TString token;
        TSet<TString> actual, referrers;
        ui64 revision = 0;
        for (ui32 page = 0; page < 3; ++page) {
            f.Start(page == 0 ? "database-incarnation-1" : TString(), token, "admin", 2, "/Root/database");
            auto request = f.Request();
            const auto& command = request->Get<TTabletEvents::TEvRequest>()->Record;
            UNIT_ASSERT(command.HasListInventory());
            f.Reply(*request, tablet.Call(command));
            const auto result = f.Result();
            if (page == 0) {
                revision = result.inventory_revision();
            }
            UNIT_ASSERT_VALUES_EQUAL(result.inventory_revision(), revision);
            UNIT_ASSERT_VALUES_EQUAL(result.delegations_size(), page == 2 ? 1 : 2);
            for (const auto& record : result.delegations()) {
                UNIT_ASSERT(actual.insert(record.operation_id()).second);
                UNIT_ASSERT(referrers.insert(record.referrer_id()).second);
                UNIT_ASSERT_VALUES_EQUAL(record.service_account_id(), "service-account-1");
            }
            token = result.next_page_token();
            UNIT_ASSERT_VALUES_EQUAL(token.empty(), page == 2);
            if (page == 0) {
                tablet.Reboot(); // subsequent real signatures must survive restart
            }
        }
        UNIT_ASSERT(actual == expected);
        UNIT_ASSERT_VALUES_EQUAL(referrers.size(), 5);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 3);
    }

    Y_UNIT_TEST(RealInventorySupportsExternalAgeAndFailureThresholds) {
        using namespace NIamDelegation::NTests;
        using TPublic = Ydb::Cms::DatabaseIamDelegation;
        TTestContext tablet;
        CreateActive(tablet);
        auto unknown = tablet.Call(StageAlterRequest(GetSecret(tablet), "uncertain", Binding("uncertain-referrer"))).GetDelegation();
        unknown = tablet.Call(StartRequest(unknown)).GetDelegation();
        unknown = tablet.Call(SetupResultRequest(unknown, SETUP_UNKNOWN, {})).GetDelegation();
        TFixture f;
        f.Runtime.GetAppData().IamConfig.SetDelegationTabletId(TTestContext::TabletId);
        const auto inventory = [&] {
            f.Start("database-incarnation-1", {}, "admin", 100, "/Root/database");
            auto request = f.Request();
            f.Reply(*request, tablet.Call(request->Get<TTabletEvents::TEvRequest>()->Record));
            return f.Result();
        };
        // External monitoring policy, deliberately configured low for this test.
        // The real tablet produces every timestamp/counter/state, and the real RPC
        // projects them. This observer never changes lifecycle ownership.
        struct TObservation { TSet<TString> Pending, Cleanup, Failures; };
        const auto observe = [&](TDuration ageThreshold, ui32 failureThreshold) {
            TObservation observation;
            const auto now = tablet.GetRuntime().GetCurrentTime().MicroSeconds();
            const auto page = inventory();
            for (const auto& record : page.delegations()) {
                if ((record.state() == TPublic::PENDING || record.state() == TPublic::WAITING_SETUP)
                    && record.has_staged_at_us() && now >= record.staged_at_us()
                    && now - record.staged_at_us() >= ageThreshold.MicroSeconds()) {
                    observation.Pending.insert(record.operation_id());
                }
                if (record.state() == TPublic::REVOCATION_PENDING || record.state() == TPublic::REVOKING) {
                    if (record.has_cleanup_queued_at_us() && now >= record.cleanup_queued_at_us()
                        && now - record.cleanup_queued_at_us() >= ageThreshold.MicroSeconds()) {
                        observation.Cleanup.insert(record.operation_id());
                    }
                    if (record.has_revoke_consecutive_failures()
                        && record.revoke_consecutive_failures() >= failureThreshold) {
                        observation.Failures.insert(record.operation_id());
                    }
                }
            }
            return observation;
        };
        auto health = observe(TDuration::Seconds(3), 2);
        UNIT_ASSERT(health.Pending.empty() && health.Cleanup.empty() && health.Failures.empty());
        tablet.Call(DropRequest(GetSecret(tablet)));
        const auto queuedAt = GetDelegation(tablet, "create-1").GetCleanupQueuedAtUs();
        tablet.AdvanceTime(TDuration::Seconds(4));
        for (ui32 failure = 0; failure < 3; ++failure) {
            const auto saved = GetDelegation(tablet, "create-1");
            tablet.GetRuntime().UpdateCurrentTime(Max(tablet.GetRuntime().GetCurrentTime(),
                TInstant::MicroSeconds(saved.GetDueAtUs())));
            const auto claims = tablet.Call(ClaimRequest());
            UNIT_ASSERT_VALUES_EQUAL(claims.DelegationsSize(), 1);
            UNIT_ASSERT_VALUES_EQUAL(claims.GetDelegations(0).GetOperationId(), "create-1");
            auto ack = FinishRequest(claims.GetDelegations(0), REVOKE_RETRY);
            ack.MutableFinishRevocation()->SetRetryReason(REVOKE_RETRY_FAILURE);
            tablet.Call(ack);
        }
        tablet.Reboot();
        const auto unresolved = GetDelegation(tablet, "uncertain").SerializeAsString();
        health = observe(TDuration::Seconds(3), 2);
        UNIT_ASSERT(health.Pending == TSet<TString>{"uncertain"});
        UNIT_ASSERT(health.Cleanup == TSet<TString>{"create-1"});
        UNIT_ASSERT(health.Failures == TSet<TString>{"create-1"});
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(tablet, "uncertain").SerializeAsString(), unresolved);
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(tablet, "create-1").GetCleanupQueuedAtUs(), queuedAt);
        // Progress clears the failure alarm, but does not invent a new cleanup age.
        auto saved = GetDelegation(tablet, "create-1");
        tablet.GetRuntime().UpdateCurrentTime(Max(tablet.GetRuntime().GetCurrentTime(), TInstant::MicroSeconds(saved.GetDueAtUs())));
        auto claim = tablet.Call(ClaimRequest()).GetDelegations(0);
        auto pending = FinishRequest(claim, REVOKE_RETRY);
        pending.MutableFinishRevocation()->SetRetryReason(REVOKE_RETRY_PENDING);
        saved = tablet.Call(pending).GetDelegation();
        health = observe(TDuration::Seconds(3), 2);
        UNIT_ASSERT(health.Failures.empty());
        UNIT_ASSERT(health.Cleanup == TSet<TString>{"create-1"});
        UNIT_ASSERT(health.Pending == TSet<TString>{"uncertain"});
        tablet.GetRuntime().UpdateCurrentTime(Max(tablet.GetRuntime().GetCurrentTime(), TInstant::MicroSeconds(saved.GetDueAtUs())));
        claim = tablet.Call(ClaimRequest()).GetDelegations(0);
        tablet.Call(FinishRequest(claim));
        health = observe(TDuration::Seconds(3), 2);
        UNIT_ASSERT(health.Cleanup.empty() && health.Failures.empty());
        UNIT_ASSERT(health.Pending == TSet<TString>{"uncertain"});
        unknown = GetDelegation(tablet, "uncertain");
        tablet.Call(SetupResultRequest(unknown, SETUP_FAILED, {}));
        health = observe(TDuration::Seconds(3), 2);
        UNIT_ASSERT(health.Pending.empty() && health.Cleanup.empty() && health.Failures.empty());
    }

    Y_UNIT_TEST(UnauthorizedNeverReadsTablet) {
        TFixture f;
        f.Start({}, {}, "ordinary-user");
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::UNAUTHORIZED);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 0);
    }

    Y_UNIT_TEST(ExplicitIncarnationExportsRetainedFullBinding) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        UNIT_ASSERT_VALUES_EQUAL(request->Get<TTabletEvents::TEvRequest>()->Record.GetListInventory().GetDatabaseIncarnation(), "old-incarnation");
        f.Reply(*request, f.Inventory());
        const auto result = f.Result();
        UNIT_ASSERT_VALUES_EQUAL(result.database_incarnation(), "old-incarnation");
        UNIT_ASSERT_VALUES_EQUAL(result.delegations_size(), 1);
        const auto& binding = result.delegations(0);
        UNIT_ASSERT_VALUES_EQUAL(binding.service_account_id(), "account");
        UNIT_ASSERT_VALUES_EQUAL(binding.cloud_id(), "cloud");
        UNIT_ASSERT_VALUES_EQUAL(binding.resource_type(), "resource-manager.cloud");
        UNIT_ASSERT_VALUES_EQUAL(binding.referrer_id(), "retained-referrer");
        UNIT_ASSERT_VALUES_EQUAL(binding.referrer_type(), "ydb.secret");
        UNIT_ASSERT_VALUES_EQUAL(binding.service_id(), "ydb");
        UNIT_ASSERT_VALUES_EQUAL(binding.microservice_id(), "data-plane");
        UNIT_ASSERT(binding.reference_policy() == Ydb::Cms::DatabaseIamDelegation::WITHOUT_REFERENCES);
        UNIT_ASSERT(binding.state() == Ydb::Cms::DatabaseIamDelegation::WAITING_SETUP);
        UNIT_ASSERT(binding.setup_state() == Ydb::Cms::DatabaseIamDelegation::SETUP_UNKNOWN);
        UNIT_ASSERT_VALUES_EQUAL(binding.setup_operation_id(), "iam-operation");
    }

    Y_UNIT_TEST(PathReuseRequiresExplicitIncarnation) {
        TFixture f;
        f.Start();
        const auto request = f.Request();
        UNIT_ASSERT(request->Get<TTabletEvents::TEvRequest>()->Record.HasListDatabases());
        TResponse response;
        response.SetStatus(SUCCESS);
        for (const auto& incarnation : {"old", "new"}) {
            auto* identity = response.AddDatabases()->MutableIdentity();
            identity->SetPath("/Root/deleted");
            identity->SetIncarnation(incarnation);
        }
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
    }

    Y_UNIT_TEST(PathLookupScansRegistryPages) {
        TFixture f;
        f.Start();
        auto request = f.Request();
        TResponse response;
        response.SetStatus(SUCCESS);
        response.SetNextAfterIncarnation("before");
        f.Reply(*request, response);
        request = f.Request();
        UNIT_ASSERT_VALUES_EQUAL(request->Get<TTabletEvents::TEvRequest>()->Record.GetListDatabases().GetAfterIncarnation(), "before");
        response.ClearNextAfterIncarnation();
        auto* identity = response.AddDatabases()->MutableIdentity();
        identity->SetPath("/Root/deleted");
        identity->SetIncarnation("old-incarnation");
        f.Reply(*request, response);
        request = f.Request();
        UNIT_ASSERT(request->Get<TTabletEvents::TEvRequest>()->Record.HasListInventory());
        f.Reply(*request, f.Inventory());
        UNIT_ASSERT_VALUES_EQUAL(f.Result().database_incarnation(), "old-incarnation");
    }

    Y_UNIT_TEST(ContinuationForwardsRequestedIdentityAndTabletRejection) {
        TFixture f;
        const TString token = "v2.tablet-owned-test-token";
        for (const auto& [incarnation, path] : TVector<std::pair<TString, TString>>{
            {"another-incarnation", "/Root/deleted"}, {{}, "/Root/another"}}) {
            f.Start(incarnation, token, "admin", 1, path);
            const auto request = f.Request();
            const auto& list = request->Get<TTabletEvents::TEvRequest>()->Record.GetListInventory();
            UNIT_ASSERT_VALUES_EQUAL(list.GetDatabaseIncarnation(), incarnation);
            UNIT_ASSERT_VALUES_EQUAL(list.GetDatabasePath(), path);
            UNIT_ASSERT_VALUES_EQUAL(list.GetContinuationToken(), token);
            TResponse response;
            response.SetStatus(INVALID_ARGUMENT);
            f.Reply(*request, response);
            UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        }
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 2);
    }

    Y_UNIT_TEST(ExpiredSnapshotRequiresCallerToRestart) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        TResponse response;
        response.SetStatus(SNAPSHOT_EXPIRED);
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::PRECONDITION_FAILED);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
    }

    Y_UNIT_TEST(InvalidPageSizeAndTokenDoNotReadTablet) {
        TFixture f;
        f.Start("old-incarnation", {}, "admin", 1001);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        f.Start({}, "invalid-token");
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 0);
    }

    Y_UNIT_TEST(MismatchedRetainedPathIsRejected) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        auto response = f.Inventory();
        response.MutableDatabase()->MutableIdentity()->SetPath("/Root/other");
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
    }

    Y_UNIT_TEST(RegisteredEmptyInventoryIsDistinctFromUnknownDatabase) {
        TFixture f;
        f.Start("old-incarnation");
        auto request = f.Request();
        auto response = f.Inventory(0);
        response.ClearDelegations();
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Result().delegations_size(), 0);
        f.Start("unknown-incarnation");
        request = f.Request();
        response.Clear();
        response.SetStatus(NOT_FOUND);
        f.Reply(*request, response);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::NOT_FOUND);
    }

    Y_UNIT_TEST(RegistryScanHasBoundedWork) {
        TFixture f;
        f.Start();
        TString cursor;
        for (ui32 page = 0; page < 100; ++page) {
            const auto request = f.Request();
            cursor += "a";
            TResponse response;
            response.SetStatus(SUCCESS);
            response.SetNextAfterIncarnation(cursor);
            f.Reply(*request, response);
        }
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 100);
    }

    Y_UNIT_TEST(InventoryDeadlineClosesChildAndIgnoresLateReply) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        ui32 childStops = 0;
        auto stops = f.Runtime.AddObserver<TEvents::TEvPoison>([&](auto& event) {
            if (event->Recipient == request->Recipient) {
                ++childStops;
            }
        });
        const auto started = f.Runtime.GetCurrentTime();
        const auto response = f.Response();
        UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_VALUES_EQUAL(f.Runtime.GetCurrentTime() - started, TDuration::Seconds(30));
        f.Until([&] { return childStops == 1; });
        f.Reply(*request, f.Inventory());
        f.Runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(f.Response().SerializeAsString(), response.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
        UNIT_ASSERT_VALUES_EQUAL(childStops, 1);
    }

    Y_UNIT_TEST(ClientDepartureCancelsOwnedInventoryWait) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        ui32 childStops = 0;
        auto stops = f.Runtime.AddObserver<TEvents::TEvPoison>([&](auto& event) {
            if (event->Recipient == request->Recipient) {
                ++childStops;
            }
        });
        f.Runtime.Send(new IEventHandle(request->Sender, {}, new TRpcServices::TEvForgetOperation));
        const auto response = f.Response();
        UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::INTERNAL_ERROR);
        f.Until([&] { return childStops == 1; });
        f.Reply(*request, f.Inventory());
        f.Runtime.SimulateSleep(TDuration::Seconds(31));
        UNIT_ASSERT_VALUES_EQUAL(f.Response().SerializeAsString(), response.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
        UNIT_ASSERT_VALUES_EQUAL(childStops, 1);
    }
}

} // namespace NKikimr::NGRpcService
