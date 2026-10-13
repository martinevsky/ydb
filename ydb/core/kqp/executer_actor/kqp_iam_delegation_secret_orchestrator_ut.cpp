#include <ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.h>
#include <ydb/core/kqp/gateway/actors/scheme.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation_lifecycle/worker.h>
#include <ydb/core/tx/iam_delegation/iam_delegation.h>
#include <ydb/core/tx/iam_delegation/public/events.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/services/scheme_secret/iam_delegation_reader.h>

#include <util/string/ascii.h>

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

    static TKikimrSettings Settings() {
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
        return TKikimrSettings(config).SetWithSampleTables(false)
            .SetFeatureFlags(flags).SetUseRealThreads(false);
    }

    TKikimrRunner Kikimr{Settings()};
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

    TOrchestratorFixture() {
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

    NKikimrScheme::TEvDescribeSchemeResult Describe(const TString& name = "secret") {
        const auto edge = Runtime.AllocateEdgeActor();
        auto request = MakeHolder<TEvTxUserProxy::TEvNavigate>();
        request->Record.SetDatabaseName("/Root");
        request->Record.MutableDescribePath()->SetPath("/Root/" + name);
        request->Record.MutableDescribePath()->MutableOptions()->SetReturnSecretValue(true);
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
        request->Record.SetUserToken(Bob->GetSerializedToken());
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

    TRunning Start(TIamDelegationSecretOperation op, bool alter = false) {
        auto future = op.Promise.GetFuture();
        const auto actor = Runtime.Register(alter ? CreateIamDelegationSecretAlterer(std::move(op))
            : CreateIamDelegationSecretCreator(std::move(op)));
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

    THolder<NSecret::TEvIamDelegationSecretReadResult> Read(const TString& name = "secret") {
        const auto schema = Describe(name);
        UNIT_ASSERT_VALUES_EQUAL(schema.GetStatus(), NKikimrScheme::StatusSuccess);
        const auto& path = schema.GetPathDescription();
        const auto& ref = path.GetSecretDescription().GetIamDelegation();
        const auto edge = Runtime.AllocateEdgeActor();
        Runtime.Register(NSecret::CreateIamDelegationSecretReaderActor(edge, 991, "/Root/" + name,
            "/Root", Bob, {ref.GetTabletId(), ref.GetDatabaseIncarnation(), ref.GetCreateOperationId(),
                path.GetSelf().GetSchemeshardId(), path.GetSelf().GetPathId()}));
        auto result = Runtime.GrabEdgeEvent<NSecret::TEvIamDelegationSecretReadResult>(edge);
        UNIT_ASSERT_VALUES_EQUAL(result->Cookie, 991);
        return result->Release();
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

} // namespace

Y_UNIT_TEST_SUITE(KqpIamDelegationSecretOrchestrator) {
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

        const auto altering = f.Start(f.Operation(true, "account-2"), true);
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
        const auto replacing = f.Start(f.Operation(true, "refused-account"), true);
        auto submit = f.Take(f.Submissions);
        const TString failed = submit->Get<TEvIamDelegation::TEvSubmitDelegationOperation>()->RequestId;
        f.ReplyIam(*submit, EDelegationOperationOutcome::Failed);
        UNIT_ASSERT(!f.Finish(replacing).Success());
        const auto state = f.Delegation(failed);
        UNIT_ASSERT_VALUES_EQUAL(state.GetDelegation().GetState(), P::CANCELLED);
        UNIT_ASSERT_VALUES_EQUAL(state.GetSecret().GetCurrentOperationId(), original);
        UNIT_ASSERT_VALUES_EQUAL(state.GetSecret().GetPendingOperationId(), "");
        UNIT_ASSERT_VALUES_EQUAL(f.Read()->Token, "token-for-account-1");
        const auto retry = f.Start(f.Operation(true, "account-2"), true);
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
        const auto altering = f.Start(f.Operation(true, "account-2"), true);
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

    Y_UNIT_TEST(NoopsNeedNoIamConfigurationOrLifecycleMutation) {
        TOrchestratorFixture f;
        const auto id = f.Create();
        const auto previous = f.Delegation(id).GetSecret();
        f.Runtime.GetAppData(0).IamConfig.Clear();
        auto op = f.Operation(false, "ignored-account");
        op.Request->Record.MutableTransaction()->MutableModifyScheme()->SetFailOnExist(false);
        op.FailedOnAlreadyExists = false;
        UNIT_ASSERT(f.Finish(f.Start(std::move(op))).Success());
        UNIT_ASSERT(f.Finish(f.Start(f.Operation(true), true)).Success());
        op = f.Operation(true, "ignored-account", "cloud-1", "absent");
        op.SuccessOnNotExist = true;
        UNIT_ASSERT(f.Finish(f.Start(std::move(op), true)).Success());
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

        const auto altering = f.Start(f.Operation(true, "account-2"), true);
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
        denied.Request->Record.SetUserToken(denied.UserToken->GetSerializedToken());
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
}

} // namespace NKikimr::NKqp
