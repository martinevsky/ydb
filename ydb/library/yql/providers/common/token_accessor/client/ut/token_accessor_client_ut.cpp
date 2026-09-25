// Tests for the token accessor credentials provider (target D of the FQ transport test plan,
// test_plan.md §4: T-LIF-12, T-LIF-13, T-TMO-11 and pins) against H5 TFakeTokenAccessor.
//
// The provider constructor blocks today (it waits for the first GetToken reply, up to
// RequestTimeout + 10 s), so every construction runs on a helper thread and the test waits for it
// with a guard. Seam S8 (non-blocking constructor + WaitReady) is a fix and is not implemented, so
// readiness is observed by polling GetAuthInfo() under a guard. Guards are failure deadlines, never
// the pass condition (§6.3 D-1). Contract tests that fail today start with YDB_SKIP_KNOWN_BUG_GTEST
// (run them with --test-env=YDB_TRANSPORT_RUN_KNOWN_BUGS=1).
//
// Teardown order in every test: open the fake's latch, join the construction threads (this
// destroys the providers), stop the gRPC client, shut the fake down.

#include <ydb/library/yql/providers/common/token_accessor/client/token_accessor_client.h>
#include <ydb/library/yql/providers/common/token_accessor/client/token_accessor_client_factory.h>
#include <ydb/library/yql/providers/common/token_accessor/client/ut_helpers/fake_token_accessor.h>

#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug_gtest.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/wait.h>

#include <library/cpp/testing/gtest/gtest.h>
#include <library/cpp/threading/future/future.h>

#include <util/generic/maybe.h>
#include <util/generic/scope.h>
#include <util/string/ascii.h>
#include <util/string/builder.h>

#include <thread>

namespace {

using namespace NYql;
using namespace NYql::NTransportTest;

const TString SA_ID = "SA";
const TString SA_SIGNATURE = "SA-SIGNATURE";
const TString SA_TOKEN = "SA-TOKEN";
const TDuration REFRESH_PERIOD = TDuration::Hours(1);

// A gRPC client and a TokenAccessorService connection to the fake, as the factory builds them.
// The production factory uses TGRpcClientLow's default number of completion threads (2).
struct TTokenAccessorConnection {
    std::shared_ptr<NYdbGrpc::TGRpcClientLow> Client;
    std::shared_ptr<NYdbGrpc::TServiceConnection<TokenAccessorService>> Connection;

    explicit TTokenAccessorConnection(const TFakeTokenAccessor& accessor, size_t completionThreads = NYdbGrpc::DEFAULT_NUM_THREADS)
        : Client(std::make_shared<NYdbGrpc::TGRpcClientLow>(completionThreads))
    {
        NYdbGrpc::TGRpcClientConfig config;
        config.Locator = accessor.Endpoint();
        Connection = Client->CreateGRpcServiceConnection<TokenAccessorService>(config);
    }

    ~TTokenAccessorConnection() {
        Connection.reset();
        Client->Stop(true);
    }
};

// Runs a (blocking today) provider construction on a helper thread. The destructor joins the
// thread; the provider is destroyed with this object.
class TProviderConstruction {
public:
    using TMake = std::function<std::shared_ptr<NYdb::ICredentialsProvider>()>;

    explicit TProviderConstruction(TMake make)
        : Promise_(NThreading::NewPromise<std::shared_ptr<NYdb::ICredentialsProvider>>())
        , Thread_([promise = Promise_, make = std::move(make)]() mutable {
            try {
                promise.SetValue(make());
            } catch (...) {
                promise.SetException(std::current_exception());
            }
        })
    {}

    ~TProviderConstruction() {
        Thread_.join();
    }

    // True when the constructor returned within `guard`.
    bool WaitConstructed(TDuration guard) const {
        return Promise_.GetFuture().Wait(guard);
    }

