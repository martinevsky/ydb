// FQ transport test plan, target A, §4.7 (R7 config wiring) and §4.9 (DNS): T-CFG-4, T-CFG-5, T-RTY-6,
// T-DNS-1, T-DNS-2.
//
// DNS tests use seam S11 (NHttpGatewayTest::TGatewayTestOptions.DnsResolve + RefreshDnsNow): no real DNS,
// no timer-based refresh.

#include "yql_http_default_retry_policy.h"
#include "yql_http_gateway_test_hooks.h"
#include "yql_http_gateway_ut_common.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/log_capture.h>

#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/network/address.h>
#include <util/network/socket.h>
#include <util/string/builder.h>

#include <atomic>

namespace NYql {

using namespace NTransportTest;
using namespace NHttpGatewayUt;

namespace {

THttpGatewayConfig WithMaxInFlight(ui32 value) {
    THttpGatewayConfig config;
    config.SetMaxInFlightCount(value);
    return config;
}

NAddr::TOpaqueAddr Ipv4Addr(const TString& ip, ui16 port) {
    NAddr::TIPv4Addr ipv4(TIpAddress(IpFromString(ip.c_str()), port));
    return NAddr::TOpaqueAddr(&ipv4);
}

THttpGatewayConfig WithDnsRecord(const TString& host, ui16 port, const TString& expectedIp) {
    THttpGatewayConfig config;
    auto* dns = config.MutableDnsResolverConfig();
    dns->SetRefreshMs(3'600'000); // the refresh thread never fires within a test; RefreshDnsNow does
    auto* record = dns->AddExplicitDNSRecord();
    record->SetAddress(host);
    record->SetPort(port);
    if (expectedIp) {
        record->SetExpectedIP(expectedIp);
    }
    return config;
}

TGatewayScope::TFactory WithResolver(NHttpGatewayTest::TGatewayTestOptions options) {
    return [options](const THttpGatewayConfig* config, ::NMonitoring::TDynamicCounterPtr counters) {
        return NHttpGatewayTest::MakeHttpGatewayForTest(config, std::move(counters), options);
    };
}

} // namespace

Y_UNIT_TEST_SUITE(THttpGatewayConfigTest) {

    // T-CFG-4 (F-A-2, P1; needs seam S4): the singleton is kept by design, but reusing it with another
    // config is observable: one WARN naming both configs (once per distinct ignored config),
    // GetEffectiveConfig() reports the config in force, and an identical config does not warn.
    Y_UNIT_TEST(SingletonReuseIsObservable) {
        TLogCapture log;
        const auto cfgA = WithMaxInFlight(7);
        const auto cfgB = WithMaxInFlight(3);
        const auto cfgA2 = WithMaxInFlight(7);
        TGatewayScope gateway(cfgA);
        auto c2 = MakeIntrusive<::NMonitoring::TDynamicCounters>();
        auto c3 = MakeIntrusive<::NMonitoring::TDynamicCounters>();
        auto g2 = IHTTPGateway::Make(&cfgB, c2);
        auto g3 = IHTTPGateway::Make(&cfgA2, c3);
        auto g4 = IHTTPGateway::Make(nullptr, c3); // no config: nothing to ignore, no warning
        auto g5 = IHTTPGateway::Make(&cfgB, c3);   // already reported: no second warning
        UNIT_ASSERT_EQUAL(g2.get(), gateway.Gateway().get());
        UNIT_ASSERT_EQUAL(g3.get(), gateway.Gateway().get());
        UNIT_ASSERT_EQUAL(g4.get(), gateway.Gateway().get());
        UNIT_ASSERT_VALUES_EQUAL(gateway->GetEffectiveConfig().GetMaxInFlightCount(), 7);
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("MaxInFlight"), 7);
        UNIT_ASSERT(!TCountersInspector(c2).Has("MaxInFlight")); // the second counters are ignored too
        g2.reset();
        g3.reset();
        g4.reset();
        g5.reset();
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL_C(log.CountLines("already created.*MaxInFlightCount: 7.*MaxInFlightCount: 3"), 1, log.Text());
        UNIT_ASSERT_VALUES_EQUAL_C(log.CountLines("already created"), 1, log.Text());
    }

    // T-CFG-5 (F-A-2, P2, pin): once the last reference is gone, the next Make builds a new gateway with
    // the new config (the test-isolation assumption behind TGatewayScope).
    Y_UNIT_TEST(SingletonLifecycle) {
        {
            TGatewayScope first(WithMaxInFlight(7));
            UNIT_ASSERT_VALUES_EQUAL(first->GetEffectiveConfig().GetMaxInFlightCount(), 7);
            Finish(first);
        }
        TGatewayScope second(WithMaxInFlight(3));
        UNIT_ASSERT_VALUES_EQUAL(second.Inspector().Get("MaxInFlight"), 3);
        UNIT_ASSERT_VALUES_EQUAL(second->GetEffectiveConfig().GetMaxInFlightCount(), 3);
        Finish(second);
    }

