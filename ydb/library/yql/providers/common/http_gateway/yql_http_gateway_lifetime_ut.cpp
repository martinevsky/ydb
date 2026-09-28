// FQ transport test plan, target A, §4.4 (R4 concurrency and lifetime), the worker (T-WRK-1) and the
// gateway-path error tests of §4.5/§4.6: T-LIF-1..7, T-WRK-1, T-ERR-2, T-ERR-3, T-RTY-3.
//
// Races are forced, not sampled: T-LIF-1 holds the curl thread inside OnFinish on a latch and runs the
// cancel while it is held. CURLM failures are injected through seam S3 (TGatewayTestOptions.CurlMulti).
// Every wait is a latch/condvar/future or a guarded sync point (server arrival, gateway gauge,
// PerformCycles). No sleeps.

#include "yql_http_default_retry_policy.h"
#include "yql_http_gateway_test_hooks.h"
#include "yql_http_gateway_ut_common.h"

#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/loopback_http_server.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/manual_clock.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/log_capture.h>

#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/algorithm.h>
#include <util/generic/map.h>
#include <util/generic/size_literals.h>
#include <util/random/fast.h>
#include <util/string/builder.h>

#include <atomic>
#include <deque>
#include <future>
#include <optional>
#include <thread>

namespace NYql {

using namespace NTransportTest;
using namespace NHttpGatewayUt;

namespace {

using TClock = std::chrono::steady_clock;

std::chrono::microseconds ToChrono(TDuration duration) {
    return std::chrono::microseconds(duration.MicroSeconds());
}

TGatewayScope::TFactory WithOptions(NHttpGatewayTest::TGatewayTestOptions options) {
    return [options = std::move(options)](const THttpGatewayConfig* config, ::NMonitoring::TDynamicCounterPtr counters) {
        return NHttpGatewayTest::MakeHttpGatewayForTest(config, std::move(counters), options);
    };
}

// S3: makes the next curl_multi_perform call of the worker loop return `code`, once, when armed.
struct TCurlmFault {
    std::shared_ptr<std::atomic<bool>> Armed = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<ui32>> Injected = std::make_shared<std::atomic<ui32>>(0);
    CURLMcode Code = CURLM_INTERNAL_ERROR;

    NHttpGatewayTest::TGatewayTestOptions Options() const {
        NHttpGatewayTest::TGatewayTestOptions options;
        options.CurlMulti = [armed = Armed, injected = Injected, code = Code](
            NHttpGatewayTest::ECurlMultiCall which, const std::function<CURLMcode()>& call)
        {
            if (which == NHttpGatewayTest::ECurlMultiCall::Perform && armed->exchange(false)) {
                ++*injected;
                return code;
            }
            return call();
        };
        return options;
    }
};

IHttpRequestContext::TPtr Pool(const TString& name) {
    return MakeIntrusive<TDefaultHttpRequestContext>(NDq::TWorkScope{"db", name});
}

TString PoolCounter(const TString& pool, const TString& name) {
    return TStringBuilder() << "db=db/pool=" << pool << "/" << name;
}

TString Paths(const TLoopbackHttpServer& server) {
    TStringBuilder result;
    for (const auto& request : server.Requests()) {
        result << (result.empty() ? "" : " ") << request.Path;
    }
    return result;
}

// Records a stream download: every callback, and the data parts delivered after OnFinish.
class TStreamProbe {
public:
    IHTTPGateway::TOnDownloadStart OnStart() {
        return [state = State_](CURLcode, long httpCode) {
            std::lock_guard lock(state->Mutex);
            state->HttpCode = httpCode;
        };
    }

    IHTTPGateway::TOnNewDataPart OnData() {
        return [state = State_](IHTTPGateway::TCountedContent&& part) {
            std::lock_guard lock(state->Mutex);
            ++state->Parts;
            if (state->Finishes) {
                ++state->PartsAfterFinish;
            }
            state->Held.push_back(std::move(part)); // backpressure: nothing is consumed until Drop()
            state->Changed.notify_all();
        };
    }

    IHTTPGateway::TOnDownloadFinish OnFinish() {
        return [state = State_](CURLcode code, TIssues issues) {
            std::lock_guard lock(state->Mutex);
            ++state->Finishes;
            state->FinishCode = code;
            state->FinishIssues = issues.ToOneLineString();
            state->Changed.notify_all();
        };
    }

