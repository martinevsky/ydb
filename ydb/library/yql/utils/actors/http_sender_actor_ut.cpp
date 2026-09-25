#include <library/cpp/testing/unittest/registar.h>

#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h>
#include <ydb/library/yql/utils/actors/http_sender_actor.h>
#include <ydb/library/yql/utils/actors/http_sender.h>

namespace NYql {

using namespace NActors;

namespace {

// Simulated time: TTestActorRuntimeBase drops every scheduled event by default; deliver the ones
// addressed to actors enabled with EnableScheduleForActor (as decorator_ut.cpp does).
bool ScheduleForEnabledActors(TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event, TDuration delay, TInstant& deadline) {
    if (runtime.IsScheduleForActorEnabled(event->GetRecipientRewrite())) {
        deadline = runtime.GetTimeProvider()->Now() + delay;
        return false;
    }
    return true;
}

struct TTestBootstrap {
    TTestActorRuntimeBase Runtime;
    TActorId SelfActorId;
    TActorId HttpSenderActorId;
    TActorId HttpProxyActorId;

    NYql::NDq::THttpSenderRetryPolicy::TPtr RetryPolicy;

    // useRealThreads = false gives simulated time: a scheduled retry fires only when the test
    // advances the time (SimulateSleep, or a Grab that has nothing else to dispatch).
    TTestBootstrap(NYql::NDq::THttpSenderRetryPolicy::TPtr retryPolicy, bool useRealThreads = true)
        : Runtime(1, useRealThreads)
        , SelfActorId(0, "SELF")
        , HttpSenderActorId(0, "SENDER")
        , HttpProxyActorId(0, "PROXY")
        , RetryPolicy(retryPolicy)
    {
        Runtime.Initialize();
        if (!useRealThreads) {
            Runtime.SetScheduledEventFilter(&ScheduleForEnabledActors);
        }

        SelfActorId = Runtime.AllocateEdgeActor();
        HttpProxyActorId = Runtime.AllocateEdgeActor();

        auto httpSender = NYql::NDq::CreateHttpSenderActor(
            SelfActorId,
            HttpProxyActorId,
            RetryPolicy
        );
        HttpSenderActorId = Runtime.Register(httpSender);
        Runtime.EnableScheduleForActor(HttpSenderActorId, true);
    }

    void SendHttpOutgoingRequest()
    {
        auto req = NHttp::THttpOutgoingRequest::CreateRequestGet("124");
        SendHttpOutgoingRequest(std::make_unique<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>(req));
    }

    void SendHttpOutgoingRequest(std::unique_ptr<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest> request)
    {
        auto sender = Runtime.AllocateEdgeActor();
        Runtime.Send(new IEventHandle(HttpSenderActorId, sender, request.release()));
    }

    template<typename T>
    std::pair<TAutoPtr<IEventHandle>, T*> Grab()
    {
        TAutoPtr<IEventHandle> handle;
        T* event = Runtime.GrabEdgeEvent<T>(handle, TDuration::Seconds(10));
        return {handle, event};
    }

    void HandleHttpProxyOutgoing(std::unique_ptr<NHttp::THttpIncomingResponse>&& response) {
        auto [_, event] = Grab<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>();
        ReplyToProxyRequest(event->Request, std::move(response));
    }

    void ReplyToProxyRequest(NHttp::THttpOutgoingRequestPtr request, std::unique_ptr<NHttp::THttpIncomingResponse>&& response) {
        Runtime.Send(new IEventHandle(
            HttpSenderActorId,
            HttpProxyActorId,
            new NHttp::TEvHttpProxy::TEvHttpIncomingResponse(request, response.release())));
    }

    // Simulated time only: the next event of type T for `edge` within `window`, or nullptr. A
    // sentinel wakeup scheduled past the window lets the runtime advance the clock beyond the
    // window even when nothing else is scheduled (an empty schedule would stall the dispatcher).
    template<typename T>
    typename T::TPtr GrabWithin(const TActorId& edge, TDuration window) {
        const TActorId sentinel = Runtime.AllocateEdgeActor();
        Runtime.Schedule(new IEventHandle(sentinel, sentinel, new TEvents::TEvWakeup()), window * 2);
        return Runtime.GrabEdgeEventIf<T>(edge, [](const typename T::TPtr&) { return true; }, window);
    }

    static std::unique_ptr<NHttp::THttpIncomingResponse> MakeResponse(TString status) {
        auto response = std::make_unique<NHttp::THttpIncomingResponse>(nullptr);
        response->Status = status;
        return response;
    }

