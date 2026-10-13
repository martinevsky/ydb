#pragma once

#include <ydb/library/actors/core/invoke.h>
#include <library/cpp/threading/future/future.h>

namespace NYql {

// Resume on the requesting actor's mailbox. A destroyed actor simply discards the
// event; the future callback never accesses captured actor state on another thread.
// The recipient must handle TEvInvokeResult and call Process on it.
template <class T, class TCallback>
void AwaitCredentials(NActors::TActorSystem* actorSystem, const NActors::TActorId& actor,
    const NThreading::TFuture<T>& future, TCallback callback) {
    future.Subscribe([actorSystem, actor, callback = std::move(callback)](const NThreading::TFuture<T>& result) mutable {
        actorSystem->Send(new NActors::IEventHandle(actor, {}, new NActors::TEvents::TEvInvokeResult(
            [callback = std::move(callback), result](auto&, const NActors::TActorContext&) mutable { callback(result); },
            [] {})));
    });
}

} // namespace NYql
