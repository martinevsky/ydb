// FQ transport test plan, target A, §4.2 (R2 timeouts and liveness): T-TMO-3..T-TMO-8.
//
// Curl timers are wall-clock (no clock seam in libcurl, plan §6.3 D-3): these tests use 1-2 s settings,
// the oracle is the curl code or a server-observed event, never an elapsed-time upper bound. A "did not
// happen within T" window is 2.5 x T and is always followed by a positive completion assertion.

#include "yql_http_default_retry_policy.h"
#include "yql_http_gateway_ut_common.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#if defined(_linux_)
#include <ydb/library/yql/providers/common/ut_helpers/transport/blackhole.h>
#endif

#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/size_literals.h>
#include <util/string/builder.h>

namespace NYql {

using namespace NTransportTest;
using namespace NHttpGatewayUt;

namespace {

// T-TMO-3/4: a stream paused by backpressure for 2.5 x the timeout must survive, then complete.
void PausedStreamSurvivesAndCompletes(const THttpGatewayConfig& config, TDuration window) {
    constexpr ui64 size = 64_MB;
    TLoopbackHttpServer server;
    server.SetDefault(TScriptedResponse::BigBody(size));
    TGatewayScope gateway(config);

    TStreamConsumer consumer;
    StartStream(gateway, server.Url(), consumer);
    server.WaitBlockedOnSend();
    const bool finishedWhilePaused = consumer.WaitFinish(window);
    UNIT_ASSERT_C(!finishedWhilePaused, "the paused stream was finished after " << consumer.Bytes()
        << " bytes: curl=" << int(consumer.FinishCode()) << " issues: " << consumer.FinishIssues());

    consumer.Release();
    UNIT_ASSERT_C(consumer.WaitFinish(TDuration::Seconds(30)), "the stream did not finish after release");
    UNIT_ASSERT_VALUES_EQUAL_C(int(consumer.FinishCode()), int(CURLE_OK), consumer.FinishIssues());
    UNIT_ASSERT_VALUES_EQUAL(consumer.FinishIssues(), "");
    UNIT_ASSERT_VALUES_EQUAL(consumer.Bytes(), size);
    Finish(gateway);
    UNIT_ASSERT_VALUES_EQUAL(consumer.Finishes(), 1);
}

} // namespace

Y_UNIT_TEST_SUITE(THttpGatewayTimeoutTest) {

    // T-TMO-3 (F-A-3, P1): a stream paused by consumer backpressure is not killed by the total request
    // timeout. Today CURLOPT_TIMEOUT counts the paused time: the stream ends with curl code 28 after 2 s.
    Y_UNIT_TEST(PausedStreamNotKilledByTotalTimeout) {
        YDB_SKIP_KNOWN_BUG("F-A-3");
        THttpGatewayConfig config;
        config.SetRequestTimeoutSeconds(2);
        config.SetBuffersSizePerStream(64_KB);
        PausedStreamSurvivesAndCompletes(config, TDuration::Seconds(5));
    }

    // T-TMO-4 (F-A-4 guard, P1, pin): with no total timeout, the LowSpeed stall guard does not kill a
    // paused stream either (the F-A-4 fix must not re-introduce F-A-3).
    Y_UNIT_TEST(PausedStreamSurvivesLowSpeedGuard) {
        THttpGatewayConfig config;
        config.SetRequestTimeoutSeconds(0);
        config.SetLowSpeedTimeSeconds(2);
        config.SetLowSpeedBytesLimit(1024);
        config.SetBuffersSizePerStream(64_KB);
        PausedStreamSurvivesAndCompletes(config, TDuration::Seconds(5));
    }

    // T-TMO-5 (F-A-3, P1, boundary pin): the total timeout still bounds a trickling buffered request
    // that the LowSpeed guard (1 byte/s) would never stop.
    Y_UNIT_TEST(TotalTimeoutBoundsTricklingBufferedRequest) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok(TString(100'000, 't')).NoRange().Trickle(1, TDuration::MilliSeconds(100)));
        THttpGatewayConfig config;
        config.SetRequestTimeoutSeconds(1);
        config.SetLowSpeedTimeSeconds(2);
        config.SetLowSpeedBytesLimit(1);
        TGatewayScope gateway(config);
        TBufferedCall call;
        const auto outcome = Download(gateway, server.Url(), {}, 0, call);
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OPERATION_TIMEDOUT), outcome.Issues);
        UNIT_ASSERT_C(outcome.Issues.Contains("timed out"), outcome.Issues);
        server.WaitConnectionClosedByPeer(0);
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
    }

#if defined(_linux_)
    // T-TMO-6 (P1): connect timeout against a listener whose accept queue is full (H12).
    Y_UNIT_TEST(ConnectTimeout) {
        TBlackholeListener blackhole;
        THttpGatewayConfig config;
        config.SetConnectionTimeoutSeconds(1);
        TGatewayScope gateway(config);
        TBufferedCall call;
        const auto outcome = Download(gateway, blackhole.Url(), {}, 0, call);
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OPERATION_TIMEDOUT), outcome.Issues);
        UNIT_ASSERT_C(outcome.Issues.Contains("onnect"), outcome.Issues);
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
    }
#endif

    // T-TMO-7 (P1): connection refused is reported as CURLE_COULDNT_CONNECT and counted per curl code.
    Y_UNIT_TEST(ConnectionRefused) {
        TGatewayScope gateway;
        TBufferedCall call;
        const auto outcome = Download(gateway, TStringBuilder() << "http://127.0.0.1:" << ClosedLoopbackPort() << "/x", {}, 0, call);
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_COULDNT_CONNECT), outcome.Issues);
        gateway.Inspector().WaitValue("method=GET/curl_code=7/count", 1);
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
    }

    // T-TMO-8 (P1): a truncated body (curl code 18) is retried by the FQ policy and the retry succeeds.
    // The callback is called once, with the full body of the second attempt.
    Y_UNIT_TEST(TruncatedBodyThenSuccessfulRetry) {
        const TString body(100, 'b');
        TLoopbackHttpServer server;
        server.Script("/t", {TScriptedResponse::Ok(body).NoRange().CloseMidBody(50), TScriptedResponse::Ok(body).NoRange()});
        TGatewayScope gateway;
        TBufferedCall call;
        gateway->Download(server.Url("/t"), {}, 0, 0, call.Callback(), {}, GetFqHTTPRetryPolicy());
        const auto outcome = call.Wait();
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OK), outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 200);
        UNIT_ASSERT_VALUES_EQUAL(outcome.Body, body);
        UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 2);
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("method=GET/curl_code=18/count"), 1);
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
    }
}

} // namespace NYql
