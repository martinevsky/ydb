#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation/lifecycle/worker.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/tablet/ut/requests.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>

#include <util/generic/algorithm.h>
#include <util/generic/hash_set.h>
#include <util/generic/scope.h>
#include <util/generic/set.h>
#include <util/random/random.h>

#include <deque>

namespace NKikimr::NIamDelegation::NTests {
namespace {

class TWorkerFixture {
public:
    TTestContext Tablet;
    TTestBasicRuntime& Runtime = Tablet.GetRuntime();
    TActorId Iam = Runtime.AllocateEdgeActor();
    TActorId Schema = Runtime.AllocateEdgeActor();
    TActorId Worker;
    std::deque<TAutoPtr<IEventHandle>> Submissions;
    std::deque<TAutoPtr<IEventHandle>> Polls;
    std::deque<TAutoPtr<IEventHandle>> Descriptions;
    std::deque<TAutoPtr<IEventHandle>> SchemaDrops;
    THashMap<TString, NProto::TDelegation> LastDelegations;
    NProto::TSecretRecord LastSecret;
    TVector<NProto::TRequest> Commands;
    ui32 SubmissionCount = 0;
    ui32 PollCount = 0;
    ui32 Claims = 0;
    ui32 InventoryPages = 0;
    ui32 StaleFinishes = 0;

    TWorkerFixture() {
        Runtime.RegisterService(MakeIamDelegationServiceId(), Iam);
        Runtime.RegisterService(MakeTxProxyID(), Schema);
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& event) {
            using EAction = TTestActorRuntimeBase::EEventAction;
            switch (event->GetTypeRewrite()) {
                case TEvIamDelegation::TEvSubmitDelegationOperation::EventType:
                    ++SubmissionCount;
                    Submissions.emplace_back(event.Release());
                    return EAction::DROP;
                case TEvIamDelegation::TEvPollDelegationOperation::EventType:
                    ++PollCount;
                    Polls.emplace_back(event.Release());
                    return EAction::DROP;
                case TEvTxUserProxy::TEvNavigate::EventType:
                    Descriptions.emplace_back(event.Release());
                    return EAction::DROP;
                case TEvTxUserProxy::TEvProposeTransaction::EventType:
                    if (event->GetRecipientRewrite() != Schema && event->Recipient != MakeTxProxyID()) {
                        break;
                    }
                    SchemaDrops.emplace_back(event.Release());
                    return EAction::DROP;
                case TEvIamDelegationTablet::TEvRequest::EventType: {
                    if (event->GetRecipientRewrite() != Tablet.TabletActor()) {
                        break;
                    }
                    const auto& request = event->Get<TEvIamDelegationTablet::TEvRequest>()->Record;
                    Commands.push_back(request);
                    Claims += request.HasClaimRevocations();
                    InventoryPages += request.HasListInventory();
                    if (request.HasFinishRevocation()) {
                        FinishCookies.insert(event->Cookie);
                    }
                    break;
                }
                case TEvIamDelegationTablet::TEvResponse::EventType: {
                    const auto& result = event->Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                    if (result.GetStatus() == NProto::SUCCESS && result.HasDelegation()) {
                        LastDelegations[result.GetDelegation().GetOperationId()] = result.GetDelegation();
                    }
                    if (result.GetStatus() == NProto::SUCCESS && result.HasSecret()) {
                        LastSecret = result.GetSecret();
                    }
                    if (FinishCookies.erase(event->Cookie) && result.GetStatus() == NProto::STALE_CLAIM) {
                        ++StaleFinishes;
                    }
                    break;
                }
                case TEvents::TEvPoison::EventType:
                    if (event->GetRecipientRewrite() == Worker) {
                        Stopped = true;
                    }
                    break;
            }
            return EAction::PROCESS;
        });
    }

    ~TWorkerFixture() {
        Runtime.SetObserverFunc(TTestActorRuntimeBase::DefaultObserverFunc);
    }

    void Start() {
        Stopped = false;
        Worker = Runtime.Register(CreateIamDelegationRecoveryWorker(Tablet.TabletId));
        Runtime.EnableScheduleForActor(Worker);
    }

    void Stop() {
        Runtime.Send(new IEventHandle(Worker, {}, new TEvents::TEvPoison), 0, true);
        Until([&] { return Stopped; });
    }

    template<class TCondition>
    void Until(TCondition condition) {
        if (!condition()) {
            TDispatchOptions options;
            options.CustomFinalCondition = condition;
            options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
            Runtime.DispatchEvents(options, TDuration::Minutes(10));
        }
        UNIT_ASSERT(condition());
    }

    TAutoPtr<IEventHandle> Take(std::deque<TAutoPtr<IEventHandle>>& queue) {
        Until([&] { return !queue.empty(); });
        TAutoPtr<IEventHandle> event(queue.front().Release());
        queue.pop_front();
        return event;
    }

