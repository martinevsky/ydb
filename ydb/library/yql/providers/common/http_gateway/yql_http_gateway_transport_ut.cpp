// Transport contract tests for THTTPMultiGateway (FQ transport test plan, target A) and the
// validation suite of the shared transport harness (ydb/library/yql/providers/common/ut_helpers/transport).
//
// Contract tests that fail on today's code are guarded by YDB_SKIP_KNOWN_BUG(<finding id>); run them with
//   --test-env=YDB_TRANSPORT_RUN_KNOWN_BUGS=1
// Every gateway test ends with the T-OBS-3 post-condition (gauges settle to 0) and TGatewayScope::Close().

#include "yql_http_gateway_ut_common.h"

#include <ydb/library/yql/providers/common/http_gateway/mock/yql_http_scripted_gateway.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/counters_inspector.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/test_pki.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/gateway_scope.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/log_capture.h>
#if defined(_linux_)
#include <ydb/library/yql/providers/common/ut_helpers/transport/blackhole.h>
#endif

#include <yql/essentials/providers/common/proto/gateways_config.pb.h>
#include <yql/essentials/utils/log/log.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/size_literals.h>
#include <util/string/builder.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace NYql {

namespace {

using namespace NTransportTest;
using namespace NHttpGatewayUt;

THttpGatewayConfig WithCaFile(const TTestPki& pki) {
    THttpGatewayConfig config;
    config.SetCaFile(pki.CaFile());
    return config;
}

// A TLS download that must be rejected before any HTTP byte is decrypted by the server.
// Chain failures abort the handshake (server: HandshakesFailed); a name mismatch (the URL host 127.0.0.1
// against the leaf's SAN) is detected by curl after the handshake completed, so the server sees a finished
// handshake and then a close.
void AssertRejectedBeforeHttp(TGatewayScope& gateway, TLoopbackHttpServer& server, const TString& secret,
    bool expectHandshakeFailure = true)
{
    TBufferedCall call;
    const auto outcome = Download(gateway, server.Url("/obj"), IHTTPGateway::MakeYcHeaders("r1", secret), 10, call);
    UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_PEER_FAILED_VERIFICATION), outcome.Issues);
    server.WaitConnectionsDone(1, GUARD); // causal: the server's view of the connection is final
    if (expectHandshakeFailure) {
        UNIT_ASSERT_GE(server.HandshakesFailed(), 1);
    }
    UNIT_ASSERT_VALUES_EQUAL(server.HttpRequestsDecrypted(), 0);
    UNIT_ASSERT(!server.AnyRequestContains(secret));
    Finish(gateway);
    UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
}

} // namespace