    void WaitParts(ui64 parts, TDuration guard = GUARD) const {
        Wait([&]() { return State_->Parts >= parts; }, guard, "stream data parts");
    }

    void WaitFinish(TDuration guard = GUARD) const {
        Wait([&]() { return State_->Finishes > 0; }, guard, "stream OnFinish");
    }

    // Releases the held parts (outside the probe's lock).
    void Drop() {
        TVector<IHTTPGateway::TCountedContent> held;
        {
            std::lock_guard lock(State_->Mutex);
            held.swap(State_->Held);
        }
    }

    ui32 Finishes() const { std::lock_guard lock(State_->Mutex); return State_->Finishes; }
    ui64 PartsAfterFinish() const { std::lock_guard lock(State_->Mutex); return State_->PartsAfterFinish; }
    CURLcode FinishCode() const { std::lock_guard lock(State_->Mutex); return State_->FinishCode; }
    TString FinishIssues() const { std::lock_guard lock(State_->Mutex); return State_->FinishIssues; }
    long HttpCode() const { std::lock_guard lock(State_->Mutex); return State_->HttpCode; }
    ui64 HeldBytes() const {
        std::lock_guard lock(State_->Mutex);
        ui64 bytes = 0;
        for (const auto& part : State_->Held) {
            bytes += part.size();
        }
        return bytes;
    }

private:
    void Wait(const std::function<bool()>& predicate, TDuration guard, TStringBuf what) const {
        std::unique_lock lock(State_->Mutex);
        UNIT_ASSERT_C(State_->Changed.wait_for(lock, ToChrono(guard), predicate), what << " not reached within " << guard);
    }

