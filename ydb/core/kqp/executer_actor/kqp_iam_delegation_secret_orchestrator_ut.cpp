#include <ydb/services/scheme_secret/secret_credentials.h>
#include <ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.h>
#include <ydb/core/kqp/gateway/actors/scheme.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation/lifecycle/worker.h>
#include <ydb/core/security/iam_delegation/tablet/iam_delegation.h>
#include <ydb/core/security/iam_delegation/tablet/public/events.h>
#include <ydb/core/security/iam_delegation/tablet/ut/ut_helpers.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/services/scheme_secret/iam_delegation_reader.h>
#include <ydb/library/ycloud/api/service_account_service.h>
#include <ydb/library/ycloud/api/folder_service.h>

#include <util/string/ascii.h>
#include <util/folder/tempdir.h>
#include <util/generic/scope.h>

#include <deque>

namespace NKikimr::NKqp {
namespace {

using namespace NIamDelegation;
namespace P = NKikimrIamDelegation;
using TGenericResult = NYql::IKikimrGateway::TGenericResult;
using EAction = TTestActorRuntimeBase::EEventAction;

// Real schema and lifecycle tablets. Only the external IAM boundary is held and
// replied to by the test; deadlines use the runtime's simulated clock.
class TOrchestratorFixture {
public:
    static constexpr ui64 TabletId = MakeTabletID(false, 8901);

    static TKikimrSettings Settings(TMaybe<NFake::TStorage> storage = {}) {
        NKikimrConfig::TAppConfig config;
        auto* iam = config.MutableIamConfig();
        iam->SetTokenServiceEndpoint("unused-token-endpoint");
        iam->SetServiceControlEndpoint("unused-control-endpoint");
        iam->SetResourceManagerEndpoint("unused-resource-manager-endpoint");
        iam->SetServiceId("saved-ydb-service");
        iam->SetMicroserviceId("saved-data-plane");
        iam->SetResourceType("resource-manager.cloud");
        iam->SetSystemTokenName("unused-system-token");
        iam->SetDelegationTabletId(TabletId);
        config.MutableAuthConfig()->SetAccessServiceDomain(BUILTIN_ACL_DOMAIN);
        // The tests register fakes, so bootstrap must not start network clients.
        config.MutableAuthConfig()->MutableTokenManager()->SetEnable(false);
        NKikimrConfig::TFeatureFlags flags;
        flags.SetEnableSchemaSecrets(true);
        flags.SetEnableIamDelegationSecrets(true);
        auto settings = TKikimrSettings(config).SetWithSampleTables(false)
            .SetFeatureFlags(flags).SetUseRealThreads(false);
        if (storage) {
            settings.SetStorage(*storage);
        }
        return settings;
    }

    TKikimrRunner Kikimr;
    TTestActorRuntime& Runtime = *Kikimr.GetTestServer().GetRuntime();
    TActorId Iam = Runtime.AllocateEdgeActor();
    TActorId TokenService = Runtime.AllocateEdgeActor();
    TActorId TabletActor;
    ui32 Generation = 0;
    TIntrusiveConstPtr<NACLib::TUserToken> Bob = MakeIntrusiveConst<NACLib::TUserToken>(
        "bob@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
    std::deque<TAutoPtr<IEventHandle>> Submissions;
    std::deque<TAutoPtr<IEventHandle>> Polls;
    TVector<TDelegationBinding> SetupBindings;
    TVector<TDelegationBinding> RevokeBindings;
    TVector<TTokenKey> MintKeys;
    THashMap<TString, P::TDelegation> ObservedDelegations;
    TVector<P::TRequest> Commands;
    std::function<EAction(TAutoPtr<IEventHandle>&)> Intercept;
    bool AutoReply = false;

    explicit TOrchestratorFixture(TMaybe<NFake::TStorage> storage = {})
        : Kikimr(Settings(std::move(storage)))
    {
        ReadyObserver = Runtime.AddObserver<TEvTablet::TEvReady>([this](auto& event) {
            if (event->Get()->TabletID == TabletId) {
                Generation = event->Get()->Generation;
                TabletActor = event->Get()->UserTabletActor;
            }
        });
        const auto bootstrap = CreateTestBootstrapper(Runtime,
            CreateTestTabletInfo(TabletId, TTabletTypes::IamDelegation), &CreateIamDelegationTablet);
        Runtime.EnableScheduleForActor(bootstrap);
        Until([&] { return bool(TabletActor); });
        Runtime.RegisterService(MakeIamDelegationServiceId(), Iam);
        Runtime.RegisterService(MakeIamDelegatedTokenServiceId(), TokenService);
        Runtime.SetObserverFunc([this](TAutoPtr<IEventHandle>& event) {
            if (Intercept && Intercept(event) == EAction::DROP) {
                return EAction::DROP;
            }
            switch (event->GetTypeRewrite()) {
                case TEvIamDelegation::TEvSubmitDelegationOperation::EventType: {
                    const auto& request = *event->Get<TEvIamDelegation::TEvSubmitDelegationOperation>();
                    (request.Kind == EDelegationOperationKind::Setup ? SetupBindings : RevokeBindings).push_back(request.Binding);
                    if (AutoReply) {
                        ReplyIam(*event, EDelegationOperationOutcome::Succeeded);
                    } else {
                        Submissions.emplace_back(event.Release());
                    }
                    return EAction::DROP;
                }
                case TEvIamDelegation::TEvPollDelegationOperation::EventType:
                    Polls.emplace_back(event.Release());
                    return EAction::DROP;
                case TEvIamDelegation::TEvGetToken::EventType: {
                    const auto key = event->Get<TEvIamDelegation::TEvGetToken>()->Key;
                    MintKeys.push_back(key);
                    auto reply = MakeHolder<TEvIamDelegation::TEvGetTokenResult>();
                    reply->Key = key;
                    reply->Token = "token-for-" + key.ServiceAccountId;
                    reply->UsableUntil = Runtime.GetCurrentTime() + TDuration::Hours(1);
                    reply->ExpiresAt = reply->UsableUntil + TDuration::Minutes(5);
                    Runtime.Send(new IEventHandle(event->Sender, TokenService, reply.Release(), 0, event->Cookie));
                    return EAction::DROP;
                }
                case TEvIamDelegationTablet::TEvRequest::EventType:
                    if (event->GetRecipientRewrite() == TabletActor) {
                        Commands.push_back(event->Get<TEvIamDelegationTablet::TEvRequest>()->Record);
                    }
                    break;
                case TEvIamDelegationTablet::TEvResponse::EventType: {
                    const auto& record = event->Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                    if (record.GetStatus() == P::SUCCESS && record.HasDelegation()) {
                        ObservedDelegations[record.GetDelegation().GetOperationId()] = record.GetDelegation();
                    }
                    break;
                }
            }
            return EAction::PROCESS;
        });
        const auto granted = Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpModifyACL);
            NACLib::TDiffACL acl;
            acl.AddAccess(NACLib::EAccessType::Allow, NACLib::GenericFull, "bob@" BUILTIN_ACL_DOMAIN);
            scheme.MutableModifyACL()->SetName("Root");
            scheme.MutableModifyACL()->SetDiffACL(acl.SerializeAsString());
        });
        UNIT_ASSERT_C(granted.Success(), granted.Issues().ToString());
    }

    ~TOrchestratorFixture() {
        Runtime.SetObserverFunc(TTestActorRuntimeBase::DefaultObserverFunc);
    }

    template<class TCondition>
    void Until(TCondition condition) {
        Runtime.WaitFor("deterministic secret operation", condition, TDuration::Minutes(10));
    }

    TAutoPtr<IEventHandle> Take(std::deque<TAutoPtr<IEventHandle>>& queue) {
        Until([&] { return !queue.empty(); });
        TAutoPtr<IEventHandle> result(queue.front().Release());
        queue.pop_front();
        return result;
    }

    void ReplyIam(const IEventHandle& request, EDelegationOperationOutcome outcome, const TString& id = {}) {
        TDelegationOperationResult result;
        result.Outcome = outcome;
        result.OperationId = id;
        result.Status = outcome == EDelegationOperationOutcome::Failed
            ? Ydb::StatusIds::UNAUTHORIZED : Ydb::StatusIds::SUCCESS;
        Runtime.Send(new IEventHandle(request.Sender, Iam,
            new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, request.Cookie));
    }

    TGenericResult Scheme(std::function<void(NKikimrSchemeOp::TModifyScheme&)> fill) {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetDatabaseName("/Root");
        fill(*request->Record.MutableTransaction()->MutableModifyScheme());
        const auto promise = NThreading::NewPromise<TGenericResult>();
        Runtime.Register(new TSchemeOpRequestHandler(request.Release(), promise, true));
        auto future = promise.GetFuture();
        Until([&] { return future.HasValue(); });
        return future.GetValue();
    }

