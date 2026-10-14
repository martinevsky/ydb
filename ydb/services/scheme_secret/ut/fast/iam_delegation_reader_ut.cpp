#include <ydb/services/scheme_secret/iam_delegation_reader.h>
#include <ydb/services/scheme_secret/service.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/iam_delegated_token_service.h>
#include <ydb/core/security/token_manager/token_manager.h>
#include <ydb/library/ycloud/api/iam_token_service.h>
#include <util/random/random.h>
#include <ydb/core/tx/iam_delegation/ut/requests.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>

namespace NKikimr::NSecret {
namespace {

using namespace NIamDelegation::NTests;
using TReference = NKqp::TEvDescribeSecretsResponse::TSecretReference;

struct TReadFixture {
    TTestContext Context;
    TReference Reference;
    bool SchemaExists = true;
    bool AccessAllowed = true;
    ui64 SchemaLocalId = 42;
    ui32 MintCalls = 0;
    TInstant UsableUntil;
    TActorId HeldRecipient;
    ui64 HeldCookie = 0;
    NIamDelegation::TTokenKey HeldKey;
    bool HoldMint = false;
    Ydb::StatusIds::StatusCode MintFailureStatus = Ydb::StatusIds::UNAVAILABLE;
    ui32 MintFailuresLeft = 0;
    THashSet<TActorId> ScheduledActors;
    TTestActorRuntimeBase::TScheduledEventFilter PreviousScheduledFilter;
    TTestActorRuntimeBase::TRegistrationObserver PreviousRegistrationObserver;

