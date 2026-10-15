#include <ydb/library/actors/core/hfunc.h>
#include <ydb/core/security/iam_delegation/lifecycle/client.h>
#include <ydb/core/security/iam_delegation/lifecycle/operations.h>
#include <ydb/core/security/iam_delegation/tablet/ut/requests.h>

namespace NKikimr::NIamDelegation::NTests {

namespace {
class TOperationCaller final : public NActors::TActorBootstrapped<TOperationCaller> {
public:
    TOperationCaller(NActors::TActorId recipient, bool polling)
        : Recipient(recipient), Polling(polling) {}

    void Bootstrap() {
        NActors::IEventBase* request = Polling
            ? static_cast<NActors::IEventBase*>(new TEvIamDelegation::TEvPollDelegationOperation("known-operation"))
            : static_cast<NActors::IEventBase*>(new TEvIamDelegation::TEvSubmitDelegationOperation(
                EDelegationOperationKind::Setup, TDelegationBinding{}, "subject"));
        auto result = co_await CallIamOperation(request);
        Send(Recipient, new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)));
        PassAway();
    }
private:
    const NActors::TActorId Recipient;
    const bool Polling;
};
} // namespace

class TTabletCaller final : public NActors::TActorBootstrapped<TTabletCaller> {
public:
    explicit TTabletCaller(NActors::TActorId recipient) : Recipient(recipient) {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        auto reply = co_await CallIamDelegationTablet(TTestContext::TabletId, ListDatabasesRequest());
        auto event = MakeHolder<TEvIamDelegationTablet::TEvResponse>();
        event->Record = std::move(reply);
        Send(Recipient, event.Release());
        PassAway();
    }

    STRICT_STFUNC(StateWork,
        IgnoreFunc(TEvIamDelegationTablet::TEvResponse);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
    )

private:
    const NActors::TActorId Recipient;
};


