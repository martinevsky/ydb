// FQ transport test plan, target A, §4.8: the pool-cap pusher actor (T-ACT-1..3).
//
// H13: TTestActorRuntimeBase with simulated time (useRealThreads = false). SimulateSleep(period) delivers
// each scheduled TEvWakeup deterministically, so a "tick" is one SimulateSleep. The gateway is H9
// (TScriptedHttpGateway), which records every UpdatePoolCaps call.

#include "yql_http_pool_cap_pusher.h"

#include <ydb/library/yql/providers/common/http_gateway/mock/yql_http_scripted_gateway.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/yql/log_capture.h>

#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/testlib/test_runtime.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/map.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>

namespace NYql {

using namespace NActors;
using namespace NTransportTest;

namespace {

const TDuration PERIOD = TDuration::Seconds(1);
constexpr size_t MAX_HANDLERS = 1000;
constexpr double MIN_DEFAULT_FRACTION = 0.1;

NDq::TWorkScope Pool(const TString& name) {
    return NDq::TWorkScope{"db", name};
}

// "ns/name=cap ..." sorted by pool, e.g. "/default=500 db/P1=500".
TString Render(const THashMap<NDq::TWorkScope, size_t>& caps) {
    TMap<TString, size_t> sorted;
    for (const auto& [pool, cap] : caps) {
        sorted[pool.Namespace + "/" + pool.Name] = cap;
    }
    TStringBuilder result;
    for (const auto& [pool, cap] : sorted) {
        result << (result.empty() ? "" : " ") << pool << "=" << cap;
    }
    return result;
}

// H13: the pusher in a simulated-time runtime. Counts the TEvWakeup deliveries addressed to the pusher.
struct TPusherFixture {
    TTestActorRuntimeBase Runtime{1, false};
    TActorId Pusher;
    ui32 Wakeups = 0;
    TTestActorRuntimeBase::TEventObserverHolder WakeupObserver;

    TPusherFixture(TPoolSharesProvider provider, IHTTPGateway::TWeakPtr gateway) {
        Runtime.Initialize();
        // The base runtime drops every actor-scheduled event by default; keep those of whitelisted actors.
        Runtime.SetScheduledEventFilter([](TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& ev, TDuration, TInstant&) {
            return !runtime.IsScheduleForActorEnabled(ev->GetRecipientRewrite());
        });
        Pusher = Runtime.Register(CreateHttpPoolCapPusher(std::move(provider), std::move(gateway), PERIOD,
            MAX_HANDLERS, MIN_DEFAULT_FRACTION));
        Runtime.EnableScheduleForActor(Pusher);
        WakeupObserver = Runtime.AddObserver<TEvents::TEvWakeup>([this](TEvents::TEvWakeup::TPtr& ev) {
            if (ev->GetRecipientRewrite() == Pusher) {
                ++Wakeups;
            }
        });
        // Bootstrap runs and schedules the first wakeup at PERIOD. Ticks then end at PERIOD * (k + 1/2):
        // each one crosses exactly one wakeup deadline, never on a tie with the sleep's own wakeup.
        Runtime.SimulateSleep(PERIOD / 2);
    }

    // One period of simulated time. Returns the message of an exception that escaped the runtime, if any.
    TString Tick() {
        try {
            Runtime.SimulateSleep(PERIOD);
        } catch (...) {
            return CurrentExceptionMessage();
        }
        return {};
    }

