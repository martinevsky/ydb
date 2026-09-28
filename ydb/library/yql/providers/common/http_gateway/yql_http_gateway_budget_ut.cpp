// FQ transport test plan, target A, §4.3 (R3 resource budget, admission, backpressure), T-CFG-6 and T-OBS-1.
//
// Sync points are server arrivals (H1), the gateway gauges (AwaitQueue, PerPool*) and PerformCycles:
// "the dispatcher had its chance and did not dispatch" = the gauge shows the request queued and the
// perform loop has run 3 more cycles. No sleeps.

#include "yql_http_gateway_ut_common.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>

#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/algorithm.h>
#include <util/generic/size_literals.h>
#include <util/string/builder.h>

namespace NYql {

using namespace NTransportTest;
using namespace NHttpGatewayUt;

namespace {

THttpGatewayConfig WithBudget(ui64 bytes) {
    THttpGatewayConfig config;
    config.SetMaxSimulatenousDownloadsSize(bytes);
    return config;
}

IHttpRequestContext::TPtr Pool(const TString& name) {
    return MakeIntrusive<TDefaultHttpRequestContext>(NDq::TWorkScope{"db", name});
}

TString PoolCounter(const TString& pool, const TString& name) {
    return TStringBuilder() << "db=db/pool=" << pool << "/" << name;
}

TVector<TString> Paths(const TLoopbackHttpServer& server) {
    TVector<TString> paths;
    for (const auto& request : server.Requests()) {
        paths.push_back(request.Path);
    }
    return paths;
}

TString Join(const TVector<TString>& paths) {
    TStringBuilder result;
    result << "[";
    for (size_t i = 0; i < paths.size(); ++i) {
        result << (i ? " " : "") << paths[i];
    }
    return result << "]";
}

void DownloadAsync(const TGatewayScope& gateway, const TLoopbackHttpServer& server, const TString& path,
    size_t sizeLimit, const TBufferedCall& call, IHttpRequestContext::TPtr context = nullptr)
{
    gateway->Download(server.Url(path), {}, 0, sizeLimit, call.Callback(), {},
        IHTTPGateway::TRetryPolicy::GetNoRetryPolicy(), std::move(context));
}

// T-BUD-5: the most a stream can receive in one perform cycle, i.e. past the pause check. libcurl 8.x
// sendrecv_dl() (lib/transfer.c) reads in a `do { ... } while(maxloops--)` loop with maxloops = 10, so one
// curl_multi_perform call delivers up to 11 reads of CURLOPT_BUFFERSIZE (DownloadBufferBytesLimit).
// Measured at 32d7a8ec18: threshold 64 KB, buffer 16 KB -> 180224 bytes held (11 parts of 16 KB, all read in
// the first cycle). The plan's "threshold + one write callback" is refuted.
ui64 StreamOvershootBound(ui64 bufferSize) {
    return 11 * bufferSize;
}

void AssertOk(const TOutcome& outcome) {
    UNIT_ASSERT_VALUES_EQUAL_C(int(outcome.CurlCode), int(CURLE_OK), outcome.Issues);
    UNIT_ASSERT_VALUES_EQUAL_C(outcome.HttpCode, 200, outcome.Issues);
}

} // namespace

Y_UNIT_TEST_SUITE(THttpGatewayBudgetTest) {

    // T-BUD-1 (P1, pin): the download budget is released on completion and the waiter is admitted.
    Y_UNIT_TEST(BudgetReleasedOnCompletionAdmitsWaiter) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        server.Gate("/a");
        TGatewayScope gateway(WithBudget(100));

        TBufferedCall a, b;
        DownloadAsync(gateway, server, "/a", 60, a);
        server.WaitForRequests(1);
        DownloadAsync(gateway, server, "/b", 60, b);
        gateway.Inspector().WaitValue("AwaitQueue", 1);
        WaitPerformCycles(gateway);
        UNIT_ASSERT_VALUES_EQUAL(Join(Paths(server)), "[/a]");
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("AllocatedMemory"), 60);