    void ReplyIam(const IEventHandle& request, EDelegationOperationOutcome outcome, const TString& operationId = {}) {
        TDelegationOperationResult result;
        result.Outcome = outcome;
        result.OperationId = operationId;
        result.Status = outcome == EDelegationOperationOutcome::Failed
            ? Ydb::StatusIds::UNAUTHORIZED : Ydb::StatusIds::SUCCESS;
        Runtime.Send(new IEventHandle(request.Sender, Iam,
            new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, request.Cookie));
    }

    void ReplySchema(const IEventHandle& request, NKikimrScheme::TEvDescribeSchemeResult record) {
        auto reply = MakeHolder<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>();
        *reply->MutableRecord() = std::move(record);
        Runtime.Send(new IEventHandle(request.Sender, Schema, reply.Release(), 0, request.Cookie));
    }

    static NKikimrScheme::TEvDescribeSchemeResult MatchingSchema(
        const NProto::TDelegation& delegation, const TString& createOperationId = "create-1")
    {
        NKikimrScheme::TEvDescribeSchemeResult record;
        record.SetStatus(NKikimrScheme::StatusSuccess);
        auto* path = record.MutablePathDescription();
        auto* self = path->MutableSelf();
        self->SetPathType(NKikimrSchemeOp::EPathTypeSecret);
        self->SetSchemeshardId(delegation.HasSecret() ? delegation.GetSecret().GetPathOwnerId() : 100);
        self->SetPathId(delegation.HasSecret() ? delegation.GetSecret().GetPathLocalId() : 42);
        self->SetCreateTxId(delegation.GetCreateTxId());
        self->SetCreateFinished(true);
        auto* reference = path->MutableSecretDescription()->MutableIamDelegation();
        reference->SetTabletId(TTestContext::TabletId);
        reference->SetDatabaseIncarnation(delegation.GetDatabaseIncarnation());
        reference->SetCreateOperationId(createOperationId);
        return record;
    }

    void WaitState(const TString& operationId, NProto::EDelegationState state) {
        Until([&] {
            const auto it = LastDelegations.find(operationId);
            return it != LastDelegations.end() && it->second.GetState() == state;
        });
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(Tablet, operationId).GetState(), state);
    }

private:
    THashSet<ui64> FinishCookies;
    bool Stopped = false;
};

NProto::TDelegation CreateUnknown(TWorkerFixture& f, const TString& remoteOperationId) {
    f.Tablet.Call(RegisterRequest(DatabaseIdentity()));
    const auto staged = f.Tablet.Call(StageCreateRequest()).GetDelegation();
    const auto bound = f.Tablet.Call(BindRequest(staged)).GetDelegation();
    const auto started = f.Tablet.Call(StartRequest(bound)).GetDelegation();
    return f.Tablet.Call(SetupResultRequest(started, NProto::SETUP_UNKNOWN, remoteOperationId)).GetDelegation();
}

enum class ERefusalDelivery { Submission, Poll };

void CheckWorkerFailureDetails(ERefusalDelivery delivery) {
    TWorkerFixture f;
    f.Tablet.Call(RegisterRequest(DatabaseIdentity()));
    auto stage = StageCreateRequest();
    stage.MutableStage()->SetSubjectId("original-cloud-subject");
    auto delegation = f.Tablet.Call(stage).GetDelegation();
    delegation = f.Tablet.Call(BindRequest(delegation)).GetDelegation();
    f.Start();
    auto describe = f.Take(f.Descriptions);
    f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(delegation));
    auto request = f.Take(f.Submissions);
    UNIT_ASSERT_VALUES_EQUAL(request->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->SubjectId,
        "original-cloud-subject");
    if (delivery == ERefusalDelivery::Poll) {
        f.ReplyIam(*request, EDelegationOperationOutcome::Accepted, "refused-operation");
        request = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(request->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId,
            "refused-operation");
    }
    TDelegationOperationResult failure;
    failure.Outcome = EDelegationOperationOutcome::Failed;
    failure.Status = Ydb::StatusIds::UNAUTHORIZED;
    failure.OperationId = "refused-operation";
    failure.Issues.AddIssue("credential-sentinel-do-not-persist");
    failure.FailureReasons = {EDelegationFailureReason::SubjectNotDelegatable,
        EDelegationFailureReason::SubjectNotDelegatable};
    f.Runtime.Send(new IEventHandle(request->Sender, f.Iam,
        new TEvIamDelegation::TEvDelegationOperationResult(std::move(failure)), 0, request->Cookie));
    f.WaitState("create-1", NProto::CANCELLED);
    f.Stop();
    f.Tablet.Reboot();
    const auto saved = GetDelegation(f.Tablet, "create-1");
    UNIT_ASSERT(saved.HasSetupFailure());
    UNIT_ASSERT_VALUES_EQUAL(saved.GetSetupFailure().GetStatus(), Ydb::StatusIds::UNAUTHORIZED);
    UNIT_ASSERT_VALUES_EQUAL(saved.GetSetupFailure().ReasonsSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(saved.GetSetupFailure().GetReasons(0), NProto::SETUP_FAILURE_SUBJECT_NOT_DELEGATABLE);
    UNIT_ASSERT_VALUES_EQUAL(saved.GetSetupOperationId(), "refused-operation");
    UNIT_ASSERT_VALUES_EQUAL(saved.GetBinding().SerializeAsString(), delegation.GetBinding().SerializeAsString());
    UNIT_ASSERT(!TString(saved.SerializeAsString()).Contains("credential-sentinel-do-not-persist"));
    UNIT_ASSERT(GetSecret(f.Tablet).GetPendingOperationId().empty());
    UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 1);
    UNIT_ASSERT_VALUES_EQUAL(f.Tablet.Call(ClaimRequest()).DelegationsSize(), 0);
}

} // namespace