Y_UNIT_TEST_SUITE(THttpGatewayTransportTest) {

    // T-TLS-1 (F-A-1, P0): the default config must reject a self-signed server and never send the token.
    Y_UNIT_TEST(SelfSignedServerRejectedByDefault) {
        YDB_SKIP_KNOWN_BUG("F-A-1");
        TTestPki pki;
        TLoopbackHttpServer server({.Tls = pki.SelfSignedLeaf()});
        server.SetDefault(TScriptedResponse::Ok("abc"));
        TGatewayScope gateway;
        AssertRejectedBeforeHttp(gateway, server, "SECRET-T");
    }

    // T-TLS-2 (F-A-1, P0): with a trusted CA configured, a certificate for another host name is rejected.
    Y_UNIT_TEST(HostnameMismatchRejected) {
        TTestPki pki;
        TLoopbackHttpServer server({.Tls = pki.WrongHostLeaf()});
        server.SetDefault(TScriptedResponse::Ok("abc"));
        TGatewayScope gateway(WithCaFile(pki));
        AssertRejectedBeforeHttp(gateway, server, "SECRET-T", /* expectHandshakeFailure */ false);
    }

    // T-TLS-3 (F-A-1, P0): positive guard. With the CA configured, GET/PUT/DELETE work over TLS.
    Y_UNIT_TEST(TrustedCaGetPutDeleteOverTls) {
        TTestPki pki;
        TLoopbackHttpServer server({.Tls = pki.Leaf()});
        server.SetHandler([](const TReceivedRequest& request) {
            if (request.Method == "PUT") {
                return TScriptedResponse::Ok();
            }
            if (request.Method == "DELETE") {
                return TScriptedResponse::WithStatus(204);
            }
            return TScriptedResponse::Ok("abc").NoRange();
        });
        TGatewayScope gateway(WithCaFile(pki));
        const TString url = server.Url("/obj");

        const auto get = Download(gateway, url, IHTTPGateway::MakeYcHeaders("r1"), 3);
        UNIT_ASSERT_VALUES_EQUAL_C(int(get.CurlCode), int(CURLE_OK), get.Issues);
        UNIT_ASSERT_VALUES_EQUAL(get.HttpCode, 200);
        UNIT_ASSERT_VALUES_EQUAL(get.Body, "abc");

        TBufferedCall put;
        gateway->Upload(url, IHTTPGateway::MakeYcHeaders("r2"), "abc", put.Callback(), true);
        const auto putOutcome = put.Wait();
        UNIT_ASSERT_VALUES_EQUAL_C(int(putOutcome.CurlCode), int(CURLE_OK), putOutcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(putOutcome.HttpCode, 200);

        TBufferedCall del;
        gateway->Delete(url, IHTTPGateway::MakeYcHeaders("r3"), del.Callback());
        const auto delOutcome = del.Wait();
        UNIT_ASSERT_VALUES_EQUAL_C(int(delOutcome.CurlCode), int(CURLE_OK), delOutcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(delOutcome.HttpCode, 204);

        const auto requests = server.Requests();
        UNIT_ASSERT_VALUES_EQUAL(requests.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(requests[1].Method, "PUT");
        UNIT_ASSERT_VALUES_EQUAL(requests[1].Body, "abc");
        UNIT_ASSERT_VALUES_EQUAL(requests[1].Header("Content-Length").GetOrElse(""), "3");
        UNIT_ASSERT_VALUES_EQUAL(requests[2].Method, "DELETE");
        UNIT_ASSERT_VALUES_EQUAL(server.HandshakesOk(), 3);
        UNIT_ASSERT_VALUES_EQUAL(server.HttpRequestsDecrypted(), 3);
        Finish(gateway);
    }

    // T-TLS-4 (F-A-1, P1): the explicit opt-out works and is logged once at gateway creation.
    // Known bug: S6 adds the opt-out (VerifyPeer=false) but deliberately no log line (behaviour-preserving).
    Y_UNIT_TEST(ExplicitOptOutIsLoud) {
        YDB_SKIP_KNOWN_BUG("F-A-1");
        TLogCapture log;
        TTestPki pki;
        TLoopbackHttpServer server({.Tls = pki.SelfSignedLeaf()});
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        THttpGatewayConfig config;
        config.SetVerifyPeer(false);
        TGatewayScope gateway(config);
        for (int i = 0; i < 2; ++i) {
            const auto outcome = Download(gateway, server.Url("/obj"), IHTTPGateway::MakeYcHeaders("r1"), 3);
            UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OK), outcome.Issues);
            UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 200);
        }
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(log.CountLines("TLS (peer )?verification (is )?disabled"), 1);
    }

    // T-TLS-5 (F-A-1, P1): an expired leaf and a leaf from an untrusted CA are both rejected.
    Y_UNIT_TEST(ExpiredLeafAndUntrustedChainRejected) {
        TTestPki pki;
        {
            TLoopbackHttpServer expired({.Tls = pki.ExpiredLeaf()});
            expired.SetDefault(TScriptedResponse::Ok("abc"));
            TGatewayScope gateway(WithCaFile(pki));
            AssertRejectedBeforeHttp(gateway, expired, "SECRET-T");
        }
        {
            TLoopbackHttpServer untrusted({.Tls = pki.UntrustedLeaf()});
            untrusted.SetDefault(TScriptedResponse::Ok("abc"));
            TGatewayScope gateway(WithCaFile(pki));
            AssertRejectedBeforeHttp(gateway, untrusted, "SECRET-T");
        }
    }

    // T-TLS-11 (DG, pin): today an IAM subject token is sent over plain http://.
    Y_UNIT_TEST(SubjectTokenOverPlainHttpIsSentToday) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        TGatewayScope gateway;
        const auto outcome = Download(gateway, server.Url(), IHTTPGateway::MakeYcHeaders("r", "SECRET-T"), 3);
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OK), outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 200);
        const auto requests = server.Requests();
        UNIT_ASSERT_VALUES_EQUAL(requests.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(requests[0].Header("X-YaCloud-SubjectToken").GetOrElse(""), "SECRET-T");
        Finish(gateway);
    }

    // T-TLS-11 (DG, proposed contract): credentials are refused over plaintext unless explicitly allowed.
    Y_UNIT_TEST(SubjectTokenOverPlainHttpRefused) {
        YDB_SKIP_KNOWN_BUG("DG-T-TLS-11");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        TGatewayScope gateway;
        const auto outcome = Download(gateway, server.Url(), IHTTPGateway::MakeYcHeaders("r", "SECRET-T"), 3);
        UNIT_ASSERT_C(outcome.Issues.Contains("plaintext"), "expected a 'credentials over plaintext' issue, got: " << outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 0);
        Finish(gateway);
    }

    // T-TLS-12 (R1 guard, pin): redirects are not followed, so the token is not forwarded cross-host.
    Y_UNIT_TEST(RedirectNotFollowedTokenNotForwarded) {
        TLoopbackHttpServer target;
        target.SetDefault(TScriptedResponse::Ok("stolen"));
        TLoopbackHttpServer origin;
        origin.SetDefault(TScriptedResponse::Redirect(301, target.Url("/steal")));
        TGatewayScope gateway;
        const auto outcome = Download(gateway, origin.Url(), IHTTPGateway::MakeYcHeaders("r", "SECRET-T"));
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OK), outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 301);
        UNIT_ASSERT_VALUES_EQUAL(origin.RequestCount(), 1);
        UNIT_ASSERT_VALUES_EQUAL(target.RequestCount(), 0);
        Finish(gateway);
    }

    // T-TLS-13 (N-1, P1): only http(s) is allowed; file:// and gopher:// are rejected by the gateway.
    Y_UNIT_TEST(NonHttpSchemesRejected) {
        YDB_SKIP_KNOWN_BUG("N-1");
        TLoopbackHttpServer server;
        THttpGatewayConfig config;
        config.SetRequestTimeoutSeconds(10);
        TGatewayScope gateway(config);

        const auto file = Download(gateway, "file:///proc/self/status");
        const auto gopher = Download(gateway, TStringBuilder() << "gopher://127.0.0.1:" << server.Port() << "/x");
        const TString observed = TStringBuilder()
            << "file:// curl=" << int(file.CurlCode) << " read " << file.Body.size() << " bytes; "
            << "gopher:// curl=" << int(gopher.CurlCode) << " server connections=" << server.ConnectionsAccepted()
            << " non-HTTP requests=" << server.RequestCount();
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL_C(int(file.CurlCode), int(CURLE_UNSUPPORTED_PROTOCOL), observed);
        UNIT_ASSERT_VALUES_EQUAL_C(file.Body.size(), 0, observed);
        UNIT_ASSERT_VALUES_EQUAL_C(int(gopher.CurlCode), int(CURLE_UNSUPPORTED_PROTOCOL), observed);
        UNIT_ASSERT_VALUES_EQUAL_C(server.ConnectionsAccepted(), 0, observed);
    }
}

