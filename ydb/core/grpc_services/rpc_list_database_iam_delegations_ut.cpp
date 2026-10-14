#include "service_cms.h"

#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/local_rpc/local_rpc.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/security/iam_delegation/tablet/public/events.h>

#include <library/cpp/testing/unittest/registar.h>

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

    Y_UNIT_TEST(ContinuationCarriesRevisionAndCursor) {
        TFixture f;
        f.Start("old-incarnation");
        auto request = f.Request();
        f.Reply(*request, f.Inventory(7, "first-operation"));
        const auto token = f.Result().next_page_token();
        UNIT_ASSERT(!token.empty());
        f.Start({}, token);
        request = f.Request();
        const auto& next = request->Get<TTabletEvents::TEvRequest>()->Record.GetListInventory();
        UNIT_ASSERT_VALUES_EQUAL(next.GetDatabaseIncarnation(), "old-incarnation");
        UNIT_ASSERT_VALUES_EQUAL(next.GetInventoryRevision(), 7);
        UNIT_ASSERT_VALUES_EQUAL(next.GetAfterOperationId(), "first-operation");
        f.Reply(*request, f.Inventory());
        UNIT_ASSERT(f.Result().next_page_token().empty());
    }

    Y_UNIT_TEST(ContinuationCannotChangePathOrIncarnation) {
        TFixture f;
        f.Start("old-incarnation");
        const auto request = f.Request();
        f.Reply(*request, f.Inventory(7, "first-operation"));
        const auto token = f.Result().next_page_token();
        f.Start("another-incarnation", token);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        f.Start({}, token, "admin", 1, "/Root/another");
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::BAD_REQUEST);
        UNIT_ASSERT_VALUES_EQUAL(f.RequestCount, 1);
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
}

} // namespace NKikimr::NGRpcService