    class TServices : public NActors::TActorBootstrapped<TServices> {
    public:
        explicit TServices(TReadFixture& fixture) : Fixture(fixture) {}
        void Bootstrap() { Become(&TServices::StateWork); }
        STRICT_STFUNC(StateWork,
            hFunc(NIamDelegation::TEvIamDelegation::TEvGetToken, HandleToken);
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySet, HandleSchema);
        )
        void HandleToken(NIamDelegation::TEvIamDelegation::TEvGetToken::TPtr& ev) {
            ++Fixture.MintCalls;
            UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Key.ServiceId, "ydb");
            UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Key.MicroserviceId, "data-plane");
            UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Key.ResourceType, "resource-manager.cloud");
            if (Fixture.HoldMint) {
                Fixture.HeldRecipient = ev->Sender;
                Fixture.HeldCookie = ev->Cookie;
                Fixture.HeldKey = ev->Get()->Key;
            } else {
                Send(ev->Sender, Fixture.Token(ev->Get()->Key), 0, ev->Cookie);
            }
        }
        void HandleSchema(TEvTxProxySchemeCache::TEvNavigateKeySet::TPtr& ev) {
            auto request = std::move(ev->Get()->Request);
            UNIT_ASSERT_VALUES_EQUAL(request->ResultSet.size(), 1);
            auto& entry = request->ResultSet.front();
            UNIT_ASSERT(entry.SyncVersion);
            UNIT_ASSERT_VALUES_EQUAL(entry.Access, ui32(NACLib::SelectRow));
            UNIT_ASSERT(request->UserToken);
            UNIT_ASSERT_VALUES_EQUAL(request->UserToken->GetUserSID(), "reader");
            if (!Fixture.AccessAllowed) {
                entry.Status = NSchemeCache::TSchemeCacheNavigate::EStatus::AccessDenied;
            } else if (!Fixture.SchemaExists) {
                entry.Status = NSchemeCache::TSchemeCacheNavigate::EStatus::PathErrorUnknown;
            } else {
                entry.Status = NSchemeCache::TSchemeCacheNavigate::EStatus::Ok;
                entry.Kind = NSchemeCache::TSchemeCacheNavigate::KindSecret;
                auto self = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TDirEntryInfo>();
                self->Info.SetSchemeshardId(100);
                self->Info.SetPathId(Fixture.SchemaLocalId);
                entry.Self = self;
                auto secret = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TSecretInfo>();
                auto* locator = secret->Description.MutableIamDelegation();
                locator->SetTabletId(Fixture.Reference.TabletId);
                locator->SetDatabaseIncarnation(Fixture.Reference.DatabaseIncarnation);
                locator->SetCreateOperationId(Fixture.Reference.CreateOperationId);
                entry.SecretInfo = secret;
            }
            Send(ev->Sender, new TEvTxProxySchemeCache::TEvNavigateKeySetResult(request.Release()), 0, ev->Cookie);
        }
    private:
        TReadFixture& Fixture;
    };

    TReadFixture() {
        CreateActive(Context);
        Reference = {TTestContext::TabletId, "database-incarnation-1", "create-1", 100, 42};
        UsableUntil = Context.GetRuntime().GetCurrentTime() + TDuration::Hours(1);
        auto& runtime = Context.GetRuntime();
        // Tablet maintenance timers are irrelevant to these reads and would add
        // thousands of scheduler turns while advancing a multi-minute deadline.
        // Keep timers for the reader/resolver/producer and all of their children.
        PreviousRegistrationObserver = runtime.SetRegistrationObserverFunc(
            [this](auto& rt, const auto& parent, const auto& actor) {
                PreviousRegistrationObserver(rt, parent, actor);
                if (ScheduledActors.contains(parent)) {
                    ScheduledActors.insert(actor);
                }
            });
        PreviousScheduledFilter = runtime.SetScheduledEventFilter(
            [this](auto&, auto& event, auto, auto&) {
                return !ScheduledActors.contains(event->GetRecipientRewrite());
            });
        const auto service = runtime.Register(new TServices(*this));
        runtime.RegisterService(NIamDelegation::MakeIamDelegatedTokenServiceId(), service);
        runtime.RegisterService(MakeSchemeCacheID(), service);
    }

    ~TReadFixture() {
        auto& runtime = Context.GetRuntime();
        runtime.SetScheduledEventFilter(std::move(PreviousScheduledFilter));
        runtime.SetRegistrationObserverFunc(std::move(PreviousRegistrationObserver));
    }

    void EnableSchedule(const TActorId& actor) {
        ScheduledActors.insert(actor);
        Context.GetRuntime().EnableScheduleForActor(actor);
    }

    NIamDelegation::TEvIamDelegation::TEvGetTokenResult* Token(const NIamDelegation::TTokenKey& key) {
        auto* result = new NIamDelegation::TEvIamDelegation::TEvGetTokenResult;
        result->Key = key;
        if (MintFailuresLeft) {
            --MintFailuresLeft;
            result->Status = MintFailureStatus;
            result->Issues.AddIssue("specific IAM mint failure");
            return result;
        }
        result->Token = "token-for-" + key.ServiceAccountId;
        result->UsableUntil = UsableUntil;
        result->ExpiresAt = UsableUntil + TDuration::Minutes(5);
        return result;
    }

    template<class TCondition>
    void Until(TCondition condition, TDuration timeout = TDuration::Minutes(10)) {
        if (!condition()) {
            TDispatchOptions options;
            options.CustomFinalCondition = condition;
            options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
            Context.GetRuntime().DispatchEvents(options, timeout);
        }
        UNIT_ASSERT(condition());
    }

    NThreading::TFuture<NKqp::TEvDescribeSecretsResponse::TDescription> StartThroughService(
            TDescribeSecretSettings settings = {}) {
        auto& runtime = Context.GetRuntime();
        runtime.GetAppData(0).FeatureFlags.SetEnableIamDelegationSecrets(true);
        const auto service = runtime.Register(new TDescribeSchemaSecretsService());
        EnableSchedule(service);
        auto promise = NThreading::NewPromise<NKqp::TEvDescribeSecretsResponse::TDescription>();
        runtime.Send(new IEventHandle(service, runtime.AllocateEdgeActor(),
            new TDescribeSchemaSecretsService::TEvResolveSecret(
                MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"}),
                "/Root/database", {"/Root/database/secret"}, promise, std::move(settings))), 0, true);
        return promise.GetFuture();
    }

    NKqp::TEvDescribeSecretsResponse::TDescription ReadThroughService(TDescribeSecretSettings settings = {}) {
        auto future = StartThroughService(std::move(settings));
        Until([&] { return future.HasValue(); });
        return future.GetValue();
    }

    TActorId StartRead() {
        auto& runtime = Context.GetRuntime();
        const auto edge = runtime.AllocateEdgeActor();
        const auto reader = runtime.Register(CreateIamDelegationSecretReaderActor(edge, 123, "/Root/database/secret",
            "/Root/database", MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"}), Reference));
        EnableSchedule(reader);
        return edge;
    }

    THolder<TEvIamDelegationSecretReadResult> FinishRead(const TActorId& edge) {
        auto response = Context.GetRuntime().GrabEdgeEvent<TEvIamDelegationSecretReadResult>(edge);
        UNIT_ASSERT_VALUES_EQUAL(response->Cookie, 123);
        return response->Release();
    }

    void WaitForHeldMint() {
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return bool(HeldRecipient); };
        Context.GetRuntime().DispatchEvents(options);
        UNIT_ASSERT(HeldRecipient);
    }

    void ReleaseMint() {
        Context.GetRuntime().Send(new IEventHandle(HeldRecipient, TActorId(), Token(HeldKey), 0, HeldCookie));
        HeldRecipient = {};
        HoldMint = false;
    }
};

} // namespace

