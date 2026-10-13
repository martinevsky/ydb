#include <ydb/core/security/iam_delegation_lifecycle/client.h>
#include <ydb/core/security/iam_delegation_lifecycle/operations.h>
#include <ydb/core/tx/iam_delegation/ut/requests.h>

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

Y_UNIT_TEST_SUITE(IamDelegationTabletClient) {
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
