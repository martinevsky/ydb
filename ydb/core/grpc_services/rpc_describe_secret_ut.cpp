#include "service_secret.h"

#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/local_rpc/local_rpc.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/security/iam_delegation/tablet/public/events.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/public/api/protos/ydb_secret.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <deque>

namespace NKikimr::NGRpcService {
namespace {

using namespace NActors;
using namespace NKikimrIamDelegation;
using TTabletEvents = NIamDelegation::TEvIamDelegationTablet;
using TRpc = TGrpcRequestOperationCall<Ydb::Secret::DescribeSecretRequest, Ydb::Secret::DescribeSecretResponse>;

struct TFixture : IFacilityProvider {
    TTestActorRuntime Runtime;
    NThreading::TPromise<Ydb::Secret::DescribeSecretResponse> Promise =
        NThreading::NewPromise<Ydb::Secret::DescribeSecretResponse>();
    std::deque<TAutoPtr<IEventHandle>> Navigations;
    std::deque<TAutoPtr<IEventHandle>> TabletRequests;
    ui32 TabletRequestCount = 0;

    TFixture() {
        Runtime.Initialize(TAppPrepare().Unwrap());
        // Route navigation to a real local destination before the observer intercepts it.
        Runtime.RegisterService(MakeTxProxyID(), Runtime.AllocateEdgeActor());
        Runtime.SetScheduledEventFilter([](auto&, auto&, auto, auto&) { return false; });
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& ev) {
            using EAction = TTestActorRuntimeBase::EEventAction;
            switch (ev->GetTypeRewrite()) {
                case TEvTxUserProxy::TEvNavigate::EventType:
                    Navigations.push_back(ev.Release());
                    return EAction::DROP;
                case TTabletEvents::TEvRequest::EventType:
                    ++TabletRequestCount;
                    TabletRequests.push_back(ev.Release());
                    return EAction::DROP;
                case NIamDelegation::TEvIamDelegation::TEvGetToken::EventType:
                    UNIT_FAIL("DescribeSecret must not request a token");
            }
            return EAction::PROCESS;
        });
        Ydb::Secret::DescribeSecretRequest request;
        request.set_path("/Root/secret");
        using TCallback = NRpcService::TPromiseWrapper<TRpc::TResponse>;
        auto context = std::make_unique<NRpcService::TLocalRpcCtx<TRpc, TCallback>>(
            std::move(request), TCallback(Promise), "/Root", Nothing(), Nothing(), false);
        DoDescribeSecretRequest(std::move(context), *this);
    }

    ~TFixture() {
        Runtime.SetObserverFunc(TTestActorRuntimeBase::DefaultObserverFunc);
    }

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

    void Describe(bool delegated = true, NKikimrScheme::EStatus status = NKikimrScheme::StatusSuccess) {
        Until([&] { return !Navigations.empty(); });
        auto request = std::move(Navigations.front());
        Navigations.pop_front();
        UNIT_ASSERT_VALUES_EQUAL(TabletRequestCount, 0);
        auto reply = MakeHolder<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResultBuilder>();
        reply->Record.SetStatus(status);
        auto* path = reply->Record.MutablePathDescription();
        auto* self = path->MutableSelf();
        self->SetName("secret");
        self->SetPathType(NKikimrSchemeOp::EPathTypeSecret);
        self->SetSchemeshardId(42);
        self->SetPathId(17);
        path->MutableSecretDescription()->SetVersion(5);
        if (delegated) {
            auto* reference = path->MutableSecretDescription()->MutableIamDelegation();
            reference->SetTabletId(900);
            reference->SetDatabaseIncarnation("database-incarnation");
            reference->SetCreateOperationId("create");
        }
        Runtime.Send(new IEventHandle(request->Sender, request->Recipient, reply.Release()));
    }

    TAutoPtr<IEventHandle> Request() {
        Until([&] { return !TabletRequests.empty() || Promise.GetFuture().HasValue(); });
        UNIT_ASSERT_C(!TabletRequests.empty(), "DescribeSecret completed before reading IAM state");
        auto result = std::move(TabletRequests.front());
        TabletRequests.pop_front();
        return result;
    }