        server.Release("/a");
        AssertOk(a.Wait());
        AssertOk(b.Wait());
        UNIT_ASSERT_VALUES_EQUAL(Join(Paths(server)), "[/a /b]");
        Finish(gateway); // AllocatedMemory == 0
    }

    // T-BUD-2 (F-A-7, P1): within one pool a queued request is not overtaken by later, smaller ones.
    // Today DispatchAwaiting takes the first queued request that fits, so /c overtakes /b.
    Y_UNIT_TEST(NoOvertakingWithinOnePool) {
        YDB_SKIP_KNOWN_BUG("F-A-7");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        server.Gate("/a");
        TGatewayScope gateway(WithBudget(100));

        TBufferedCall a, b, c;
        DownloadAsync(gateway, server, "/a", 60, a);
        server.WaitForRequests(1);
        DownloadAsync(gateway, server, "/b", 60, b);
        gateway.Inspector().WaitValue("AwaitQueue", 1);
        DownloadAsync(gateway, server, "/c", 30, c);
        WaitPerformCycles(gateway);
        UNIT_ASSERT_VALUES_EQUAL_C(Join(Paths(server)), "[/a]", "a later, smaller request overtook the queued /b");

        server.Release("/a");
        for (const auto* call : {&a, &b, &c}) {
            AssertOk(call->Wait());
        }
        // /b and /c fit the budget together and are admitted in one cycle: their arrival order at the
        // server is a race, so compare them as a set.
        auto paths = Paths(server);
        UNIT_ASSERT_VALUES_EQUAL(paths.size(), 3u);
        Sort(paths.begin() + 1, paths.end());
        UNIT_ASSERT_VALUES_EQUAL(Join(paths), "[/a /b /c]");
        Finish(gateway);
    }

    // T-BUD-3 (F-A-7 intended part, P1, pin): a blocked head in one pool does not block another pool.
    Y_UNIT_TEST(BlockedPoolDoesNotBlockOtherPool) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        server.Gate("/p1a");
        TGatewayScope gateway(WithBudget(100));

        TBufferedCall p1a, p1b, p2;
        DownloadAsync(gateway, server, "/p1a", 60, p1a, Pool("P1"));
        server.WaitForRequests(1);
        DownloadAsync(gateway, server, "/p1b", 60, p1b, Pool("P1"));
        gateway.Inspector().WaitValue(PoolCounter("P1", "PerPoolAwait"), 1);
        DownloadAsync(gateway, server, "/p2", 30, p2, Pool("P2"));
        AssertOk(p2.Wait());
        UNIT_ASSERT_VALUES_EQUAL(Join(Paths(server)), "[/p1a /p2]");
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get(PoolCounter("P1", "PerPoolAwait")), 1);

        server.Release("/p1a");
        AssertOk(p1a.Wait());
        AssertOk(p1b.Wait());
        Finish(gateway);
    }

    // T-BUD-4 (P1, pin). Plan claim refuted: per-pool caps are floors, not limits. FillHandlers runs
    // DispatchAwaiting(respectCap = true) and then DispatchAwaiting(false); the second pass ignores the
    // caps while Allocated.size() < MaxHandlers. A cap only gives an under-cap pool priority for a freed
    // slot when the gateway is saturated. This test pins both halves.
    Y_UNIT_TEST(PoolCapIsAFloorNotALimit) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        for (const TString gate : {"/p1a", "/p1b", "/d"}) {
            server.Gate(gate);
        }
        THttpGatewayConfig config;
        config.SetMaxInFlightCount(2);
        TGatewayScope gateway(config);
        gateway->UpdatePoolCaps({{NDq::TWorkScope{"db", "P1"}, 1}});
        const auto& inspector = gateway.Inspector();

        // Unsaturated: P1 exceeds its cap of 1.
        TBufferedCall p1a, p1b, p1c, d;
        DownloadAsync(gateway, server, "/p1a", 0, p1a, Pool("P1"));
        server.WaitForRequests(1); // the arrival order of two concurrent requests is a race; the pin below lists it
        DownloadAsync(gateway, server, "/p1b", 0, p1b, Pool("P1"));
        server.WaitForRequests(2);
        UNIT_ASSERT_VALUES_EQUAL(server.MaxConcurrent(), 2);
        UNIT_ASSERT_VALUES_EQUAL(inspector.Get(PoolCounter("P1", "PerPoolAllocated")), 2);
        UNIT_ASSERT_VALUES_EQUAL(inspector.Get(PoolCounter("P1", "PerPoolCapFloor")), 1);

        // Saturated: both slots are held. Queue /p1c (P1) first, then /d (default pool).
        DownloadAsync(gateway, server, "/p1c", 0, p1c, Pool("P1"));
        DownloadAsync(gateway, server, "/d", 0, d);
        inspector.WaitValue("AwaitQueue", 2);
        WaitPerformCycles(gateway);
        UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 2);

        // A freed P1 slot goes to the default pool (under its cap), not to the earlier /p1c (P1 at cap).
        server.Release("/p1a");
        AssertOk(p1a.Wait());
        server.WaitForRequests(3);
        UNIT_ASSERT_VALUES_EQUAL(Join(Paths(server)), "[/p1a /p1b /d]");
        UNIT_ASSERT_VALUES_EQUAL(inspector.Get(PoolCounter("P1", "PerPoolAwait")), 1);
        UNIT_ASSERT_VALUES_EQUAL(inspector.Get(PoolCounter("P1", "PerPoolAllocated")), 1);

        server.Release("/p1b");
        server.Release("/d");
        for (const auto* call : {&p1b, &p1c, &d}) {
            AssertOk(call->Wait());
        }
        UNIT_ASSERT_VALUES_EQUAL(Join(Paths(server)), "[/p1a /p1b /d /p1c]");
        inspector.WaitValue(PoolCounter("P1", "PerPoolAllocated"), 0);
        UNIT_ASSERT_VALUES_EQUAL(inspector.Get(PoolCounter("P1", "PerPoolAwait")), 0);
        Finish(gateway);
    }

    // T-BUD-5 (P1, boundary): how far a stream runs past BuffersSizePerStream when nothing is consumed.
    // FillHandlers calls GetAction once per perform cycle and pauses the stream only once the held
    // bytes reach the threshold; everything curl delivers within that one curl_multi_perform call is
    // already accepted. So the overshoot is bounded by what one perform call reads, not by one write
    // callback (see the measured bound below). Every part is at most DownloadBufferBytesLimit.
    Y_UNIT_TEST(StreamPauseBound) {
        constexpr ui64 threshold = 64_KB;
        constexpr ui64 bufferSize = 16_KB;
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::BigBody(256_MB));
        THttpGatewayConfig config;
        config.SetBuffersSizePerStream(threshold);
        config.SetDownloadBufferBytesLimit(bufferSize);
        TGatewayScope gateway(config);

        TStreamConsumer consumer;
        auto cancel = StartStream(gateway, server.Url(), consumer);
        server.WaitBlockedOnSend();
        WaitPerformCycles(gateway);
        const ui64 held = consumer.Bytes();
        WaitPerformCycles(gateway);
        Cerr << "T-BUD-5: held " << held << " bytes in " << consumer.Parts() << " parts, max part "
             << consumer.MaxPart() << Endl;
        UNIT_ASSERT_VALUES_EQUAL_C(consumer.Bytes(), held, "the paused stream kept delivering");
        UNIT_ASSERT_GE(held, threshold);
        UNIT_ASSERT_LE_C(held, threshold + StreamOvershootBound(bufferSize), held);
        UNIT_ASSERT_LE(consumer.MaxPart(), bufferSize);

        cancel(TIssue("stop"));
        UNIT_ASSERT(consumer.WaitFinish(GUARD));
        consumer.Release();
        Finish(gateway);
    }

    // T-BUD-6 (P1, pin): a paused stream resumes after the consumer releases the held parts.
    Y_UNIT_TEST(StreamResumesAfterRelease) {
        constexpr ui64 size = 256_MB;
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::BigBody(size));
        THttpGatewayConfig config;
        config.SetBuffersSizePerStream(64_KB);
        config.SetDownloadBufferBytesLimit(16_KB);
        TGatewayScope gateway(config);

        TStreamConsumer consumer;
        StartStream(gateway, server.Url(), consumer);
        server.WaitBlockedOnSend();
        UNIT_ASSERT_VALUES_EQUAL(consumer.Finishes(), 0);
        consumer.Release();
        UNIT_ASSERT_C(consumer.WaitFinish(TDuration::Seconds(30)), "the stream did not finish after release");
        UNIT_ASSERT_VALUES_EQUAL_C(int(consumer.FinishCode()), int(CURLE_OK), consumer.FinishIssues());
        UNIT_ASSERT_VALUES_EQUAL(consumer.FinishIssues(), "");
        UNIT_ASSERT_VALUES_EQUAL(consumer.Bytes(), size);
        UNIT_ASSERT_VALUES_EQUAL(consumer.HttpCode(), 200);
        Finish(gateway);
        UNIT_ASSERT_VALUES_EQUAL(consumer.Finishes(), 1);
    }

    // T-BUD-9 (N-2, P1): sizeLimit bounds the received body. Today Write() appends whatever the server
    // sends: a server that ignores Range delivers 2 x sizeLimit.
    Y_UNIT_TEST(SizeLimitEnforcedOnReceivedBody) {
        YDB_SKIP_KNOWN_BUG("N-2");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok(TString(2000, 'z')).NoRange());
        TGatewayScope gateway;
        const auto outcome = Download(gateway, server.Url(), {}, 1000);
        Finish(gateway);
        const bool rejected = outcome.CurlCode != CURLE_OK || outcome.Issues.Contains("limit");
        UNIT_ASSERT_C(rejected || outcome.Body.size() <= 1000,
            "received " << outcome.Body.size() << " bytes for sizeLimit 1000, curl=" << int(outcome.CurlCode)
            << " http=" << outcome.HttpCode << " issues: " << outcome.Issues);
    }

    // T-BUD-11 (P2, pin): a request above the whole budget is rejected synchronously, before Download
    // returns, with CURLE_OK and an issue (F-B-4 context: the caller sees "HTTP 0").
    Y_UNIT_TEST(TooBigRequestRejectedSynchronously) {
        TLoopbackHttpServer server;
        TGatewayScope gateway(WithBudget(100));
        bool inDownload = true;
        bool calledInside = false;
        TBufferedCall call;
        const auto callback = call.Callback();
        gateway->Download(server.Url(), {}, 0, 101, [&](IHTTPGateway::TResult&& result) {
            calledInside = inDownload;
            callback(std::move(result));
        });
        inDownload = false;
        UNIT_ASSERT(calledInside);
        const auto outcome = call.Wait();
        UNIT_ASSERT_VALUES_EQUAL(int(outcome.CurlCode), int(CURLE_OK));
        UNIT_ASSERT_C(outcome.Issues.Contains("Too big file for downloading"), outcome.Issues);
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("Requests"), 1);
        UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 0);
        Finish(gateway);
    }

    // T-CFG-6 (P1): config knobs reach behaviour (the deterministic subset; BytesPerSecondLimit is
    // excluded, its only oracle would be elapsed time, RJ-11).
    Y_UNIT_TEST(ConfigKnobsReachBehaviour) {
        {   // (a) MaxInFlightCount
            TLoopbackHttpServer server;
            server.SetDefault(TScriptedResponse::Ok("x").NoRange());
            server.Gate("/g");
            THttpGatewayConfig config;
            config.SetMaxInFlightCount(2);
            TGatewayScope gateway(config);
            UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("MaxInFlight"), 2);
            TVector<TBufferedCall> calls(3);
            for (const auto& call : calls) {
                gateway->Download(server.Url("/g"), {}, 0, 0, call.Callback());
            }
            server.WaitForRequests(2);
            gateway.Inspector().WaitValue("AwaitQueue", 1);
            WaitPerformCycles(gateway);
            UNIT_ASSERT_VALUES_EQUAL(server.RequestCount(), 2);
            server.Release("/g");
            for (const auto& call : calls) {
                AssertOk(call.Wait());
            }
            UNIT_ASSERT_VALUES_EQUAL(server.MaxConcurrent(), 2);
            Finish(gateway);
        }
        {   // (b) DownloadBufferBytesLimit bounds every stream part
            TLoopbackHttpServer server;
            server.SetDefault(TScriptedResponse::BigBody(1_MB));
            THttpGatewayConfig config;
            config.SetDownloadBufferBytesLimit(4096);
            TGatewayScope gateway(config);
            TStreamConsumer consumer;
            consumer.Release(); // consume on arrival
            StartStream(gateway, server.Url(), consumer);
            UNIT_ASSERT(consumer.WaitFinish(GUARD));
            UNIT_ASSERT_VALUES_EQUAL_C(int(consumer.FinishCode()), int(CURLE_OK), consumer.FinishIssues());
            UNIT_ASSERT_VALUES_EQUAL(consumer.Bytes(), 1_MB);
            UNIT_ASSERT_LE(consumer.MaxPart(), 4096);
            Finish(gateway);
        }
        {   // (c) BuffersSizePerStream is reported as configured
            THttpGatewayConfig config;
            config.SetBuffersSizePerStream(12345);
            TGatewayScope gateway(config);
            UNIT_ASSERT_VALUES_EQUAL(gateway->GetBuffersSizePerStream(), 12345);
            Finish(gateway);
        }
    }

    // T-OBS-1 (F-A-11, P2): per-method result counters, DELETE included. Today Done() has subgroups for
    // GET/PUT/POST only, so DELETE results are not counted per method.
    Y_UNIT_TEST(PerMethodCountersIncludeDelete) {
        YDB_SKIP_KNOWN_BUG("F-A-11");
        TLoopbackHttpServer server;
        server.SetHandler([](const TReceivedRequest& request) {
            if (request.Method == "POST") {
                return TScriptedResponse::WithStatus(500);
            }
            if (request.Method == "DELETE") {
                return TScriptedResponse::WithStatus(204);
            }
            return TScriptedResponse::Ok("abc").NoRange();
        });
        TGatewayScope gateway;

        AssertOk(Download(gateway, server.Url()));
        TBufferedCall put, post, del;
        gateway->Upload(server.Url(), {}, "abc", put.Callback(), true);
        AssertOk(put.Wait());
        gateway->Upload(server.Url(), {}, "abc", post.Callback(), false);
        UNIT_ASSERT_VALUES_EQUAL(post.Wait().HttpCode, 500);
        gateway->Delete(server.Url(), {}, del.Callback());
        UNIT_ASSERT_VALUES_EQUAL(del.Wait().HttpCode, 204);
        const auto refused = Download(gateway, TStringBuilder() << "http://127.0.0.1:" << ClosedLoopbackPort() << "/x");
        UNIT_ASSERT_VALUES_EQUAL_C(int(refused.CurlCode), int(CURLE_COULDNT_CONNECT), refused.Issues);

        const auto& inspector = gateway.Inspector();
        const TString all = TCountersInspector::ToString(inspector.Snapshot());
        UNIT_ASSERT_VALUES_EQUAL_C(inspector.Get("method=GET/code=200/count"), 1, all);
        UNIT_ASSERT_VALUES_EQUAL_C(inspector.Get("method=PUT/code=200/count"), 1, all);
        UNIT_ASSERT_VALUES_EQUAL_C(inspector.Get("method=POST/code=500/count"), 1, all);
        UNIT_ASSERT_VALUES_EQUAL_C(inspector.Get("method=GET/curl_code=7/count"), 1, all);
        UNIT_ASSERT_VALUES_EQUAL_C(inspector.Get("Requests"), 5, all);
        UNIT_ASSERT_VALUES_EQUAL_C(inspector.Get("method=DELETE/code=204/count"), 1, all);
        Finish(gateway);
    }
}

} // namespace NYql
