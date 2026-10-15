#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/iam_delegated_token_service.h>
#include <ydb/core/security/iam_delegation/cloud_resolver.h>
#include <ydb/core/security/iam_delegation/iam_actor_base.h>
#include <ydb/library/ycloud/api/folder_service.h>
#include <ydb/library/ycloud/api/service_account_service.h>
#include <ydb/core/security/token_manager/token_manager.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/ycloud/api/iam_token_service.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/set.h>
#include <util/generic/scope.h>
#include <util/random/random.h>

#include <deque>

namespace NKikimr::NIamDelegation {
namespace {

using namespace NActors;
using TCloudToken = NCloud::TEvIamTokenService;

struct TEvCloudResult : TEventLocal<TEvCloudResult, EventSpaceBegin(TEvents::ES_PRIVATE) + 11> {
    TResolvedCloud Cloud;
    bool Success = false;
};

class TResolveActor : public TActorBootstrapped<TResolveActor> {
public:
    TResolveActor(TIamDelegationSettings settings, TActorId recipient, TString token)
        : Settings(std::move(settings)), Recipient(recipient), Token(std::move(token)) {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        auto result = MakeHolder<TEvCloudResult>();
        try {
            result->Cloud = co_await ResolveCloud(Settings, Token, "account");
            result->Success = true;
        } catch (const TIamCallError&) {
        }
        Send(Recipient, result.Release());
        PassAway();
    }

    STFUNC(StateWork) { Y_UNUSED(ev); }
private:
    TIamDelegationSettings Settings;
    TActorId Recipient;
    TString Token;
};

struct TFixture {
    class TRecipient : public TActor<TRecipient> {
    public:
        TRecipient() : TActor(&TRecipient::StateWork) {}
        STFUNC(StateWork) { Y_UNUSED(ev); }
    };

    TTestActorRuntime Runtime;
    TActorId Sender;
    TActorId Manager;
    TActorId Service;
    ui32 Subscriptions = 0;
    std::deque<TAutoPtr<IEventHandle>> AccountLookups;
    std::deque<TAutoPtr<IEventHandle>> FolderLookups;
    struct TCloudResult { TResolvedCloud Cloud; bool Success; };
    std::deque<TCloudResult> Clouds;
    std::deque<TAutoPtr<IEventHandle>> Mints;
    struct TTokenResult {
        TString Token;
        TInstant ExpiresAt;
        TInstant UsableUntil;
        Ydb::StatusIds::StatusCode Status;
        bool IsSuccess() const { return Status == Ydb::StatusIds::SUCCESS; }
    };
    std::deque<std::pair<TTokenResult, ui64>> Results;

    TFixture() {
        Runtime.Initialize(TAppPrepare().Unwrap());
        Runtime.SetScheduledEventFilter([](auto&, auto&, auto, auto&) { return false; });
        Sender = Runtime.Register(new TRecipient());
        Manager = Runtime.AllocateEdgeActor();
        Runtime.RegisterService(MakeTokenManagerID(), Manager);
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& ev) {
            using EAction = TTestActorRuntimeBase::EEventAction;
            switch (ev->GetTypeRewrite()) {
                case TEvTokenManager::TEvSubscribeUpdateToken::EventType:
                    ++Subscriptions;
                    UNIT_ASSERT_VALUES_EQUAL(ev->Get<TEvTokenManager::TEvSubscribeUpdateToken>()->Id, "system-account");
                    return EAction::DROP;
                case TCloudToken::TEvCreateForServiceRequest::EventType:
                    Mints.push_back(ev.Release());
                    return EAction::DROP;
                case NCloud::TEvServiceAccountService::TEvGetServiceAccountRequest::EventType:
                    AccountLookups.push_back(ev.Release());
                    return EAction::DROP;
                case NCloud::TEvFolderService::TEvResolveFoldersRequest::EventType:
                    FolderLookups.push_back(ev.Release());
                    return EAction::DROP;
                case TEvCloudResult::EventType:
                    Clouds.push_back({ev->Get<TEvCloudResult>()->Cloud, ev->Get<TEvCloudResult>()->Success});
                    return EAction::DROP;
                case TEvIamDelegation::TEvGetTokenResult::EventType: {
                    const auto& result = *ev->Get<TEvIamDelegation::TEvGetTokenResult>();
                    Results.emplace_back(TTokenResult{result.Token, result.ExpiresAt, result.UsableUntil, result.Status}, ev->Cookie);
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        });
        TIamDelegationSettings settings;
        settings.Config.SetTokenServiceEndpoint("localhost:1");
        settings.Config.SetServiceId("ydb");
        settings.Config.SetMicroserviceId("data-plane");
        settings.Config.SetResourceType("resource-manager.cloud");
        settings.Config.SetEnableSsl(false);
        settings.Config.SetSystemTokenName("system-account");
        Service = Runtime.Register(CreateIamDelegatedTokenService(settings));
        Runtime.EnableScheduleForActor(Service);
        Until([&] { return Subscriptions == 1; });
        Update();
    }

