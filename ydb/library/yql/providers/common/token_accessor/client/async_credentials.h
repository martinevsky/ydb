#pragma once

#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/library/actors/core/invoke.h>
#include <library/cpp/threading/future/future.h>

namespace NYql {

// Only the ready credential crosses the thread boundary. The caller's request
// and payload stay in its coroutine frame and are released on actor cancellation.
// Actor-system shutdown must still outlive the external credential producer.
template <class T>
NActors::async<NThreading::TFuture<T>> AwaitCredentials(NThreading::TFuture<T> future) {
    const auto cookie = NActors::AllocateWaitCookie();
    auto* actorSystem = NActors::TActivationContext::ActorSystem();
    const auto actor = NActors::TActivationContext::AsActorContext().SelfID;
    future.Subscribe([actorSystem, actor, cookie](const NThreading::TFuture<T>& ready) {
        actorSystem->Send(new NActors::IEventHandle(actor, {}, new NActors::TEvents::TEvInvokeResult(
            [](auto&, const NActors::TActorContext&) {},
            [ready] { return ready; }), 0, cookie));
    });
    auto event = co_await NActors::ActorWaitForEvent<NActors::TEvents::TEvInvokeResult>(cookie);
    using TReady = NThreading::TFuture<T> (*)(const NActors::TActorContext&);
    co_return event->Get()->template GetResult<TReady>();
}

} // namespace NYql