    std::shared_ptr<NYdb::ICredentialsProvider> Provider() const {
        return Promise_.GetFuture().GetValue();
    }

private:
    NThreading::TPromise<std::shared_ptr<NYdb::ICredentialsProvider>> Promise_;
    std::thread Thread_;
};

// The GetAuthInfo() error message, or Nothing() when a token is returned.
TMaybe<TString> AuthError(const NYdb::ICredentialsProvider& provider) {
    try {
        provider.GetAuthInfo();
        return Nothing();
    } catch (const std::exception& e) {
        return TString(e.what());
    }
}

bool ContainsIgnoreCase(TStringBuf text, TStringBuf what) {
    return to_lower(TString(text)).Contains(to_lower(TString(what)));
}

void WaitToken(const NYdb::ICredentialsProvider& provider, const TString& token, TDuration guard) {
    WaitUntil([&]() {
        return !AuthError(provider).Defined() && provider.GetAuthInfo() == token;
    }, guard, TStringBuilder() << "the provider returns the token " << token);
}

TProviderConstruction::TMake ViaFactory(const TTokenAccessorConnection& connection, TDuration requestTimeout) {
    auto factory = CreateTokenAccessorCredentialsProviderFactory(
        connection.Client, connection.Connection, SA_ID, SA_SIGNATURE, REFRESH_PERIOD, requestTimeout);
    return [factory]() {
        return factory->CreateProvider();
    };
}

// Pin: the first GetToken is held; once it is answered, the provider returns the token, and the
// request carries the service account id, its signature and the service account type.
TEST(TTokenAccessorClientTest, ProviderReturnsTokenOnceReleased) {
    TFakeTokenAccessor accessor(SA_TOKEN);
    TTokenAccessorConnection connection(accessor);
    accessor.GateReplies();

    TProviderConstruction construction(ViaFactory(connection, TDuration::Seconds(10)));
    Y_DEFER {
        accessor.Release();
    };

    accessor.WaitForCalls(1);
    accessor.Release();
    ASSERT_TRUE(construction.WaitConstructed(TDuration::Seconds(10)))
        << "the provider was not constructed within 10 s after the reply was released";

    const auto provider = construction.Provider();
    EXPECT_EQ(provider->GetAuthInfo(), SA_TOKEN);

    const auto calls = accessor.Calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].Type, static_cast<int>(GetTokenRequest::TYPE_SERVICE_ACCOUNT));
    EXPECT_EQ(calls[0].TokenId, SA_ID);
    EXPECT_EQ(calls[0].Signature, SA_SIGNATURE);
}

// Pin: one UNAVAILABLE reply is retried during construction and the provider ends up ready.
TEST(TTokenAccessorClientTest, ConstructionRetriesAfterUnavailable) {
    TFakeTokenAccessor accessor(SA_TOKEN);
    TTokenAccessorConnection connection(accessor);
    accessor.FailNThenSucceed(1, grpc::StatusCode::UNAVAILABLE);

    TProviderConstruction construction(ViaFactory(connection, TDuration::Seconds(10)));

    ASSERT_TRUE(construction.WaitConstructed(TDuration::Seconds(10)))
        << "the provider was not constructed within 10 s";
    EXPECT_EQ(construction.Provider()->GetAuthInfo(), SA_TOKEN);
    EXPECT_EQ(accessor.CallCount(SA_ID), 2u);
}

// T-LIF-12 (F-B-5): creating a provider must not block on the token accessor. Today the
// constructor calls UpdateTicket(true), which waits for the first reply up to RequestTimeout + 10 s.
TEST(TTokenAccessorClientTest, ConstructionIsNonBlocking) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-5");

    TFakeTokenAccessor accessor(SA_TOKEN);
    TTokenAccessorConnection connection(accessor);
    accessor.GateReplies();

    TProviderConstruction construction(ViaFactory(connection, TDuration::Seconds(10)));
    Y_DEFER {
        accessor.Release();
    };

    accessor.WaitForCalls(1);
    ASSERT_TRUE(construction.WaitConstructed(TDuration::Seconds(3)))
        << "CreateProvider() did not return within 3 s while the first GetToken reply is held"
        << " (held replies: " << accessor.HeldNow() << ")";
    ASSERT_EQ(accessor.HeldNow(), 1u) << "the latch must still be closed when construction returns";

    const auto provider = construction.Provider();
    const auto error = AuthError(*provider);
    ASSERT_TRUE(error.Defined()) << "GetAuthInfo() returned a token before the reply was released";
    EXPECT_TRUE(error->Contains("not ready")) << *error;

    accessor.Release();
    WaitToken(*provider, SA_TOKEN, TDuration::Seconds(10));
}