    ~TFixture() { Runtime.SetObserverFunc(TTestActorRuntimeBase::DefaultObserverFunc); }

    template<class TCondition>
    void Until(TCondition condition) {
        if (!condition()) {
            TDispatchOptions options;
            options.CustomFinalCondition = condition;
            options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
            Runtime.DispatchEvents(options, TDuration::Minutes(5));
        }
        UNIT_ASSERT(condition());
    }

    void Update(const TString& token = "system-token") {
        Runtime.Send(new IEventHandle(Service, Manager, new TEvTokenManager::TEvUpdateToken(
            "system-account", token, {TEvTokenManager::TStatus::ECode::SUCCESS, {}})));
    }

    void Get(ui64 cookie = 1, TTokenKey key = {"account", "cloud", {}, {}, {}}) {
        Runtime.Send(new IEventHandle(Service, Sender, new TEvIamDelegation::TEvGetToken(std::move(key)), 0, cookie));
    }

    TAutoPtr<IEventHandle> Mint() {
        Until([&] { return !Mints.empty(); });
        TAutoPtr<IEventHandle> request(Mints.front().Release());
        Mints.pop_front();
        return request;
    }

    void Reply(const IEventHandle& request, TDuration lifetime = TDuration::Hours(12),
            const TString& token = "delegated-token", grpc::StatusCode code = grpc::StatusCode::OK)
    {
        auto response = MakeHolder<TCloudToken::TEvCreateForServiceResponse>();
        response->Status.GRpcStatusCode = code;
        response->Response.set_iam_token(token);
        response->Response.mutable_expires_at()->set_seconds((Runtime.GetCurrentTime() + lifetime).Seconds());
        Runtime.Send(new IEventHandle(request.Sender, request.Recipient, response.Release(), 0, request.Cookie));
    }

    TTokenResult Result(ui64 cookie = 1) {
        Until([&] { return !Results.empty(); });
        auto result = std::move(Results.front());
        Results.pop_front();
        UNIT_ASSERT_VALUES_EQUAL(result.second, cookie);
        return std::move(result.first);
    }

    void Resolve(const TString& token = "user-token") {
        TIamDelegationSettings settings;
        settings.Config.SetServiceControlEndpoint("localhost:1");
        settings.Config.SetResourceManagerEndpoint("localhost:1");
        settings.Config.SetEnableSsl(false);
        const auto actor = Runtime.Register(new TResolveActor(settings, Sender, token));
        Runtime.EnableScheduleForActor(actor);
    }

    void InitialToken() {
        Get();
        auto mint = Mint();
        Reply(*mint);
        UNIT_ASSERT_VALUES_EQUAL(Result().Token, "delegated-token");
    }