    struct TState {
        std::mutex Mutex;
        std::condition_variable Changed;
        ui64 Parts = 0;
        ui64 PartsAfterFinish = 0;
        ui32 Finishes = 0;
        CURLcode FinishCode = CURLE_OK;
        TString FinishIssues;
        long HttpCode = 0;
        TVector<IHTTPGateway::TCountedContent> Held;
    };
    std::shared_ptr<TState> State_ = std::make_shared<TState>();
};

IHTTPGateway::TCancelHook StartStream(const TGatewayScope& gateway, const TString& url, TStreamProbe& probe,
    IHttpRequestContext::TPtr context = nullptr,
    const ::NMonitoring::TDynamicCounters::TCounterPtr& inflightCounter = nullptr)
{
    return gateway->Download(url, {}, 0, 0, probe.OnStart(), probe.OnData(), probe.OnFinish(), inflightCounter,
        std::move(context));
}

// "METHOD /path=n ..." in method/path order: requests the server received per method and path.
TString AttemptsPerRequest(const TLoopbackHttpServer& server) {
    TMap<TString, ui32> attempts;
    for (const auto& request : server.Requests()) {
        ++attempts[request.Method + " " + request.Path];
    }
    TStringBuilder result;
    for (const auto& [request, count] : attempts) {
        result << (result.empty() ? "" : " ") << request << "=" << count;
    }
    return result;
}

TString Head(const TString& text, size_t size = 200) {
    return TString(TStringBuf(text).Head(size));
}

} // namespace

Y_UNIT_TEST_SUITE(THttpGatewayLifetimeTest) {

    // T-LIF-1 (F-A-5, P1): a cancel racing the completion delivers exactly one OnFinish and runs no user code
    // under the gateway lock. The race is forced: OnFinish #1 holds the curl thread inside Done on latch L
    // while the test cancels. Today the cancel hook sees the stream still alive (Done's local reference),
    // calls OnFinish a second time and does so under the gateway mutex, so a Download issued from inside
    // that OnFinish blocks until the cancel returns.
    Y_UNIT_TEST(CancelRacingCompletionDeliversOneOnFinish) {
        YDB_SKIP_KNOWN_BUG("F-A-5");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        TGatewayScope gateway;
        const TDuration nestedGuard = TDuration::Seconds(3);

        struct TState {
            std::mutex Mutex;
            std::condition_variable Changed;
            ui32 Finishes = 0;
            ui32 NestedReturned = 0;
            ui32 NestedBlocked = 0;
            TVector<std::future<void>> Nested;
            TVector<TBufferedCall> NestedCalls;
        };
        const auto state = std::make_shared<TState>();
        std::promise<void> latch;
        const std::shared_future<void> latchReleased = latch.get_future().share();
        const IHTTPGateway::TWeakPtr weakGateway = gateway.Gateway();
        const TString nestedUrl = server.Url("/nested");

        const IHTTPGateway::TOnDownloadFinish onFinish = [=](CURLcode, TIssues) {
            ui32 finish = 0;
            {
                std::lock_guard lock(state->Mutex);
                finish = ++state->Finishes;
                state->Changed.notify_all();
            }
            if (finish == 1) {
                latchReleased.wait_for(ToChrono(GUARD)); // L: hold the curl thread inside Done
            }
            // User code that calls back into the gateway, the way a read actor schedules its next request.
            TBufferedCall call;
            auto nested = std::async(std::launch::async, [weakGateway, nestedUrl, call]() {
                if (const auto gw = weakGateway.lock()) {
                    gw->Download(nestedUrl, {}, 0, 0, call.Callback());
                }
            });
            const bool returned = nested.wait_for(ToChrono(nestedGuard)) == std::future_status::ready;
            std::lock_guard lock(state->Mutex);
            ++(returned ? state->NestedReturned : state->NestedBlocked);
            state->Nested.push_back(std::move(nested));
            state->NestedCalls.push_back(call);
        };

        const auto cancel = gateway->Download(server.Url("/obj"), {}, 0, 0, [](CURLcode, long) {},
            [](IHTTPGateway::TCountedContent&&) {}, onFinish, nullptr);
        {
            std::unique_lock lock(state->Mutex);
            UNIT_ASSERT_C(state->Changed.wait_for(lock, ToChrono(GUARD), [&]() { return state->Finishes > 0; }),
                "OnFinish #1 not entered");
        }

        auto cancelled = std::async(std::launch::async, [&]() { cancel(TIssue("c")); });
        const bool cancelReturnedUnderLatch = cancelled.wait_for(ToChrono(GUARD)) == std::future_status::ready;
        latch.set_value();
        cancelled.wait();
        WaitPerformCycles(gateway, 3);

        TVector<TBufferedCall> nestedCalls;
        ui32 finishes = 0, nestedReturned = 0, nestedBlocked = 0;
        {
            std::lock_guard lock(state->Mutex);
            for (auto& nested : state->Nested) {
                nested.wait();
            }
            nestedCalls = state->NestedCalls;
            finishes = state->Finishes;
            nestedReturned = state->NestedReturned;
            nestedBlocked = state->NestedBlocked;
        }
        for (const auto& call : nestedCalls) {
            UNIT_ASSERT_VALUES_EQUAL(call.Wait().HttpCode, 200);
        }
        Finish(gateway);

        UNIT_ASSERT_C(cancelReturnedUnderLatch, "cancel() waited for the curl thread that is inside OnFinish");
        UNIT_ASSERT_VALUES_EQUAL_C(finishes, 1, "OnFinish delivered more than once");
        UNIT_ASSERT_VALUES_EQUAL_C(nestedBlocked, 0, "a Download issued from OnFinish blocked on the gateway lock");
        UNIT_ASSERT_VALUES_EQUAL(nestedReturned, 1);
    }

    // T-LIF-2 (P1, coverage gap): cancelling a stream that is still queued finishes it once, and it is
    // never sent. Caps are floors (T-BUD-4), so MaxInFlightCount = 1 is what keeps S2 queued.
    Y_UNIT_TEST(CancelQueuedStream) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        server.Gate("/s1");
        THttpGatewayConfig config;
        config.SetMaxInFlightCount(1);
        TGatewayScope gateway(config);
        gateway->UpdatePoolCaps({{NDq::TWorkScope{"db", "P1"}, 1}});

        TStreamProbe s1, s2;
        StartStream(gateway, server.Url("/s1"), s1, Pool("P1"));
        server.WaitForRequests(1);
        const auto cancel2 = StartStream(gateway, server.Url("/s2"), s2, Pool("P1"));
        gateway.Inspector().WaitValue(PoolCounter("P1", "PerPoolAwait"), 1);

        cancel2(TIssue("c2"));
        UNIT_ASSERT_VALUES_EQUAL(s2.Finishes(), 1);
        UNIT_ASSERT_VALUES_EQUAL(int(s2.FinishCode()), int(CURLE_OK));
        UNIT_ASSERT_STRING_CONTAINS(s2.FinishIssues(), "c2");

        server.Release("/s1");
        s1.WaitFinish();
        UNIT_ASSERT_VALUES_EQUAL_C(s1.HttpCode(), 200, s1.FinishIssues());
        gateway.Inspector().WaitValue(PoolCounter("P1", "PerPoolAwait"), 0);
        gateway.Inspector().WaitValue(PoolCounter("P1", "PerPoolAllocated"), 0);
        WaitPerformCycles(gateway, 3);
        server.WaitConnectionsDone(1);
        UNIT_ASSERT_VALUES_EQUAL(Paths(server), "/s1");
        UNIT_ASSERT_VALUES_EQUAL(s2.Finishes(), 1);
        s1.Drop();
        s2.Drop();
        Finish(gateway); // InFlightStreams == 0
    }