// Validation of the transport harness components that the THttpGatewayTransportTest cases above do not
// exercise. Kept small; each test documents the harness contract other targets build on.
Y_UNIT_TEST_SUITE(TTransportHarnessTest) {

    // H1: per-attempt scripts, Range handling, header echo.
    Y_UNIT_TEST(LoopbackScriptsAttemptsAndRange) {
        TLoopbackHttpServer server;
        server.Script("/a", {TScriptedResponse::WithStatus(503, "busy"), TScriptedResponse::Ok("0123456789")});
        server.Script("/echo", {TScriptedResponse::EchoRequestHeaders()});
        TGatewayScope gateway;

        const auto first = Download(gateway, server.Url("/a"));
        UNIT_ASSERT_VALUES_EQUAL(first.HttpCode, 503);
        UNIT_ASSERT_VALUES_EQUAL(first.Body, "busy");

        TBufferedCall ranged;
        gateway->Download(server.Url("/a"), {}, 2, 3, ranged.Callback());
        const auto second = ranged.Wait();
        UNIT_ASSERT_VALUES_EQUAL(second.HttpCode, 206);
        UNIT_ASSERT_VALUES_EQUAL(second.Body, "234");

        const auto echo = Download(gateway, server.Url("/echo?x=1"), IHTTPGateway::MakeYcHeaders("req-42"));
        UNIT_ASSERT_VALUES_EQUAL(echo.HttpCode, 200);
        UNIT_ASSERT_C(echo.Body.Contains("X-Request-ID: req-42") || echo.Body.Contains("X-Request-ID:req-42"), echo.Body);

        const auto requests = server.Requests();
        UNIT_ASSERT_VALUES_EQUAL(requests.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(requests[0].Attempt, 0);
        UNIT_ASSERT_VALUES_EQUAL(requests[1].Attempt, 1);
        UNIT_ASSERT_VALUES_EQUAL(requests[1].Header("Range").GetOrElse(""), "bytes=2-4");
        UNIT_ASSERT_VALUES_EQUAL(requests[2].Path, "/echo");
        UNIT_ASSERT_VALUES_EQUAL(requests[2].Query, "x=1");
        Finish(gateway);
    }

    // H1: a gate holds the response until Release; the request is observable before the answer.
    Y_UNIT_TEST(LoopbackGateHoldsUntilRelease) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("done").NoRange());
        server.Gate("/g");
        TGatewayScope gateway;
        TBufferedCall call;
        gateway->Download(server.Url("/g"), {}, 0, 0, call.Callback());
        server.WaitForRequests(1);
        UNIT_ASSERT_VALUES_EQUAL(server.Concurrent(), 1);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 0); // causal: the server has not answered yet
        server.Release("/g");
        const auto outcome = call.Wait();
        UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 200);
        UNIT_ASSERT_VALUES_EQUAL(outcome.Body, "done");
        UNIT_ASSERT_VALUES_EQUAL(server.MaxConcurrent(), 1);
        Finish(gateway);
    }

    // H1: CloseMidBody makes curl report a partial file; Trickle delivers the whole body in pieces.
    Y_UNIT_TEST(LoopbackCloseMidBodyAndTrickle) {
        TLoopbackHttpServer server;
        server.Script("/cut", {TScriptedResponse::Ok("0123456789").CloseMidBody(4)});
        server.Script("/slow", {TScriptedResponse::Ok("abcdef").Trickle(2, TDuration::MilliSeconds(5))});
        TGatewayScope gateway;
        const auto cut = Download(gateway, server.Url("/cut"));
        UNIT_ASSERT_VALUES_EQUAL_C(int(cut.CurlCode), int(CURLE_PARTIAL_FILE), cut.Issues);
        const auto slow = Download(gateway, server.Url("/slow"));
        UNIT_ASSERT_VALUES_EQUAL(slow.HttpCode, 200);
        UNIT_ASSERT_VALUES_EQUAL(slow.Body, "abcdef");
        Finish(gateway);
    }

    // H1: a stalled request is reported as closed by the peer once curl gives up.
    Y_UNIT_TEST(LoopbackStallSeesPeerClose) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::StallForever());
        THttpGatewayConfig config;
        config.SetRequestTimeoutSeconds(1);
        TGatewayScope gateway(config);
        const auto outcome = Download(gateway, server.Url());
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OPERATION_TIMEDOUT), outcome.Issues);
        server.WaitConnectionClosedByPeer(0);
        UNIT_ASSERT(server.Requests()[0].ConnectionClosedByPeer);
        Finish(gateway);
    }

    // H1 BigBody + BlockedOnSend, H6 counters: a paused stream fills the socket buffers; cancel settles.
    Y_UNIT_TEST(LoopbackBigBodyBlocksOnSend) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::BigBody(256_MB));
        THttpGatewayConfig config;
        config.SetBuffersSizePerStream(64_KB);
        TGatewayScope gateway(config);
        std::mutex mutex;
        TVector<IHTTPGateway::TCountedContent> held;
        TBufferedCall finished;
        auto finishCallback = finished.Callback();
        auto cancel = gateway->Download(server.Url(), {}, 0, 0,
            [](CURLcode, long) {},
            [&](IHTTPGateway::TCountedContent&& part) {
                std::lock_guard lock(mutex);
                held.push_back(std::move(part));
            },
            [finishCallback](CURLcode code, TIssues issues) {
                finishCallback(IHTTPGateway::TResult(code, issues));
            },
            nullptr);
        server.WaitBlockedOnSend();
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("InFlightStreams"), 1);
        cancel(TIssue("stop"));
        const auto outcome = finished.Wait();
        UNIT_ASSERT_C(outcome.Issues.Contains("stop"), outcome.Issues);
        {
            std::lock_guard lock(mutex);
            held.clear();
        }
        server.WaitConnectionClosedByPeer(0);
        Finish(gateway);
    }

    // H6: per-method result counters by label path; Snapshot/Diff.
    Y_UNIT_TEST(CountersInspectorPaths) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        TGatewayScope gateway;
        const auto before = gateway.Inspector().Snapshot();
        const auto outcome = Download(gateway, server.Url());
        UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 200);
        gateway.Inspector().WaitValue("method=GET/code=200/count", 1);
        UNIT_ASSERT(!gateway.Inspector().Has("method=GET/code=404/count"));
        const auto diff = TCountersInspector::Diff(before, gateway.Inspector().Snapshot());
        UNIT_ASSERT_VALUES_EQUAL_C(diff.Value("Requests", 0), 1, TCountersInspector::ToString(diff));
        Finish(gateway);
    }

    // H7: the YQL log is captured; AssertNoSecret throws on a leak.
    Y_UNIT_TEST(LogCaptureAndNoSecret) {
        TLogCapture log;
        TGatewayScope gateway;
        gateway->UpdatePoolCaps({});
        Finish(gateway);
        UNIT_ASSERT_C(log.Contains("HTTPGateway UpdatePoolCaps"), log.Text());
        log.AssertNoSecret("SECRET-T");
        YQL_LOG(INFO) << "token=SECRET-T";
        UNIT_ASSERT_EXCEPTION(log.AssertNoSecret("SECRET-T"), yexception);
    }

    // H11: a second scope while the first is alive would share the singleton and is refused.
    Y_UNIT_TEST(GatewayScopeRefusesInheritedSingleton) {
        TGatewayScope first;
        UNIT_ASSERT_EXCEPTION(TGatewayScope(), yexception);
        first.Close();
        TGatewayScope second;
        second.Close();
    }