    void SetAccess(const TString& name, const TString& sid, ui32 allow, ui32 deny = 0) {
        const auto result = Scheme([&](auto& scheme) {
            scheme.SetWorkingDir(name == "Root" ? "/" : "/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpModifyACL);
            NACLib::TDiffACL acl;
            if (allow) {
                acl.AddAccess(NACLib::EAccessType::Allow, allow, sid);
            }
            if (deny) {
                acl.AddAccess(NACLib::EAccessType::Deny, deny, sid);
            }
            scheme.MutableModifyACL()->SetName(name);
            scheme.MutableModifyACL()->SetDiffACL(acl.SerializeAsString());
        });
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
    }

    NKikimrScheme::TEvDescribeSchemeResult Describe(const TString& name = "secret",
        TIntrusiveConstPtr<NACLib::TUserToken> token = {}, bool returnValue = true) {
        const auto edge = Runtime.AllocateEdgeActor();
        auto request = MakeHolder<TEvTxUserProxy::TEvNavigate>();
        request->Record.SetDatabaseName("/Root");
        if (token) {
            request->Record.SetUserToken(token->SerializeAsString());
        }
        request->Record.MutableDescribePath()->SetPath("/Root/" + name);
        request->Record.MutableDescribePath()->MutableOptions()->SetReturnSecretValue(returnValue);
        Runtime.Send(new IEventHandle(MakeTxProxyID(), edge, request.Release()));
        return Runtime.GrabEdgeEvent<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>(edge)->Get()->GetRecord();
    }

    P::TResponse Tablet(P::TRequest request) {
        const auto edge = Runtime.AllocateEdgeActor();
        auto event = MakeHolder<TEvIamDelegationTablet::TEvRequest>();
        event->Record.Swap(&request);
        Runtime.SendToPipe(TabletId, edge, event.Release(), 0, GetPipeConfigWithRetries());
        auto response = Runtime.GrabEdgeEvent<TEvIamDelegationTablet::TEvResponse>(edge)->Get()->Record;
        UNIT_ASSERT_VALUES_EQUAL_C(response.GetStatus(), P::SUCCESS, response.GetError());
        return response;
    }

    P::TResponse Delegation(const TString& id) {
        P::TRequest request;
        request.MutableGetDelegation()->SetOperationId(id);
        return Tablet(std::move(request));
    }

    void RebootTablet() {
        const auto previous = Generation;
        Runtime.Send(new IEventHandle(TabletActor, {}, new TEvents::TEvPoison));
        Until([&] { return Generation > previous; });
        InvalidateTabletResolverCache(Runtime, TabletId);
    }

    TIamDelegationSecretOperation Operation(bool alter = false, const TString& account = "account-1",
        const TString& cloud = "cloud-1", const TString& name = "secret")
    {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetDatabaseName("/Root");
        request->Record.SetUserToken(Bob->SerializeAsString());
        auto* scheme = request->Record.MutableTransaction()->MutableModifyScheme();
        scheme->SetWorkingDir("/Root");
        scheme->SetOperationType(alter ? NKikimrSchemeOp::ESchemeOpAlterSecret : NKikimrSchemeOp::ESchemeOpCreateSecret);
        scheme->SetFailOnExist(true);
        scheme->SetFailedOnAlreadyExists(true);
        auto* op = alter ? scheme->MutableAlterSecret() : scheme->MutableCreateSecret();
        op->SetName(name);
        if (account) {
            op->MutableIamDelegation()->SetServiceAccountId(account);
        }
        if (cloud) {
            op->MutableIamDelegation()->SetCloudId(cloud);
        }
        return {.Request = std::move(request), .Database = "/Root", .UserToken = Bob,
            .Promise = NThreading::NewPromise<TGenericResult>(), .FailedOnAlreadyExists = true};
    }

    struct TRunning {
        TActorId Actor;
        NThreading::TFuture<TGenericResult> Future;
    };

    TRunning Start(TIamDelegationSecretOperation op) {
        auto future = op.Promise.GetFuture();
        const auto type = op.Request->Record.GetTransaction().GetModifyScheme().GetOperationType();
        UNIT_ASSERT(type == NKikimrSchemeOp::ESchemeOpCreateSecret || type == NKikimrSchemeOp::ESchemeOpAlterSecret);
        const auto actor = Runtime.Register(type == NKikimrSchemeOp::ESchemeOpAlterSecret
            ? CreateIamDelegationSecretAlterer(std::move(op)) : CreateIamDelegationSecretCreator(std::move(op)));
        Runtime.EnableScheduleForActor(actor);
        return {actor, future};
    }

    TGenericResult Finish(const TRunning& running) {
        Until([&] { return running.Future.HasValue(); });
        return running.Future.GetValue();
    }

    P::TDelegation StageWithoutSchema(TIamDelegationSecretOperation op) {
        P::TDelegation staged;
        TActorId actor;
        Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType
                && event->GetRecipientRewrite() == actor) {
                const auto& reply = event->template Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                if (reply.HasDelegation() && !reply.GetDelegation().HasSecret()) {
                    staged = reply.GetDelegation();
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto running = Start(std::move(op));
        actor = running.Actor;
        Until([&] { return !staged.GetOperationId().empty(); });
        Runtime.Send(new IEventHandle(actor, {}, new TEvents::TEvPoison));
        UNIT_ASSERT(!Finish(running).Success());
        Intercept = {};
        return staged;
    }

    TString Create() {
        const auto running = Start(Operation());
        auto submit = Take(Submissions);
        const auto id = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        const auto result = Finish(running);
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        return id;
    }

    TActorId StartWorker() {
        const auto worker = Runtime.Register(CreateIamDelegationRecoveryWorker(TabletId));
        Runtime.EnableScheduleForActor(worker);
        return worker;
    }

    TActorId StartRead(const TString& name = "secret", TIntrusiveConstPtr<NACLib::TUserToken> token = {}) {
        const auto schema = Describe(name);
        UNIT_ASSERT_VALUES_EQUAL(schema.GetStatus(), NKikimrScheme::StatusSuccess);
        const auto& path = schema.GetPathDescription();
        const auto& ref = path.GetSecretDescription().GetIamDelegation();
        const auto edge = Runtime.AllocateEdgeActor();
        Runtime.Register(NSecret::CreateIamDelegationSecretReaderActor(edge, 991, "/Root/" + name,
            "/Root", token ? std::move(token) : Bob, {ref.GetTabletId(), ref.GetDatabaseIncarnation(), ref.GetCreateOperationId(),
                path.GetSelf().GetSchemeshardId(), path.GetSelf().GetPathId()}));
        return edge;
    }

    THolder<NSecret::TEvIamDelegationSecretReadResult> FinishRead(const TActorId& edge) {
        auto result = Runtime.GrabEdgeEvent<NSecret::TEvIamDelegationSecretReadResult>(edge);
        UNIT_ASSERT_VALUES_EQUAL(result->Cookie, 991);
        return result->Release();
    }

    THolder<NSecret::TEvIamDelegationSecretReadResult> Read(const TString& name = "secret",
        TIntrusiveConstPtr<NACLib::TUserToken> token = {})
    {
        return FinishRead(StartRead(name, std::move(token)));
    }

    void Drop(const TString& name = "secret") {
        const auto result = Scheme([&](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpDropSecret);
            scheme.MutableDrop()->SetName(name);
        });
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
    }

private:
    TTestActorRuntime::TEventObserverHolder ReadyObserver;
};

void CheckNoopAlterPreservesPending(P::ESetupState outcome, TDuration age = TDuration::Zero()) {
    TOrchestratorFixture f;
    const auto currentId = f.Create();
    const auto current = f.Delegation(currentId);
    P::TRequest stage;
    auto* intent = stage.MutableStage();
    intent->SetMode(P::ALTER);
    intent->SetOperationId("pending-replacement");
    intent->SetDatabaseIncarnation(current.GetDelegation().GetDatabaseIncarnation());
    intent->SetSecretPath(current.GetDelegation().GetSecretPath());
    intent->SetCreateTxId(current.GetDelegation().GetCreateTxId());
    *intent->MutableSecret() = current.GetSecret().GetIdentity();
    intent->SetExpectedSecretRevision(current.GetSecret().GetRevision());
    *intent->MutableBinding() = current.GetDelegation().GetBinding();
    intent->MutableBinding()->SetServiceAccountId("account-2");
    intent->MutableBinding()->SetReferrerId("pending-referrer");
    auto pending = f.Tablet(std::move(stage)).GetDelegation();
    if (outcome != P::SETUP_NOT_STARTED) {
        P::TRequest start;
        start.MutableStartSetup()->SetOperationId(pending.GetOperationId());
        start.MutableStartSetup()->SetExpectedRevision(pending.GetRevision());
        pending = f.Tablet(std::move(start)).GetDelegation();
        P::TRequest result;
        result.MutableSetSetupResult()->SetOperationId(pending.GetOperationId());
        result.MutableSetSetupResult()->SetExpectedRevision(pending.GetRevision());
        result.MutableSetSetupResult()->SetOutcome(outcome);
        pending = f.Tablet(std::move(result)).GetDelegation();
    }
    f.Runtime.AdvanceCurrentTime(age);
    f.RebootTablet();
    const auto before = f.Delegation(pending.GetOperationId());
    const auto activeBefore = f.Delegation(currentId).GetDelegation();
    const auto result = f.Finish(f.Start(f.Operation(true)));
    UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
    const auto after = f.Delegation(pending.GetOperationId());
    UNIT_ASSERT_VALUES_EQUAL(after.GetDelegation().SerializeAsString(), before.GetDelegation().SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(after.GetSecret().SerializeAsString(), before.GetSecret().SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(f.Delegation(currentId).GetDelegation().SerializeAsString(), activeBefore.SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
    UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    UNIT_ASSERT(f.RevokeBindings.empty());
}

enum class ERefusalDelivery { Submission, Poll };
enum class ERefusalReload { Live, TabletRestart };

void CheckWorkerOwnedRefusal(ERefusalDelivery delivery, ERefusalReload reload) {
    TOrchestratorFixture f;
    const auto currentId = f.Create();
    const auto current = f.Delegation(currentId).GetDelegation();
    TActorId coordinator;
    TAutoPtr<IEventHandle> heldStage;
    P::TDelegation pending;
    f.Intercept = [&](auto& event) {
        if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType
            && event->GetRecipientRewrite() == coordinator && pending.GetOperationId().empty()) {
            const auto& response = event->template Get<TEvIamDelegationTablet::TEvResponse>()->Record;
            if (response.HasDelegation() && response.GetDelegation().GetOperationId() != currentId
                && response.GetDelegation().GetSetupState() == P::SETUP_NOT_STARTED) {
                pending = response.GetDelegation();
                heldStage.Reset(event.Release());
                return EAction::DROP;
            }
        }
        return EAction::PROCESS;
    };
    const auto altering = f.Start(f.Operation(true, "account-2"));
    coordinator = altering.Actor;
    f.Until([&] { return bool(heldStage); });
    const auto worker = f.StartWorker();
    auto submission = f.Take(f.Submissions);
    const auto& setup = *submission->Get<TEvIamDelegation::TEvSubmitDelegationOperation>();
    UNIT_ASSERT(setup.Kind == EDelegationOperationKind::Setup);
    UNIT_ASSERT_VALUES_EQUAL(setup.SubjectId, "bob");
    UNIT_ASSERT_VALUES_EQUAL(setup.RequestId, pending.GetOperationId());
    UNIT_ASSERT_VALUES_EQUAL(setup.Binding.ServiceAccountId, "account-2");
    if (delivery == ERefusalDelivery::Poll) {
        f.ReplyIam(*submission, EDelegationOperationOutcome::Accepted, "worker-setup-operation");
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId,
            "worker-setup-operation");
        f.ReplyIam(*poll, EDelegationOperationOutcome::Failed, "worker-setup-operation");
    } else {
        f.ReplyIam(*submission, EDelegationOperationOutcome::Failed);
    }
    f.Until([&] { return f.ObservedDelegations[pending.GetOperationId()].GetSetupState() == P::SETUP_FAILED; });
    f.Runtime.Send(new IEventHandle(worker, {}, new TEvents::TEvPoison));
    if (reload == ERefusalReload::TabletRestart) {
        f.RebootTablet();
    }
    f.Intercept = {};
    f.Runtime.Send(heldStage.Release());
    const auto result = f.Finish(altering);
    UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAUTHORIZED),
        result.Issues().ToString());
    UNIT_ASSERT(result.Issues().ToString().Contains("IAM refused delegation setup"));
    UNIT_ASSERT_VALUES_EQUAL(f.Delegation(currentId).GetDelegation().SerializeAsString(), current.SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(f.Delegation(currentId).GetSecret().GetCurrentOperationId(), currentId);
    UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
    UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 2);
    UNIT_ASSERT(f.RevokeBindings.empty());
}

void ReplyCredentialBearingRefusal(TOrchestratorFixture& f, const IEventHandle& request) {
    TDelegationOperationResult result;
    result.Outcome = EDelegationOperationOutcome::Failed;
    result.Status = Ydb::StatusIds::UNAUTHORIZED;
    result.OperationId = "refused-setup-operation";
    result.Issues.AddIssue("credential-sentinel-do-not-persist");
    result.FailureReasons = {EDelegationFailureReason::SubjectNotDelegatable};
    f.Runtime.Send(new IEventHandle(request.Sender, f.Iam,
        new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, request.Cookie));
}

void CheckCoordinatorOwnedRefusal(ERefusalDelivery delivery) {
    TOrchestratorFixture f;
    const auto currentId = f.Create();
    const auto current = f.Delegation(currentId).GetDelegation();
    const auto altering = f.Start(f.Operation(true, "account-2"));
    auto request = f.Take(f.Submissions);
    const auto pendingId = request->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
    if (delivery == ERefusalDelivery::Poll) {
        f.ReplyIam(*request, EDelegationOperationOutcome::Accepted, "refused-setup-operation");
        request = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(request->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId,
            "refused-setup-operation");
    }
    ReplyCredentialBearingRefusal(f, *request);
    const auto result = f.Finish(altering);
    UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAUTHORIZED),
        result.Issues().ToString());
    UNIT_ASSERT(result.Issues().ToString().Contains("IAM refused delegation setup"));
    UNIT_ASSERT(!result.Issues().ToString().Contains("credential-sentinel-do-not-persist"));
    f.RebootTablet();
    const auto saved = f.Delegation(pendingId);
    UNIT_ASSERT(saved.GetDelegation().HasSetupFailure());
    UNIT_ASSERT_VALUES_EQUAL(saved.GetDelegation().GetSetupFailure().GetStatus(), Ydb::StatusIds::UNAUTHORIZED);
    UNIT_ASSERT_VALUES_EQUAL(saved.GetDelegation().GetSetupFailure().ReasonsSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(saved.GetDelegation().GetSetupFailure().GetReasons(0),
        P::SETUP_FAILURE_SUBJECT_NOT_DELEGATABLE);
    UNIT_ASSERT(!TString(saved.SerializeAsString()).Contains("credential-sentinel-do-not-persist"));
    UNIT_ASSERT(saved.GetSecret().GetPendingOperationId().empty());
    UNIT_ASSERT_VALUES_EQUAL(saved.GetSecret().GetCurrentOperationId(), currentId);
    UNIT_ASSERT_VALUES_EQUAL(f.Delegation(currentId).GetDelegation().SerializeAsString(), current.SerializeAsString());
    UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
    UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 2);
    UNIT_ASSERT(f.RevokeBindings.empty());
}

} // namespace

