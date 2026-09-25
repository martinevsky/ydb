// FQ transport test plan, target H: HTTP gateway config of a KQP node (seam S5).
// T-CFG-1, T-CFG-2, T-CFG-3 (§4.7) are pure; T-TMO-1 (§4.2) drives the real curl gateway made from the
// KQP default config against a stalled loopback server (H1, H11).

#include <ydb/core/kqp/federated_query/http_gateway_config/kqp_http_gateway_config.h>
#include <ydb/core/protos/config.pb.h>

#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug_gtest.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/gateway_scope.h>

#include <library/cpp/testing/gtest/gtest.h>
#include <library/cpp/threading/future/future.h>

#include <atomic>
#include <memory>

namespace NKikimr::NKqp {

namespace {

using namespace NYql::NTransportTest;

// Today's DefaultHttpGatewayConfig() (kqp_federated_query_helpers.cpp before S5), field by field.
NYql::THttpGatewayConfig TodayDefaultHttpGatewayConfig() {
    NYql::THttpGatewayConfig config;
    config.SetMaxInFlightCount(2000);
    config.SetMaxSimulatenousDownloadsSize(2000000000);
    config.SetBuffersSizePerStream(5000000);
    config.SetConnectionTimeoutSeconds(15);
    config.SetRequestTimeoutSeconds(0);
    return config;
}

// T-TMO-1 helper: the same config with the stall guard divided by `factor` (at least 1 s when set).
// A zero (absent) LowSpeedTimeSeconds stays zero: the test asserts on the behaviour, not here.
NYql::THttpGatewayConfig ScaleForTest(NYql::THttpGatewayConfig config, ui64 factor) {
    if (config.GetLowSpeedTimeSeconds() > 0) {
        config.SetLowSpeedTimeSeconds(Max<ui64>(1, config.GetLowSpeedTimeSeconds() / factor));
    }
    return config;
}

struct TCallResult {
    CURLcode CurlCode = CURLE_OK;
    long HttpCode = 0;
    TString Issues;
};

// A buffered Download whose callback completes a future and counts invocations.
class TBufferedCall {
public:
    NYql::IHTTPGateway::TOnResult Callback() const {
        return [promise = Promise_, calls = Calls_](NYql::IHTTPGateway::TResult&& result) mutable {
            calls->fetch_add(1);
            promise.TrySetValue(TCallResult{
                .CurlCode = result.CurlResponseCode,
                .HttpCode = result.Content.HttpResponseCode,
                .Issues = result.Issues.ToOneLineString(),
            });
        };
    }

    // False if the callback did not come within `guard` (a failure deadline, not an oracle).
    bool Wait(TDuration guard) const {
        return Promise_.GetFuture().Wait(guard);
    }

    TCallResult Result() const {
        return Promise_.GetFuture().GetValue();
    }

    ui32 Calls() const {
        return Calls_->load();
    }

private:
    NThreading::TPromise<TCallResult> Promise_ = NThreading::NewPromise<TCallResult>();
    std::shared_ptr<std::atomic<ui32>> Calls_ = std::make_shared<std::atomic<ui32>>(0);
};

}  // namespace

// Pin (seam S5 is behaviour-preserving): DefaultHttpGatewayConfig() returns exactly today's values.
// The F-A-4 fix adds LowSpeedTimeSeconds / LowSpeedBytesLimit here (see T-CFG-1) and updates this pin.
TEST(TKqpHttpGatewayConfigTest, DefaultHttpGatewayConfigPinnedToday) {
    const auto config = DefaultHttpGatewayConfig();
    EXPECT_EQ(config.ShortDebugString(), TodayDefaultHttpGatewayConfig().ShortDebugString());
    EXPECT_EQ(config.GetMaxInFlightCount(), 2000u);
    EXPECT_EQ(config.GetMaxSimulatenousDownloadsSize(), 2000000000u);
    EXPECT_EQ(config.GetBuffersSizePerStream(), 5000000u);
    EXPECT_EQ(config.GetConnectionTimeoutSeconds(), 15u);
    EXPECT_TRUE(config.HasRequestTimeoutSeconds());
    EXPECT_EQ(config.GetRequestTimeoutSeconds(), 0u);
    EXPECT_FALSE(config.HasLowSpeedTimeSeconds());
    EXPECT_FALSE(config.HasLowSpeedBytesLimit());
}

// T-CFG-1 (F-A-4, P0): the KQP default config has a stall guard. Today it disables the total timeout
// (RequestTimeoutSeconds = 0) and sets no LowSpeed limit, so a stalled GET never ends.
// Reference values: the kqprun/fqrun configs cited in F-A-4.
TEST(TKqpHttpGatewayConfigTest, DefaultHttpGatewayConfigHasStallGuard) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-A-4");
    const auto config = DefaultHttpGatewayConfig();
    EXPECT_EQ(config.GetLowSpeedTimeSeconds(), 20u);
    EXPECT_EQ(config.GetLowSpeedBytesLimit(), 1024u);
    EXPECT_EQ(config.GetRequestTimeoutSeconds(), 0u);
    EXPECT_EQ(config.GetMaxInFlightCount(), 2000u);
}