#if defined(_linux_)
    // H12: connect to a saturated accept queue hangs until curl's connect timeout.
    Y_UNIT_TEST(BlackholeTriggersConnectTimeout) {
        TBlackholeListener blackhole;
        THttpGatewayConfig config;
        config.SetConnectionTimeoutSeconds(1);
        TGatewayScope gateway(config);
        const auto outcome = Download(gateway, blackhole.Url());
        UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OPERATION_TIMEDOUT), outcome.Issues);
        Finish(gateway);
    }
#endif

    // H9: buffered calls complete inline or later from another thread; streams mirror cancel semantics.
    Y_UNIT_TEST(ScriptedGatewayBufferedAndStream) {
        auto gateway = TScriptedHttpGateway::Make();
        gateway->SetBufferedScript([](const TScriptedHttpGateway::TCall& call) -> std::optional<IHTTPGateway::TResult> {
            if (call.Method == TScriptedHttpGateway::EMethod::Delete) {
                return IHTTPGateway::TResult(IHTTPGateway::TContent("", 204));
            }
            return std::nullopt;
        });
        TBufferedCall deleted;
        gateway->Delete("http://s3/b/k", {}, deleted.Callback());
        UNIT_ASSERT_VALUES_EQUAL(deleted.Calls(), 1);
        UNIT_ASSERT_VALUES_EQUAL(deleted.Wait().HttpCode, 204);

        TBufferedCall put;
        gateway->Upload("http://s3/b/k", IHTTPGateway::MakeYcHeaders("r"), "body", put.Callback(), true);
        UNIT_ASSERT_VALUES_EQUAL(gateway->PendingCalls().size(), 1);
        const ui64 putId = gateway->PendingCalls()[0];
        std::thread completer([&]() {
            gateway->Complete(putId, IHTTPGateway::TResult(IHTTPGateway::TContent("", 200)));
        });
        completer.join();
        UNIT_ASSERT_VALUES_EQUAL(put.Wait().HttpCode, 200);

        std::atomic<ui32> finishes = 0;
        auto cancel = gateway->Download("http://s3/b/k", {}, 0, 0,
            [](CURLcode, long) {},
            [](IHTTPGateway::TCountedContent&&) {},
            [&](CURLcode, TIssues) {
                ++finishes;
            },
            nullptr);
        auto stream = gateway->Streams().back();
        UNIT_ASSERT(stream->Start(CURLE_OK, 200));
        UNIT_ASSERT(stream->Data("abc"));
        cancel(TIssue("cancelled"));
        UNIT_ASSERT(!stream->Finish(CURLE_OK));
        UNIT_ASSERT_VALUES_EQUAL(finishes.load(), 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway->Cancels().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway->BufferedStreamBytes(), 0);

        gateway->UpdatePoolCaps({{NDq::TWorkScope{"db", "pool"}, 3}});
        UNIT_ASSERT_VALUES_EQUAL(gateway->PoolCapsUpdates().size(), 1);

        const auto calls = gateway->Calls();
        UNIT_ASSERT_VALUES_EQUAL(calls.size(), 3);
        UNIT_ASSERT(calls[1].Method == TScriptedHttpGateway::EMethod::Put);
        UNIT_ASSERT_VALUES_EQUAL(calls[1].Body, "body");
        UNIT_ASSERT(calls[1].HasHeader("X-Request-ID:"));
        UNIT_ASSERT(calls[2].Method == TScriptedHttpGateway::EMethod::GetStream);
    }
}

} // namespace NYql