Y_UNIT_TEST_SUITE(KqpIamDelegationSecretOrchestrator) {
Y_UNIT_TEST(LongLivedProviderReadsAlteredBindingAndRejectsLaterAclLoss) {
    TOrchestratorFixture f;
    const auto creating = f.Start(f.Operation());
    auto request = f.Take(f.Submissions);
    const auto creatingId = request->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
    f.ReplyIam(*request, EDelegationOperationOutcome::Succeeded);
    UNIT_ASSERT(f.Finish(creating).Success());
    const auto initial = f.Delegation(creatingId).GetDelegation();
    const NKqp::TEvDescribeSecretsResponse::TSecretReference reference{
        TOrchestratorFixture::TabletId, initial.GetDatabaseIncarnation(), creatingId,
        initial.GetSecret().GetPathOwnerId(), initial.GetSecret().GetPathLocalId()};
    const auto reader = MakeIntrusiveConst<NACLib::TUserToken>("provider-reader", TVector<TString>{});
    f.SetAccess("secret", reader->GetUserSID(), NACLib::SelectRow);
    const auto provider = NSecret::CreateRefreshingSecretCredentialsProviderFactory(
        f.Runtime.GetActorSystem(0), "/Root/secret", "/Root", reference, reader,
        NSecret::EBearerPrefix::None)->CreateProvider();
    const auto acquire = [&] {
        auto result = provider->GetAuthInfoAsync();
        f.Until([&] { return result.HasValue() || result.HasException(); });
        return result;
    };
    UNIT_ASSERT_VALUES_EQUAL(acquire().GetValue(), "token-for-account-1");
    const auto altering = f.Start(f.Operation(true, "account-2"));
    request = f.Take(f.Submissions);
    f.ReplyIam(*request, EDelegationOperationOutcome::Succeeded);
    UNIT_ASSERT(f.Finish(altering).Success());
    UNIT_ASSERT_VALUES_EQUAL(acquire().GetValue(), "token-for-account-2");
    // A later producer result for the same account must also be read through
    // the same provider instance, without retaining its previous credential.
    f.Intercept = [&](auto& event) {
        if (event->GetTypeRewrite() == TEvIamDelegation::TEvGetToken::EventType) {
            const auto& key = event->template Get<TEvIamDelegation::TEvGetToken>()->Key;
            UNIT_ASSERT_VALUES_EQUAL(key.ServiceAccountId, "account-2");
            auto reply = MakeHolder<TEvIamDelegation::TEvGetTokenResult>();
            reply->Key = key;
            reply->Token = "refreshed-account-2-token";
            reply->UsableUntil = f.Runtime.GetCurrentTime() + TDuration::Hours(1);
            reply->ExpiresAt = reply->UsableUntil + TDuration::Minutes(5);
            f.Runtime.Send(new IEventHandle(event->Sender, f.TokenService, reply.Release(), 0, event->Cookie));
            return EAction::DROP;
        }
        return EAction::PROCESS;
    };
    UNIT_ASSERT_VALUES_EQUAL(acquire().GetValue(), "refreshed-account-2-token");
    f.SetAccess("secret", reader->GetUserSID(), 0, NACLib::SelectRow);
    const auto denied = acquire();
    UNIT_ASSERT(denied.HasException());
    UNIT_ASSERT_EXCEPTION(denied.GetValue(), std::exception);
}


    Y_UNIT_TEST(CloudLookupForwardsCallerCredentialAndAlterRetainsChosenCloud) {
        TOrchestratorFixture f;
        f.Bob = MakeIntrusiveConst<NACLib::TUserToken>("creator-bearer-for-test",
            "bob@" BUILTIN_ACL_DOMAIN, TVector<TString>{});
        ui32 accountCalls = 0;
        ui32 folderCalls = 0;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == NCloud::TEvServiceAccountService::TEvGetServiceAccountRequest::EventType) {
                ++accountCalls;
                const auto& request = *event->template Get<NCloud::TEvServiceAccountService::TEvGetServiceAccountRequest>();
                UNIT_ASSERT_VALUES_EQUAL(request.Token, "creator-bearer-for-test");
                UNIT_ASSERT_VALUES_EQUAL(request.Request.service_account_id(), "account-1");
                auto reply = MakeHolder<NCloud::TEvServiceAccountService::TEvGetServiceAccountResponse>();
                reply->Response.set_folder_id("selected-folder");
                f.Runtime.Send(new IEventHandle(event->Sender, event->Recipient, reply.Release(), 0, event->Cookie));
                return EAction::DROP;
            }
            if (event->GetTypeRewrite() == NCloud::TEvFolderService::TEvResolveFoldersRequest::EventType) {
                ++folderCalls;
                const auto& request = *event->template Get<NCloud::TEvFolderService::TEvResolveFoldersRequest>();
                UNIT_ASSERT_VALUES_EQUAL(request.Token, "creator-bearer-for-test");
                auto reply = MakeHolder<NCloud::TEvFolderService::TEvResolveFoldersResponse>();
                auto* folder = reply->Response.add_resolved_folders();
                folder->set_id("selected-folder");
                folder->set_cloud_id("selected-cloud");
                f.Runtime.Send(new IEventHandle(event->Sender, event->Recipient, reply.Release(), 0, event->Cookie));
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation(false, "account-1", {}));
        auto setup = f.Take(f.Submissions);
        const auto id = setup->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        UNIT_ASSERT_VALUES_EQUAL(setup->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Binding.CloudId, "selected-cloud");
        f.ReplyIam(*setup, EDelegationOperationOutcome::Succeeded);
        UNIT_ASSERT(f.Finish(creating).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetDelegation().GetBinding().GetCloudId(), "selected-cloud");
        UNIT_ASSERT(!TString(f.Delegation(id).GetDelegation().SerializeAsString()).Contains("creator-bearer-for-test"));

        const auto altering = f.Start(f.Operation(true, "account-2", {}));
        setup = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(setup->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Binding.CloudId, "selected-cloud");
        f.ReplyIam(*setup, EDelegationOperationOutcome::Succeeded);
        UNIT_ASSERT(f.Finish(altering).Success());
        UNIT_ASSERT_VALUES_EQUAL(accountCalls, 1);
        UNIT_ASSERT_VALUES_EQUAL(folderCalls, 1);
        const auto current = f.Delegation(id).GetSecret().GetCurrentOperationId();
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(current).GetDelegation().GetBinding().GetCloudId(), "selected-cloud");
    }

    Y_UNIT_TEST(ConfiguredCloudLookupFailureWithoutDatabaseCloudNeverStages) {
        TOrchestratorFixture f;
        f.Bob = MakeIntrusiveConst<NACLib::TUserToken>("creator-bearer-for-test",
            "bob@" BUILTIN_ACL_DOMAIN, TVector<TString>{});
        ui32 lookups = 0;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() != NCloud::TEvServiceAccountService::TEvGetServiceAccountRequest::EventType) {
                return EAction::PROCESS;
            }
            ++lookups;
            UNIT_ASSERT_VALUES_EQUAL(event->template Get<NCloud::TEvServiceAccountService::TEvGetServiceAccountRequest>()->Token,
                "creator-bearer-for-test");
            auto reply = MakeHolder<NCloud::TEvServiceAccountService::TEvGetServiceAccountResponse>();
            reply->Status.GRpcStatusCode = grpc::StatusCode::PERMISSION_DENIED;
            reply->Status.Msg = "lookup denied";
            f.Runtime.Send(new IEventHandle(event->Sender, event->Recipient, reply.Release(), 0, event->Cookie));
            return EAction::DROP;
        };
        const auto result = f.Finish(f.Start(f.Operation(false, "account-1", {})));
        UNIT_ASSERT(!result.Success());
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "specify RESOURCE explicitly");
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "has no cloud_id attribute");
        UNIT_ASSERT_VALUES_EQUAL(lookups, 1);
        UNIT_ASSERT(f.Commands.empty());
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT(f.MintKeys.empty());
    }


    Y_UNIT_TEST(BuiltinSubjectWithSchemaRightsCannotStageDelegation) {
        TOrchestratorFixture f;
        // Bob already has ordinary schema rights. A different cloud domain
        // makes this identity local rather than a cloud delegation subject.
        f.Runtime.GetAppData(0).AuthConfig.SetAccessServiceDomain("as");
        const auto result = f.Finish(f.Start(f.Operation()));
        UNIT_ASSERT(!result.Success());
        UNIT_ASSERT(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::BAD_REQUEST));
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "not a cloud subject");
        UNIT_ASSERT(f.Commands.empty());
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
    }

    Y_UNIT_TEST(ValueAccessNeedsSelectRowWithoutDescribeSchema) {
        TOrchestratorFixture f;
        f.Create();
        const auto reader = MakeIntrusiveConst<NACLib::TUserToken>("select-only", TVector<TString>{});
        const auto describer = MakeIntrusiveConst<NACLib::TUserToken>("describe-only", TVector<TString>{});
        // Secret ACLs interrupt parent inheritance by default. Establish the
        // rights on the secret itself and verify the effective ACL before reading.
        f.SetAccess("secret", reader->GetUserSID(), NACLib::SelectRow, NACLib::DescribeSchema);
        f.SetAccess("secret", describer->GetUserSID(), NACLib::DescribeSchema, NACLib::SelectRow);
        const auto self = f.Describe().GetPathDescription().GetSelf();
        NACLib::TSecurityObject target(self.GetOwner(), false);
        UNIT_ASSERT(target.MutableACL()->ParseFromString(self.GetEffectiveACL()));
        UNIT_ASSERT_VALUES_UNEQUAL(self.GetOwner(), reader->GetUserSID());
        UNIT_ASSERT(target.CheckAccess(NACLib::SelectRow, *reader));
        UNIT_ASSERT(!target.CheckAccess(NACLib::DescribeSchema, *reader));
        UNIT_ASSERT(!target.CheckAccess(NACLib::SelectRow, *describer));
        UNIT_ASSERT(target.CheckAccess(NACLib::DescribeSchema, *describer));
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("secret", reader, false).GetStatus(), NKikimrScheme::StatusAccessDenied);
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("secret", describer, false).GetStatus(), NKikimrScheme::StatusSuccess);
        const auto value = f.Read("secret", reader);
        UNIT_ASSERT_VALUES_EQUAL(value->Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(value->Token, "token-for-account-1");
        const auto denied = f.Read("secret", describer);
        UNIT_ASSERT_VALUES_UNEQUAL(denied->Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT(denied->Token.empty());
    }

    Y_UNIT_TEST(SelectRowRevocationWhileMintHeldPreventsValueReturn) {
        TOrchestratorFixture f;
        f.Create();
        const auto reader = MakeIntrusiveConst<NACLib::TUserToken>("revoked-reader", TVector<TString>{});
        f.SetAccess("secret", reader->GetUserSID(), NACLib::SelectRow);
        UNIT_ASSERT_VALUES_EQUAL(f.Read("secret", reader)->Status, Ydb::StatusIds::SUCCESS);
        f.MintKeys.clear();
        TAutoPtr<IEventHandle> held;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegation::TEvGetToken::EventType) {
                UNIT_ASSERT(!held);
                held.Reset(event.Release());
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto read = f.StartRead("secret", reader);
        f.Until([&] { return bool(held); });
        f.SetAccess("secret", reader->GetUserSID(), 0, NACLib::SelectRow);
        f.Intercept = {};
        // Re-enter mailbox dispatch so the external-token boundary observer
        // replies. Direct Send would deliver to its inert edge actor instead.
        f.Runtime.Send(held.Release(), 0, true);
        const auto denied = f.FinishRead(read);
        UNIT_ASSERT_VALUES_EQUAL_C(denied->Status, Ydb::StatusIds::UNAUTHORIZED, denied->Issues.ToString());
        UNIT_ASSERT(denied->Token.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.MintKeys.size(), 1);
    }

    Y_UNIT_TEST(MetadataRemainsAvailableDuringIamOutageWithoutMint) {
        TOrchestratorFixture f;
        f.Create();
        ui32 remoteCalls = 0;
        f.Intercept = [&](auto& event) {
            const auto type = event->GetTypeRewrite();
            if (type == TEvIamDelegation::TEvSubmitDelegationOperation::EventType
                || type == TEvIamDelegation::TEvPollDelegationOperation::EventType
                || type == TEvIamDelegation::TEvGetToken::EventType) {
                ++remoteCalls;
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto described = f.Describe("secret", f.Bob, false);
        UNIT_ASSERT_VALUES_EQUAL(described.GetStatus(), NKikimrScheme::StatusSuccess);
        UNIT_ASSERT(described.GetPathDescription().GetSecretDescription().HasIamDelegation());
        UNIT_ASSERT(!described.GetPathDescription().GetSecretDescription().HasValue());
        UNIT_ASSERT_VALUES_EQUAL(remoteCalls, 0);
        UNIT_ASSERT(f.MintKeys.empty());
    }

    Y_UNIT_TEST(DropCommitsDuringRevokeOutageAndWorkerLaterDrainsCleanup) {
        TOrchestratorFixture f;
        const auto id = f.Create();
        f.Drop();
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        const auto worker = f.StartWorker();
        auto revoke = f.Take(f.Submissions);
        UNIT_ASSERT(revoke->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Kind
            == EDelegationOperationKind::Revoke);
        f.ReplyIam(*revoke, EDelegationOperationOutcome::Unknown);
        f.Until([&] { return f.ObservedDelegations[id].GetState() == P::REVOCATION_PENDING
            && f.ObservedDelegations[id].GetRevokeConsecutiveFailures() == 1; });
        f.Runtime.Send(new IEventHandle(worker, {}, new TEvents::TEvPoison));
        f.RebootTablet();
        const auto retained = f.Delegation(id).GetDelegation();
        UNIT_ASSERT_VALUES_EQUAL(retained.GetState(), P::REVOCATION_PENDING);
        UNIT_ASSERT(retained.HasCleanupQueuedAtUs());
        f.Runtime.UpdateCurrentTime(Max(f.Runtime.GetCurrentTime(), TInstant::MicroSeconds(retained.GetDueAtUs())));
        const auto replacement = f.StartWorker();
        revoke = f.Take(f.Submissions);
        f.ReplyIam(*revoke, EDelegationOperationOutcome::Succeeded);
        f.Until([&] { return f.ObservedDelegations[id].GetState() == P::REVOKED; });
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetDelegation().GetCleanupQueuedAtUs(), retained.GetCleanupQueuedAtUs());
        f.Runtime.Send(new IEventHandle(replacement, {}, new TEvents::TEvPoison));
    }

    Y_UNIT_TEST(CoordinatorOwnedSubmissionRefusalPersistsSafeDetails) {
        CheckCoordinatorOwnedRefusal(ERefusalDelivery::Submission);
    }

    Y_UNIT_TEST(CoordinatorOwnedPolledRefusalPersistsSafeDetails) {
        CheckCoordinatorOwnedRefusal(ERefusalDelivery::Poll);
    }

    Y_UNIT_TEST(LegacyWorkerRefusalKeepsGenericFallbackAfterRestart) {
        TOrchestratorFixture f;
        const auto currentId = f.Create();
        const auto current = f.Delegation(currentId).GetDelegation();
        TActorId coordinator;
        TAutoPtr<IEventHandle> heldStage;
        P::TDelegation pending;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType
                && event->GetRecipientRewrite() == coordinator && pending.GetOperationId().empty()) {
                const auto& response = event->template Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                if (response.HasDelegation() && response.GetDelegation().GetOperationId() != currentId
                    && response.GetDelegation().GetSetupState() == P::SETUP_NOT_STARTED) {
                    pending = response.GetDelegation();
                    heldStage.Reset(event.Release());
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto altering = f.Start(f.Operation(true, "account-2"));
        coordinator = altering.Actor;
        f.Until([&] { return bool(heldStage); });
        f.Intercept = {};
        P::TRequest start;
        start.MutableStartSetup()->SetOperationId(pending.GetOperationId());
        start.MutableStartSetup()->SetExpectedRevision(pending.GetRevision());
        pending = f.Tablet(std::move(start)).GetDelegation();
        P::TRequest legacyResult;
        auto* outcome = legacyResult.MutableSetSetupResult();
        outcome->SetOperationId(pending.GetOperationId());
        outcome->SetExpectedRevision(pending.GetRevision());
        outcome->SetOutcome(P::SETUP_FAILED);
        const auto saved = f.Tablet(std::move(legacyResult)).GetDelegation();
        UNIT_ASSERT(!saved.HasSetupFailure());
        f.RebootTablet();
        f.Runtime.Send(heldStage.Release());
        const auto result = f.Finish(altering);
        UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAVAILABLE),
            result.Issues().ToString());
        UNIT_ASSERT(result.Issues().ToString().Contains("IAM refused delegation setup"));
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(pending.GetOperationId()).GetDelegation().SerializeAsString(),
            saved.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(currentId).GetDelegation().SerializeAsString(), current.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
        UNIT_ASSERT(f.RevokeBindings.empty());
    }

    Y_UNIT_TEST(WorkerOwnedSetupRefusalReturnsOriginalStatus) {
        CheckWorkerOwnedRefusal(ERefusalDelivery::Submission, ERefusalReload::Live);
    }
    Y_UNIT_TEST(WorkerOwnedSetupRefusalReturnsOriginalStatusAfterRestart) {
        CheckWorkerOwnedRefusal(ERefusalDelivery::Submission, ERefusalReload::TabletRestart);
    }
    Y_UNIT_TEST(WorkerOwnedPolledRefusalReturnsOriginalStatus) {
        CheckWorkerOwnedRefusal(ERefusalDelivery::Poll, ERefusalReload::Live);
    }
    Y_UNIT_TEST(WorkerOwnedPolledRefusalReturnsOriginalStatusAfterRestart) {
        CheckWorkerOwnedRefusal(ERefusalDelivery::Poll, ERefusalReload::TabletRestart);
    }

    Y_UNIT_TEST(ExtractCloudSubjectId) {
        struct TCase { TString Sid; TString Domain; TMaybe<TString> Expected; };
        for (const auto& entry : TVector<TCase>{
            {"bob@as", "as", TString("bob")}, {"a@b@as", "as", TString("a@b")},
            {"bob@builtin", "as", Nothing()}, {"@as", "as", Nothing()},
            {"bob@as", "", Nothing()}, {"bob@AS", "as", Nothing()}, {"", "as", Nothing()}})
        {
            const auto actual = NKqp::ExtractCloudSubjectId(NACLib::TUserToken(entry.Sid, {}), entry.Domain);
            UNIT_ASSERT_VALUES_EQUAL(actual.Defined(), entry.Expected.Defined());
            if (actual) {
                UNIT_ASSERT_VALUES_EQUAL(*actual, *entry.Expected);
            }
        }
        UNIT_ASSERT(!NKqp::ExtractCloudSubjectId(NACLib::TUserToken("metadata@system", {}), "system"));
    }

    Y_UNIT_TEST(ReferrerFitsIamIdentityLimit) {
        const TString first = NewReferrerId();
        UNIT_ASSERT_VALUES_UNEQUAL(first, NewReferrerId());
        UNIT_ASSERT(first.StartsWith("ydb.delegation."));
        UNIT_ASSERT_VALUES_EQUAL(first.size(), TStringBuf("ydb.delegation.").size() + 32);
        UNIT_ASSERT(first.size() <= 50);
        for (const char character : TStringBuf(first).Skip(TStringBuf("ydb.delegation.").size())) {
            UNIT_ASSERT(IsAsciiHex(character) && !IsAsciiUpper(character));
        }
    }

    Y_UNIT_TEST(CreateAlterReadAndDropUseIndependentTablet) {
        TOrchestratorFixture f;
        const auto creating = f.Start(f.Operation());
        auto submit = f.Take(f.Submissions);
        const auto& request = *submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>();
        const TString createId = request.RequestId;
        UNIT_ASSERT(request.Kind == EDelegationOperationKind::Setup);
        UNIT_ASSERT_VALUES_EQUAL(request.SubjectId, "bob");
        const auto started = f.Delegation(createId);
        UNIT_ASSERT_VALUES_EQUAL(started.GetDelegation().GetSetupState(), P::SETUP_STARTED);
        UNIT_ASSERT_VALUES_EQUAL(started.GetDelegation().GetSubjectId(), "bob");
        UNIT_ASSERT_VALUES_EQUAL(started.GetSecret().GetPendingOperationId(), createId);
        const auto schema = f.Describe();
        const auto reference = schema.GetPathDescription().GetSecretDescription().GetIamDelegation();
        UNIT_ASSERT_VALUES_EQUAL(reference.GetTabletId(), f.TabletId);
        UNIT_ASSERT_VALUES_EQUAL(reference.GetCreateOperationId(), createId);
        UNIT_ASSERT_VALUES_EQUAL(schema.GetPathDescription().GetSelf().GetCreateTxId(), started.GetDelegation().GetCreateTxId());
        UNIT_ASSERT(!schema.GetPathDescription().GetSecretDescription().HasValue());
        UNIT_ASSERT(f.Read()->Status != Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT(f.MintKeys.empty());
        f.ReplyIam(*submit, EDelegationOperationOutcome::Accepted, "accepted-setup");
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(createId).GetDelegation().GetSetupOperationId(), "accepted-setup");
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "accepted-setup");
        f.ReplyIam(*poll, EDelegationOperationOutcome::Succeeded, "accepted-setup");
        UNIT_ASSERT(f.Finish(creating).Success());
        auto read = f.Read();
        UNIT_ASSERT_VALUES_EQUAL(read->Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(read->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.MintKeys.back().ServiceId, "saved-ydb-service");

        const auto altering = f.Start(f.Operation(true, "account-2"));
        submit = f.Take(f.Submissions);
        const TString alterId = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        UNIT_ASSERT_VALUES_UNEQUAL(alterId, createId);
        UNIT_ASSERT_VALUES_UNEQUAL(f.SetupBindings[0].ReferrerId, f.SetupBindings[1].ReferrerId);
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        UNIT_ASSERT(f.Finish(altering).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-2");
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetPathDescription().GetSecretDescription().GetIamDelegation().SerializeAsString(), reference.SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(createId).GetDelegation().GetState(), P::REVOCATION_PENDING);
        f.Drop();
        f.RebootTablet();
        f.StartWorker();
        for (ui32 i = 0; i < 2; ++i) {
            auto revoke = f.Take(f.Submissions);
            UNIT_ASSERT(revoke->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Kind == EDelegationOperationKind::Revoke);
            f.ReplyIam(*revoke, EDelegationOperationOutcome::Succeeded);
        }
        f.Until([&] { return f.ObservedDelegations[createId].GetState() == P::REVOKED
            && f.ObservedDelegations[alterId].GetState() == P::REVOKED; });
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(alterId).GetSecret().GetState(), P::SECRET_DROPPED);
        UNIT_ASSERT_VALUES_EQUAL(f.RevokeBindings.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(f.RevokeBindings[0].ServiceId, "saved-ydb-service");
    }

    Y_UNIT_TEST(AlterNeedsAlterSchemaWithoutDescribeOrValueAccess) {
        TOrchestratorFixture f;
        const auto createId = f.Create();
        const auto previous = f.Delegation(createId).GetSecret().GetCurrentOperationId();
        const auto editor = MakeIntrusiveConst<NACLib::TUserToken>(
            "editor@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        UNIT_ASSERT_VALUES_UNEQUAL(f.Describe().GetPathDescription().GetSelf().GetOwner(), editor->GetUserSID());
        UNIT_ASSERT(f.Scheme([&](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpModifyACL);
            NACLib::TDiffACL acl;
            acl.AddAccess(NACLib::EAccessType::Allow, NACLib::AlterSchema, editor->GetUserSID());
            acl.AddAccess(NACLib::EAccessType::Deny, NACLib::DescribeSchema | NACLib::SelectRow, editor->GetUserSID());
            scheme.MutableModifyACL()->SetName("secret");
            scheme.MutableModifyACL()->SetDiffACL(acl.SerializeAsString());
        }).Success());
        f.Bob = editor;
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("secret", editor).GetStatus(), NKikimrScheme::StatusAccessDenied);
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("secret", editor, false).GetStatus(), NKikimrScheme::StatusAccessDenied);
        f.AutoReply = true;
        const auto result = f.Finish(f.Start(f.Operation(true, "account-2")));
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        const auto secret = f.Delegation(createId).GetSecret();
        UNIT_ASSERT_VALUES_UNEQUAL(secret.GetCurrentOperationId(), previous);
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(secret.GetCurrentOperationId()).GetDelegation().GetBinding().GetServiceAccountId(), "account-2");
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("secret", editor).GetStatus(), NKikimrScheme::StatusAccessDenied);
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("secret", editor, false).GetStatus(), NKikimrScheme::StatusAccessDenied);
    }

    Y_UNIT_TEST(DescribeSchemaAndHiddenPathsDoNotAuthorizeAlter) {
        TOrchestratorFixture f;
        const auto createId = f.Create();
        const auto previous = f.Delegation(createId).GetSecret().SerializeAsString();
        struct TCase { TString Sid; ui32 Allow; ui32 Deny; };
        for (const auto& entry : TVector<TCase>{
            {"viewer@" BUILTIN_ACL_DOMAIN, NACLib::DescribeSchema, NACLib::AlterSchema},
            {"hidden@" BUILTIN_ACL_DOMAIN, 0, NACLib::AlterSchema | NACLib::DescribeSchema},
            {"unlisted@" BUILTIN_ACL_DOMAIN, 0, 0}})
        {
            const auto user = MakeIntrusiveConst<NACLib::TUserToken>(entry.Sid, TVector<NACLib::TSID>{});
            if (entry.Allow || entry.Deny) {
                f.SetAccess("secret", user->GetUserSID(), entry.Allow, entry.Deny);
            }
            f.Bob = user;
            f.Commands.clear();
            auto op = f.Operation(true, "account-2");
            op.SuccessOnNotExist = true; // a hidden denial must not look like a missing IF EXISTS target
            const auto result = f.Finish(f.Start(std::move(op)));
            UNIT_ASSERT(!result.Success());
            UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAUTHORIZED), result.Issues().ToString());
            UNIT_ASSERT(f.Commands.empty());
            UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
            UNIT_ASSERT(f.RevokeBindings.empty());
            UNIT_ASSERT_VALUES_EQUAL(f.Delegation(createId).GetSecret().SerializeAsString(), previous);
        }
    }

    Y_UNIT_TEST(AlterRechecksRevokedAclBeforeLifecycleMutation) {
        for (const TString account : {"account-1", "account-2"}) {
            TOrchestratorFixture f;
            const auto createId = f.Create();
            const auto previous = f.Delegation(createId).GetSecret().SerializeAsString();
            const auto editor = MakeIntrusiveConst<NACLib::TUserToken>(
                "editor@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
            f.SetAccess("secret", editor->GetUserSID(), NACLib::AlterSchema | NACLib::DescribeSchema);
            f.Bob = editor;
            TAutoPtr<IEventHandle> held;
            f.Intercept = [&](auto& event) {
                if (event->GetTypeRewrite() == TEvTxUserProxy::TEvProposeTransaction::EventType
                    && event->template Get<TEvTxUserProxy::TEvProposeTransaction>()->Record
                        .GetTransaction().GetModifyScheme().GetOperationType() == NKikimrSchemeOp::ESchemeOpAlterSecret) {
                    UNIT_ASSERT(!held);
                    held.Reset(event.Release());
                    return EAction::DROP;
                }
                return EAction::PROCESS;
            };
            const auto altering = f.Start(f.Operation(true, account));
            f.Until([&] { return bool(held); });
            UNIT_ASSERT_VALUES_UNEQUAL(held->Get<TEvTxUserProxy::TEvProposeTransaction>()->Record.GetUserToken(), "");
            f.SetAccess("secret", editor->GetUserSID(), 0, NACLib::AlterSchema);
            f.Intercept = {};
            f.Runtime.Send(held.Release());
            const auto result = f.Finish(altering);
            UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAUTHORIZED), result.Issues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
            UNIT_ASSERT(f.RevokeBindings.empty());
            UNIT_ASSERT_VALUES_EQUAL(f.Delegation(createId).GetSecret().SerializeAsString(), previous);
        }
    }

    Y_UNIT_TEST(AlterCannotTouchRecreatedSecretDuringResolution) {
        TOrchestratorFixture f;
        const auto createId = f.Create();
        const auto previous = f.Delegation(createId).GetSecret().SerializeAsString();
        TActorId orchestrator;
        TAutoPtr<IEventHandle> held;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvTxProxySchemeCache::TEvNavigateKeySetResult::EventType
                && event->GetRecipientRewrite() == orchestrator && !held) {
                held.Reset(event.Release());
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto altering = f.Start(f.Operation(true, "account-2"));
        orchestrator = altering.Actor;
        f.Until([&] { return bool(held); });
        f.Drop();
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
            scheme.MutableCreateSecret()->SetName("secret");
            scheme.MutableCreateSecret()->SetValue("recreated-during-resolution");
        }).Success());
        f.Intercept = {};
        f.Runtime.Send(held.Release());
        UNIT_ASSERT(!f.Finish(altering).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
        UNIT_ASSERT(f.RevokeBindings.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(createId).GetSecret().SerializeAsString(), previous);
        const auto schema = f.Describe();
        UNIT_ASSERT(!schema.GetPathDescription().GetSecretDescription().HasIamDelegation());
        UNIT_ASSERT_VALUES_EQUAL(schema.GetPathDescription().GetSecretDescription().GetValue(), "recreated-during-resolution");
    }

    Y_UNIT_TEST(CreateOrReplaceRequiresParentCreateAndTargetAlterWithoutDescribe) {
        TOrchestratorFixture f;
        const TVector<TString> principals = {"parent-only@" BUILTIN_ACL_DOMAIN,
            "object-only@" BUILTIN_ACL_DOMAIN, "editor@" BUILTIN_ACL_DOMAIN};
        // Exercise one completed ACL configuration for every principal.
        for (const auto& sid : principals) {
            if (!sid.StartsWith("object-only")) {
                f.SetAccess("Root", sid, NACLib::CreateTable);
            }
        }
        const auto createId = f.Create();
        const auto previous = f.Delegation(createId).GetSecret().SerializeAsString();
        for (const auto& sid : principals) {
            f.SetAccess("secret", sid, sid.StartsWith("parent-only") ? 0 : NACLib::AlterSchema,
                NACLib::DescribeSchema | NACLib::SelectRow | (sid.StartsWith("parent-only") ? NACLib::AlterSchema : 0));
        }
        for (const auto& sid : principals) {
            f.Bob = MakeIntrusiveConst<NACLib::TUserToken>(sid, TVector<NACLib::TSID>{});
            const auto self = f.Describe().GetPathDescription().GetSelf();
            NACLib::TSecurityObject target(self.GetOwner(), false);
            UNIT_ASSERT(target.MutableACL()->ParseFromString(self.GetEffectiveACL()));
            UNIT_ASSERT_VALUES_EQUAL_C(target.CheckAccess(NACLib::AlterSchema, *f.Bob),
                !sid.StartsWith("parent-only"), target.ToString());
            UNIT_ASSERT(!target.CheckAccess(NACLib::DescribeSchema, *f.Bob));
            UNIT_ASSERT(!target.CheckAccess(NACLib::SelectRow, *f.Bob));
            const auto parent = f.Describe("").GetPathDescription().GetSelf();
            NACLib::TSecurityObject directory(parent.GetOwner(), true);
            UNIT_ASSERT(directory.MutableACL()->ParseFromString(parent.GetEffectiveACL()));
            UNIT_ASSERT_VALUES_EQUAL_C(directory.CheckAccess(NACLib::CreateTable, *f.Bob),
                !sid.StartsWith("object-only"), directory.ToString());
            f.Commands.clear();
            auto op = f.Operation(false, "account-2");
            op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetReplaceIfExists(true);
            f.AutoReply = true;
            const auto result = f.Finish(f.Start(std::move(op)));
            if (sid.StartsWith("editor")) {
                UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
                const auto secret = f.Delegation(createId).GetSecret();
                UNIT_ASSERT_VALUES_EQUAL(f.Delegation(secret.GetCurrentOperationId()).GetDelegation().GetBinding().GetServiceAccountId(), "account-2");
                UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 2);
            } else {
                UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAUTHORIZED), result.Issues().ToString());
                for (const auto& command : f.Commands) {
                    UNIT_ASSERT(command.HasGetSecret() || command.HasGetDelegation());
                }
                UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(f.Delegation(createId).GetSecret().SerializeAsString(), previous);
            }
            UNIT_ASSERT(f.RevokeBindings.empty());
        }
    }

    Y_UNIT_TEST(CreateOrReplaceMissingPathRequiresParentCreatePermission) {
        TOrchestratorFixture f;
        const auto creator = f.Bob;
        const auto viewer = MakeIntrusiveConst<NACLib::TUserToken>(
            "viewer@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        f.SetAccess("Root", viewer->GetUserSID(), NACLib::DescribeSchema, NACLib::CreateTable);
        f.Bob = viewer;
        f.Commands.clear();
        auto op = f.Operation();
        op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetReplaceIfExists(true);
        const auto denied = f.Finish(f.Start(std::move(op)));
        UNIT_ASSERT_C(denied.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAUTHORIZED), denied.Issues().ToString());
        UNIT_ASSERT(f.Commands.empty());
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        f.Bob = creator;
        f.AutoReply = true;
        op = f.Operation();
        op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetReplaceIfExists(true);
        const auto created = f.Finish(f.Start(std::move(op)));
        UNIT_ASSERT_C(created.Success(), created.Issues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    }

    Y_UNIT_TEST(FailedCreateReleasesSchemaAndNameReservation) {
        TOrchestratorFixture f;
        const auto creating = f.Start(f.Operation());
        auto submit = f.Take(f.Submissions);
        const TString id = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        f.ReplyIam(*submit, EDelegationOperationOutcome::Failed, "refused-setup");
        UNIT_ASSERT(!f.Finish(creating).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetDelegation().GetState(), P::CANCELLED);
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetSecret().GetState(), P::SECRET_DROPPED);
        const auto replacement = f.Create();
        UNIT_ASSERT_VALUES_UNEQUAL(replacement, id);
        UNIT_ASSERT(f.RevokeBindings.empty());
    }

    Y_UNIT_TEST(PoisonAfterSubmissionKeepsUnknownSetupWithoutRetry) {
        TOrchestratorFixture f;
        const auto creating = f.Start(f.Operation());
        auto submit = f.Take(f.Submissions);
        const TString id = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        f.Runtime.Send(new IEventHandle(creating.Actor, {}, new TEvents::TEvPoison));
        const auto result = f.Finish(creating);
        UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::CANCELLED), result.Issues().ToString());
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded, "late-setup");
        f.RebootTablet();
        const auto durable = f.Delegation(id);
        UNIT_ASSERT_VALUES_EQUAL(durable.GetDelegation().GetSetupState(), P::SETUP_STARTED);
        UNIT_ASSERT_VALUES_EQUAL(durable.GetDelegation().GetSetupOperationId(), "");
        UNIT_ASSERT_VALUES_EQUAL(durable.GetSecret().GetPendingOperationId(), id);
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
        UNIT_ASSERT(f.RevokeBindings.empty());
    }

    Y_UNIT_TEST(FailedAlterKeepsCurrentReadableAndAllowsAnotherReplacement) {
        TOrchestratorFixture f;
        const auto original = f.Create();
        const auto replacing = f.Start(f.Operation(true, "refused-account"));
        auto submit = f.Take(f.Submissions);
        const TString failed = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        f.ReplyIam(*submit, EDelegationOperationOutcome::Failed);
        UNIT_ASSERT(!f.Finish(replacing).Success());
        const auto state = f.Delegation(failed);
        UNIT_ASSERT_VALUES_EQUAL(state.GetDelegation().GetState(), P::CANCELLED);
        UNIT_ASSERT_VALUES_EQUAL(state.GetSecret().GetCurrentOperationId(), original);
        UNIT_ASSERT_VALUES_EQUAL(state.GetSecret().GetPendingOperationId(), "");
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        const auto retry = f.Start(f.Operation(true, "account-2"));
        submit = f.Take(f.Submissions);
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        UNIT_ASSERT(f.Finish(retry).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-2");
        UNIT_ASSERT(f.RevokeBindings.empty());
    }

    Y_UNIT_TEST(WorkerResumesCrashAfterDurableStageBeforeSchemaSubmission) {
        TOrchestratorFixture f;
        TActorId orchestrator;
        P::TDelegation staged;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType
                && event->GetRecipientRewrite() == orchestrator) {
                const auto& reply = event->template Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                if (reply.HasDelegation() && !reply.GetDelegation().HasSecret()) {
                    staged = reply.GetDelegation();
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        orchestrator = creating.Actor;
        f.Until([&] { return !staged.GetOperationId().empty(); });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        f.Runtime.Send(new IEventHandle(orchestrator, {}, new TEvents::TEvPoison));
        UNIT_ASSERT(!f.Finish(creating).Success());
        f.Intercept = {};
        f.RebootTablet();
        f.AutoReply = true;
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[staged.GetOperationId()].GetState() == P::ACTIVE; });
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetPathDescription().GetSelf().GetCreateTxId(), staged.GetCreateTxId());
    }

    Y_UNIT_TEST(WorkerRecoversLostStageReplyWithoutNewUserRequest) {
        TOrchestratorFixture f;
        P::TDelegation staged;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType && staged.GetOperationId().empty()) {
                const auto& reply = event->template Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                if (reply.HasDelegation() && !reply.GetDelegation().HasSecret()) {
                    staged = reply.GetDelegation();
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        UNIT_ASSERT(!f.Finish(creating).Success());
        UNIT_ASSERT(!staged.GetOperationId().empty());
        f.Runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(false);
        f.Intercept = {};
        f.RebootTablet();
        f.AutoReply = true;
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[staged.GetOperationId()].GetState() == P::ACTIVE; });
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    }

    Y_UNIT_TEST(DelayedOriginalCreateAndWorkerReplayUseOneSchemaIdentity) {
        TOrchestratorFixture f;
        TAutoPtr<IEventHandle> original;
        f.Intercept = [&](auto& event) {
            if (!original && event->GetTypeRewrite() == NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction::EventType) {
                const auto& request = event->template Get<NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
                if (request.TransactionSize() == 1 && request.GetTransaction(0).GetCreateSecret().HasIamDelegationReference()) {
                    original = event.Release();
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        f.Until([&] { return bool(original); });
        const auto txId = original->Get<NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction>()->Record.GetTxId();
        f.Runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(false);
        f.Intercept = {};
        f.AutoReply = true;
        f.StartWorker();
        f.Until([&] { return !f.SetupBindings.empty(); });
        const auto before = f.Describe().GetPathDescription().GetSelf();
        f.Runtime.Send(original.Release());
        UNIT_ASSERT(f.Finish(creating).Success());
        const auto after = f.Describe().GetPathDescription().GetSelf();
        UNIT_ASSERT_VALUES_EQUAL(after.GetPathId(), before.GetPathId());
        UNIT_ASSERT_VALUES_EQUAL(after.GetCreateTxId(), txId);
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    }

    Y_UNIT_TEST(AdmissionDisabledAfterStageStillRecoversCreate) {
        TOrchestratorFixture f;
        TAutoPtr<IEventHandle> original;
        f.Intercept = [&](auto& event) {
            if (!original && event->GetTypeRewrite() == NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction::EventType) {
                const auto& request = event->template Get<NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
                if (request.TransactionSize() == 1 && request.GetTransaction(0).GetCreateSecret().HasIamDelegationReference()) {
                    original = event.Release();
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        f.Until([&] { return bool(original); });
        const auto request = original->Get<NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
        const auto operationId = request.GetTransaction(0).GetCreateSecret().GetIamDelegationReference().GetCreateOperationId();
        f.Runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(false);
        f.Intercept = {};
        f.RebootTablet();
        f.AutoReply = true;
        f.Runtime.Send(original.Release());
        const auto result = f.Finish(creating);
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[operationId].GetState() == P::ACTIVE; });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetPathDescription().GetSelf().GetCreateTxId(), request.GetTxId());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    }

    Y_UNIT_TEST(DisabledAdmissionRejectsFreshSqlMutations) {
        TOrchestratorFixture f;
        const auto id = f.Create();
        const auto before = f.Delegation(id);
        f.Runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(false);
        for (const TString sql : {
            "CREATE SECRET another WITH (SOURCE = 'YC_IAM_DELEGATION', SERVICE_ACCOUNT_ID = 'account-2', RESOURCE = 'cloud-1');",
            "ALTER SECRET secret WITH (SERVICE_ACCOUNT_ID = 'account-2');",
            "CREATE OR REPLACE SECRET secret WITH (SOURCE = 'YC_IAM_DELEGATION', SERVICE_ACCOUNT_ID = 'account-2', RESOURCE = 'cloud-1');"}) {
            const auto result = f.Kikimr.RunCall([&] {
                auto client = f.Kikimr.GetQueryClient(NYdb::NQuery::TClientSettings().AuthToken("bob@" BUILTIN_ACL_DOMAIN));
                return client.ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
            });
            UNIT_ASSERT_C(!result.IsSuccess(), sql);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED);
        }
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetSecret().SerializeAsString(), before.GetSecret().SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    }

    Y_UNIT_TEST(DropBeforeBindingFencesEveryDelayedCreateCopy) {
        TOrchestratorFixture f;
        TAutoPtr<IEventHandle> original;
        TString operationId;
        bool heldBind = false;
        f.Intercept = [&](auto& event) {
            if (!original && event->GetTypeRewrite() == NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction::EventType) {
                const auto& request = event->template Get<NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
                if (request.TransactionSize() == 1 && request.GetTransaction(0).GetCreateSecret().HasIamDelegationReference()) {
                    operationId = request.GetTransaction(0).GetCreateSecret().GetIamDelegationReference().GetCreateOperationId();
                    original = event.Release();
                    return EAction::DROP;
                }
            }
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvRequest::EventType
                && event->GetRecipientRewrite() == f.TabletActor
                && event->template Get<TEvIamDelegationTablet::TEvRequest>()->Record.HasBindSecret()) {
                heldBind = true;
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        f.Until([&] { return bool(original); });
        f.Runtime.Send(new IEventHandle(creating.Actor, {}, new TEvents::TEvPoison));
        UNIT_ASSERT(!f.Finish(creating).Success());
        f.StartWorker();
        f.Until([&] { return heldBind; });
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpDropSecret);
            scheme.MutableDrop()->SetName("secret");
        }).Success());
        f.Intercept = {};
        f.Runtime.Send(original.Release());
        f.Until([&] { return f.ObservedDelegations[operationId].GetState() == P::CANCELLED; });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT(!f.Create().empty());
    }

    Y_UNIT_TEST(PreDispatchRejectionPreservesRecoverableIntent) {
        TOrchestratorFixture f;
        TString operationId;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvTxUserProxy::TEvProposeTransaction::EventType) {
                const auto& scheme = event->template Get<TEvTxUserProxy::TEvProposeTransaction>()
                    ->Record.GetTransaction().GetModifyScheme();
                if (scheme.GetCreateSecret().HasIamDelegationReference()) {
                    operationId = scheme.GetCreateSecret().GetIamDelegationReference().GetCreateOperationId();
                    f.Runtime.Send(new IEventHandle(event->Sender, event->Recipient,
                        new TEvTxUserProxy::TEvProposeTransactionStatus(TEvTxUserProxy::TResultStatus::AccessDenied)));
                    return EAction::DROP;
                }
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        const auto result = f.Finish(creating);
        UNIT_ASSERT(!result.Success());
        UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAVAILABLE), result.Issues().ToString());
        UNIT_ASSERT(!operationId.empty());
        const auto pending = f.Delegation(operationId).GetDelegation();
        UNIT_ASSERT_VALUES_EQUAL(pending.GetState(), P::PENDING);
        UNIT_ASSERT(!pending.HasSecret());
        UNIT_ASSERT(pending.HasSchemaCreate());
        f.Intercept = {};
        f.AutoReply = true;
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[operationId].GetState() == P::ACTIVE; });
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
    }

    Y_UNIT_TEST(SiblingMutationConsumesSavedCreateFence) {
        TOrchestratorFixture f;
        const auto staged = f.StageWithoutSchema(f.Operation());
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpMkDir);
            scheme.MutableMkDir()->SetName("sibling");
        }).Success());
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[staged.GetOperationId()].GetState() == P::CANCELLED; });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT(!f.Create().empty());
    }

    Y_UNIT_TEST(ParentNameReuseCannotReplayOldCreateIntent) {
        TOrchestratorFixture f;
        const auto createParent = [&] {
            UNIT_ASSERT(f.Scheme([](auto& scheme) {
                scheme.SetWorkingDir("/Root");
                scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpMkDir);
                scheme.MutableMkDir()->SetName("parent");
            }).Success());
        };
        createParent();
        const auto staged = f.StageWithoutSchema(f.Operation(false, "account-1", "cloud-1", "parent/secret"));
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpRmDir);
            scheme.MutableDrop()->SetName("parent");
        }).Success());
        createParent();
        UNIT_ASSERT_VALUES_UNEQUAL(f.Describe("parent").GetPathDescription().GetSelf().GetPathId(),
            staged.GetSchemaCreate().GetParentPathLocalId());
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[staged.GetOperationId()].GetState() == P::CANCELLED; });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("parent/secret").GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        UNIT_ASSERT(f.SetupBindings.empty());
        f.AutoReply = true;
        const auto creating = f.Start(f.Operation(false, "account-2", "cloud-1", "parent/secret"));
        UNIT_ASSERT(f.Finish(creating).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.Read("parent/secret")->Token, "token-for-account-2");
    }

    Y_UNIT_TEST(DatabaseNameReuseCannotReplayOldCreateIntent) {
        TOrchestratorFixture f;
        const auto createDatabase = [&] {
            UNIT_ASSERT(f.Scheme([](auto& scheme) {
                scheme.SetWorkingDir("/Root");
                scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSubDomain);
                scheme.MutableSubDomain()->SetName("database");
                scheme.MutableSubDomain()->SetCoordinators(0);
                scheme.MutableSubDomain()->SetMediators(0);
            }).Success());
        };
        const auto operation = [&] {
            auto op = f.Operation();
            op.Database = "/Root/database";
            op.Request->Record.SetDatabaseName(op.Database);
            op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetWorkingDir(op.Database);
            return op;
        };
        createDatabase();
        const auto staged = f.StageWithoutSchema(operation());
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpDropSubDomain);
            scheme.MutableDrop()->SetName("database");
        }).Success());
        createDatabase();
        UNIT_ASSERT_VALUES_UNEQUAL(f.Describe("database").GetPathDescription().GetSelf().GetPathId(),
            staged.GetSchemaCreate().GetDatabaseDomainLocalId());
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[staged.GetOperationId()].GetState() == P::CANCELLED; });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe("database/secret").GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        UNIT_ASSERT(f.SetupBindings.empty());
        f.AutoReply = true;
        const auto creating = f.Start(operation());
        UNIT_ASSERT(f.Finish(creating).Success());
        UNIT_ASSERT_VALUES_UNEQUAL(f.Describe("database/secret").GetPathDescription()
            .GetSecretDescription().GetIamDelegation().GetDatabaseIncarnation(), staged.GetDatabaseIncarnation());
    }

    Y_UNIT_TEST(WorkerResumesCrashAfterSchemaBeforeSetup) {
        TOrchestratorFixture f;
        bool heldStart = false;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvRequest::EventType
                && event->GetRecipientRewrite() == f.TabletActor
                && event->template Get<TEvIamDelegationTablet::TEvRequest>()->Record.HasStartSetup()) {
                heldStart = true;
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        f.Until([&] { return heldStart; });
        const auto reference = f.Describe().GetPathDescription().GetSecretDescription().GetIamDelegation();
        f.Runtime.Send(new IEventHandle(creating.Actor, {}, new TEvents::TEvPoison));
        UNIT_ASSERT(!f.Finish(creating).Success());
        f.Intercept = {};
        f.RebootTablet();
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(reference.GetCreateOperationId()).GetDelegation().GetSetupState(), P::SETUP_NOT_STARTED);
        f.StartWorker();
        auto submit = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->SubjectId, "bob");
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        f.Until([&] { return f.ObservedDelegations[reference.GetCreateOperationId()].GetState() == P::ACTIVE; });
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
    }

    Y_UNIT_TEST(WorkerPollsAcceptedSetupAfterClientAndTabletRestart) {
        TOrchestratorFixture f;
        const auto creating = f.Start(f.Operation());
        auto submit = f.Take(f.Submissions);
        const TString id = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        f.ReplyIam(*submit, EDelegationOperationOutcome::Accepted, "accepted-before-crash");
        f.Until([&] { return f.ObservedDelegations[id].GetSetupOperationId() == "accepted-before-crash"; });
        f.Runtime.Send(new IEventHandle(creating.Actor, {}, new TEvents::TEvPoison));
        UNIT_ASSERT(!f.Finish(creating).Success());
        f.RebootTablet();
        f.StartWorker();
        auto poll = f.Take(f.Polls);
        UNIT_ASSERT_VALUES_EQUAL(poll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId, "accepted-before-crash");
        f.ReplyIam(*poll, EDelegationOperationOutcome::Succeeded, "accepted-before-crash");
        f.Until([&] { return f.ObservedDelegations[id].GetState() == P::ACTIVE; });
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
    }

    Y_UNIT_TEST(IfNotExistsRaceDoesNotSetupOrDropCompetitor) {
        TOrchestratorFixture f;
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
            scheme.MutableCreateSecret()->SetName("secret");
            scheme.MutableCreateSecret()->SetValue("competitor-value");
        }).Success());
        TActorId orchestrator;
        ui32 forged = 0;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::EventType
                && event->GetRecipientRewrite() == orchestrator && !forged) {
                auto& record = *event->template Get<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>()->MutableRecord();
                record.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
                record.ClearPathDescription();
                ++forged;
            }
            return EAction::PROCESS;
        };
        auto op = f.Operation();
        op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetFailOnExist(false);
        op.FailedOnAlreadyExists = false;
        const auto creating = f.Start(std::move(op));
        orchestrator = creating.Actor;
        UNIT_ASSERT(f.Finish(creating).Success());
        UNIT_ASSERT_VALUES_EQUAL(forged, 1);
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT(f.RevokeBindings.empty());
        const auto schema = f.Describe();
        UNIT_ASSERT_VALUES_EQUAL(schema.GetStatus(), NKikimrScheme::StatusSuccess);
        UNIT_ASSERT(!schema.GetPathDescription().GetSecretDescription().HasIamDelegation());
        UNIT_ASSERT_VALUES_EQUAL(schema.GetPathDescription().GetSecretDescription().GetValue(), "competitor-value");
        f.Intercept = {};
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/Root");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpDropSecret);
            scheme.MutableDrop()->SetName("secret");
        }).Success());
        UNIT_ASSERT(!f.Create().empty()); // rejected unbound CREATE must release its name
    }

    Y_UNIT_TEST(AlterCannotTouchRecreatedSecretAfterAuthorization) {
        for (const TString account : {"account-1", "account-2"}) {
            TOrchestratorFixture f;
            const auto id = f.Create();
            TAutoPtr<IEventHandle> held;
            f.Intercept = [&](auto& event) {
                if (event->GetTypeRewrite() == TEvTxUserProxy::TEvProposeTransaction::EventType
                    && event->template Get<TEvTxUserProxy::TEvProposeTransaction>()->Record
                        .GetTransaction().GetModifyScheme().GetOperationType() == NKikimrSchemeOp::ESchemeOpAlterSecret) {
                    UNIT_ASSERT(!held);
                    held.Reset(event.Release());
                    return EAction::DROP;
                }
                return EAction::PROCESS;
            };
            const auto altering = f.Start(f.Operation(true, account));
            f.Until([&] { return bool(held); });
            f.Drop();
            UNIT_ASSERT(f.Scheme([](auto& scheme) {
                scheme.SetWorkingDir("/Root");
                scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
                scheme.MutableCreateSecret()->SetName("secret");
                scheme.MutableCreateSecret()->SetValue("recreated-value");
            }).Success());
            f.Intercept = {};
            f.Runtime.Send(held.Release());
            UNIT_ASSERT(!f.Finish(altering).Success());
            UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetSecret().GetPendingOperationId(), "");
            const auto schema = f.Describe();
            UNIT_ASSERT(!schema.GetPathDescription().GetSecretDescription().HasIamDelegation());
            UNIT_ASSERT_VALUES_EQUAL(schema.GetPathDescription().GetSecretDescription().GetValue(), "recreated-value");
        }
    }

    Y_UNIT_TEST(NoopAlterPreservesPendingNotStarted) {
        CheckNoopAlterPreservesPending(P::SETUP_NOT_STARTED);
    }

    Y_UNIT_TEST(NoopAlterPreservesPendingSucceeded) {
        CheckNoopAlterPreservesPending(P::SETUP_SUCCEEDED);
    }

    Y_UNIT_TEST(NoopAlterPreservesYoungUnknownAfterRestart) {
        CheckNoopAlterPreservesPending(P::SETUP_UNKNOWN, TDuration::Minutes(1));
    }

    Y_UNIT_TEST(NoopAlterPreservesExpiredUnknownAfterRestart) {
        CheckNoopAlterPreservesPending(P::SETUP_UNKNOWN, TDuration::Minutes(10));
    }

    Y_UNIT_TEST(NoopsNeedNoIamConfigurationOrLifecycleMutation) {
        TOrchestratorFixture f;
        const auto id = f.Create();
        const auto previous = f.Delegation(id).GetSecret();
        f.Runtime.GetAppData(0).IamConfig.Clear();
        auto op = f.Operation(false, "ignored-account");
        op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetFailOnExist(false);
        op.FailedOnAlreadyExists = false;
        UNIT_ASSERT(f.Finish(f.Start(std::move(op))).Success());
        UNIT_ASSERT(f.Finish(f.Start(f.Operation(true))).Success());
        op = f.Operation(true, "ignored-account", "cloud-1", "absent");
        op.SuccessOnNotExist = true;
        UNIT_ASSERT(f.Finish(f.Start(std::move(op))).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);
        UNIT_ASSERT(f.RevokeBindings.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetSecret().SerializeAsString(), previous.SerializeAsString());
    }

    Y_UNIT_TEST(FailedCreateRecoveryDropCannotRemoveConcurrentReplacement) {
        TOrchestratorFixture f;
        TAutoPtr<IEventHandle> heldDrop;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvTxUserProxy::TEvProposeTransaction::EventType
                && event->template Get<TEvTxUserProxy::TEvProposeTransaction>()->Record
                    .GetTransaction().GetModifyScheme().GetOperationType() == NKikimrSchemeOp::ESchemeOpDropSecret) {
                UNIT_ASSERT(!heldDrop);
                heldDrop.Reset(event.Release());
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto creating = f.Start(f.Operation());
        auto submit = f.Take(f.Submissions);
        const TString id = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        f.ReplyIam(*submit, EDelegationOperationOutcome::Failed);
        f.Until([&] { return bool(heldDrop); });
        f.Runtime.Send(new IEventHandle(creating.Actor, {}, new TEvents::TEvPoison));
        UNIT_ASSERT(!f.Finish(creating).Success());
        heldDrop.Reset(); // creator died before its cleanup reached TxProxy
        f.RebootTablet();
        f.StartWorker();
        f.Until([&] { return bool(heldDrop); });
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetPathDescription().GetSecretDescription().GetVersion(), 0);

        const auto altering = f.Start(f.Operation(true, "account-2"));
        submit = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetPathDescription().GetSecretDescription().GetVersion(), 1);
        bool dropRejected = false;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvTxUserProxy::TEvProposeTransactionStatus::EventType
                && event->template Get<TEvTxUserProxy::TEvProposeTransactionStatus>()->Record.GetSchemeShardStatus()
                    == NKikimrScheme::StatusPreconditionFailed) {
                dropRejected = true;
            }
            return EAction::PROCESS;
        };
        f.Runtime.Send(heldDrop.Release());
        f.Until([&] { return dropRejected; });
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        UNIT_ASSERT(f.Finish(altering).Success());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-2");
        UNIT_ASSERT_VALUES_EQUAL(f.Delegation(id).GetSecret().GetState(), P::SECRET_LIVE);
        UNIT_ASSERT(f.RevokeBindings.empty());
    }

    Y_UNIT_TEST(NoBearerUsesDatabaseCloudWithoutLookup) {
        TOrchestratorFixture f;
        UNIT_ASSERT(f.Scheme([](auto& scheme) {
            scheme.SetWorkingDir("/");
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpAlterUserAttributes);
            auto* attributes = scheme.MutableAlterUserAttributes();
            attributes->SetPathName("Root");
            auto* cloud = attributes->AddUserAttributes();
            cloud->SetKey("cloud_id");
            cloud->SetValue("database-cloud");
        }).Success());
        const auto creating = f.Start(f.Operation(false, "account-1", {}));
        auto submit = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Binding.CloudId, "database-cloud");
        f.ReplyIam(*submit, EDelegationOperationOutcome::Succeeded);
        const auto result = f.Finish(creating);
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "RESOURCE database-cloud was taken from the database");
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "the user has no IAM token");
        UNIT_ASSERT(!result.Issues().Empty());
        for (const auto& issue : result.Issues()) {
            UNIT_ASSERT(issue.GetSeverity() == NYql::TSeverityIds::S_WARNING);
        }
    }

    Y_UNIT_TEST(DescribeTimeoutFailsBeforeStagingOrIam) {
        TOrchestratorFixture f;
        ui32 dropped = 0;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvTxUserProxy::TEvNavigate::EventType) {
                ++dropped;
                return EAction::DROP;
            }
            return EAction::PROCESS;
        };
        const auto result = f.Finish(f.Start(f.Operation()));
        UNIT_ASSERT(!result.Success());
        UNIT_ASSERT_C(result.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAVAILABLE), result.Issues().ToString());
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "/Root/secret");
        UNIT_ASSERT_VALUES_EQUAL(dropped, 1);
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT(f.Commands.empty());
    }

    Y_UNIT_TEST(UnauthorizedAndMissingCloudFailBeforeStagingOrIam) {
        TOrchestratorFixture f;
        auto denied = f.Operation();
        denied.UserToken = MakeIntrusiveConst<NACLib::TUserToken>("eve@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        denied.Request->Record.SetUserToken(denied.UserToken->SerializeAsString());
        UNIT_ASSERT(!f.Finish(f.Start(std::move(denied))).Success());
        const auto missingCloud = f.Finish(f.Start(f.Operation(false, "account-1", {})));
        UNIT_ASSERT(!missingCloud.Success());
        UNIT_ASSERT_STRING_CONTAINS(missingCloud.Issues().ToString(), "has no cloud_id attribute");
        UNIT_ASSERT(f.Commands.empty());
        UNIT_ASSERT(f.SetupBindings.empty());
    }

    Y_UNIT_TEST(ExplainCompilesWithoutLifecycleWritesOrIamConfiguration) {
        TOrchestratorFixture f;
        f.Runtime.GetAppData(0).IamConfig.Clear();
        for (const TString& sql : {
            TString("CREATE SECRET secret WITH (SOURCE = 'YC_IAM_DELEGATION', SERVICE_ACCOUNT_ID = 'account-1');"),
            TString("CREATE OR REPLACE SECRET secret WITH (SOURCE = 'YC_IAM_DELEGATION', SERVICE_ACCOUNT_ID = 'account-2');"),
            TString("ALTER SECRET secret WITH (SERVICE_ACCOUNT_ID = 'account-2');")})
        {
            const auto result = f.Kikimr.RunCall([&] {
                return f.Kikimr.GetQueryClient().ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx(),
                    NYdb::NQuery::TExecuteQuerySettings().ExecMode(NYdb::NQuery::EExecMode::Explain)).GetValueSync();
            });
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        UNIT_ASSERT(f.Commands.empty());
        UNIT_ASSERT(f.SetupBindings.empty());
        UNIT_ASSERT(f.Polls.empty());
        UNIT_ASSERT(f.MintKeys.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
    }

    Y_UNIT_TEST(SqlCreateAlterAndDropReachTabletAndReader) {
        TOrchestratorFixture f;
        f.AutoReply = true;
        auto query = [&](const TString& sql) {
            auto result = f.Kikimr.RunCall([&] {
                auto client = f.Kikimr.GetQueryClient(NYdb::NQuery::TClientSettings().AuthToken("bob@" BUILTIN_ACL_DOMAIN));
                return client.ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
            });
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        };
        query("CREATE SECRET secret WITH (SOURCE = 'YC_IAM_DELEGATION', SERVICE_ACCOUNT_ID = 'account-1', RESOURCE = 'cloud-1');");
        const TString createId = f.Describe().GetPathDescription().GetSecretDescription().GetIamDelegation().GetCreateOperationId();
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        query("ALTER SECRET secret WITH (SERVICE_ACCOUNT_ID = 'account-2');");
        const TString alterId = f.Delegation(createId).GetSecret().GetCurrentOperationId();
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-2");
        query("CREATE SECRET IF NOT EXISTS secret WITH (SOURCE = 'YC_IAM_DELEGATION', SERVICE_ACCOUNT_ID = 'ignored', RESOURCE = 'cloud-1');");
        query("ALTER SECRET IF EXISTS absent WITH (SERVICE_ACCOUNT_ID = 'ignored');");
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 2);
        query("DROP SECRET secret;");
        UNIT_ASSERT_VALUES_EQUAL(f.Describe().GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
        f.StartWorker();
        f.Until([&] { return f.ObservedDelegations[createId].GetState() == P::REVOKED
            && f.ObservedDelegations[alterId].GetState() == P::REVOKED; });
        UNIT_ASSERT_VALUES_EQUAL(f.RevokeBindings.size(), 2);
    }
    Y_UNIT_TEST(PopulatedTenantCleanupSurvivesWholeRuntimeDiskReopen) {
        using NIamDelegation::NTests::TTestContext;
        TTempDir directory;
        enum class EDiskOpen { Format, Reopen };
        const auto storage = [&](EDiskOpen mode) {
            NFake::TStorage disk;
            disk.UseDisk = true;
            disk.FormatDisk = mode == EDiskOpen::Format;
            disk.DiskPath = directory.Name() + "/";
            disk.DiskSize = 8ull << 30;
            disk.SectorSize = 4096;
            disk.ChunkSize = 32ull << 20;
            return disk;
        };
        TString savedActive, savedAccepted, savedUnknown, savedUndispatched, savedNew;
        TString oldDatabase;
        THashMap<TString, P::TDelegation> before;
        NKikimrScheme::TEvDescribeSchemeResult replacementSchema;
        TInstant clock;
        {
            TOrchestratorFixture f(storage(EDiskOpen::Format));
            const auto createDatabase = [&] {
                UNIT_ASSERT(f.Scheme([](auto& scheme) {
                    scheme.SetWorkingDir("/Root");
                    scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSubDomain);
                    scheme.MutableSubDomain()->SetName("database");
                    scheme.MutableSubDomain()->SetCoordinators(0);
                    scheme.MutableSubDomain()->SetMediators(0);
                }).Success());
            };
            const auto operation = [&](const TString& name, const TString& account) {
                auto op = f.Operation(false, account, "cloud-1", name);
                op.Database = "/Root/database";
                op.Request->Record.SetDatabaseName(op.Database);
                op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetWorkingDir(op.Database);
                return op;
            };
            const auto stop = [&](const TOrchestratorFixture::TRunning& running) {
                f.Runtime.Send(new IEventHandle(running.Actor, {}, new TEvents::TEvPoison));
                UNIT_ASSERT(!f.Finish(running).Success());
            };
            createDatabase();
            const auto creating = f.Start(operation("active", "old-active-account"));
            auto request = f.Take(f.Submissions);
            const TString activeId = request->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
            f.ReplyIam(*request, EDelegationOperationOutcome::Succeeded);
            UNIT_ASSERT(f.Finish(creating).Success());
            const auto original = f.Delegation(activeId).GetDelegation();
            const TString oldIncarnation = original.GetDatabaseIncarnation();

            const auto accepted = f.Start(operation("accepted", "accepted-account"));
            request = f.Take(f.Submissions);
            const TString acceptedId = request->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
            f.ReplyIam(*request, EDelegationOperationOutcome::Accepted, "tenant-setup-operation");
            auto latePoll = f.Take(f.Polls);
            UNIT_ASSERT_VALUES_EQUAL(latePoll->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId,
                "tenant-setup-operation");
            stop(accepted);

            const auto unknown = f.Start(operation("unknown", "unknown-account"));
            auto lateSetup = f.Take(f.Submissions); // the initial remote reply is lost
            const TString unknownId = lateSetup->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
            stop(unknown);
            UNIT_ASSERT(f.Delegation(unknownId).GetDelegation().GetSetupOperationId().empty());

            TAutoPtr<IEventHandle> lateStart;
            f.Intercept = [&](auto& event) {
                if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvRequest::EventType
                    && event->GetRecipientRewrite() == f.TabletActor
                    && event->template Get<TEvIamDelegationTablet::TEvRequest>()->Record.HasStartSetup()) {
                    UNIT_ASSERT(!lateStart);
                    lateStart.Reset(event.Release());
                    return EAction::DROP;
                }
                return EAction::PROCESS;
            };
            const auto undispatched = f.Start(operation("undispatched", "undispatched-account"));
            f.Until([&] { return bool(lateStart); });
            const TString undispatchedId = lateStart->Get<TEvIamDelegationTablet::TEvRequest>()->Record.GetStartSetup().GetOperationId();
            stop(undispatched);
            f.Intercept = {};
            UNIT_ASSERT_VALUES_EQUAL(f.Delegation(undispatchedId).GetDelegation().GetSetupState(), P::SETUP_NOT_STARTED);
            UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 3);

            UNIT_ASSERT(f.Scheme([](auto& scheme) {
                scheme.SetWorkingDir("/Root");
                scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpForceDropSubDomain);
                scheme.MutableDrop()->SetName("database");
            }).Success());
            UNIT_ASSERT_VALUES_EQUAL(f.Describe("database").GetStatus(), NKikimrScheme::StatusPathDoesNotExist);
            // The independent service tablet remains available after actual schema teardown.
            P::TRequest inventory;
            inventory.MutableListInventory()->SetDatabaseIncarnation(oldIncarnation);
            inventory.MutableListInventory()->SetPageSize(100);
            UNIT_ASSERT_VALUES_EQUAL(f.Tablet(inventory).DelegationsSize(), 4);
            createDatabase();
            const auto replacement = f.Start(operation("active", "new-active-account"));
            request = f.Take(f.Submissions);
            const TString newId = request->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
            f.ReplyIam(*request, EDelegationOperationOutcome::Succeeded);
            UNIT_ASSERT(f.Finish(replacement).Success());
            const auto newRecord = f.Delegation(newId).GetDelegation();
            UNIT_ASSERT_VALUES_UNEQUAL(newRecord.GetDatabaseIncarnation(), oldIncarnation);
            UNIT_ASSERT_VALUES_UNEQUAL(newRecord.GetSecret().SerializeAsString(), original.GetSecret().SerializeAsString());
            savedActive = activeId;
            savedAccepted = acceptedId;
            savedUnknown = unknownId;
            savedUndispatched = undispatchedId;
            savedNew = newId;
            oldDatabase = oldIncarnation;
            replacementSchema = f.Describe("database/active");
            for (const auto& id : {savedActive, savedAccepted, savedUnknown, savedUndispatched, savedNew}) {
                before[id] = f.Delegation(id).GetDelegation();
            }
            clock = f.Runtime.GetCurrentTime();
        } // All original actors and storage services are destroyed here.

        TTestContext reopened(storage(EDiskOpen::Reopen), TOrchestratorFixture::TabletId);
        auto& runtime = reopened.GetRuntime();
        runtime.UpdateCurrentTime(Max(clock, runtime.GetCurrentTime()));
        for (const auto& [id, expected] : before) {
            P::TRequest read;
            read.MutableGetDelegation()->SetOperationId(id);
            UNIT_ASSERT_VALUES_EQUAL(reopened.Call(read).GetDelegation().SerializeAsString(), expected.SerializeAsString());
        }
        enum class ECleanupIam { Outage, Recovered };
        auto iamMode = ECleanupIam::Outage;
        ui32 setupPolls = 0;
        ui32 revokeCalls = 0;
        THashMap<TString, P::TDelegation> observed;
        runtime.RegisterService(MakeTxProxyID(), runtime.AllocateEdgeActor());
        runtime.RegisterService(MakeIamDelegationServiceId(), runtime.AllocateEdgeActor());
        const auto previousSchedule = runtime.SetScheduledEventFilter([](auto&, auto&, auto, auto&) { return false; });
        Y_DEFER { runtime.SetScheduledEventFilter(previousSchedule); };
        runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == TEvTxUserProxy::TEvNavigate::EventType) {
                const auto& request = event->Get<TEvTxUserProxy::TEvNavigate>()->Record;
                auto reply = MakeHolder<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResultBuilder>();
                if (request.GetDescribePath().GetPath() == "/Root/database/active") {
                    reply->Record = replacementSchema;
                } else {
                    reply->Record.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
                }
                // Controlled observation after reopening: the real preceding runtime
                // already proved force-drop/name reuse; this only supplies its state.
                runtime.Send(new IEventHandle(event->Sender, event->Recipient, reply.Release(), 0, event->Cookie));
                return EAction::DROP;
            }
            if (event->GetTypeRewrite() == TEvIamDelegation::TEvPollDelegationOperation::EventType) {
                UNIT_ASSERT_VALUES_EQUAL(event->Get<TEvIamDelegation::TEvPollDelegationOperation>()->OperationId,
                    "tenant-setup-operation");
                ++setupPolls;
                TDelegationOperationResult result;
                result.Outcome = EDelegationOperationOutcome::Succeeded;
                result.Status = Ydb::StatusIds::SUCCESS;
                result.OperationId = "tenant-setup-operation";
                runtime.Send(new IEventHandle(event->Sender, event->Recipient,
                    new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, event->Cookie));
                return EAction::DROP;
            }
            if (event->GetTypeRewrite() == TEvIamDelegation::TEvSubmitDelegationOperation::EventType) {
                const auto& request = *event->Get<TEvIamDelegation::TEvSubmitDelegationOperation>();
                UNIT_ASSERT(request.Kind == EDelegationOperationKind::Revoke);
                UNIT_ASSERT_C(request.RequestId == savedActive || request.RequestId == savedAccepted,
                    "Unexpected cleanup: " << request.RequestId << "; active=" << savedActive
                    << "; accepted=" << savedAccepted << "; unknown=" << savedUnknown
                    << "; undispatched=" << savedUndispatched << "; new=" << savedNew
                    << "; replacement=" << replacementSchema.ShortDebugString()
                    << "; saved=" << before.at(request.RequestId).ShortDebugString());
                UNIT_ASSERT_VALUES_UNEQUAL(request.Binding.ReferrerId, before.at(savedNew).GetBinding().GetReferrerId());
                ++revokeCalls;
                TDelegationOperationResult result;
                result.Outcome = iamMode == ECleanupIam::Recovered
                    ? EDelegationOperationOutcome::Succeeded : EDelegationOperationOutcome::Unknown;
                result.Status = iamMode == ECleanupIam::Recovered ? Ydb::StatusIds::SUCCESS : Ydb::StatusIds::UNAVAILABLE;
                runtime.Send(new IEventHandle(event->Sender, event->Recipient,
                    new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, event->Cookie));
                return EAction::DROP;
            }
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType) {
                const auto& reply = event->Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                if (reply.GetStatus() == P::SUCCESS && reply.HasDelegation()) {
                    observed[reply.GetDelegation().GetOperationId()] = reply.GetDelegation();
                }
            }
            return EAction::PROCESS;
        });
        Y_DEFER { runtime.SetObserverFunc(TTestActorRuntimeBase::DefaultObserverFunc); };
        const auto until = [&](auto condition) {
            TDispatchOptions options;
            options.CustomFinalCondition = condition;
            options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
            runtime.DispatchEvents(options, TDuration::Minutes(10));
            UNIT_ASSERT(condition());
        };
        auto worker = runtime.Register(CreateIamDelegationRecoveryWorker(reopened.GetTabletId()));
        runtime.EnableScheduleForActor(worker);
        until([&] {
            return observed[savedActive].GetRevokeConsecutiveFailures() >= 1
                && observed[savedAccepted].GetRevokeConsecutiveFailures() >= 1
                && observed[savedUnknown].GetState() == P::WAITING_SETUP
                && observed[savedUndispatched].GetState() == P::CANCELLED;
        });
        runtime.Send(new IEventHandle(worker, {}, new TEvents::TEvPoison));
        reopened.Reboot();
        const auto read = [&](const TString& id) {
            P::TRequest request;
            request.MutableGetDelegation()->SetOperationId(id);
            return reopened.Call(request).GetDelegation();
        };
        const auto activeAge = read(savedActive).GetCleanupQueuedAtUs();
        const auto acceptedAge = read(savedAccepted).GetCleanupQueuedAtUs();
        UNIT_ASSERT(activeAge && acceptedAge);
        UNIT_ASSERT_VALUES_EQUAL(read(savedNew).SerializeAsString(), before.at(savedNew).SerializeAsString());
        UNIT_ASSERT(!read(savedUnknown).HasCleanupQueuedAtUs());
        iamMode = ECleanupIam::Recovered;
        worker = runtime.Register(CreateIamDelegationRecoveryWorker(reopened.GetTabletId()));
        runtime.EnableScheduleForActor(worker);
        until([&] { return observed[savedActive].GetState() == P::REVOKED
            && observed[savedAccepted].GetState() == P::REVOKED; });
        runtime.Send(new IEventHandle(worker, {}, new TEvents::TEvPoison));
        reopened.Reboot();
        UNIT_ASSERT_VALUES_EQUAL(read(savedActive).GetCleanupQueuedAtUs(), activeAge);
        UNIT_ASSERT_VALUES_EQUAL(read(savedAccepted).GetCleanupQueuedAtUs(), acceptedAge);
        UNIT_ASSERT_VALUES_EQUAL(read(savedNew).SerializeAsString(), before.at(savedNew).SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(read(savedUnknown).GetState(), P::WAITING_SETUP);
        UNIT_ASSERT(read(savedUnknown).GetSetupOperationId().empty());
        UNIT_ASSERT_VALUES_EQUAL(setupPolls, 1);
        UNIT_ASSERT(revokeCalls >= 4);
        P::TRequest inventory;
        inventory.MutableListInventory()->SetDatabaseIncarnation(oldDatabase);
        inventory.MutableListInventory()->SetPageSize(100);
        const auto retained = reopened.Call(inventory);
        UNIT_ASSERT_VALUES_EQUAL(retained.DelegationsSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(retained.GetDelegations(0).GetOperationId(), savedUnknown);
    }

    Y_UNIT_TEST(ReplacementGuardUsesOriginalTenMinuteBoundaryAfterRestart) {
        TOrchestratorFixture f;
        const auto currentId = f.Create();
        const auto current = f.Delegation(currentId);
        P::TRequest stage;
        auto* intent = stage.MutableStage();
        intent->SetMode(P::ALTER);
        intent->SetOperationId("pending-before-restart");
        intent->SetDatabaseIncarnation(current.GetDelegation().GetDatabaseIncarnation());
        intent->SetSecretPath(current.GetDelegation().GetSecretPath());
        intent->SetCreateTxId(current.GetDelegation().GetCreateTxId());
        *intent->MutableSecret() = current.GetSecret().GetIdentity();
        intent->SetExpectedSecretRevision(current.GetSecret().GetRevision());
        *intent->MutableBinding() = current.GetDelegation().GetBinding();
        intent->MutableBinding()->SetServiceAccountId("account-2");
        intent->MutableBinding()->SetReferrerId("boundary-pending-referrer");
        auto pending = f.Tablet(std::move(stage)).GetDelegation();
        const auto originalStagedAt = pending.GetStagedAtUs();
        P::TRequest start;
        start.MutableStartSetup()->SetOperationId(pending.GetOperationId());
        start.MutableStartSetup()->SetExpectedRevision(pending.GetRevision());
        pending = f.Tablet(std::move(start)).GetDelegation();
        P::TRequest outcome;
        outcome.MutableSetSetupResult()->SetOperationId(pending.GetOperationId());
        outcome.MutableSetSetupResult()->SetExpectedRevision(pending.GetRevision());
        outcome.MutableSetSetupResult()->SetOutcome(P::SETUP_UNKNOWN);
        pending = f.Tablet(std::move(outcome)).GetDelegation();
        f.Runtime.AdvanceCurrentTime(TDuration::Minutes(9));
        f.RebootTablet();
        const auto before = f.Delegation(pending.GetOperationId());
        UNIT_ASSERT_VALUES_EQUAL(before.GetDelegation().GetStagedAtUs(), originalStagedAt);
        const auto deadline = TInstant::MicroSeconds(originalStagedAt) + TDuration::Minutes(10);
        TActorId coordinator;
        TInstant decisionTime = deadline - TDuration::MicroSeconds(1);
        ui32 observedDecisions = 0;
        f.Intercept = [&](auto& event) {
            if (event->GetTypeRewrite() == TEvIamDelegationTablet::TEvResponse::EventType
                && event->GetRecipientRewrite() == coordinator) {
                const auto& response = event->template Get<TEvIamDelegationTablet::TEvResponse>()->Record;
                if (response.HasDelegation() && response.GetDelegation().GetOperationId() == pending.GetOperationId()
                    && response.GetDelegation().GetState() == P::PENDING) {
                    UNIT_ASSERT(f.Runtime.GetCurrentTime() <= decisionTime);
                    f.Runtime.UpdateCurrentTime(decisionTime);
                    ++observedDecisions;
                }
            }
            return EAction::PROCESS;
        };
        // Position the clock at delivery of the exact state the guard consumes,
        // after all reboot/pipe recovery and asynchronous authorization work.
        const auto early = f.Start(f.Operation(true, "account-3"));
        coordinator = early.Actor;
        const auto denied = f.Finish(early);
        UNIT_ASSERT(denied.Status() == NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::PRECONDITION_FAILED));
        UNIT_ASSERT_VALUES_EQUAL(observedDecisions, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Runtime.GetCurrentTime(), decisionTime);
        const auto unchanged = f.Delegation(pending.GetOperationId());
        UNIT_ASSERT_VALUES_EQUAL(unchanged.GetDelegation().SerializeAsString(), before.GetDelegation().SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(unchanged.GetSecret().SerializeAsString(), before.GetSecret().SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 1);

        decisionTime = deadline;
        const auto admitted = f.Start(f.Operation(true, "account-3"));
        coordinator = admitted.Actor;
        auto setup = f.Take(f.Submissions);
        UNIT_ASSERT_VALUES_EQUAL(observedDecisions, 2);
        UNIT_ASSERT_VALUES_EQUAL(setup->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->Binding.ServiceAccountId, "account-3");
        f.ReplyIam(*setup, EDelegationOperationOutcome::Succeeded);
        UNIT_ASSERT(f.Finish(admitted).Success());
        const auto retained = f.Delegation(pending.GetOperationId()).GetDelegation();
        UNIT_ASSERT_VALUES_EQUAL(retained.GetState(), P::WAITING_SETUP);
        UNIT_ASSERT_VALUES_EQUAL(retained.GetSetupState(), P::SETUP_UNKNOWN);
        UNIT_ASSERT_VALUES_EQUAL(retained.GetStagedAtUs(), originalStagedAt);
        UNIT_ASSERT_VALUES_EQUAL(f.SetupBindings.size(), 2);
        UNIT_ASSERT(f.RevokeBindings.empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-3");
    }

}

} // namespace NKikimr::NKqp