// Pin: without an HttpGateway section the node uses DefaultHttpGatewayConfig().
TEST(TKqpHttpGatewayConfigTest, EffectiveConfigWithoutSectionIsDefault) {
    const NKikimrConfig::TQueryServiceConfig queryServiceConfig;
    EXPECT_EQ(GetEffectiveHttpGatewayConfig(queryServiceConfig).ShortDebugString(),
        DefaultHttpGatewayConfig().ShortDebugString());
}

// T-CFG-2 (F-A-3 reach, P2), pin of today's rule: a present section replaces the defaults as a whole.
// A section that omits RequestTimeoutSeconds leaves it unset, so the curl gateway applies its own
// built-in 150 s total timeout (TCurlInitConfig), and no LowSpeed guard.
TEST(TKqpHttpGatewayConfigTest, EffectiveConfigSectionReplacesDefaults) {
    NKikimrConfig::TQueryServiceConfig queryServiceConfig;
    queryServiceConfig.MutableHttpGateway()->SetMaxInFlightCount(10);
    const auto config = GetEffectiveHttpGatewayConfig(queryServiceConfig);
    EXPECT_EQ(config.ShortDebugString(), queryServiceConfig.GetHttpGateway().ShortDebugString());
    EXPECT_EQ(config.GetMaxInFlightCount(), 10u);
    EXPECT_FALSE(config.HasRequestTimeoutSeconds());
    EXPECT_FALSE(config.HasLowSpeedTimeSeconds());
    EXPECT_FALSE(config.HasMaxSimulatenousDownloadsSize());
}

// T-CFG-2, decision-gated contract variant "merge the section over the defaults": the effective config
// keeps the section's MaxInFlightCount and inherits RequestTimeoutSeconds = 0 and the LowSpeed guard.
TEST(TKqpHttpGatewayConfigTest, EffectiveConfigSectionMergesOverDefaults) {
    YDB_SKIP_KNOWN_BUG_GTEST("DG-T-CFG-2");
    NKikimrConfig::TQueryServiceConfig queryServiceConfig;
    queryServiceConfig.MutableHttpGateway()->SetMaxInFlightCount(10);
    const auto config = GetEffectiveHttpGatewayConfig(queryServiceConfig);
    EXPECT_EQ(config.GetMaxInFlightCount(), 10u);
    EXPECT_TRUE(config.HasRequestTimeoutSeconds());
    EXPECT_EQ(config.GetRequestTimeoutSeconds(), 0u);
    EXPECT_GT(config.GetLowSpeedTimeSeconds(), 0u);
    EXPECT_GT(config.GetLowSpeedBytesLimit(), 0u);
}