    // T-LIF-3 (P1, coverage gap): a cancel in the middle of a stream stops the data and closes the
    // connection. The consumer holds every part, so the stream is paused (the server blocks on send) when
    // the cancel runs: no part can be in flight on the curl thread at that moment.
    Y_UNIT_TEST(CancelMidStreamStopsDeliveryAndClosesConnection) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::BigBody(64_MB));
        TGatewayScope gateway;

        TStreamProbe probe;
        const auto cancel = StartStream(gateway, server.Url("/big"), probe);
        probe.WaitParts(1);
        server.WaitBlockedOnSend(1);

        cancel(TIssue("stop"));
        UNIT_ASSERT_VALUES_EQUAL(probe.Finishes(), 1);
        UNIT_ASSERT_STRING_CONTAINS(probe.FinishIssues(), "stop");
        server.WaitConnectionClosedByPeer(0, TDuration::Seconds(5));
        WaitPerformCycles(gateway, 3);
        UNIT_ASSERT_VALUES_EQUAL(probe.PartsAfterFinish(), 0);
        UNIT_ASSERT_VALUES_EQUAL(probe.Finishes(), 1);
        probe.Drop();
        Finish(gateway); // InFlightStreams == 0
    }

    // T-LIF-4 (F-A-5, P2, pin): a cancel after OnFinish has returned is a no-op. Scratch A waited a fixed 300 ms;
    // here the sync point is PerformCycles.
    Y_UNIT_TEST(CancelAfterFinishReturnedIsNoop) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        TGatewayScope gateway;

        TStreamProbe probe;
        const auto cancel = StartStream(gateway, server.Url("/obj"), probe);
        probe.WaitFinish();
        WaitPerformCycles(gateway, 2);
        cancel(TIssue("late"));
        UNIT_ASSERT_VALUES_EQUAL(probe.Finishes(), 1);
        UNIT_ASSERT_VALUES_EQUAL(int(probe.FinishCode()), int(CURLE_OK));
        UNIT_ASSERT_C(!probe.FinishIssues().Contains("late"), probe.FinishIssues());
        probe.Drop();
        Finish(gateway);
    }

    // T-LIF-5 (F-A-5, P1): cancel/complete stress. 200 streams with small bodies; 8 canceller threads fire
    // each stream's cancel at a random point (right after Download returns, at OnStart, at the first part,
    // from inside OnFinish, or never). A callback that requests a cancel yields the CPU a few hundred times
    // afterwards (a stand-in for user work; no sleeps), which widens the window the canceller races into.
    // Every stream must see exactly one OnFinish. Today the cancel hook can deliver a second
    // OnFinish while the curl thread is in Done (T-LIF-1 forces that interleaving deterministically);
    // under --sanitize=thread the unsynchronised `bool Cancelled` is also reported (not run here).
    Y_UNIT_TEST(CancelCompleteStress) {
        YDB_SKIP_KNOWN_BUG("F-A-5");
        constexpr size_t STREAMS = 200;
        constexpr size_t CANCELLERS = 8;
        constexpr ui32 USER_WORK_YIELDS = 300;
        enum class EPoint { AfterDownload, OnStart, OnFirstPart, InOnFinish, Never };
        const auto userWork = []() {
            for (ui32 i = 0; i < USER_WORK_YIELDS; ++i) {
                std::this_thread::yield();
            }
        };

        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("0123456789").NoRange());
        TGatewayScope gateway;

        struct TShared {
            std::mutex Mutex;
            std::condition_variable Changed;
            TVector<std::optional<IHTTPGateway::TCancelHook>> Hooks;
            TVector<EPoint> Points;
            TVector<ui32> Finishes;
            TVector<bool> Queued;
            std::deque<size_t> Queue;
            bool Stop = false;

            void Enqueue(size_t i) {
                std::lock_guard lock(Mutex);
                if (!std::exchange(Queued[i], true)) {
                    Queue.push_back(i);
                    Changed.notify_all();
                }
            }
        };
        const auto shared = std::make_shared<TShared>();
        shared->Hooks.resize(STREAMS);
        shared->Finishes.resize(STREAMS);
        shared->Queued.resize(STREAMS);
        TReallyFastRng32 rng(20260925);
        for (size_t i = 0; i < STREAMS; ++i) {
            shared->Points.push_back(static_cast<EPoint>(rng.Uniform(5)));
        }

        TVector<std::thread> cancellers;
        std::atomic<ui32> hookMissing = 0;
        for (size_t t = 0; t < CANCELLERS; ++t) {
            cancellers.emplace_back([shared, &hookMissing]() {
                while (true) {
                    IHTTPGateway::TCancelHook hook;
                    {
                        std::unique_lock lock(shared->Mutex);
                        shared->Changed.wait(lock, [&]() { return shared->Stop || !shared->Queue.empty(); });
                        if (shared->Queue.empty()) {
                            return;
                        }
                        const size_t i = shared->Queue.front();
                        shared->Queue.pop_front();
                        if (!shared->Changed.wait_for(lock, ToChrono(GUARD), [&]() { return shared->Hooks[i].has_value(); })) {
                            ++hookMissing;
                            continue;
                        }
                        hook = *shared->Hooks[i];
                    }
                    hook(TIssue("stress"));
                }
            });
        }

        for (size_t i = 0; i < STREAMS; ++i) {
            const EPoint point = shared->Points[i];
            auto hook = gateway->Download(server.Url(TStringBuilder() << "/s" << i), {}, 0, 0,
                [shared, i, point, userWork](CURLcode, long) {
                    if (point == EPoint::OnStart) {
                        shared->Enqueue(i);
                        userWork();
                    }
                },
                [shared, i, point, userWork](IHTTPGateway::TCountedContent&&) {
                    if (point == EPoint::OnFirstPart) {
                        shared->Enqueue(i);
                        userWork();
                    }
                },
                [shared, i, point, userWork](CURLcode, TIssues) {
                    {
                        std::lock_guard lock(shared->Mutex);
                        ++shared->Finishes[i];
                        shared->Changed.notify_all();
                    }
                    if (point == EPoint::InOnFinish) {
                        shared->Enqueue(i);
                        userWork();
                    }
                },
                nullptr);
            {
                std::lock_guard lock(shared->Mutex);
                shared->Hooks[i] = std::move(hook);
                shared->Changed.notify_all();
            }
            if (point == EPoint::AfterDownload) {
                shared->Enqueue(i);
            }
        }

        bool allFinished = false;
        {
            std::unique_lock lock(shared->Mutex);
            allFinished = shared->Changed.wait_for(lock, ToChrono(GUARD), [&]() {
                return AllOf(shared->Finishes, [](ui32 n) { return n > 0; });
            });
            shared->Stop = true;
            shared->Changed.notify_all();
        }
        for (auto& canceller : cancellers) {
            canceller.join();
        }
        WaitPerformCycles(gateway, 3);
        Finish(gateway);

        UNIT_ASSERT_C(allFinished, "not every stream finished within the guard");
        UNIT_ASSERT_VALUES_EQUAL(hookMissing.load(), 0);
        TStringBuilder duplicates;
        size_t duplicated = 0;
        for (size_t i = 0; i < STREAMS; ++i) {
            if (shared->Finishes[i] != 1) {
                ++duplicated;
                if (duplicated <= 5) {
                    duplicates << " s" << i << "=" << shared->Finishes[i];
                }
            }
        }
        UNIT_ASSERT_VALUES_EQUAL_C(duplicated, 0, "streams without exactly one OnFinish:" << duplicates);
    }

    // T-LIF-6 (P2, coverage gap): a data part that outlives the gateway is destroyed safely. The part holds
    // a weak reference to the curl multi handle; with BuffersSizePerStream = 4 its release crosses the
    // threshold, so the destructor tries to wake the (destroyed) worker. The per-stream inflight counter
    // returns to 0. The static OutputSize cannot be read once the gateway is gone (it is only exported
    // through the OutputMemory gauge of a live gateway); the test checks that the part is counted there
    // while the gateway lives. ASAN is not run in this package (BRIEF decision 4).
    Y_UNIT_TEST(CountedContentOutlivesGateway) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abcdefgh").NoRange());
        THttpGatewayConfig config;
        config.SetBuffersSizePerStream(4);
        TGatewayScope gateway(config);
        const auto inflight = MakeIntrusive<::NMonitoring::TCounterForPtr>();

        WaitPerformCycles(gateway, 1);
        const i64 outputBefore = gateway.Inspector().Get("OutputMemory");
        TStreamProbe probe;
        StartStream(gateway, server.Url("/obj"), probe, nullptr, inflight);
        probe.WaitFinish();
        UNIT_ASSERT_VALUES_EQUAL(probe.HeldBytes(), 8);
        UNIT_ASSERT_VALUES_EQUAL(inflight->Val(), 8);
        gateway.Inspector().WaitValue("OutputMemory", outputBefore + 8);

        Finish(gateway); // the gateway is destroyed: the part is the last thing referring to it
        probe.Drop();
        UNIT_ASSERT_VALUES_EQUAL(inflight->Val(), 0);
        UNIT_ASSERT_VALUES_EQUAL(probe.HeldBytes(), 0);
    }

    // T-LIF-7 (N-3, P2): destroying the gateway completes every outstanding callback exactly once with a
    // "shutting down" issue: one buffered request in flight (stalled server), one queued behind it
    // (MaxInFlightCount = 1) and one waiting in the retry queue (manual clock, never due). Today
    // ~THTTPMultiGateway joins the worker and drops Allocated/AwaitPerPool/Delayed without calling them,
    // so a caller waiting on a promise (e.g. the Solomon accessor) never completes. The fix is seam S14,
    // not implemented here (BRIEF decision 1).
    Y_UNIT_TEST(DestructionCompletesOutstandingCallbacks) {
        YDB_SKIP_KNOWN_BUG("N-3");
        TManualClock clock;
        TLoopbackHttpServer server;
        server.Script("/retry", {TScriptedResponse::WithStatus(503)});
        server.Script("/stall", {TScriptedResponse::StallForever()});
        server.SetDefault(TScriptedResponse::Ok("x").NoRange());
        THttpGatewayConfig config;
        config.SetMaxInFlightCount(1);
        TGatewayScope gateway(config, WithOptions({.Now = clock.AsFunction()}));

        TBufferedCall delayed, inFlight, queued;
        gateway->Download(server.Url("/retry"), {}, 0, 0, delayed.Callback(), {}, GetHTTPDefaultRetryPolicy());
        gateway.Inspector().WaitValue("method=GET/code=503/count", 1);
        WaitPerformCycles(gateway, 1); // the retry is in Delayed; its deadline is on the manual clock
        gateway->Download(server.Url("/stall"), {}, 0, 0, inFlight.Callback());
        server.WaitForRequests(2);
        gateway->Download(server.Url("/queued"), {}, 0, 0, queued.Callback());
        gateway.Inspector().WaitValue("AwaitQueue", 1);
        UNIT_ASSERT_VALUES_EQUAL(delayed.Calls() + inFlight.Calls() + queued.Calls(), 0);

        gateway.Close(); // last owner: ~THTTPMultiGateway runs here (the test timeout is the backstop)

        const TVector<std::pair<TString, const TBufferedCall*>> calls = {
            {"delayed", &delayed}, {"in flight", &inFlight}, {"queued", &queued}};
        for (const auto& [name, call] : calls) {
            UNIT_ASSERT_VALUES_EQUAL_C(call->Calls(), 1, name << " callback after gateway destruction");
            UNIT_ASSERT_STRING_CONTAINS_C(call->Wait().Issues, "shutting down", name);
        }
    }

    // T-WRK-1 (F-A-6, P1): a CURLM error does not strand later requests. Injection (S3): one
    // curl_multi_perform returns CURLM_INTERNAL_ERROR while download #1 is in flight. Today Perform() calls
    // Fail(c) and `break`s: the only worker thread exits, #2 is queued forever, and nothing counts or logs
    // the dead worker. Contract: #2 completes (recovered worker, or an immediate "gateway failed" error),
    // MultiErrors == 1 and one ERROR log line.
    Y_UNIT_TEST(CurlmErrorDoesNotStrandLaterRequests) {
        YDB_SKIP_KNOWN_BUG("F-A-6");
        TLogCapture log;
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        server.Gate("/one");
        TCurlmFault fault;
        TGatewayScope gateway({}, WithOptions(fault.Options()));
        const TString reason = curl_multi_strerror(CURLM_INTERNAL_ERROR);

        TBufferedCall first, second;
        gateway->Download(server.Url("/one"), {}, 0, 0, first.Callback());
        server.WaitForRequests(1);
        *fault.Armed = true;
        const auto firstOutcome = first.Wait();
        UNIT_ASSERT_STRING_CONTAINS(firstOutcome.Issues, reason);
        UNIT_ASSERT_VALUES_EQUAL(fault.Injected->load(), 1);

        gateway->Download(server.Url("/two"), {}, 0, 0, second.Callback());
        const auto secondOutcome = second.Wait(); // guard 10 s: a stranded request fails here
        UNIT_ASSERT_C(secondOutcome.HttpCode == 200 || !secondOutcome.Issues.empty(), "#2 neither succeeded nor failed");
        UNIT_ASSERT_VALUES_EQUAL(second.Calls(), 1);
        UNIT_ASSERT_VALUES_EQUAL(gateway.Inspector().Get("MultiErrors"), 1);
        UNIT_ASSERT_VALUES_EQUAL_C(log.CountLines("ERROR.*" + reason), 1, Head(log.Text(), 1000));
        server.Release("/one");
        Finish(gateway);
    }

    // T-ERR-2 (F-B-4 / F-A-6, P2, pin): the CURLM failure reason reaches the caller of a buffered request
    // that was in flight when the multi handle failed (S3 injection).
    Y_UNIT_TEST(CurlmFailureReasonReachesCaller) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok("abc").NoRange());
        server.Gate("/one");
        TCurlmFault fault;
        TGatewayScope gateway({}, WithOptions(fault.Options()));

        TBufferedCall call;
        gateway->Download(server.Url("/one"), {}, 0, 0, call.Callback());
        server.WaitForRequests(1);
        *fault.Armed = true;
        const auto outcome = call.Wait();
        UNIT_ASSERT_STRING_CONTAINS(outcome.Issues, curl_multi_strerror(CURLM_INTERNAL_ERROR));
        UNIT_ASSERT_VALUES_EQUAL(outcome.HttpCode, 0);
        UNIT_ASSERT_VALUES_EQUAL(call.Calls(), 1);
        server.Release("/one");
        Finish(gateway);
    }

    // T-ERR-3 (R6, P1, pin): no secret in gateway-path issues or logs, with the YQL log at TRACE. The secret
    // is the IAM token (X-YaCloud-SubjectToken) and the secret part of the SigV4 user:password. Cases:
    // (a) connection refused; (b) a 403 whose body echoes the request headers (the echo contains the token:
    // proof that the secret was on the wire, while issues and logs stay clean). Case (c), the S3 lister,
    // belongs to the S3 provider target: http_gateway/ut must not depend on providers/s3.
    Y_UNIT_TEST(NoSecretInIssuesOrLogs) {
        const TString secret = "SECRET-XYZ-123";
        TLogCapture log;
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::EchoRequestHeaders(403));
        TGatewayScope gateway;
        const auto headers = [&]() {
            return IHTTPGateway::MakeYcHeaders("req", secret, {}, "AKID:" + secret, "aws:amz:ru-central1:s3");
        };
        TRefusingPort refusingPort;
        const TString refused = refusingPort.Url("/obj");

        TVector<std::pair<TString, TOutcome>> outcomes;
        outcomes.emplace_back("refused GET", Download(gateway, refused, headers()));
        {
            TBufferedCall call;
            gateway->Upload(refused, headers(), "body", call.Callback(), true);
            outcomes.emplace_back("refused PUT", call.Wait());
        }
        outcomes.emplace_back("403 GET", Download(gateway, server.Url("/obj"), headers()));
        {
            TBufferedCall call;
            gateway->Delete(server.Url("/obj"), headers(), call.Callback());
            outcomes.emplace_back("403 DELETE", call.Wait());
        }

        UNIT_ASSERT_VALUES_EQUAL(int(outcomes[0].second.CurlCode), int(CURLE_COULDNT_CONNECT));
        UNIT_ASSERT_VALUES_EQUAL(outcomes[2].second.HttpCode, 403);
        UNIT_ASSERT_C(outcomes[2].second.Body.Contains(secret), "the echo should carry the token");
        UNIT_ASSERT(server.AnyRequestContains(secret));
        for (const auto& [name, outcome] : outcomes) {
            UNIT_ASSERT_C(!outcome.Issues.Contains(secret), name << ": " << Head(outcome.Issues));
        }
        log.AssertNoSecret(secret);
        Finish(gateway);
    }

    // T-RTY-3 (P2, pin + DG): method idempotency on connection loss after the request was sent. Attempt 1 of
    // every request reads the whole request and closes without answering (CURLE_GOT_NOTHING, retried by the
    // FQ policy); attempt 2 answers 200. Today every method is re-sent, including POST ?uploads, which
    // creates a second multipart upload and orphans the first (s3fq #22).
    Y_UNIT_TEST(RetryOnConnectionLossResendsEveryMethod) {
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok().NoRange());
        for (const TString path : {"/put-part", "/create-upload", "/complete-upload", "/abort-upload"}) {
            server.Script(path, {TScriptedResponse::CloseAfterRequest(), TScriptedResponse::Ok().NoRange()});
        }
        TGatewayScope gateway;
        const auto policy = GetFqHTTPRetryPolicy();

        TBufferedCall put, create, complete, abort;
        gateway->Upload(server.Url("/put-part") + "?partNumber=1&uploadId=u1", {}, "part", put.Callback(), true, policy);
        gateway->Upload(server.Url("/create-upload") + "?uploads", {}, "", create.Callback(), false, policy);
        gateway->Upload(server.Url("/complete-upload") + "?uploadId=u1", {}, "<CompleteMultipartUpload/>", complete.Callback(), false, policy);
        gateway->Delete(server.Url("/abort-upload") + "?uploadId=u1", {}, abort.Callback(), policy);

        for (const auto* call : {&put, &create, &complete, &abort}) {
            const auto outcome = call->Wait();
            UNIT_ASSERT_VALUES_EQUAL_C(outcome.HttpCode, 200, outcome.Issues);
        }
        UNIT_ASSERT_VALUES_EQUAL(AttemptsPerRequest(server),
            "DELETE /abort-upload=2 POST /complete-upload=2 POST /create-upload=2 PUT /put-part=2");
        Finish(gateway);
    }

    // T-RTY-3 (DG proposed contract): POST ?uploads (CreateMultipartUpload) is not re-sent after the
    // request may have reached the server; PUT and DELETE keep their retry. Decision-gated (s3fq #22).
    Y_UNIT_TEST(CreateMultipartUploadNotResentAfterConnectionLoss) {
        YDB_SKIP_KNOWN_BUG("DG-T-RTY-3");
        TLoopbackHttpServer server;
        server.SetDefault(TScriptedResponse::Ok().NoRange());
        for (const TString path : {"/put-part", "/create-upload", "/abort-upload"}) {
            server.Script(path, {TScriptedResponse::CloseAfterRequest(), TScriptedResponse::Ok().NoRange()});
        }
        TGatewayScope gateway;
        const auto policy = GetFqHTTPRetryPolicy();

        TBufferedCall put, create, abort;
        gateway->Upload(server.Url("/put-part") + "?partNumber=1&uploadId=u1", {}, "part", put.Callback(), true, policy);
        gateway->Upload(server.Url("/create-upload") + "?uploads", {}, "", create.Callback(), false, policy);
        gateway->Delete(server.Url("/abort-upload") + "?uploadId=u1", {}, abort.Callback(), policy);
        for (const auto* call : {&put, &create, &abort}) {
            call->Wait();
        }
        UNIT_ASSERT_VALUES_EQUAL(AttemptsPerRequest(server), "DELETE /abort-upload=2 POST /create-upload=1 PUT /put-part=2");
        Finish(gateway);
    }

} // Y_UNIT_TEST_SUITE(THttpGatewayLifetimeTest)

} // namespace NYql
