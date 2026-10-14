#include "credentials.h"
#include <library/cpp/testing/unittest/registar.h>

using namespace NYql;

namespace {

class TControlledCredentialsFactory : public IStructuredTokenCredentialsFactory {
public:
    struct TState {
        NThreading::TPromise<std::string> Result = NThreading::NewPromise<std::string>();
        std::weak_ptr<NYdb::ICredentialsProvider> Provider;
        ui32 AsyncCalls = 0;
        ui32 SyncCalls = 0;
    };

    const std::shared_ptr<TState> State = std::make_shared<TState>();

    class TProvider : public NYdb::ICredentialsProvider {
    public:
        explicit TProvider(std::shared_ptr<TState> state) : State(std::move(state)) {}
        std::string GetAuthInfo() const override {
            ++State->SyncCalls;
            UNIT_FAIL("unexpected synchronous credential acquisition");
            return {};
        }
        NThreading::TFuture<std::string> GetAuthInfoAsync() const override {
            ++State->AsyncCalls;
            return State->Result.GetFuture();
        }
        bool IsValid() const override { return true; }
    private:
        std::shared_ptr<TState> State;
    };

    class TFactory : public NYdb::ICredentialsProviderFactory {
    public:
        explicit TFactory(std::shared_ptr<TState> state) : State(std::move(state)) {}
        NYdb::TCredentialsProviderPtr CreateProvider() const override {
            auto provider = std::make_shared<TProvider>(State);
            State->Provider = provider;
            return provider;
        }
        std::string GetClientIdentity() const override { return "wrapper-lifetime-test"; }
    private:
        std::shared_ptr<TState> State;
    };

    std::shared_ptr<NYdb::ICredentialsProviderFactory> Create(const TString&, bool = false) override {
        return std::make_shared<TFactory>(State);
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TCredentials) {
    Y_UNIT_TEST(CheckToken) {
        TS3Credentials::TAuthInfo authInfo{.Token = "test"};
        UNIT_ASSERT_VALUES_EQUAL(authInfo.GetToken(), "test");
    }

    Y_UNIT_TEST(CheckAws) {
        TS3Credentials::TAuthInfo authInfo{.Token = {}, .AwsAccessKey = "key", .AwsAccessSecret = "secret", .AwsRegion = "region"};
        UNIT_ASSERT_VALUES_EQUAL(authInfo.GetAwsSigV4(), "aws:amz:region:s3");
        UNIT_ASSERT_VALUES_EQUAL(authInfo.GetAwsUserPwd(), "key:secret");
    }

    Y_UNIT_TEST(AsyncSuccessAfterWrapperDestruction) {
        auto factory = std::make_shared<TControlledCredentialsFactory>();
        NThreading::TFuture<TS3Credentials::TAuthInfo> result;
        {
            TS3Credentials wrapper(factory, "test-credentials");
            result = wrapper.GetAuthInfoAsync();
            UNIT_ASSERT(!result.HasValue());
            UNIT_ASSERT(!result.HasException());
            UNIT_ASSERT_VALUES_EQUAL(factory->State->AsyncCalls, 1);
            UNIT_ASSERT_VALUES_EQUAL(factory->State->SyncCalls, 0);
        }
        UNIT_ASSERT_C(!factory->State->Provider.expired(), "pending request lost its provider");
        factory->State->Result.SetValue("completed-token");
        UNIT_ASSERT(result.Wait(TDuration::Seconds(5)));
        UNIT_ASSERT_VALUES_EQUAL(result.GetValue().GetToken(), "completed-token");
        UNIT_ASSERT_VALUES_EQUAL(factory->State->SyncCalls, 0);
    }

    Y_UNIT_TEST(AsyncFailureAfterWrapperDestruction) {
        auto factory = std::make_shared<TControlledCredentialsFactory>();
        NThreading::TFuture<TS3Credentials::TAuthInfo> result;
        {
            TS3Credentials wrapper(factory, "test-credentials");
            result = wrapper.GetAuthInfoAsync();
            UNIT_ASSERT(!result.HasValue());
            UNIT_ASSERT(!result.HasException());
            UNIT_ASSERT_VALUES_EQUAL(factory->State->AsyncCalls, 1);
            UNIT_ASSERT_VALUES_EQUAL(factory->State->SyncCalls, 0);
        }
        UNIT_ASSERT_C(!factory->State->Provider.expired(), "pending request lost its provider");
        factory->State->Result.SetException(TString("controlled provider failure"));
        UNIT_ASSERT(result.Wait(TDuration::Seconds(5)));
        UNIT_ASSERT(result.HasException());
        UNIT_ASSERT(!result.HasValue());
        UNIT_ASSERT_EXCEPTION_CONTAINS(result.GetValue(), yexception, "controlled provider failure");
        UNIT_ASSERT_VALUES_EQUAL(factory->State->SyncCalls, 0);
    }

}