// Pin (seam S5 is behaviour-preserving): PoolCapMaxHandlers reproduces the value kqp_proxy_service
// computed inline before S5: the section's MaxInFlightCount if set, else 1024.
TEST(TKqpHttpGatewayConfigTest, PoolCapMaxHandlersPinnedToday) {
    NKikimrConfig::TQueryServiceConfig noSection;
    EXPECT_EQ(PoolCapMaxHandlers(noSection), 1024u);

    NKikimrConfig::TQueryServiceConfig sectionWithoutMaxInFlight;
    sectionWithoutMaxInFlight.MutableHttpGateway()->SetConnectionTimeoutSeconds(5);
    EXPECT_EQ(PoolCapMaxHandlers(sectionWithoutMaxInFlight), 1024u);

    NKikimrConfig::TQueryServiceConfig section;
    section.MutableHttpGateway()->SetMaxInFlightCount(500);
    EXPECT_EQ(PoolCapMaxHandlers(section), 500u);
}

// T-CFG-3 (F-A-2 second mismatch, P1): the pool-cap pusher is sized from the effective
// MaxInFlightCount. Without a section the gateway runs with 2000 handlers (DefaultHttpGatewayConfig),
// but the pusher divides 1024 among the pools.
TEST(TKqpHttpGatewayConfigTest, PoolCapMaxHandlersFollowsEffectiveConfig) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-A-2");
    NKikimrConfig::TQueryServiceConfig noSection;
    EXPECT_EQ(PoolCapMaxHandlers(noSection), GetEffectiveHttpGatewayConfig(noSection).GetMaxInFlightCount());
    EXPECT_EQ(PoolCapMaxHandlers(noSection), 2000u);

    NKikimrConfig::TQueryServiceConfig section;
    section.MutableHttpGateway()->SetMaxInFlightCount(500);
    EXPECT_EQ(PoolCapMaxHandlers(section), 500u);
}

// T-TMO-1 (F-A-4 + F-B-2, P0): the gateway made from the KQP default config fails a stalled GET with
// curl code 28 ("too slow") and frees its budget: a follow-up request of the full memory budget is
// admitted and reaches the server. Today the default has no LowSpeed guard and no total timeout, so
// the callback never fires (scratch A StalledServerWithKqpDefaultTimeoutsNeverCompletes).
// Wall-clock is inherent (curl timer); the oracle is the curl code, elapsed time is not asserted.
TEST(TKqpHttpGatewayStallTest, DefaultConfigFailsStalledGet) {
    YDB_SKIP_KNOWN_BUG_GTEST("F-A-4");
    const TDuration guard = TDuration::Seconds(15);
    const auto config = ScaleForTest(DefaultHttpGatewayConfig(), 10);
    EXPECT_GT(config.GetLowSpeedTimeSeconds(), 0u) << "the KQP default config has no stall guard";
    EXPECT_EQ(config.GetRequestTimeoutSeconds(), 0u);

    // The gateway is declared first so that the server stops first: it closes stalled connections,
    // which completes whatever the gateway still has in flight when an assertion fails.
    TGatewayScope gateway(config);
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::StallForever());

    TBufferedCall first;
    gateway->Download(server.Url("/stalled"), {}, 0, 100, first.Callback());
    ASSERT_TRUE(first.Wait(guard)) << "the stalled GET was not completed within " << guard.ToString()
        << " (server saw " << server.RequestCount() << " request(s))";
    const auto result = first.Result();
    EXPECT_EQ(int(result.CurlCode), int(CURLE_OPERATION_TIMEDOUT)) << result.Issues;
    EXPECT_NE(result.Issues.find("too slow"), TString::npos) << result.Issues;

    gateway.Inspector().WaitValue("AllocatedMemory", 0);
    gateway.Inspector().WaitValue("InFlight", 0);

    TBufferedCall second;
    gateway->Download(server.Url("/stalled"), {}, 0, config.GetMaxSimulatenousDownloadsSize(), second.Callback());
    server.WaitForRequests(2);
    ASSERT_TRUE(second.Wait(guard)) << "the second stalled GET was not completed within " << guard.ToString();
    EXPECT_EQ(int(second.Result().CurlCode), int(CURLE_OPERATION_TIMEDOUT)) << second.Result().Issues;

    gateway.Inspector().AssertSettled();
    gateway.Close();
    EXPECT_EQ(first.Calls(), 1u);
    EXPECT_EQ(second.Calls(), 1u);
}

}  // namespace NKikimr::NKqp