Y_UNIT_TEST_SUITE(IamDelegationRecoveryWorker) {
Y_UNIT_TEST(WorkersDisperseScansBeforeClaimingWithoutReducingMinimumPause) {
    TWorkerFixture f;
    SetRandomSeed(55683);
    THashMap<TString, TVector<TInstant>> claims;
    auto observe = f.Runtime.AddObserver<TEvIamDelegationTablet::TEvRequest>([&](auto& event) {
        if (event->GetRecipientRewrite() == f.Tablet.TabletActor()
            && event->Get()->Record.HasClaimRevocations()) {
            const auto& request = event->Get()->Record.GetClaimRevocations();
            UNIT_ASSERT_VALUES_EQUAL(request.GetLimit(), 1);
            claims[request.GetWorkerId()].push_back(f.Runtime.GetCurrentTime());
        }
    });
    const auto started = f.Runtime.GetCurrentTime();
    TVector<TActorId> workers;
    for (ui32 index = 0; index < 32; ++index) {
        const auto worker = f.Runtime.Register(CreateIamDelegationRecoveryWorker(f.Tablet.TabletId));
        f.Runtime.EnableScheduleForActor(worker);
        workers.push_back(worker);
    }
    Y_DEFER {
        for (const auto& worker : workers) {
            f.Runtime.Send(new IEventHandle(worker, {}, new TEvents::TEvPoison));
        }
    };
    f.Until([&] {
        return claims.size() == workers.size()
            && AllOf(claims, [](const auto& item) { return item.second.size() >= 2; });
    });
    TSet<ui64> firstDispatches;
    for (const auto& [worker, times] : claims) {
        Y_UNUSED(worker);
        UNIT_ASSERT(times[0] >= started + TDuration::Seconds(1));
        UNIT_ASSERT(times[0] < started + TDuration::MilliSeconds(1250));
        UNIT_ASSERT(times[1] >= times[0] + TDuration::Seconds(1));
        UNIT_ASSERT(times[1] < times[0] + TDuration::MilliSeconds(1250));
        firstDispatches.insert((times[0] - started).MicroSeconds());
    }
    UNIT_ASSERT_C(firstDispatches.size() > 8,
        "Independent workers still synchronize their initial revocation scans");
    UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
    UNIT_ASSERT_VALUES_EQUAL(f.PollCount, 0);
}

    Y_UNIT_TEST(RevocationOutcomeHistorySurvivesWorkerAndTabletTakeover) {
        TWorkerFixture f;
        CreateActive(f.Tablet);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Start();
        auto request = f.Take(f.Submissions);
        f.ReplyIam(*request, EDelegationOperationOutcome::Accepted, "saved-revoke");
        f.Until([&] { return f.LastDelegations["create-1"].GetRevokeOperationId() == "saved-revoke"; });
        const auto queuedAt = f.LastDelegations["create-1"].GetCleanupQueuedAtUs();
        UNIT_ASSERT(queuedAt != 0);
        f.Stop();

        for (ui32 failure = 1; failure <= 4; ++failure) {
            f.Tablet.Reboot();
            const auto pending = GetDelegation(f.Tablet, "create-1");
            f.Runtime.UpdateCurrentTime(Max(f.Runtime.GetCurrentTime(), TInstant::MicroSeconds(pending.GetDueAtUs())));
            f.Start();
            auto poll = f.Take(f.Polls);
            UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "saved-revoke");
            // A transport failure with no returned ID cannot forget the saved ID.
            f.ReplyIam(*poll, EDelegationOperationOutcome::Unknown);
            f.Until([&] { return f.LastDelegations["create-1"].GetRevokeConsecutiveFailures() == failure; });
            const auto saved = GetDelegation(f.Tablet, "create-1");
            UNIT_ASSERT_VALUES_EQUAL(saved.GetState(), NProto::REVOCATION_PENDING);
            UNIT_ASSERT_VALUES_EQUAL(saved.GetRevokeOperationId(), "saved-revoke");
            UNIT_ASSERT_VALUES_EQUAL(saved.GetCleanupQueuedAtUs(), queuedAt);
            UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 1);
            f.Stop();
        }

        // Remote progress resets the streak without resetting age or submitting
        // another mutation; a later definitive failure permits a new submission.
        f.Start();
        auto poll = f.Take(f.Polls);
        f.ReplyIam(*poll, EDelegationOperationOutcome::Accepted, "saved-revoke");
        f.Until([&] { return f.LastDelegations["create-1"].GetRevokeConsecutiveFailures() == 0; });
        poll = f.Take(f.Polls);
        f.ReplyIam(*poll, EDelegationOperationOutcome::Failed, "saved-revoke");
        f.Until([&] {
            const auto& current = f.LastDelegations["create-1"];
            return current.GetRevokeConsecutiveFailures() == 1 && current.GetRevokeOperationId().empty();
        });
        request = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 2);
        f.ReplyIam(*request, EDelegationOperationOutcome::Succeeded, "replacement-revoke");
        f.WaitState("create-1", NProto::REVOKED);
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetCleanupQueuedAtUs(), queuedAt);
        f.Stop();
    }

    Y_UNIT_TEST(FailingRevocationDoesNotHoldAnotherRecordsLease) {
        TWorkerFixture f;
        CreateActive(f.Tablet);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        auto second = f.Tablet.Call(StageCreateRequest("create-2", Binding("second-referrer"),
            "/Root/database/second", "database-incarnation-1", 20)).GetDelegation();
        second = f.Tablet.Call(BindRequest(second, SecretIdentity(43))).GetDelegation();
        CompleteSetup(f.Tablet, second);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet, SecretIdentity(43))));
        f.Start();
        auto firstRequest = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(firstRequest->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId, "create-1");
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-2").GetState(), NProto::REVOCATION_PENDING);
        f.ReplyIam(*firstRequest, EDelegationOperationOutcome::Failed);
        auto secondRequest = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(secondRequest->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId, "create-2");
        const auto retried = GetDelegation(f.Tablet, "create-1");
        UNIT_ASSERT_VALUES_EQUAL(retried.GetState(), NProto::REVOCATION_PENDING);
        UNIT_ASSERT_VALUES_EQUAL(retried.GetRevokeConsecutiveFailures(), 1);
        f.ReplyIam(*secondRequest, EDelegationOperationOutcome::Succeeded);
        f.WaitState("create-2", NProto::REVOKED);
        for (const auto& command : f.Commands) {
            if (command.HasClaimRevocations()) {
                UNIT_ASSERT_VALUES_EQUAL(command.GetClaimRevocations().GetLimit(), 1);
            }
        }
        f.Stop();
    }


    Y_UNIT_TEST(SubmissionRefusalPersistsSafeDetails) {
        CheckWorkerFailureDetails(ERefusalDelivery::Submission);
    }

    Y_UNIT_TEST(PolledRefusalPersistsSafeDetails) {
        CheckWorkerFailureDetails(ERefusalDelivery::Poll);
    }

    Y_UNIT_TEST(AcceptedRevokePersistsAndRestartPollsSavedOperation) {
        TWorkerFixture f;
        CreateActive(f.Tablet);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Start();
        auto submit = f.Take(f.Submissions);
        UNIT_ASSERT(submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Kind == EDelegationOperationKind::Revoke);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Accepted, "accepted-revoke");
        f.Until([&] { return f.LastDelegations["create-1"].GetRevokeOperationId() == "accepted-revoke"; });
        f.Stop();
        f.Tablet.Reboot();
        f.Tablet.AdvanceTime(TDuration::Seconds(2));
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetRevokeOperationId(), "accepted-revoke");

        f.Start();
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "accepted-revoke");
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 1);
        f.ReplyIam(*poll, EDelegationOperationOutcome::Succeeded, "accepted-revoke");
        f.WaitState("create-1", NProto::REVOKED);
    }

    Y_UNIT_TEST(RevokeUsesEntireSavedBindingWithoutNodeConfiguration) {
        TWorkerFixture f;
        auto saved = Binding();
        saved.SetServiceId("saved-service");
        saved.SetMicroserviceId("saved-microservice");
        saved.SetResourceType("saved-resource");
        saved.SetReferrerType("saved-referrer-type");
        saved.SetReferencePolicy(NProto::WITH_REFERENCES);
        f.Tablet.Call(RegisterRequest(DatabaseIdentity()));
        auto delegation = f.Tablet.Call(StageCreateRequest("create-1", saved)).GetDelegation();
        delegation = f.Tablet.Call(BindRequest(delegation)).GetDelegation();
        CompleteSetup(f.Tablet, delegation);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Tablet.Reboot();
        // No IAM settings are supplied to the worker: only persisted identity may drive revocation.
        f.Start();
        const auto submit = f.Take(f.Submissions);
        const auto& binding = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Binding;
        UNIT_ASSERT_VALUES_EQUAL(binding.ServiceAccountId, "service-account-1");
        UNIT_ASSERT_VALUES_EQUAL(binding.CloudId, "cloud-1");
        UNIT_ASSERT_VALUES_EQUAL(binding.ReferrerId, "ydb.delegation.first");
        UNIT_ASSERT_VALUES_EQUAL(binding.ServiceId, "saved-service");
        UNIT_ASSERT_VALUES_EQUAL(binding.MicroserviceId, "saved-microservice");
        UNIT_ASSERT_VALUES_EQUAL(binding.ResourceType, "saved-resource");
        UNIT_ASSERT_VALUES_EQUAL(binding.ReferrerType, "saved-referrer-type");
        UNIT_ASSERT(binding.WithReferences);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        f.WaitState("create-1", NProto::REVOKED);
    }

    Y_UNIT_TEST(TerminalRevokeFailureClearsOperationBeforeResubmitting) {
        TWorkerFixture f;
        CreateActive(f.Tablet);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Start();
        auto submit = f.Take(f.Submissions);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Accepted, "failed-revoke");
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "failed-revoke");
        f.ReplyIam(*poll, EDelegationOperationOutcome::Failed, "failed-revoke");
        auto retry = f.Take(f.Submissions);
        UNIT_ASSERT(GetDelegation(f.Tablet, "create-1").GetRevokeOperationId().empty());
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 2);
        UNIT_ASSERT_VALUES_EQUAL(f.PollCount, 1);
        f.ReplyIam(*retry, EDelegationOperationOutcome::Succeeded, "successful-revoke");
        f.WaitState("create-1", NProto::REVOKED);
    }

    Y_UNIT_TEST(UnknownSetupWithoutOperationIsNeverRevoked) {
        TWorkerFixture f;
        CreateUnknown(f, "");
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Tablet.Reboot();
        f.Tablet.AdvanceTime(TDuration::Days(30));
        f.Start();
        f.Until([&] { return f.InventoryPages >= 3; });
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetState(), NProto::WAITING_SETUP);
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
        UNIT_ASSERT_VALUES_EQUAL(f.PollCount, 0);
    }

    Y_UNIT_TEST(KnownSetupAfterDropIsPolledBeforeRevocation) {
        TWorkerFixture f;
        CreateUnknown(f, "accepted-setup");
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Tablet.Reboot();
        f.Start();
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "accepted-setup");
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
        f.ReplyIam(*poll, EDelegationOperationOutcome::Succeeded, "accepted-setup");
        auto submit = f.Take(f.Submissions);
        UNIT_ASSERT(submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Kind == EDelegationOperationKind::Revoke);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        f.WaitState("create-1", NProto::REVOKED);
    }

    Y_UNIT_TEST(LateRevokeReplyCannotAcknowledgeClaimAfterTabletReboot) {
        TWorkerFixture f;
        CreateActive(f.Tablet);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Start();
        auto submit = f.Take(f.Submissions);
        f.Tablet.Reboot();
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded, "old-generation-revoke");
        f.Until([&] { return f.StaleFinishes == 1; });
        UNIT_ASSERT_VALUES_UNEQUAL(GetDelegation(f.Tablet, "create-1").GetState(), NProto::REVOKED);
        f.Tablet.AdvanceTime(TDuration::Minutes(2));
        auto retry = f.Take(f.Submissions);
        f.ReplyIam(*retry, EDelegationOperationOutcome::Succeeded, "current-generation-revoke");
        f.WaitState("create-1", NProto::REVOKED);
    }

    Y_UNIT_TEST(MatchingSchemaPromotesRecoveredSuccessfulSetup) {
        TWorkerFixture f;
        const auto unknown = CreateUnknown(f, "accepted-setup");
        f.Start();
        auto poll = f.Take(f.Polls);
        f.ReplyIam(*poll, EDelegationOperationOutcome::Succeeded, "accepted-setup");
        auto describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(unknown));
        f.WaitState("create-1", NProto::ACTIVE);
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetCurrentOperationId(), "create-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
    }

    Y_UNIT_TEST(VerifiedShellResumesSetupWithDurableSubjectAndPollsAfterRestart) {
        TWorkerFixture f;
        f.Tablet.Call(RegisterRequest(DatabaseIdentity()));
        auto stage = StageCreateRequest();
        stage.MutableStage()->SetSubjectId("original-cloud-subject");
        const auto staged = f.Tablet.Call(std::move(stage)).GetDelegation();
        f.Tablet.Reboot();
        f.Start();
        auto describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(staged));
        // Binding the recovered shell and starting IAM are separate durable steps.
        describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(staged));
        auto submit = f.Take(f.Submissions);
        const auto* request = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>();
        UNIT_ASSERT(request->Kind == EDelegationOperationKind::Setup);
        UNIT_ASSERT_VALUES_EQUAL(request->SubjectId, "original-cloud-subject");
        UNIT_ASSERT_VALUES_EQUAL(request->RequestId, "create-1");
        UNIT_ASSERT_VALUES_EQUAL(request->Binding.ReferrerId, staged.GetBinding().GetReferrerId());
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetSetupState(), NProto::SETUP_STARTED);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Accepted, "recovered-setup");
        f.Until([&] { return f.LastDelegations["create-1"].GetSetupOperationId() == "recovered-setup"; });
        f.Stop();
        f.Tablet.Reboot();

        f.Start();
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "recovered-setup");
        f.ReplyIam(*poll, EDelegationOperationOutcome::Succeeded, "recovered-setup");
        describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(staged));
        f.WaitState("create-1", NProto::ACTIVE);
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 1);
    }

    Y_UNIT_TEST(SetupStartRaceAndUnknownOutcomeNeverResubmit) {
        TWorkerFixture f;
        f.Tablet.Call(RegisterRequest(DatabaseIdentity()));
        auto stage = StageCreateRequest();
        stage.MutableStage()->SetSubjectId("original-cloud-subject");
        auto delegation = f.Tablet.Call(std::move(stage)).GetDelegation();
        delegation = f.Tablet.Call(BindRequest(delegation)).GetDelegation();
        f.Start();
        auto describe = f.Take(f.Descriptions);
        // The statement wins StartSetup while the recovery worker is verifying schema.
        delegation = f.Tablet.Call(StartRequest(delegation)).GetDelegation();
        const auto claims = f.Claims;
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(delegation));
        f.Until([&] { return f.Claims > claims; });
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
        f.Tablet.Call(SetupResultRequest(delegation, NProto::SETUP_UNKNOWN, {}));
        f.Tablet.AdvanceTime(TDuration::Days(30));
        describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(delegation));
        const auto nextClaims = f.Claims;
        f.Until([&] { return f.Claims > nextClaims; });
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
        UNIT_ASSERT_VALUES_EQUAL(f.PollCount, 0);
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetSetupState(), NProto::SETUP_UNKNOWN);
    }

    Y_UNIT_TEST(LateReplyCannotCompleteNewerRevocationClaim) {
        TWorkerFixture f;
        CreateActive(f.Tablet);
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        f.Start();
        auto submit = f.Take(f.Submissions);
        const auto original = GetDelegation(f.Tablet, "create-1");
        f.Tablet.AdvanceTime(TDuration::Minutes(2));
        const auto replacement = f.Tablet.Call(ClaimRequest("replacement-worker", 1)).GetDelegations(0);
        UNIT_ASSERT_VALUES_EQUAL(replacement.GetClaim().GetAttempt(), original.GetClaim().GetAttempt() + 1);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded, "late-revoke");
        // The worker's 30s request timeout may already have finished the old claim;
        // either way the tablet must retain the replacement worker's claim unchanged.
        f.Until([&] { return f.StaleFinishes >= 1; });
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetClaim().SerializeAsString(),
            replacement.GetClaim().SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetState(), NProto::REVOKING);
        f.Tablet.Call(FinishRequest(replacement));
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetState(), NProto::REVOKED);
    }

    Y_UNIT_TEST(FailedCreateRemainsDiscoverableUntilSchemaDropReleasesName) {
        TWorkerFixture f;
        const auto unknown = CreateUnknown(f, "refused-setup");
        f.Start();
        auto poll = f.Take(f.Polls);
        f.ReplyIam(*poll, EDelegationOperationOutcome::Failed, "refused-setup");
        auto describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(unknown));
        const auto drop = f.Take(f.SchemaDrops);
        const auto& scheme = drop->Get<TEvTxUserProxy::TEvProposeTransaction>()->Record.GetTransaction().GetModifyScheme();
        UNIT_ASSERT(scheme.GetOperationType() == NKikimrSchemeOp::ESchemeOpDropSecret);
        UNIT_ASSERT_VALUES_EQUAL(scheme.GetWorkingDir(), "/Root/database");
        UNIT_ASSERT_VALUES_EQUAL(scheme.GetDrop().GetName(), "secret");
        UNIT_ASSERT_VALUES_EQUAL(scheme.GetDrop().GetId(), 42);
        UNIT_ASSERT_VALUES_EQUAL(scheme.GetApplyIf(0).GetPathOwnerId(), 100);
        UNIT_ASSERT_VALUES_EQUAL(scheme.GetApplyIf(0).GetPathId(), 42);
        UNIT_ASSERT(scheme.GetApplyIf(0).HasPathVersion());
        f.Runtime.Send(new IEventHandle(drop->Sender, f.Schema,
            new TEvTxUserProxy::TEvProposeTransactionStatus(TEvTxUserProxy::TResultStatus::ExecInProgress)));
        f.Stop();
        f.Tablet.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "create-1").GetState(), NProto::CANCELLED);

        // The creating client dropped the shell but died before retiring the tablet secret.
        f.Start();
        describe = f.Take(f.Descriptions);
        NKikimrScheme::TEvDescribeSchemeResult absent;
        absent.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
        f.ReplySchema(*describe, absent);
        f.Until([&] { return f.LastSecret.GetState() == NProto::SECRET_DROPPED; });
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetState(), NProto::SECRET_DROPPED);
        // Name reservation has been released; no failed grant was revoked.
        f.Tablet.Call(StageCreateRequest("new-create", Binding("new-referrer")));
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
    }

    Y_UNIT_TEST(FailedCreateCleanupPreservesChangedShellAndReplacement) {
        for (bool stagedReplacement : {false, true}) {
            TWorkerFixture f;
            auto failed = CreateUnknown(f, "refused-setup");
            failed = f.Tablet.Call(SetupResultRequest(failed, NProto::SETUP_FAILED, "refused-setup")).GetDelegation();
            if (stagedReplacement) {
                f.Tablet.Call(StageAlterRequest(GetSecret(f.Tablet), "replacement", Binding("replacement-ref")));
            }
            f.Start();
            auto describe = f.Take(f.Descriptions);
            auto shell = TWorkerFixture::MatchingSchema(failed);
            if (!stagedReplacement) {
                shell.MutablePathDescription()->MutableSecretDescription()->SetVersion(1);
            }
            f.ReplySchema(*describe, shell);
            const auto claims = f.Claims;
            f.Until([&] { return f.Claims > claims; });
            UNIT_ASSERT(f.SchemaDrops.empty());
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetState(), NProto::SECRET_LIVE);
        }
    }

    Y_UNIT_TEST(FailedCreateDescribeErrorDoesNotRemoveShell) {
        TWorkerFixture f;
        auto failed = CreateUnknown(f, "refused-setup");
        f.Tablet.Call(SetupResultRequest(failed, NProto::SETUP_FAILED, "refused-setup"));
        f.Start();
        auto describe = f.Take(f.Descriptions);
        NKikimrScheme::TEvDescribeSchemeResult error;
        error.SetStatus(NKikimrScheme::StatusNotAvailable);
        f.ReplySchema(*describe, error);
        describe = f.Take(f.Descriptions);
        UNIT_ASSERT(f.SchemaDrops.empty());
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetState(), NProto::SECRET_LIVE);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(failed));
        f.Take(f.SchemaDrops);
        // Proposal alone is insufficient to retire the tablet secret.
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetState(), NProto::SECRET_LIVE);
    }

    Y_UNIT_TEST(ReusedPathAndChangedCreatingReferenceRetireOldBinding) {
        for (ui32 mismatch = 0; mismatch != 4; ++mismatch) {
            TWorkerFixture f;
            const auto active = CreateActive(f.Tablet).GetDelegation();
            f.Start();
            auto describe = f.Take(f.Descriptions);
            auto schema = TWorkerFixture::MatchingSchema(active);
            if (mismatch == 0) {
                schema.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
            } else if (mismatch == 1) {
                schema.MutablePathDescription()->MutableSelf()->SetPathId(99);
            } else if (mismatch == 2) {
                schema.MutablePathDescription()->MutableSelf()->SetCreateTxId(99);
            } else {
                schema.MutablePathDescription()->MutableSecretDescription()->MutableIamDelegation()->SetCreateOperationId("other-create");
            }
            f.ReplySchema(*describe, std::move(schema));
            auto revoke = f.Take(f.Submissions);
            UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetState(), NProto::SECRET_DROPPED);
            f.ReplyIam(*revoke, EDelegationOperationOutcome::Succeeded);
            f.WaitState("create-1", NProto::REVOKED);
        }
    }

    Y_UNIT_TEST(SchemaErrorsAndUnboundAbsenceNeverProveDeletion) {
        TWorkerFixture f;
        const auto active = CreateActive(f.Tablet).GetDelegation();
        f.Start();
        auto describe = f.Take(f.Descriptions);
        NKikimrScheme::TEvDescribeSchemeResult error;
        error.SetStatus(NKikimrScheme::StatusNotAvailable);
        f.ReplySchema(*describe, error);
        describe = f.Take(f.Descriptions);
        UNIT_ASSERT_VALUES_EQUAL(GetSecret(f.Tablet).GetState(), NProto::SECRET_LIVE);
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(active));
        f.Stop();

        f.Tablet.Call(StageCreateRequest("unbound", Binding("unbound-referrer"), "/Root/database/unbound"));
        f.Start();
        describe = f.Take(f.Descriptions);
        f.ReplySchema(*describe, TWorkerFixture::MatchingSchema(active));
        describe = f.Take(f.Descriptions);
        UNIT_ASSERT_VALUES_EQUAL(describe->Get<TEvTxUserProxy::TEvNavigate>()->Record.GetDescribePath().GetPath(), "/Root/database/unbound");
        NKikimrScheme::TEvDescribeSchemeResult absent;
        absent.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
        const auto claimsBeforeAbsence = f.Claims;
        f.ReplySchema(*describe, absent);
        f.Until([&] { return f.Claims > claimsBeforeAbsence; });
        const auto pending = GetDelegation(f.Tablet, "unbound");
        UNIT_ASSERT_VALUES_EQUAL(pending.GetState(), NProto::PENDING);
        UNIT_ASSERT_VALUES_EQUAL(pending.GetSetupState(), NProto::SETUP_NOT_STARTED);
        UNIT_ASSERT(!pending.HasSecret());
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
    }

    Y_UNIT_TEST(InventoryTurnsAreSharedBetweenDatabases) {
        TWorkerFixture f;
        f.Tablet.Call(RegisterRequest(DatabaseIdentity("a", "/Root/a", "id-a")));
        f.Tablet.Call(RegisterRequest(DatabaseIdentity("b", "/Root/b", "id-b")));
        for (const TString& operation : {TString("a-first"), TString("a-second")}) {
            f.Tablet.Call(StageCreateRequest(operation, Binding(operation), "/Root/a/" + operation, "a"));
        }
        f.Tablet.Call(StageCreateRequest("b-only", Binding("b-only"), "/Root/b/only", "b"));
        f.Start();
        NKikimrScheme::TEvDescribeSchemeResult unavailable;
        unavailable.SetStatus(NKikimrScheme::StatusNotAvailable);
        for (const TString& path : {TString("/Root/a/a-first"), TString("/Root/b/only"), TString("/Root/a/a-second")}) {
            auto describe = f.Take(f.Descriptions);
            UNIT_ASSERT_VALUES_EQUAL(describe->Get<TEvTxUserProxy::TEvNavigate>()->Record.GetDescribePath().GetPath(), path);
            f.ReplySchema(*describe, unavailable);
        }
        UNIT_ASSERT_VALUES_EQUAL(f.SubmissionCount, 0);
    }

    Y_UNIT_TEST(InventoryProgressSurvivesOwnMutationsAndInterleavesRevocations) {
        TWorkerFixture f;
        f.Tablet.Call(RegisterRequest(DatabaseIdentity()));
        // Two records suffice: every successful bind changes the inventory revision.
        for (const TString& operation : {TString("a"), TString("z")}) {
            f.Tablet.Call(StageCreateRequest(operation, Binding(operation + "-ref"), "/Root/database/" + operation));
        }
        f.Start();
        auto first = f.Take(f.Descriptions);
        UNIT_ASSERT_VALUES_EQUAL(first->Get<TEvTxUserProxy::TEvNavigate>()->Record.GetDescribePath().GetPath(), "/Root/database/a");
        auto schema = TWorkerFixture::MatchingSchema(GetDelegation(f.Tablet, "a"), "a");
        f.ReplySchema(*first, schema);
        f.Until([&] { return f.LastDelegations["a"].HasSecret(); });
        // A new ready revocation must run before the next schema lookup, even with inventory remaining.
        const auto started = f.Tablet.Call(StartRequest(GetDelegation(f.Tablet, "a"))).GetDelegation();
        f.Tablet.Call(SetupResultRequest(started, NProto::SETUP_SUCCEEDED));
        f.Tablet.Call(DropRequest(GetSecret(f.Tablet)));
        auto revoke = f.Take(f.Submissions);
        UNIT_ASSERT(f.Descriptions.empty());
        f.ReplyIam(*revoke, EDelegationOperationOutcome::Succeeded);

        auto second = f.Take(f.Descriptions);
        UNIT_ASSERT_VALUES_EQUAL(second->Get<TEvTxUserProxy::TEvNavigate>()->Record.GetDescribePath().GetPath(), "/Root/database/z");
        schema = TWorkerFixture::MatchingSchema(GetDelegation(f.Tablet, "z"), "z");
        schema.MutablePathDescription()->MutableSelf()->SetPathId(43);
        f.ReplySchema(*second, schema);
        f.Until([&] { return f.LastDelegations["z"].HasSecret(); });
        UNIT_ASSERT_VALUES_EQUAL(GetDelegation(f.Tablet, "z").GetSecret().GetPathLocalId(), 43);
    }
}

} // namespace NKikimr::NIamDelegation::NTests