    static TSecretRecord Secret(ui64 revision = 10, const TString& current = "current", const TString& pending = "pending") {
        TSecretRecord secret;
        auto* identity = secret.MutableIdentity();
        identity->SetDatabaseIncarnation("database-incarnation");
        identity->SetPathOwnerId(42);
        identity->SetPathLocalId(17);
        secret.SetCreateOperationId("create");
        secret.SetRevision(revision);
        secret.SetState(SECRET_LIVE);
        secret.SetCurrentOperationId(current);
        secret.SetPendingOperationId(pending);
        return secret;
    }

    void ReplySecret(const TSecretRecord& secret) {
        auto request = Request();
        const auto& record = request->Get<TTabletEvents::TEvRequest>()->Record;
        UNIT_ASSERT(record.HasGetSecret());
        UNIT_ASSERT_VALUES_EQUAL(record.GetGetSecret().GetSecret().SerializeAsString(), secret.GetIdentity().SerializeAsString());
        auto reply = MakeHolder<TTabletEvents::TEvResponse>();
        reply->Record.SetStatus(SUCCESS);
        *reply->Record.MutableSecret() = secret;
        Runtime.Send(new IEventHandle(request->Sender, request->Recipient, reply.Release(), 0, request->Cookie));
    }

    void ReplyDelegation(const TSecretRecord& secret, const TString& operationId, const TString& account) {
        auto request = Request();
        const auto& record = request->Get<TTabletEvents::TEvRequest>()->Record;
        UNIT_ASSERT(record.HasGetDelegation());
        UNIT_ASSERT_VALUES_EQUAL(record.GetGetDelegation().GetOperationId(), operationId);
        auto reply = MakeHolder<TTabletEvents::TEvResponse>();
        reply->Record.SetStatus(SUCCESS);
        *reply->Record.MutableSecret() = secret;
        auto* delegation = reply->Record.MutableDelegation();
        delegation->SetOperationId(operationId);
        delegation->SetDatabaseIncarnation(secret.GetIdentity().GetDatabaseIncarnation());
        *delegation->MutableSecret() = secret.GetIdentity();
        delegation->SetState(operationId == secret.GetCurrentOperationId() ? ACTIVE : PENDING);
        delegation->SetSetupState(operationId == secret.GetCurrentOperationId() ? SETUP_SUCCEEDED : SETUP_UNKNOWN);
        delegation->MutableBinding()->SetServiceAccountId(account);
        delegation->MutableBinding()->SetCloudId("cloud");
        delegation->MutableBinding()->SetReferrerId("referrer-" + operationId);
        Runtime.Send(new IEventHandle(request->Sender, request->Recipient, reply.Release(), 0, request->Cookie));
    }

    Ydb::Secret::DescribeSecretResponse Response() {
        Until([&] { return Promise.GetFuture().HasValue(); });
        return Promise.GetFuture().GetValue();
    }

    Ydb::Secret::DescribeSecretResult Result() {
        const auto response = Response();
        UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
        Ydb::Secret::DescribeSecretResult result;
        UNIT_ASSERT(response.operation().result().UnpackTo(&result));
        return result;
    }
};

} // namespace