    // T-RTY-6 (F-A-9, P1): THttpGatewayConfig.MaxRetries takes effect. Today the field is never read: the
    // FQ policy keeps retrying a 503 until its 5-minute budget. Seam S13 is not implemented, so the test
    // uses the policy the production call sites use (GetFqHTTPRetryPolicy()).
    Y_UNIT_TEST(MaxRetriesTakesEffect) {
        YDB_SKIP_KNOWN_BUG("F-A-9");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::WithStatus(503));
        THttpGatewayConfig config;
        config.SetMaxRetries(2);
        TGatewayScope gateway(config);
        TBufferedCall call;
        gateway->Download(server.Url(), {}, 0, 0, call.Callback(), {}, GetFqHTTPRetryPolicy());
        WaitUntil([&]() {
            return call.Calls() > 0 || server.RequestCount() > 3;
        }, TDuration::Seconds(15), "the retries stop or exceed MaxRetries");
        UNIT_ASSERT_VALUES_EQUAL_C(server.RequestCount(), 3, "MaxRetries=2 was ignored");
        UNIT_ASSERT_VALUES_EQUAL(call.Wait().HttpCode, 503);
        Finish(gateway);
    }
}

Y_UNIT_TEST_SUITE(THttpGatewayDnsTest) {

    // T-DNS-1 (P1, pin): an explicit DNS record routes requests (CURLOPT_RESOLVE wiring). The injected
    // resolver fails for the host, so the configured ExpectedIP is what curl gets.
    Y_UNIT_TEST(ExplicitDnsRecordRoutesRequests) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("routed").NoRange());
        std::atomic<ui32> resolves = 0;
        NHttpGatewayTest::TGatewayTestOptions options;
        options.DnsResolve = [&resolves](const TString&, ui16) -> std::vector<NAddr::TOpaqueAddr> {
            ++resolves;
            throw TNetworkResolutionError(EAI_NONAME);
        };
        TGatewayScope gateway(WithDnsRecord("s3.fake.invalid", server.Port(), "127.0.0.1"), WithResolver(options));
        UNIT_ASSERT_GE(resolves.load(), 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("subsystem=dns_gateway/ResolutionErrors"), resolves.load());

        const TString authority = TStringBuilder() << "s3.fake.invalid:" << server.Port();
        const auto outcome = Download(gateway, TStringBuilder() << "http://" << authority << "/o");
        UNIT_ASSERT_VALUES_EQUAL_C(outcome.HttpCode, 200, outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(outcome.Body, "routed");
        UNIT_ASSERT_VALUES_EQUAL(server.Requests().at(0).Header("Host").GetOrElse(""), authority);
        Finish(gateway);
    }

    // T-DNS-2 (P2, pin): a DNS refresh changes the route of new requests without disturbing a request
    // in flight. Two servers share a port on 127.0.0.1 and 127.0.0.2; the resolver switches between them.
    Y_UNIT_TEST(RefreshChangesRouteWithoutDisturbingInFlight) {
        TLoopbackHttpServer first;
        TLoopbackHttpServer second({.BindAddress = "127.0.0.2", .Port = first.Port()});
        first.SetDefault(TScriptedResponse::Ok("first").NoRange());
        second.SetDefault(TScriptedResponse::Ok("second").NoRange());
        first.Gate("/o");
        const ui16 port = first.Port();

        std::atomic<bool> switched = false;
        NHttpGatewayTest::TGatewayTestOptions options;
        options.DnsResolve = [&switched](const TString&, ui16 port) {
            return std::vector<NAddr::TOpaqueAddr>{Ipv4Addr(switched ? "127.0.0.2" : "127.0.0.1", port)};
        };
        TGatewayScope gateway(WithDnsRecord("s3.fake.invalid", port, {}), WithResolver(options));
        const TString url = TStringBuilder() << "http://s3.fake.invalid:" << port << "/o";

        TBufferedCall held, routed;
        gateway->Download(url, {}, 0, 0, held.Callback());
        first.WaitForRequests(1);
        switched = true;
        NHttpGatewayTest::RefreshDnsNow(gateway.Gateway());
        gateway->Download(url, {}, 0, 0, routed.Callback());
        const auto outcome = routed.Wait();
        UNIT_ASSERT_VALUES_EQUAL_C(outcome.Body, "second", outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(held.Calls(), 0);

        first.Release("/o");
        const auto heldOutcome = held.Wait();
        UNIT_ASSERT_VALUES_EQUAL_C(heldOutcome.Body, "first", heldOutcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(first.RequestCount(), 1);
        UNIT_ASSERT_VALUES_EQUAL(second.RequestCount(), 1);
        Finish(gateway);
    }
}

} // namespace NYql