Y_UNIT_TEST_SUITE(IamDelegationSecretReader) {
    Y_UNIT_TEST(DefaultResolverAndConcurrentReaderWaitThroughRealProducerBackoff) {
        TReadFixture fixture;
        SetRandomSeed(42);
        auto& runtime = fixture.Context.GetRuntime();
        using TCloudToken = NCloud::TEvIamTokenService;
        TActorId producer;
        TDuration longBackoff;
        ui32 subscriptionsSeen = 0;
        ui32 mintCalls = 0;
        ui32 getCalls = 0;
        ui32 readerReplies = 0;
        bool recover = false;
        TTestActorRuntimeBase::TScheduledEventFilter previousFilter;
        previousFilter = runtime.SetScheduledEventFilter([&](auto& rt, auto& event, TDuration delay, auto& deadline) {
            if (event->Recipient == producer && delay > TDuration::Seconds(65)
                    && delay <= TDuration::Seconds(75)) {
                longBackoff = delay;
            }
            return previousFilter(rt, event, delay, deadline);
        });
        Y_DEFER { runtime.SetScheduledEventFilter(std::move(previousFilter)); };
        auto subscriptions = runtime.AddObserver<TEvTokenManager::TEvSubscribeUpdateToken>(
            [&](auto& event) { ++subscriptionsSeen; event.Reset(); });
        auto mints = runtime.AddObserver<TCloudToken::TEvCreateForServiceRequest>([&](auto& event) {
            ++mintCalls;
            UNIT_ASSERT_C(mintCalls < 200, "Producer never reached its maximum backoff");
            UNIT_ASSERT_VALUES_EQUAL(event->Get()->Request.target_service_account_id(), "service-account-1");
            auto response = MakeHolder<TCloudToken::TEvCreateForServiceResponse>();
            response->Status.GRpcStatusCode = recover ? grpc::StatusCode::OK : grpc::StatusCode::UNAVAILABLE;
            response->Response.set_iam_token("recovered-token");
            response->Response.mutable_expires_at()->set_seconds(
                (runtime.GetCurrentTime() + TDuration::Hours(1)).Seconds());
            runtime.Send(new IEventHandle(event->Sender, event->Recipient, response.Release(), 0, event->Cookie));
            event.Reset();
        });
        auto gets = runtime.AddObserver<NIamDelegation::TEvIamDelegation::TEvGetToken>(
            [&](auto&) { ++getCalls; });
        auto replies = runtime.AddObserver<TEvIamDelegationSecretReadResult>([&](auto& event) {
            ++readerReplies;
            UNIT_ASSERT_VALUES_EQUAL_C(event->Get()->Status, Ydb::StatusIds::SUCCESS,
                event->Get()->Issues.ToString());
        });
        NIamDelegation::TIamDelegationSettings settings;
        settings.Config.SetTokenServiceEndpoint("localhost:1");
        settings.Config.SetSystemTokenName("system-account");
        settings.Config.SetEnableSsl(false);
        const auto manager = runtime.AllocateEdgeActor();
        runtime.RegisterService(MakeTokenManagerID(), manager);
        producer = runtime.Register(NIamDelegation::CreateIamDelegatedTokenService(settings));
        runtime.RegisterService(NIamDelegation::MakeIamDelegatedTokenServiceId(), producer);
        fixture.EnableSchedule(producer);
        fixture.Until([&] { return subscriptionsSeen == 1; }, TDuration::Seconds(1));
        runtime.Send(new IEventHandle(producer, manager, new TEvTokenManager::TEvUpdateToken(
            "system-account", "system-token", {TEvTokenManager::TStatus::ECode::SUCCESS, {}})));
        runtime.Send(new IEventHandle(producer, runtime.AllocateEdgeActor(),
            new NIamDelegation::TEvIamDelegation::TEvGetToken(
                {"service-account-1", "cloud-1", "ydb", "data-plane", "resource-manager.cloud"})));
        fixture.Until([&] { return longBackoff != TDuration::Zero(); });
        recover = true;
        const auto mintsBeforeRead = mintCalls;
        const auto getsBeforeRead = getCalls;
        const auto edge = fixture.StartRead();
        auto resolved = fixture.StartThroughService(); // exercise the default retry policy
        fixture.Until([&] { return getCalls == getsBeforeRead + 2; }, TDuration::Seconds(1));
        runtime.SimulateSleep(TDuration::Seconds(61));
        UNIT_ASSERT_VALUES_EQUAL(readerReplies, 0);
        UNIT_ASSERT_VALUES_EQUAL(getCalls, getsBeforeRead + 2); // neither reader abandoned the shared next attempt
        UNIT_ASSERT_VALUES_EQUAL(mintCalls, mintsBeforeRead);
        UNIT_ASSERT(!resolved.HasValue());
        fixture.Until([&] { return resolved.HasValue(); });
        const auto direct = fixture.FinishRead(edge);
        UNIT_ASSERT_VALUES_EQUAL(direct->Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(direct->Token, "recovered-token");
        UNIT_ASSERT_VALUES_EQUAL(resolved.GetValue().Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(resolved.GetValue().SecretValues.at(0), "recovered-token");
        UNIT_ASSERT_VALUES_EQUAL(mintCalls, mintsBeforeRead + 1);
        UNIT_ASSERT_VALUES_EQUAL(getCalls, getsBeforeRead + 2);
    }

    Y_UNIT_TEST(MissingTokenReplyTimesOutAndLateReplyIsIgnored) {
        TReadFixture fixture;
        fixture.HoldMint = true;
        const auto edge = fixture.StartRead();
        fixture.WaitForHeldMint();
        const auto started = fixture.Context.GetRuntime().GetCurrentTime();
        const auto result = fixture.FinishRead(edge);
        UNIT_ASSERT_VALUES_EQUAL(result->Status, Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_STRING_CONTAINS(result->Issues.ToString(), "timed out");
        const auto elapsed = fixture.Context.GetRuntime().GetCurrentTime() - started;
        UNIT_ASSERT_C(elapsed > TDuration::Seconds(246), elapsed);
        UNIT_ASSERT_C(elapsed < TDuration::Minutes(5), elapsed);
        ui32 lateReplies = 0;
        auto replies = fixture.Context.GetRuntime().AddObserver<TEvIamDelegationSecretReadResult>(
            [&](auto&) { ++lateReplies; });
        fixture.ReleaseMint();
        fixture.Context.GetRuntime().SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(lateReplies, 0);
    }

    Y_UNIT_TEST(StorageWaitRemainsBounded) {
        TReadFixture fixture;
        auto& runtime = fixture.Context.GetRuntime();
        auto responses = runtime.AddObserver<NIamDelegation::TEvIamDelegationTablet::TEvResponse>(
            [&](auto& event) { event.Reset(); });
        const auto started = runtime.GetCurrentTime();
        const auto result = fixture.FinishRead(fixture.StartRead());
        UNIT_ASSERT_VALUES_EQUAL(result->Status, Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_VALUES_EQUAL(runtime.GetCurrentTime() - started, TDuration::Seconds(30));
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 0);
    }

    Y_UNIT_TEST(StaleTokenTimerDoesNotShortenBoundedSchemaWait) {
        TReadFixture fixture;
        fixture.HoldMint = true;
        auto& runtime = fixture.Context.GetRuntime();
        const auto edge = fixture.StartRead();
        fixture.WaitForHeldMint();
        runtime.SimulateSleep(TDuration::Seconds(240));
        ui32 schemaRequests = 0;
        ui32 readerReplies = 0;
        auto schemas = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySet>([&](auto& event) {
            ++schemaRequests;
            event.Reset();
        });
        auto replies = runtime.AddObserver<TEvIamDelegationSecretReadResult>(
            [&](auto&) { ++readerReplies; });
        fixture.ReleaseMint();
        fixture.Until([&] { return schemaRequests == 1; });
        const auto schemaStarted = runtime.GetCurrentTime();
        runtime.SimulateSleep(TDuration::Seconds(8)); // the old token-phase deadline has passed
        UNIT_ASSERT_VALUES_EQUAL(readerReplies, 0);
        const auto result = fixture.FinishRead(edge);
        UNIT_ASSERT_VALUES_EQUAL(result->Status, Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_VALUES_EQUAL(runtime.GetCurrentTime() - schemaStarted, TDuration::Seconds(30));
        UNIT_ASSERT_VALUES_EQUAL(readerReplies, 1);
    }

    Y_UNIT_TEST(CancelledReaderIgnoresLateMintAndTimer) {
        TReadFixture fixture;
        fixture.HoldMint = true;
        auto& runtime = fixture.Context.GetRuntime();
        fixture.StartRead();
        fixture.WaitForHeldMint();
        ui32 readerReplies = 0;
        ui32 schemaRequests = 0;
        auto replies = runtime.AddObserver<TEvIamDelegationSecretReadResult>(
            [&](auto&) { ++readerReplies; });
        auto schemas = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySet>(
            [&](auto&) { ++schemaRequests; });
        runtime.Send(new IEventHandle(fixture.HeldRecipient, {}, new TEvents::TEvPoison));
        fixture.ReleaseMint();
        runtime.SimulateSleep(TDuration::Minutes(5));
        UNIT_ASSERT_VALUES_EQUAL(readerReplies, 0);
        UNIT_ASSERT_VALUES_EQUAL(schemaRequests, 0);
    }

    Y_UNIT_TEST(TokenAtUsableDeadlineDuringSchemaCheckIsNotReturned) {
        TReadFixture fixture;
        auto& runtime = fixture.Context.GetRuntime();
        fixture.UsableUntil = runtime.GetCurrentTime() + TDuration::Seconds(1);
        auto schemas = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySetResult>([&](auto&) {
            runtime.AdvanceCurrentTime(TDuration::Seconds(1));
        });
        const auto result = fixture.FinishRead(fixture.StartRead());
        UNIT_ASSERT_VALUES_EQUAL(result->Status, Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_STRING_CONTAINS(result->Issues.ToString(), "expired during authorization");
    }

    Y_UNIT_TEST(SecretServiceRetriesEveryTransientMintStatusWithCallerPolicy) {
        for (const auto status : {Ydb::StatusIds::UNAVAILABLE, Ydb::StatusIds::TIMEOUT,
                Ydb::StatusIds::OVERLOADED, Ydb::StatusIds::INTERNAL_ERROR}) {
            TReadFixture fixture;
            fixture.MintFailureStatus = status;
            fixture.MintFailuresLeft = 1;
            const auto result = fixture.ReadThroughService({.RetryPolicy = MakeShortRetryPolicy()});
            UNIT_ASSERT_VALUES_EQUAL_C(result.Status, Ydb::StatusIds::SUCCESS,
                status << ": " << result.Issues.ToString());
            UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 2);
            UNIT_ASSERT_VALUES_EQUAL(result.SecretValues.at(0), "token-for-service-account-1");
        }
    }

    Y_UNIT_TEST(SecretServiceUsesDefaultRetryPolicyForTransientMintFailure) {
        TReadFixture fixture;
        fixture.MintFailuresLeft = 1;
        const auto result = fixture.ReadThroughService();
        UNIT_ASSERT_VALUES_EQUAL(result.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 2);
    }

    Y_UNIT_TEST(SecretServiceDoesNotRetryDefinitiveMintErrors) {
        for (const auto status : {Ydb::StatusIds::UNAUTHORIZED, Ydb::StatusIds::NOT_FOUND,
                Ydb::StatusIds::BAD_REQUEST}) {
            TReadFixture fixture;
            fixture.MintFailureStatus = status;
            fixture.MintFailuresLeft = 1;
            const auto result = fixture.ReadThroughService({.RetryPolicy = MakeShortRetryPolicy()});
            UNIT_ASSERT_VALUES_EQUAL(result.Status, status);
            UNIT_ASSERT_STRING_CONTAINS(result.Issues.ToString(), "specific IAM mint failure");
            UNIT_ASSERT(result.SecretValues.empty());
            UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 1);
        }
    }

    Y_UNIT_TEST(SecretServiceHonorsCallerRetryBudgetAndFinalStatus) {
        TReadFixture fixture;
        fixture.MintFailureStatus = Ydb::StatusIds::TIMEOUT;
        fixture.MintFailuresLeft = 3;
        auto policy = IRetryPolicy<>::GetFixedIntervalPolicy(
            [] { return ERetryErrorClass::ShortRetry; }, TDuration::MilliSeconds(1),
            TDuration::MilliSeconds(1), 1);
        const auto result = fixture.ReadThroughService({.RetryPolicy = std::move(policy)});
        UNIT_ASSERT_VALUES_EQUAL(result.Status, Ydb::StatusIds::TIMEOUT);
        UNIT_ASSERT(result.SecretValues.empty());
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 2);
    }

    Y_UNIT_TEST(ReadsOnlyBoundActiveDelegationAndChecksAcl) {
        TReadFixture fixture;
        const auto result = fixture.FinishRead(fixture.StartRead());
        UNIT_ASSERT_VALUES_EQUAL_C(result->Status, Ydb::StatusIds::SUCCESS, result->Issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(result->Token, "token-for-service-account-1");
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 1);
        UNIT_ASSERT_VALUES_EQUAL(result->UsableUntil, fixture.UsableUntil);
    }

    Y_UNIT_TEST(CreatingOperationMismatchNeverMints) {
        TReadFixture fixture;
        fixture.Reference.CreateOperationId = "another-create";
        const auto result = fixture.FinishRead(fixture.StartRead());
        UNIT_ASSERT_VALUES_EQUAL(result->Status, Ydb::StatusIds::NOT_FOUND);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 0);
    }

    Y_UNIT_TEST(HeldTokenCannotCrossPromotion) {
        TReadFixture fixture;
        fixture.HoldMint = true;
        const auto edge = fixture.StartRead();
        fixture.WaitForHeldMint();
        auto binding = Binding("referrer-new");
        binding.SetServiceAccountId("new-account");
        const auto staged = fixture.Context.Call(StageAlterRequest(GetSecret(fixture.Context), "new", binding));
        const auto setup = CompleteSetup(fixture.Context, staged.GetDelegation());
        fixture.Context.Call(PromoteRequest(setup.GetDelegation(), GetSecret(fixture.Context).GetRevision()));
        fixture.ReleaseMint();
        UNIT_ASSERT_VALUES_EQUAL(fixture.FinishRead(edge)->Status, Ydb::StatusIds::UNAVAILABLE);
        const auto next = fixture.FinishRead(fixture.StartRead());
        UNIT_ASSERT_VALUES_EQUAL(next->Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(next->Token, "token-for-new-account");
    }

    Y_UNIT_TEST(SchemaDropEndsAccessBeforeLifecycleWorkerRetiresBinding) {
        TReadFixture fixture;
        fixture.HoldMint = true;
        const auto edge = fixture.StartRead();
        fixture.WaitForHeldMint();
        fixture.SchemaExists = false;
        fixture.ReleaseMint();
        UNIT_ASSERT_VALUES_EQUAL(fixture.FinishRead(edge)->Status, Ydb::StatusIds::NOT_FOUND);
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(fixture.Context).GetState(), NProto::SECRET_LIVE);
    }

    Y_UNIT_TEST(ReusedSchemaPathCannotReuseOldAuthorization) {
        TReadFixture fixture;
        fixture.SchemaLocalId = 43;
        UNIT_ASSERT_VALUES_EQUAL(fixture.FinishRead(fixture.StartRead())->Status, Ydb::StatusIds::NOT_FOUND);
    }

    Y_UNIT_TEST(AclRevocationDeniesAlreadyMintedValue) {
        TReadFixture fixture;
        fixture.AccessAllowed = false;
        UNIT_ASSERT_VALUES_EQUAL(fixture.FinishRead(fixture.StartRead())->Status, Ydb::StatusIds::UNAUTHORIZED);
    }

    Y_UNIT_TEST(TokenAtUsableDeadlineIsNotReturned) {
        TReadFixture fixture;
        fixture.UsableUntil = fixture.Context.GetRuntime().GetCurrentTime();
        UNIT_ASSERT_VALUES_EQUAL(fixture.FinishRead(fixture.StartRead())->Status, Ydb::StatusIds::UNAVAILABLE);
    }
}

} // namespace NKikimr::NSecret