Y_UNIT_TEST_SUITE(DescribeSecretIamDelegation) {
    Y_UNIT_TEST(PlainSecretDoesNotReadTablet) {
        TFixture f;
        f.Describe(false);
        const auto result = f.Result();
        UNIT_ASSERT_VALUES_EQUAL(result.version(), 5);
        UNIT_ASSERT(!result.has_iam_delegation());
        UNIT_ASSERT(!result.has_pending_iam_delegation());
        UNIT_ASSERT_VALUES_EQUAL(f.TabletRequestCount, 0);
    }

    Y_UNIT_TEST(AccessDeniedDoesNotReadTablet) {
        TFixture f;
        f.Describe(true, NKikimrScheme::StatusAccessDenied);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::UNAUTHORIZED);
        UNIT_ASSERT_VALUES_EQUAL(f.TabletRequestCount, 0);
    }

    Y_UNIT_TEST(DescribesCurrentAndPendingFromOneRevision) {
        TFixture f;
        f.Describe();
        const auto secret = f.Secret();
        f.ReplySecret(secret);
        f.ReplyDelegation(secret, "current", "current-account");
        f.ReplyDelegation(secret, "pending", "pending-account");
        const auto result = f.Result();
        UNIT_ASSERT_VALUES_EQUAL(result.iam_delegation().service_account_id(), "current-account");
        UNIT_ASSERT_VALUES_EQUAL(result.iam_delegation().cloud_id(), "cloud");
        UNIT_ASSERT_VALUES_EQUAL(result.iam_delegation().referrer_id(), "referrer-current");
        UNIT_ASSERT_VALUES_EQUAL(result.pending_iam_delegation().service_account_id(), "pending-account");
    }

    Y_UNIT_TEST(CreateWithUnknownSetupHasOnlyPendingBinding) {
        TFixture f;
        f.Describe();
        const auto secret = f.Secret(10, "", "pending");
        f.ReplySecret(secret);
        f.ReplyDelegation(secret, "pending", "pending-account");
        const auto result = f.Result();
        UNIT_ASSERT(!result.has_iam_delegation());
        UNIT_ASSERT_VALUES_EQUAL(result.pending_iam_delegation().service_account_id(), "pending-account");
    }

    Y_UNIT_TEST(ChangedRevisionRestartsAndDiscardsOldBinding) {
        TFixture f;
        f.Describe();
        const auto before = f.Secret();
        const auto after = f.Secret(11, "pending", "");
        f.ReplySecret(before);
        f.ReplyDelegation(before, "current", "old-account");
        f.ReplyDelegation(after, "pending", "new-account");
        f.ReplySecret(after);
        f.ReplyDelegation(after, "pending", "new-account");
        const auto result = f.Result();
        UNIT_ASSERT_VALUES_EQUAL(result.iam_delegation().service_account_id(), "new-account");
        UNIT_ASSERT(!result.has_pending_iam_delegation());
    }

    Y_UNIT_TEST(ConcurrentChangesHaveBoundedRetries) {
        TFixture f;
        f.Describe();
        for (ui64 revision = 1; revision <= 3; ++revision) {
            f.ReplySecret(f.Secret(revision));
            f.ReplyDelegation(f.Secret(revision + 1), "current", "account");
        }
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_VALUES_EQUAL(f.TabletRequestCount, 6);
    }

    Y_UNIT_TEST(CreateIdentityMismatchDoesNotExposeBinding) {
        TFixture f;
        f.Describe();
        auto secret = f.Secret();
        secret.SetCreateOperationId("another-create");
        f.ReplySecret(secret);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::SCHEME_ERROR);
        UNIT_ASSERT_VALUES_EQUAL(f.TabletRequestCount, 1);
    }

    Y_UNIT_TEST(DroppedSecretDoesNotExposeBinding) {
        TFixture f;
        f.Describe();
        auto secret = f.Secret();
        secret.SetState(SECRET_DROPPED);
        f.ReplySecret(secret);
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::SCHEME_ERROR);
    }

    Y_UNIT_TEST(TabletUnavailableFailsDescription) {
        TFixture f;
        f.Describe();
        auto request = f.Request();
        auto reply = MakeHolder<TTabletEvents::TEvResponse>();
        reply->Record.SetStatus(STATUS_UNSPECIFIED);
        f.Runtime.Send(new IEventHandle(request->Sender, request->Recipient, reply.Release(), 0, request->Cookie));
        UNIT_ASSERT_VALUES_EQUAL(f.Response().operation().status(), Ydb::StatusIds::UNAVAILABLE);
    }
}

} // namespace NKikimr::NGRpcService
