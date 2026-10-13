#include <ydb/services/scheme_secret/iam_delegation_reader.h>
#include <ydb/services/scheme_secret/service.h>
#include <ydb/core/base/iam_delegation.h>
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
        const auto service = runtime.Register(new TServices(*this));
        runtime.RegisterService(NIamDelegation::MakeIamDelegatedTokenServiceId(), service);
        runtime.RegisterService(MakeSchemeCacheID(), service);
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

    NKqp::TEvDescribeSecretsResponse::TDescription ReadThroughService(TDescribeSecretSettings settings = {}) {
        auto& runtime = Context.GetRuntime();
        runtime.GetAppData(0).FeatureFlags.SetEnableIamDelegationSecrets(true);
        const auto service = runtime.Register(new TDescribeSchemaSecretsService());
        runtime.EnableScheduleForActor(service);
        auto promise = NThreading::NewPromise<NKqp::TEvDescribeSecretsResponse::TDescription>();
        runtime.Send(new IEventHandle(service, runtime.AllocateEdgeActor(),
            new TDescribeSchemaSecretsService::TEvResolveSecret(
                MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"}),
                "/Root/database", {"/Root/database/secret"}, promise, std::move(settings))), 0, true);
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return promise.HasValue(); };
        options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
        runtime.DispatchEvents(options, TDuration::Seconds(30));
        UNIT_ASSERT(promise.HasValue());
        runtime.Send(new IEventHandle(service, {}, new TEvents::TEvPoison));
        return promise.GetFuture().GetValue();
    }

    TActorId StartRead() {
        auto& runtime = Context.GetRuntime();
        const auto edge = runtime.AllocateEdgeActor();
        runtime.Register(CreateIamDelegationSecretReaderActor(edge, 123, "/Root/database/secret",
            "/Root/database", MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"}), Reference));
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