    TAutoPtr<IEventHandle> Refresh() {
        const auto deadline = Runtime.GetCurrentTime() + TDuration::Hours(1) + TDuration::Seconds(1);
        auto nextDemand = Runtime.GetCurrentTime() + TDuration::Minutes(5);
        while (Mints.empty()) {
            // Observe the actual scheduled mint before its client timeout, and
            // retain ordinary demand during the ten-minute idle window.
            Runtime.SimulateSleep(TDuration::Seconds(1));
            if (!Mints.empty()) {
                break;
            }
            UNIT_ASSERT(Runtime.GetCurrentTime() < deadline);
            if (Runtime.GetCurrentTime() >= nextDemand) {
                Get();
                UNIT_ASSERT_VALUES_EQUAL(Result().Token, "delegated-token");
                nextDemand = Runtime.GetCurrentTime() + TDuration::Minutes(5);
            }
        }
        return Mint();
    }
};

} // namespace

Y_UNIT_TEST_SUITE(IamDelegatedTokenActor) {
    Y_UNIT_TEST(SuccessfulRefreshesAreDispersedAcrossEntriesAndNodes) {
        SetRandomSeed(314159);
        Y_DEFER { ResetRandomState(); };
        TSet<ui64> offsets;
        for (ui32 node = 0; node < 2; ++node) {
            TFixture f;
            f.Runtime.UpdateCurrentTime(TInstant::Seconds(f.Runtime.GetCurrentTime().Seconds() + 1));
            const auto started = f.Runtime.GetCurrentTime();
            for (ui32 entry = 0; entry < 32; ++entry) {
                f.Get(entry + 1, {TStringBuilder() << "account-" << entry, "cloud", {}, {}, {}});
                auto mint = f.Mint();
                // Cold demand keeps its immediate first attempt.
                UNIT_ASSERT_VALUES_EQUAL(f.Runtime.GetCurrentTime(), started);
                f.Reply(*mint, TDuration::Minutes(10));
                UNIT_ASSERT(f.Result(entry + 1).IsSuccess());
            }
            TSet<TString> refreshed;
            for (ui32 entry = 0; entry < 32; ++entry) {
                auto mint = f.Mint();
                const auto elapsed = f.Runtime.GetCurrentTime() - started;
                UNIT_ASSERT(elapsed >= TDuration::Minutes(4));
                UNIT_ASSERT(elapsed <= TDuration::Minutes(5));
                const auto account = mint->Get<TCloudToken::TEvCreateForServiceRequest>()->Request.target_service_account_id();
                UNIT_ASSERT(refreshed.insert(account).second);
                offsets.insert(elapsed.MicroSeconds());
                f.Reply(*mint, TDuration::Minutes(10), "refreshed-token");
            }
            UNIT_ASSERT_VALUES_EQUAL(refreshed.size(), 32);
        }
        // Deterministic seed, broad bound: a fixed half-life timer has one offset.
        // Do not pin the test to the standard helper's individual random samples.
        UNIT_ASSERT_C(offsets.size() > 8, "healthy refreshes remained synchronized");
    }

    Y_UNIT_TEST(NearBoundaryRefreshRemainsPacedAndNeverServesExpiredToken) {
        TFixture f;
        f.Runtime.UpdateCurrentTime(TInstant::Seconds(f.Runtime.GetCurrentTime().Seconds() + 1)
            + TDuration::MilliSeconds(500));
        f.Get(1);
        auto mint = f.Mint();
        const auto mintedAt = f.Runtime.GetCurrentTime();
        // The IAM fixture's seconds-only expiry gives exactly half a second of
        // usability. The refresh pacing floor must not extend that serving cutoff.
        f.Reply(*mint, TDuration::Minutes(5) + TDuration::Seconds(1), "short-token");
        const auto first = f.Result(1);
        UNIT_ASSERT(first.IsSuccess());
        UNIT_ASSERT_VALUES_EQUAL(first.UsableUntil - mintedAt, TDuration::MilliSeconds(500));
        f.Runtime.UpdateCurrentTime(first.UsableUntil - TDuration::MicroSeconds(1));
        f.Get(2);
        UNIT_ASSERT_VALUES_EQUAL(f.Result(2).Token, "short-token");
        f.Runtime.UpdateCurrentTime(first.UsableUntil);
        f.Get(3);
        auto refresh = f.Mint();
        UNIT_ASSERT(f.Results.empty());
        UNIT_ASSERT(f.Runtime.GetCurrentTime() >= mintedAt + TDuration::Seconds(1));
        UNIT_ASSERT(f.Runtime.GetCurrentTime() < mintedAt + TDuration::Seconds(2));
        f.Reply(*refresh, TDuration::Minutes(20), "fresh-token");
        UNIT_ASSERT_VALUES_EQUAL(f.Result(3).Token, "fresh-token");
    }

    Y_UNIT_TEST(SuccessfulMintResetsFailureBackoffBeforeNextFailure) {
        TFixture f;
        f.Get();
        TInstant previous;
        for (ui32 failure = 1; failure <= 5; ++failure) {
            auto mint = f.Mint();
            const auto now = f.Runtime.GetCurrentTime();
            if (failure == 5) {
                UNIT_ASSERT(now - previous >= TDuration::Seconds(3));
            }
            f.Reply(*mint, TDuration::Hours(12), {}, grpc::StatusCode::PERMISSION_DENIED);
            if (failure == 1) {
                UNIT_ASSERT(!f.Result().IsSuccess());
            }
            previous = now;
        }
        auto recovered = f.Mint();
        f.Reply(*recovered, TDuration::Minutes(10));
        auto healthyRefresh = f.Mint();
        const auto failedAt = f.Runtime.GetCurrentTime();
        f.Reply(*healthyRefresh, TDuration::Hours(12), {}, grpc::StatusCode::PERMISSION_DENIED);
        auto firstRetry = f.Mint();
        const auto delay = f.Runtime.GetCurrentTime() - failedAt;
        UNIT_ASSERT(delay >= TDuration::Seconds(1));
        UNIT_ASSERT(delay < TDuration::MilliSeconds(1250));
        f.Reply(*firstRetry, TDuration::Minutes(10));
    }

    // Coverage control: idle eviction must survive the new healthy-refresh jitter.
    Y_UNIT_TEST(IdleEntryStopsBackgroundMintingAndNextDemandStartsImmediately) {
        TFixture f;
        f.InitialToken();
        f.Runtime.SimulateSleep(TDuration::Hours(1) + TDuration::Seconds(1));
        UNIT_ASSERT(f.Mints.empty()); // idle entry was removed before its background refresh
        UNIT_ASSERT(f.Results.empty());
        const auto demandAt = f.Runtime.GetCurrentTime();
        f.Get(2);
        auto mint = f.Mint();
        UNIT_ASSERT(f.Runtime.GetCurrentTime() - demandAt < TDuration::Seconds(1));
        UNIT_ASSERT(f.Results.empty());
        f.Reply(*mint, TDuration::Hours(12), "new-entry-token");
        UNIT_ASSERT_VALUES_EQUAL(f.Result(2).Token, "new-entry-token");
        UNIT_ASSERT_VALUES_EQUAL(f.Subscriptions, 1);
    }


    Y_UNIT_TEST(ConcurrentReadersShareMintAndReceiveUsableBoundary) {
        TFixture f;
        f.Get(1);
        f.Get(2);
        auto mint = f.Mint();
        const auto& request = *mint->Get<TCloudToken::TEvCreateForServiceRequest>();
        UNIT_ASSERT_VALUES_EQUAL(request.Token, "system-token");
        UNIT_ASSERT_VALUES_EQUAL(request.Request.target_service_account_id(), "account");
        UNIT_ASSERT_VALUES_EQUAL(request.Request.resource_id(), "cloud");
        f.Reply(*mint, TDuration::Minutes(20));
        auto first = f.Result(1);
        auto second = f.Result(2);
        UNIT_ASSERT(first.IsSuccess());
        UNIT_ASSERT_VALUES_EQUAL(first.Token, "delegated-token");
        UNIT_ASSERT_VALUES_EQUAL(second.Token, first.Token);
        UNIT_ASSERT_VALUES_EQUAL(first.ExpiresAt - first.UsableUntil, TDuration::Minutes(5));
        f.Get(3);
        UNIT_ASSERT_VALUES_EQUAL(f.Result(3).Token, first.Token);
        UNIT_ASSERT(f.Mints.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Subscriptions, 1u);
    }

    Y_UNIT_TEST(PersistedServiceIdentitySeparatesCachedTokens) {
        TFixture f;
        f.Get(1, {"account", "cloud", "old-service", "old-microservice", "old-resource-type"});
        auto mint = f.Mint();
        const auto& request = mint->Get<TCloudToken::TEvCreateForServiceRequest>()->Request;
        UNIT_ASSERT_VALUES_EQUAL(request.service_id(), "old-service");
        UNIT_ASSERT_VALUES_EQUAL(request.microservice_id(), "old-microservice");
        UNIT_ASSERT_VALUES_EQUAL(request.resource_type(), "old-resource-type");
        f.Reply(*mint);
        UNIT_ASSERT(f.Result().IsSuccess());
        f.Get(2, {"account", "cloud", "other-service", "old-microservice", "old-resource-type"});
        auto other = f.Mint();
        UNIT_ASSERT_VALUES_EQUAL(other->Get<TCloudToken::TEvCreateForServiceRequest>()->Request.service_id(), "other-service");
        f.Reply(*other, TDuration::Hours(12), "other-token");
        UNIT_ASSERT_VALUES_EQUAL(f.Result(2).Token, "other-token");
    }

    Y_UNIT_TEST(TokensWithAtMostFiveMinutesRemainingAreRejected) {
        for (const auto lifetime : {TDuration::Zero(), TDuration::Minutes(4), TDuration::Minutes(5)}) {
            TFixture f;
            f.Get();
            auto mint = f.Mint();
            f.Reply(*mint, lifetime);
            const auto result = f.Result();
            UNIT_ASSERT_VALUES_EQUAL(result.Status, Ydb::StatusIds::INTERNAL_ERROR);
            UNIT_ASSERT(result.Token.empty());
        }
    }

    Y_UNIT_TEST(TokenAtUsableBoundaryWaitsForFreshMint) {
        TFixture f;
        f.Get();
        auto mint = f.Mint();
        f.Reply(*mint, TDuration::Minutes(20));
        const auto first = f.Result();
        f.Runtime.AdvanceCurrentTime(first.UsableUntil - f.Runtime.GetCurrentTime());
        f.Get();
        auto refresh = f.Mint();
        UNIT_ASSERT(f.Results.empty());
        f.Reply(*refresh, TDuration::Hours(12), "new-token");
        UNIT_ASSERT_VALUES_EQUAL(f.Result().Token, "new-token");
    }

    Y_UNIT_TEST(MalformedRefreshKeepsUsableCachedToken) {
        TFixture f;
        f.InitialToken();
        auto refresh = f.Refresh();
        f.Reply(*refresh, TDuration::Minutes(1), "too-short");
        f.Get();
        UNIT_ASSERT_VALUES_EQUAL(f.Result().Token, "delegated-token");
    }

    Y_UNIT_TEST(RevocationDiscardsCachedToken) {
        for (auto code : {grpc::StatusCode::PERMISSION_DENIED, grpc::StatusCode::NOT_FOUND}) {
            TFixture f;
            f.InitialToken();
            auto refresh = f.Refresh();
            f.Reply(*refresh, TDuration::Hours(12), {}, code);
            f.Get();
            auto retry = f.Mint();
            f.Reply(*retry, TDuration::Hours(12), {}, code);
            const auto result = f.Result();
            UNIT_ASSERT(!result.IsSuccess());
            UNIT_ASSERT(result.Token.empty());
        }
    }

    Y_UNIT_TEST(RotatedSystemTokenAuthorizesBackgroundRefresh) {
        TFixture f;
        f.InitialToken();
        f.Update("rotated-system-token");
        auto refresh = f.Refresh();
        UNIT_ASSERT_VALUES_EQUAL(refresh->Get<TCloudToken::TEvCreateForServiceRequest>()->Token, "rotated-system-token");
        f.Reply(*refresh);
        f.Get();
        UNIT_ASSERT(f.Result().IsSuccess());
        UNIT_ASSERT_VALUES_EQUAL(f.Subscriptions, 1u);
    }
}

Y_UNIT_TEST_SUITE(IamCloudResolverActor) {
    Y_UNIT_TEST(ResolvesMatchingFolderWithUserCredentials) {
        TFixture f;
        f.Resolve();
        f.Until([&] { return !f.AccountLookups.empty(); });
        auto& lookup = *f.AccountLookups.front();
        UNIT_ASSERT_VALUES_EQUAL(lookup.Get<NCloud::TEvServiceAccountService::TEvGetServiceAccountRequest>()->Token, "user-token");
        auto account = MakeHolder<NCloud::TEvServiceAccountService::TEvGetServiceAccountResponse>();
        account->Response.set_folder_id("folder");
        f.Runtime.Send(new IEventHandle(lookup.Sender, lookup.Recipient, account.Release(), 0, lookup.Cookie));
        f.Until([&] { return !f.FolderLookups.empty(); });
        auto& folderLookup = *f.FolderLookups.front();
        UNIT_ASSERT_VALUES_EQUAL(folderLookup.Get<NCloud::TEvFolderService::TEvResolveFoldersRequest>()->Token, "user-token");
        auto folders = MakeHolder<NCloud::TEvFolderService::TEvResolveFoldersResponse>();
        auto* other = folders->Response.add_resolved_folders();
        other->set_id("other");
        other->set_cloud_id("wrong-cloud");
        auto* expected = folders->Response.add_resolved_folders();
        expected->set_id("folder");
        expected->set_cloud_id("expected-cloud");
        f.Runtime.Send(new IEventHandle(folderLookup.Sender, folderLookup.Recipient, folders.Release(), 0, folderLookup.Cookie));
        f.Until([&] { return !f.Clouds.empty(); });
        UNIT_ASSERT(f.Clouds.front().Success);
        UNIT_ASSERT_VALUES_EQUAL(f.Clouds.front().Cloud.CloudId, "expected-cloud");
        UNIT_ASSERT_VALUES_EQUAL(f.Subscriptions, 1u); // only delegated-token actor subscribes
    }

    Y_UNIT_TEST(EmptyUserTokenNeverFallsBackToSystemCredentials) {
        TFixture f;
        f.Resolve("");
        f.Until([&] { return !f.Clouds.empty(); });
        UNIT_ASSERT(!f.Clouds.front().Success);
        UNIT_ASSERT(f.AccountLookups.empty());
        UNIT_ASSERT(f.FolderLookups.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Subscriptions, 1u);
    }
}

} // namespace NKikimr::NIamDelegation
