#include <ydb/services/scheme_secret/secret_credentials.h>
#include <ydb/services/scheme_secret/service.h>
#include <ydb/core/security/iam_delegation/tablet/ut/requests.h>
#include <yql/essentials/providers/common/structured_token/yql_structured_token.h>

namespace NKikimr::NSecret {
namespace {
using TDescription = NKqp::TEvDescribeSecretsResponse::TDescription;
using TReference = NKqp::TEvDescribeSecretsResponse::TSecretReference;

struct TCredentialsFixture {
    NIamDelegation::NTests::TTestContext Context;
    const TReference Reference{123, "incarnation", "creating-operation", 456, 789};
    TIntrusiveConstPtr<NACLib::TUserToken> User = MakeIntrusive<NACLib::TUserToken>(
        "original-bearer-must-not-travel", "reader", TVector<TString>{"readers"});
    std::optional<NThreading::TPromise<TDescription>> Pending;
    ui32 Reads = 0;

    class TService : public NActors::TActorBootstrapped<TService> {
    public:
        explicit TService(TCredentialsFixture& fixture) : Fixture(fixture) {}
        void Bootstrap() { Become(&TService::StateWork); }
        STRICT_STFUNC(StateWork, hFunc(TDescribeSchemaSecretsService::TEvResolveSecret, Handle);)
        void Handle(TDescribeSchemaSecretsService::TEvResolveSecret::TPtr& ev) {
            ++Fixture.Reads;
            UNIT_ASSERT(ev->Get()->Settings.ExpectedReference);
            UNIT_ASSERT(*ev->Get()->Settings.ExpectedReference == Fixture.Reference);
            UNIT_ASSERT(ev->Get()->UserToken);
            UNIT_ASSERT_VALUES_EQUAL(ev->Get()->UserToken->GetUserSID(), "reader");
            UNIT_ASSERT(ev->Get()->UserToken->GetOriginalUserToken().empty());
            UNIT_ASSERT(ev->Get()->UserToken->IsExist("readers"));
            Fixture.Pending = ev->Get()->Promise;
        }
    private:
        TCredentialsFixture& Fixture;
    };

    TCredentialsFixture() {
        auto& runtime = Context.GetRuntime();
        runtime.RegisterService(MakeDescribeSchemaSecretServiceId(runtime.GetNodeId()),
            runtime.Register(new TService(*this)));
    }

    auto Provider() {
        return CreateRefreshingSecretCredentialsProviderFactory(Context.GetRuntime().GetActorSystem(0),
            "/Root/secret", "/Root", Reference, User, EBearerPrefix::None)->CreateProvider();
    }

    void WaitForRead() {
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return Pending.has_value(); };
        Context.GetRuntime().DispatchEvents(options);
        UNIT_ASSERT(Pending);
    }

    void Reply(TDescription result) {
        auto promise = *Pending;
        Pending.reset();
        promise.SetValue(std::move(result));
    }
};
} // namespace

Y_UNIT_TEST_SUITE(RefreshingSecretCredentials) {
    Y_UNIT_TEST(BearerWrapperPreservesAsynchronousAcquisition) {
        TCredentialsFixture fixture;
        const auto provider = CreateRefreshingSecretCredentialsProviderFactory(
            fixture.Context.GetRuntime().GetActorSystem(0), "/Root/secret", "/Root", fixture.Reference,
            fixture.User, EBearerPrefix::Add)->CreateProvider();
        auto value = provider->GetAuthInfoAsync();
        fixture.WaitForRead();
        UNIT_ASSERT(!value.HasValue());
        fixture.Reply(TDescription({"minted"}, {fixture.Reference}, {TInstant::Max()}));
        UNIT_ASSERT_VALUES_EQUAL(value.GetValue(), "Bearer minted");
    }

    Y_UNIT_TEST(EveryUseReadsCurrentStateAndConcurrentUsesShareOneRead) {
        TCredentialsFixture fixture;
        const auto provider = fixture.Provider();
        auto first = provider->GetAuthInfoAsync();
        auto concurrent = provider->GetAuthInfoAsync();
        fixture.WaitForRead();
        UNIT_ASSERT_VALUES_EQUAL(fixture.Reads, 1);
        UNIT_ASSERT(!first.HasValue());
        fixture.Reply(TDescription({"token-one"}, {fixture.Reference}, {TInstant::Max()}));
        UNIT_ASSERT_VALUES_EQUAL(first.GetValue(), "token-one");
        UNIT_ASSERT_VALUES_EQUAL(concurrent.GetValue(), "token-one");
        auto next = provider->GetAuthInfoAsync();
        UNIT_ASSERT(!next.HasValue());
        fixture.WaitForRead();
        fixture.Reply(TDescription({"token-two"}, {fixture.Reference}, {TInstant::Max()}));
        UNIT_ASSERT_VALUES_EQUAL(next.GetValue(), "token-two");
        UNIT_ASSERT_VALUES_EQUAL(fixture.Reads, 2);
    }

    Y_UNIT_TEST(FailedOrExpiredReadNeverServesPreviousValue) {
        TCredentialsFixture fixture;
        const auto provider = fixture.Provider();
        auto first = provider->GetAuthInfoAsync();
        fixture.WaitForRead();
        fixture.Reply(TDescription({"previous"}, {fixture.Reference}, {TInstant::Max()}));
        UNIT_ASSERT_VALUES_EQUAL(first.GetValue(), "previous");
        auto denied = provider->GetAuthInfoAsync();
        fixture.WaitForRead();
        fixture.Reply(TDescription(Ydb::StatusIds::UNAUTHORIZED, {NYql::TIssue("revoked")}));
        UNIT_ASSERT(denied.HasException());
        auto expired = provider->GetAuthInfoAsync();
        fixture.WaitForRead();
        fixture.Reply(TDescription({"expired"}, {fixture.Reference}, {TInstant::Zero()}));
        UNIT_ASSERT(expired.HasException());
    }

    Y_UNIT_TEST(StructuredMetadataContainsIdentityButNoOriginalBearer) {
        TCredentialsFixture fixture;
        const auto result = KeepTokenSecretReference("{\"token\":\"delegated-token\"}",
            "/Root/secret", "/Root", fixture.Reference, fixture.User);
        UNIT_ASSERT(!result.Contains("original-bearer-must-not-travel"));
        const auto token = NYql::ParseStructuredToken(result);
        UNIT_ASSERT_VALUES_EQUAL(token.GetField("secret_ref"), "/Root/secret");
        UNIT_ASSERT_VALUES_EQUAL(token.GetField("secret_create_operation"), "creating-operation");
        UNIT_ASSERT_VALUES_EQUAL(token.GetField("secret_user_sid"), "reader");
    }
}
} // namespace NKikimr::NSecret