// T-LIF-13 (N-4): an error reply must not occupy the gRPC completion thread. Today the reply
// callback runs ProcessResponse, which sleeps for the backoff and, during construction, re-sends
// the request and waits for its reply (up to RequestTimeout + 10 s) inside the callback.
// One completion thread makes the effect deterministic: provider 2's reply can only be processed
// by the thread that provider 1's callback occupies. (Production shares one client with 2 threads
// between all providers of a factory, so two retrying providers starve every other one.)
// Provider 1's retry is held by the fake, so "provider 1 is still retrying" is a latch, not a window.
TEST(TTokenAccessorClientTest, BackoffDoesNotBlockCompletionThread) {
    YDB_SKIP_KNOWN_BUG_GTEST("N-4");

    const TString sa1 = "SA-1";
    const TString sa2 = "SA-2";
    const TDuration requestTimeout = TDuration::Seconds(1);

    TFakeTokenAccessor accessor;
    accessor.SetScript([&](const TTokenAccessorCall& call) {
        if (call.TokenId == sa1) {
            if (call.Attempt == 1) {
                return TTokenReply::Fail(grpc::StatusCode::UNAVAILABLE);
            }
            return TTokenReply::Ok("SA-1-TOKEN").Held();
        }
        return TTokenReply::Ok("SA-2-TOKEN");
    });
    TTokenAccessorConnection connection(accessor, 1);

    TProviderConstruction construction1([&]() {
        return CreateTokenAccessorCredentialsProvider(
            connection.Client, connection.Connection, sa1, SA_SIGNATURE, REFRESH_PERIOD, requestTimeout);
    });
    accessor.WaitForCalls(sa1, 2);
    accessor.WaitForHeldCalls(1);

    TProviderConstruction construction2([&]() {
        return CreateTokenAccessorCredentialsProvider(
            connection.Client, connection.Connection, sa2, SA_SIGNATURE, REFRESH_PERIOD, requestTimeout);
    });
    Y_DEFER {
        accessor.Release();
    };

    accessor.WaitForCalls(sa2, 1);
    ASSERT_TRUE(construction2.WaitConstructed(TDuration::Seconds(5)))
        << "provider 2 was not constructed within 5 s: its reply is not processed while provider 1's"
        << " retry is pending (the completion thread is blocked)";
    WaitToken(*construction2.Provider(), "SA-2-TOKEN", TDuration::Seconds(5));

    EXPECT_EQ(accessor.CallCount(sa1), 2u);
    EXPECT_EQ(accessor.HeldNow(), 1u) << "provider 1's retry must still be pending";
}

// T-TMO-11: the request deadline is reported. The accessor never answers (requestTimeout = 1 s);
// the provider must be available at once and GetAuthInfo() must report "not ready" with the
// DEADLINE_EXCEEDED status of the last request. Today the constructor blocks for
// RequestTimeout + 10 s = 11 s (the guard is 5 s).
TEST(TTokenAccessorClientTest, RequestDeadlineReported) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-B-5");

    TFakeTokenAccessor accessor(SA_TOKEN);
    TTokenAccessorConnection connection(accessor);
    accessor.GateReplies();

    TProviderConstruction construction(ViaFactory(connection, TDuration::Seconds(1)));
    Y_DEFER {
        accessor.Release();
    };

    ASSERT_TRUE(construction.WaitConstructed(TDuration::Seconds(5)))
        << "CreateProvider() did not return within 5 s with requestTimeout = 1 s and no reply";
    const auto provider = construction.Provider();

    TString error;
    WaitUntil([&]() {
        const auto current = AuthError(*provider);
        error = current.GetOrElse("");
        return ContainsIgnoreCase(error, "deadline");
    }, TDuration::Seconds(5), "GetAuthInfo() reports the request deadline");
    EXPECT_TRUE(error.Contains("not ready")) << error;
}

} // namespace