    bool PusherAlive() const {
        return Runtime.FindActor(Pusher) != nullptr;
    }
};

TString Head(const TString& text, size_t size = 300) {
    return TString(TStringBuf(text).Head(size));
}

} // namespace

Y_UNIT_TEST_SUITE(THttpPoolCapPusherTest) {

    // T-ACT-1 (F-A-10, P1): the pusher survives an exception thrown by the shares provider. Tick 1 throws,
    // tick 2 returns {P1: 0.5}. Today the pusher has no IActorExceptionHandler (ydb/agents/NO_ABORT.md): the
    // exception escapes the actor into the runtime (in production: the process), and the next wakeup is
    // never scheduled. The fix (IActorExceptionHandler, log and recover) is not part of this package.
    Y_UNIT_TEST(SurvivesProviderException) {
        YDB_SKIP_KNOWN_BUG("F-A-10");
        const auto gateway = TScriptedHttpGateway::Make();
        ui32 providerCalls = 0;
        TPusherFixture fixture([&]() -> THashMap<NDq::TWorkScope, double> {
            if (++providerCalls == 1) {
                ythrow yexception() << "provider failed";
            }
            return {{Pool("P1"), 0.5}};
        }, gateway);

        const TString escaped1 = fixture.Tick();
        const TString escaped2 = fixture.Tick();
        UNIT_ASSERT_C(escaped1.empty() && escaped2.empty(), "an exception escaped the pusher (from the provider: "
            << escaped1.Contains("provider failed") << ")");
        UNIT_ASSERT_C(fixture.PusherAlive(), "the pusher died");
        UNIT_ASSERT_VALUES_EQUAL(providerCalls, 2);
        const auto updates = gateway->PoolCapsUpdates();
        UNIT_ASSERT_VALUES_EQUAL(updates.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(Render(updates[0]), "/default=500 db/P1=500");
    }

    // T-ACT-2 (F-A-12, P1): cap arithmetic. A share of 0 gives cap 0; a tiny positive share is rounded up to
    // 1; the default pool gets the leftover, floored at maxHandlers * minDefaultFraction (100). Shares
    // summing above 1 leave the default pool at its floor.
    Y_UNIT_TEST(CapArithmetic) {
        const auto gateway = TScriptedHttpGateway::Make();
        THashMap<NDq::TWorkScope, double> shares = {{Pool("A"), 0.0}, {Pool("B"), 0.0004}, {Pool("C"), 0.6}};
        TPusherFixture fixture([&]() { return shares; }, gateway);

        UNIT_ASSERT_VALUES_EQUAL(fixture.Tick(), "");
        shares = {{Pool("A"), 0.7}, {Pool("B"), 0.7}};
        UNIT_ASSERT_VALUES_EQUAL(fixture.Tick(), "");

        const auto updates = gateway->PoolCapsUpdates();
        UNIT_ASSERT_VALUES_EQUAL(updates.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(Render(updates[0]), "/default=399 db/A=0 db/B=1 db/C=600");
        UNIT_ASSERT_VALUES_EQUAL(Render(updates[1]), "/default=100 db/A=700 db/B=700");
    }

    // T-ACT-3 (F-A-10 area, P2): with the gateway gone the pusher does nothing but keeps ticking (each tick
    // re-schedules the next wakeup and logs "gateway is gone"); after TEvPoison it is dead and no further
    // wakeup is handled.
    Y_UNIT_TEST(GatewayGoneThenPoison) {
        TLogCapture log;
        auto gateway = TScriptedHttpGateway::Make();
        const IHTTPGateway::TWeakPtr weak = gateway;
        gateway.reset();
        UNIT_ASSERT(weak.expired());
        ui32 providerCalls = 0;
        TPusherFixture fixture([&]() -> THashMap<NDq::TWorkScope, double> {
            ++providerCalls;
            return {};
        }, weak);
        const TString gone = "HttpPoolCapPusher tick: gateway is gone";

        UNIT_ASSERT_VALUES_EQUAL(fixture.Tick(), "");
        UNIT_ASSERT_VALUES_EQUAL(fixture.Wakeups, 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Tick(), "");
        UNIT_ASSERT_VALUES_EQUAL(fixture.Wakeups, 2); // the second wakeup was scheduled by the first tick
        UNIT_ASSERT_VALUES_EQUAL_C(log.CountLines(gone), 2, Head(log.Text()));
        UNIT_ASSERT_VALUES_EQUAL(providerCalls, 0);

        const TActorId edge = fixture.Runtime.AllocateEdgeActor();
        fixture.Runtime.Send(new IEventHandle(fixture.Pusher, edge, new TEvents::TEvPoison()));
        UNIT_ASSERT_VALUES_EQUAL(fixture.Tick(), "");
        UNIT_ASSERT_C(!fixture.PusherAlive(), "the pusher survived TEvPoison");
        UNIT_ASSERT_VALUES_EQUAL(fixture.Tick(), "");
        UNIT_ASSERT_VALUES_EQUAL_C(log.CountLines(gone), 2, Head(log.Text()));
        UNIT_ASSERT_VALUES_EQUAL(providerCalls, 0);
    }

} // Y_UNIT_TEST_SUITE(THttpPoolCapPusherTest)

} // namespace NYql