    void RaiseHttpProxySuccessResponse() {
        auto response = std::make_unique<NHttp::THttpIncomingResponse>(nullptr);
        response->Status = "200";
        HandleHttpProxyOutgoing(std::move(response));
    }

    void RaiseHttpProxyErrorResponse() {
        auto response = std::make_unique<NHttp::THttpIncomingResponse>(nullptr);
        response->Status = "500";
        HandleHttpProxyOutgoing(std::move(response));
    }
};

} // namespace

Y_UNIT_TEST_SUITE(THttpSenderTests) {
    Y_UNIT_TEST(SuccessResponse)
    {
        auto retryPolicy = NYql::NDq::THttpSenderRetryPolicy::GetNoRetryPolicy();
        TTestBootstrap bootstrap(retryPolicy);
        bootstrap.SendHttpOutgoingRequest();

        auto response = std::make_unique<NHttp::THttpIncomingResponse>(nullptr);
        response->Status = "200";
        bootstrap.HandleHttpProxyOutgoing(std::move(response));

        auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
        UNIT_ASSERT(event->IsTerminal);
        UNIT_ASSERT_EQUAL(event->RetryCount, 0);
        UNIT_ASSERT(event->HttpIncomingResponse->Get()->GetError().empty());
    }

    Y_UNIT_TEST(FailResponse)
    {
        auto retryPolicy = NYql::NDq::THttpSenderRetryPolicy::GetNoRetryPolicy();
        TTestBootstrap bootstrap(retryPolicy);
        bootstrap.SendHttpOutgoingRequest();

        auto response = std::make_unique<NHttp::THttpIncomingResponse>(nullptr);
        response->Status = "500";
        bootstrap.HandleHttpProxyOutgoing(std::move(response));

        auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
        UNIT_ASSERT(event->IsTerminal);
        UNIT_ASSERT_EQUAL(event->RetryCount, 0);
        UNIT_ASSERT(!event->HttpIncomingResponse->Get()->GetError().empty());
    }

    Y_UNIT_TEST(RetrySuccess)
    {
        auto retryPolicy = NYql::NDq::THttpSenderRetryPolicy::GetExponentialBackoffPolicy(
            [](const NHttp::TEvHttpProxy::TEvHttpIncomingResponse*){
                return ERetryErrorClass::ShortRetry;
            });
        TTestBootstrap bootstrap(retryPolicy);

        bootstrap.SendHttpOutgoingRequest();

        bootstrap.RaiseHttpProxyErrorResponse();
        {
            auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
            UNIT_ASSERT(!event->IsTerminal);
            UNIT_ASSERT_EQUAL(event->RetryCount, 0);
            UNIT_ASSERT(!event->HttpIncomingResponse->Get()->GetError().empty());
        }

        bootstrap.RaiseHttpProxySuccessResponse();
        {
            auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
            UNIT_ASSERT(event->IsTerminal);
            UNIT_ASSERT_EQUAL(event->RetryCount, 1);
            UNIT_ASSERT(event->HttpIncomingResponse->Get()->GetError().empty());
        }
    }

    Y_UNIT_TEST(RetryUnsuccess)
    {
        auto retryPolicy = NYql::NDq::THttpSenderRetryPolicy::GetExponentialBackoffPolicy(
            [](const NHttp::TEvHttpProxy::TEvHttpIncomingResponse*){
                return ERetryErrorClass::ShortRetry;
            },
            TDuration::MilliSeconds(10),
            TDuration::MilliSeconds(200),
            TDuration::Seconds(30),
            1);
        TTestBootstrap bootstrap(retryPolicy);

        bootstrap.SendHttpOutgoingRequest();

        bootstrap.RaiseHttpProxyErrorResponse();
        {
            auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
            UNIT_ASSERT(!event->IsTerminal);
            UNIT_ASSERT_EQUAL(event->RetryCount, 0);
            UNIT_ASSERT(!event->HttpIncomingResponse->Get()->GetError().empty());
        }

        bootstrap.RaiseHttpProxyErrorResponse();
        {
            auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
            UNIT_ASSERT(event->IsTerminal);
            UNIT_ASSERT_EQUAL(event->RetryCount, 1);
            UNIT_ASSERT(!event->HttpIncomingResponse->Get()->GetError().empty());
        }
    }

    // T-RTY-11 (F-B-10): the request flags reach the proxy unchanged on every attempt. Today
    // SendRequestToProxy() rebuilds the event from (Request, Timeout) only, so AllowConnectionReuse,
    // UseHttp2 and StreamContentTypes are dropped (already on the first attempt).
    Y_UNIT_TEST(RetryPreservesRequestFlags)
    {
        YDB_SKIP_KNOWN_BUG("F-B-10");

        auto retryPolicy = NYql::NDq::THttpSenderRetryPolicy::GetFixedIntervalPolicy(
            [](const NHttp::TEvHttpProxy::TEvHttpIncomingResponse*){
                return ERetryErrorClass::ShortRetry;
            },
            TDuration::Seconds(1),
            TDuration::Seconds(1),
            1);
        TTestBootstrap bootstrap(retryPolicy, /*useRealThreads=*/false);

        const TDuration timeout = TDuration::Seconds(7);
        auto request = std::make_unique<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>(
            NHttp::THttpOutgoingRequest::CreateRequestGet("http://127.0.0.1/flags"), timeout);
        request->AllowConnectionReuse = true;
        request->UseHttp2 = true;
        request->StreamContentTypes = {"a"};
        bootstrap.SendHttpOutgoingRequest(std::move(request));

        for (ui32 attempt = 0; attempt < 2; ++attempt) {
            auto [_, event] = bootstrap.Grab<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>();
            UNIT_ASSERT_C(event, "the proxy received no request for attempt " << attempt);
            UNIT_ASSERT_VALUES_EQUAL_C(event->Timeout, timeout, "attempt " << attempt);
            UNIT_ASSERT_C(event->AllowConnectionReuse, "AllowConnectionReuse dropped on attempt " << attempt);
            UNIT_ASSERT_C(event->UseHttp2, "UseHttp2 dropped on attempt " << attempt);
            UNIT_ASSERT_VALUES_EQUAL_C(event->StreamContentTypes.size(), 1, "StreamContentTypes dropped on attempt " << attempt);
            UNIT_ASSERT_VALUES_EQUAL(event->StreamContentTypes[0], "a");

            // Attempt 0 fails and is retried after the (simulated) 1 s delay; attempt 1 succeeds.
            bootstrap.ReplyToProxyRequest(event->Request, TTestBootstrap::MakeResponse(attempt == 0 ? "500" : "200"));
            auto [__, result] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
            UNIT_ASSERT(result);
            UNIT_ASSERT_VALUES_EQUAL(result->RetryCount, attempt);
            UNIT_ASSERT_VALUES_EQUAL(result->IsTerminal, attempt == 1);
        }
    }

    // T-ACT-4: the sender is poisoned while a retry is scheduled. After the (simulated) retry time
    // has passed, the proxy has seen no second request, the requester got no further result and
    // the sender actor is gone.
    Y_UNIT_TEST(PoisonDuringScheduledRetry)
    {
        auto retryPolicy = NYql::NDq::THttpSenderRetryPolicy::GetFixedIntervalPolicy(
            [](const NHttp::TEvHttpProxy::TEvHttpIncomingResponse*){
                return ERetryErrorClass::ShortRetry;
            },
            TDuration::Seconds(1),
            TDuration::Seconds(1));
        TTestBootstrap bootstrap(retryPolicy, /*useRealThreads=*/false);
        auto& runtime = bootstrap.Runtime;

        bootstrap.SendHttpOutgoingRequest();
        bootstrap.RaiseHttpProxyErrorResponse();
        {
            auto [_, event] = bootstrap.Grab<NYql::NDq::TEvHttpBase::TEvSendResult>();
            UNIT_ASSERT(event);
            UNIT_ASSERT(!event->IsTerminal);
        }

        // The retry wakeup is scheduled 1 s ahead (simulated). The poison is queued now, so it is
        // delivered before the time reaches the wakeup.
        runtime.Send(new IEventHandle(bootstrap.HttpSenderActorId, bootstrap.SelfActorId, new TEvents::TEvPoison()));

        // Simulated time: the runtime dispatches the poison, then advances the clock past the 1 s
        // retry wakeup; no request may reach the proxy and no result the requester.
        UNIT_ASSERT_C(!bootstrap.GrabWithin<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>(bootstrap.HttpProxyActorId, TDuration::Seconds(5)),
            "the proxy received a retry after the sender was poisoned");
        UNIT_ASSERT_C(!bootstrap.GrabWithin<NYql::NDq::TEvHttpBase::TEvSendResult>(bootstrap.SelfActorId, TDuration::Seconds(1)),
            "the requester received a result after the sender was poisoned");
        UNIT_ASSERT_C(!runtime.FindActor(bootstrap.HttpSenderActorId), "the sender actor is still alive");
    }
};

} // namespace NYql