Y_UNIT_TEST_SUITE(IamDelegationTabletClient) {
    Y_UNIT_TEST(CancellingCoroutineClosesItsTabletClientBeforeTimeout) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        auto& runtime = ctx.GetRuntime();
        const auto edge = runtime.AllocateEdgeActor();
        TActorId caller;
        TActorId client;
        TAutoPtr<IEventHandle> heldReply;
        ui32 returnedResults = 0;
        ui32 childStops = 0;
        auto requests = runtime.AddObserver<TEvIamDelegationTablet::TEvRequest>([&](auto& event) {
            if (event->Sender == caller) {
                UNIT_ASSERT(!client);
                client = event->GetRecipientRewrite();
                runtime.EnableScheduleForActor(client);
            }
        });
        auto responses = runtime.AddObserver<TEvIamDelegationTablet::TEvResponse>([&](auto& event) {
            if (event->GetRecipientRewrite() == client && !heldReply) {
                heldReply.Reset(event.Release());
            } else if (event->GetRecipientRewrite() == edge) {
                ++returnedResults;
                event.Reset();
            }
        });
        auto stops = runtime.AddObserver<NActors::TEvents::TEvPoison>([&](auto& event) {
            if (event->GetRecipientRewrite() == client) {
                ++childStops;
            }
        });
        caller = runtime.Register(new TTabletCaller(edge));
        runtime.EnableScheduleForActor(caller);
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return bool(heldReply); };
        runtime.DispatchEvents(options, TDuration::Seconds(1));
        UNIT_ASSERT(heldReply);
        UNIT_ASSERT(client);
        UNIT_ASSERT(runtime.FindActor(client));
        runtime.Send(new IEventHandle(caller, {}, new NActors::TEvents::TEvPoison), 0, true);
        options.CustomFinalCondition = [&] { return childStops == 1; };
        runtime.DispatchEvents(options, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL_C(childStops, 1,
            "The cancelled coroutine left its tablet client alive until the 30-second timeout");
        runtime.SimulateSleep(TDuration::MilliSeconds(1));
        UNIT_ASSERT(!runtime.FindActor(client));
        UNIT_ASSERT(!runtime.FindActor(caller));
        runtime.Send(heldReply.Release());
        runtime.SimulateSleep(TDuration::Seconds(31));
        UNIT_ASSERT_VALUES_EQUAL(returnedResults, 0);
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListDatabasesRequest()).DatabasesSize(), 1);
    }

    Y_UNIT_TEST(MissingLocalIamServiceProvesSubmitWasNotDispatched) {
        TTestContext ctx;
        const auto edge = ctx.GetRuntime().AllocateEdgeActor();
        ctx.GetRuntime().Register(new TOperationCaller(edge, false));
        const auto response = ctx.GetRuntime().GrabEdgeEvent<TEvIamDelegation::TEvDelegationOperationResult>(edge);
        UNIT_ASSERT(response->Get()->Result.Outcome == EDelegationOperationOutcome::Failed);
        UNIT_ASSERT_VALUES_EQUAL(response->Get()->Result.Status, Ydb::StatusIds::UNAVAILABLE);
    }

    Y_UNIT_TEST(MissingLocalIamServiceDoesNotProveKnownOperationFailed) {
        TTestContext ctx;
        const auto edge = ctx.GetRuntime().AllocateEdgeActor();
        ctx.GetRuntime().Register(new TOperationCaller(edge, true));
        const auto response = ctx.GetRuntime().GrabEdgeEvent<TEvIamDelegation::TEvDelegationOperationResult>(edge);
        UNIT_ASSERT(response->Get()->Result.Outcome == EDelegationOperationOutcome::Unknown);
        UNIT_ASSERT_VALUES_EQUAL(response->Get()->Result.Status, Ydb::StatusIds::UNAVAILABLE);
    }

    Y_UNIT_TEST(ForwardsCookieAndDurableReply) {
        TTestContext ctx;
        const auto edge = ctx.GetRuntime().AllocateEdgeActor();
        const auto client = ctx.GetRuntime().Register(CreateIamDelegationTabletRequestActor(ctx.TabletId));
        auto request = MakeHolder<TEvIamDelegationTablet::TEvRequest>();
        request->Record = RegisterRequest(DatabaseIdentity());
        ctx.GetRuntime().Send(new IEventHandle(client, edge, request.Release(), 0, 719), 0, true);
        const auto reply = ctx.GetRuntime().GrabEdgeEvent<TEvIamDelegationTablet::TEvResponse>(edge);
        UNIT_ASSERT_VALUES_EQUAL(reply->Cookie, 719);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->Record.GetStatus(), NProto::SUCCESS);
        ctx.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListDatabasesRequest()).DatabasesSize(), 1);
    }

    Y_UNIT_TEST(NewRequestResolvesRebootedTablet) {
        TTestContext ctx;
        ctx.Call(RegisterRequest(DatabaseIdentity()));
        ctx.Reboot();
        const auto edge = ctx.GetRuntime().AllocateEdgeActor();
        const auto client = ctx.GetRuntime().Register(CreateIamDelegationTabletRequestActor(ctx.TabletId));
        auto request = MakeHolder<TEvIamDelegationTablet::TEvRequest>();
        request->Record = ListDatabasesRequest();
        ctx.GetRuntime().Send(new IEventHandle(client, edge, request.Release()), 0, true);
        const auto reply = ctx.GetRuntime().GrabEdgeEvent<TEvIamDelegationTablet::TEvResponse>(edge);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->Record.GetStatus(), NProto::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->Record.DatabasesSize(), 1);
    }

    Y_UNIT_TEST(LostDurableReplyDoesNotReplayMutationAfterDisconnect) {
        TTestContext ctx;
        auto& runtime = ctx.GetRuntime();
        const auto edge = runtime.AllocateEdgeActor();
        const auto client = runtime.Register(CreateIamDelegationTabletRequestActor(ctx.TabletId));
        ui32 mutations = 0;
        bool replyLost = false;
        auto requests = runtime.AddObserver<TEvIamDelegationTablet::TEvRequest>([&](auto& event) {
            if (event->GetRecipientRewrite() == ctx.TabletActor() && event->Get()->Record.HasRegisterDatabase()) {
                ++mutations;
            }
        });
        auto replies = runtime.AddObserver<TEvIamDelegationTablet::TEvResponse>([&](auto& event) {
            if (event->GetRecipientRewrite() == client && event->Get()->Record.GetStatus() == NProto::SUCCESS) {
                replyLost = true;
                event.Reset();
            }
        });
        auto request = MakeHolder<TEvIamDelegationTablet::TEvRequest>();
        request->Record = RegisterRequest(DatabaseIdentity());
        runtime.Send(new IEventHandle(client, edge, request.Release(), 0, 991), 0, true);
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return replyLost; };
        runtime.DispatchEvents(options, TDuration::Seconds(30));
        UNIT_ASSERT(replyLost);
        ctx.Reboot();
        const auto reply = runtime.GrabEdgeEvent<TEvIamDelegationTablet::TEvResponse>(edge);
        UNIT_ASSERT_VALUES_EQUAL(reply->Cookie, 991);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->Record.GetStatus(), NProto::STATUS_UNSPECIFIED);
        UNIT_ASSERT_VALUES_EQUAL(mutations, 1);
        // The transport reports an uncertain result even though the original write committed.
        UNIT_ASSERT_VALUES_EQUAL(ctx.Call(ListDatabasesRequest()).DatabasesSize(), 1);
    }
}

} // namespace NKikimr::NIamDelegation::NTests
