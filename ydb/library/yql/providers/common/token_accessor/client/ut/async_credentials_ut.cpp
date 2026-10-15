#include <ydb/library/yql/providers/common/token_accessor/client/async_credentials.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <library/cpp/testing/unittest/registar.h>

#include <memory>

namespace NYql {
namespace {
using namespace NActors;

struct TWaitState {
    std::weak_ptr<TString> Payload;
    ui32 Started = 0;
    ui32 Completed = 0;
    ui32 Destroyed = 0;
    TString Token;
};

class TWaitingConsumer final : public TActorBootstrapped<TWaitingConsumer> {
public:
    TWaitingConsumer(NThreading::TFuture<TString> future, std::shared_ptr<TWaitState> state)
        : Future(std::move(future)), State(std::move(state)) {}
    ~TWaitingConsumer() { ++State->Destroyed; }

    void Bootstrap() {
        Become(&TThis::StateWork);
        auto payload = std::make_shared<TString>(65536, 'x');
        State->Payload = payload;
        AwaitCredentials(TActivationContext::ActorSystem(), SelfId(), Future,
            [this, payload = std::move(payload)](const auto& ready) {
                UNIT_ASSERT_VALUES_EQUAL(payload->size(), 65536);
                State->Token = ready.GetValue();
                ++State->Completed;
            });
        ++State->Started;
    }

private:
    STRICT_STFUNC(StateWork,
        hFunc(TEvents::TEvInvokeResult, Handle);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )
    void Handle(TEvents::TEvInvokeResult::TPtr& event) {
        event->Get()->Process(ActorContext());
    }
    NThreading::TFuture<TString> Future;
    const std::shared_ptr<TWaitState> State;
};

void DispatchUntil(TTestActorRuntimeBase& runtime, std::function<bool()> condition) {
    TDispatchOptions options;
    options.CustomFinalCondition = condition;
    runtime.DispatchEvents(options, TDuration::Seconds(1));
    UNIT_ASSERT(condition());
}
} // namespace

Y_UNIT_TEST_SUITE(AsynchronousCredentialOwnership) {
    Y_UNIT_TEST(CancellationReleasesPayloadBeforeCredentialReply) {
        TTestActorRuntimeBase runtime;
        runtime.Initialize();
        auto promise = NThreading::NewPromise<TString>();
        auto state = std::make_shared<TWaitState>();
        const auto actor = runtime.Register(new TWaitingConsumer(promise.GetFuture(), state));
        DispatchUntil(runtime, [&] { return state->Started == 1; });
        UNIT_ASSERT(!state->Payload.expired());
        runtime.GetActorSystem(0)->Send(actor, new TEvents::TEvPoison);
        DispatchUntil(runtime, [&] { return state->Destroyed == 1; });
        UNIT_ASSERT_C(state->Payload.expired(), "Cancelled consumer payload is still owned by the pending credential future");
        UNIT_ASSERT_VALUES_EQUAL(state->Completed, 0);
        promise.SetValue("late-token");
        runtime.SimulateSleep(TDuration::MilliSeconds(1));
        UNIT_ASSERT_VALUES_EQUAL(state->Completed, 0);
    }

    Y_UNIT_TEST(CancellingOneConsumerPreservesSharedAcquisition) {
        TTestActorRuntimeBase runtime;
        runtime.Initialize();
        auto promise = NThreading::NewPromise<TString>();
        auto cancelled = std::make_shared<TWaitState>();
        auto survivor = std::make_shared<TWaitState>();
        const auto first = runtime.Register(new TWaitingConsumer(promise.GetFuture(), cancelled));
        runtime.Register(new TWaitingConsumer(promise.GetFuture(), survivor));
        DispatchUntil(runtime, [&] { return cancelled->Started == 1 && survivor->Started == 1; });
        runtime.GetActorSystem(0)->Send(first, new TEvents::TEvPoison);
        DispatchUntil(runtime, [&] { return cancelled->Destroyed == 1; });
        UNIT_ASSERT(cancelled->Payload.expired());
        UNIT_ASSERT(!survivor->Payload.expired());
        promise.SetValue("shared-token");
        DispatchUntil(runtime, [&] { return survivor->Completed == 1; });
        UNIT_ASSERT_VALUES_EQUAL(cancelled->Completed, 0);
        UNIT_ASSERT_VALUES_EQUAL(survivor->Token, "shared-token");
        UNIT_ASSERT(survivor->Payload.expired());
    }
}
} // namespace NYql
