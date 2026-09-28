#include <ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.h>
#include <ydb/core/kqp/gateway/actors/scheme.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/testlib/service_mocks/folder_service_mock.h>
#include <ydb/library/testlib/service_mocks/service_account_service_mock.h>
#include <ydb/services/scheme_secret/ut/common/helpers.h>

#include <grpcpp/server_builder.h>

#include <util/generic/maybe.h>
#include <util/network/sock.h>
#include <util/generic/vector.h>
#include <util/string/ascii.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

Y_UNIT_TEST_SUITE(KqpIamDelegationSecretOrchestrator) {
    Y_UNIT_TEST(ExtractCloudSubjectId) {
        struct TCase {
            TString UserSid;
            TString Domain;
            TMaybe<TString> Expected;
        };
        const TVector<TCase> cases = {
            // cloud subjects: the SID minus "@<AccessServiceDomain>"
            {"bob@as", "as", TString("bob")},
            {"ajexxxx@as", "as", TString("ajexxxx")},
            {"a@b@as", "as", TString("a@b")},
            // not cloud subjects
            {"bob@builtin", "as", Nothing()},
            {"@as", "as", Nothing()},
            {"bob@as", "", Nothing()},
            {"bob@AS", "as", Nothing()},
            {"", "as", Nothing()},
        };
        for (const auto& [userSid, domain, expected] : cases) {
            const NACLib::TUserToken userToken(userSid, {});
            const auto actual = ExtractCloudSubjectId(userToken, domain);
            UNIT_ASSERT_VALUES_EQUAL_C(actual.Defined(), expected.Defined(), "sid '" << userSid << "', domain '" << domain << "'");
            if (expected) {
                UNIT_ASSERT_VALUES_EQUAL_C(*actual, *expected, "sid '" << userSid << "', domain '" << domain << "'");
            }
        }

        // system users are never cloud subjects even when their domain matches
        const NACLib::TUserToken systemToken("metadata@system", {});
        UNIT_ASSERT(systemToken.IsSystemUser());
        UNIT_ASSERT(!ExtractCloudSubjectId(systemToken, "system").Defined());
    }

    // A runner with the IAM config of the tests, a fake delegation service registered after the KQP proxy has
    // bootstrapped (it registers the real services then), and helpers to run an orchestrator directly.
    // the orchestrator checks the YDB rights of the user before IAM: the test user may do everything
    void GrantAllToBob(TKikimrRunner& kikimr) {
        const auto result = kikimr.GetQueryClient().ExecuteQuery(
            "GRANT ALL ON `/Root` TO `bob@" BUILTIN_ACL_DOMAIN "`;", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    struct TOrchestratorRunner {
        static NKikimrConfig::TAppConfig MakeAppConfig() {
            NKikimrConfig::TAppConfig appConfig;
            auto& iamConfig = *appConfig.MutableIamConfig();
            iamConfig.SetTokenServiceEndpoint("localhost:1"); // never called: the IAM services are fakes
            iamConfig.SetServiceControlEndpoint("localhost:1");
            iamConfig.SetServiceId("ydb");
            iamConfig.SetMicroserviceId("data-plane");
            iamConfig.SetResourceType("resource-manager.cloud");
            appConfig.MutableAuthConfig()->SetAccessServiceDomain(BUILTIN_ACL_DOMAIN); // builtin logins stand for cloud subjects
            return appConfig;
        }

        static TKikimrSettings MakeSettings(const NKikimrConfig::TAppConfig& appConfig) {
            NKikimrConfig::TFeatureFlags featureFlags;
            featureFlags.SetEnableSchemaSecrets(true);
            featureFlags.SetEnableIamDelegationSecrets(true);
            return TKikimrSettings(appConfig).SetWithSampleTables(false).SetFeatureFlags(featureFlags);
        }

        // Whether the IAM delegation service of the node is replaced with the recording fake (needs a served
        // query first, so it is not possible in the single-threaded mode of the runtime).
        enum class EFakeDelegationService {
            Register,
            Keep,
        };

        explicit TOrchestratorRunner(const TKikimrSettings& settings = MakeSettings(MakeAppConfig()), EFakeDelegationService fake = EFakeDelegationService::Register)
            : Kikimr(settings)
            , Runtime(*Kikimr.GetTestServer().GetRuntime())
        {
            Runtime.SetLogPriority(NKikimrServices::IAM_DELEGATION, NLog::PRI_DEBUG);
            if (fake == EFakeDelegationService::Keep) {
                return;
            }
            // the KQP proxy registers the real IAM services when it bootstraps, which has happened once it has
            // served a query: the fake registered afterwards replaces them
            const auto result = Kikimr.GetQueryClient().ExecuteQuery("SELECT 1;", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            GrantAllToBob(Kikimr);
            Calls = NSecret::RegisterFakeIamDelegationService(Runtime);
        }

        // CREATE SECRET of a delegation secret prepared the way the scheme executer prepares it
        TIamDelegationSecretOperation CreateOperation(const TString& database, const TString& name, const TString& serviceAccountId, const TString& cloudId,
            const TIntrusiveConstPtr<NACLib::TUserToken>& userToken)
        {
            auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
            request->Record.SetDatabaseName(database);
            request->Record.SetUserToken(userToken->GetSerializedToken());
            auto& scheme = *request->Record.MutableTransaction()->MutableModifyScheme();
            scheme.SetWorkingDir(database);
            scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
            scheme.SetFailOnExist(true);
            scheme.SetFailedOnAlreadyExists(true);
            auto& op = *scheme.MutableCreateSecret();
            op.SetName(name);
                op.MutableIamDelegation()->SetServiceAccountId(serviceAccountId);
            if (cloudId) {
                op.MutableIamDelegation()->SetCloudId(cloudId);
            }
            return {
                .Request = std::move(request),
                .Database = database,
                .UserToken = userToken,
                .Promise = NThreading::NewPromise<NYql::IKikimrGateway::TGenericResult>(),
                .FailedOnAlreadyExists = true,
                .SuccessOnNotExist = false,
            };
        }

        // Hang guard around the promise of an orchestrator (the test makes its completion inevitable)
        static NYql::IKikimrGateway::TGenericResult WaitResult(NThreading::TPromise<NYql::IKikimrGateway::TGenericResult> promise) {
            auto future = promise.GetFuture();
            UNIT_ASSERT_C(future.Wait(TDuration::Seconds(120)), "the promise of the orchestrator was not completed");
            return future.GetValue();
        }

        NKikimrSchemeOp::TSecretDescription Describe(const TString& path) {
            const auto navigate = Navigate(Runtime, Runtime.AllocateEdgeActor(), path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
            const auto& entry = navigate->ResultSet.at(0);
            UNIT_ASSERT_VALUES_EQUAL_C(entry.Status, NSchemeCache::TSchemeCacheNavigate::EStatus::Ok, path);
            UNIT_ASSERT(entry.SecretInfo);
            return entry.SecretInfo->Description;
        }

        bool Exists(const TString& path) {
            return Navigate(Runtime, Runtime.AllocateEdgeActor(), path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown)->ResultSet.at(0).Status
                == NSchemeCache::TSchemeCacheNavigate::EStatus::Ok;
        }

        TKikimrRunner Kikimr;
        TTestActorRuntime& Runtime;
        TIntrusivePtr<NSecret::TFakeDelegationCalls> Calls;
    };

    // The scheme cache never answers the lookup of the secret: the statement fails after the navigate timeout
    // of the operation, names the path, sends nothing to the delegation service, and completes its promise. The
    // runtime runs single-threaded so that its observer can drop the navigate and count the IAM events.
    Y_UNIT_TEST(NavigateTimeoutFailsTheStatement) {
        TOrchestratorRunner t(TOrchestratorRunner::MakeSettings(TOrchestratorRunner::MakeAppConfig()).SetUseRealThreads(false), TOrchestratorRunner::EFakeDelegationService::Keep);

        // the navigate of the secret path is dropped on its way to the scheme cache; every request to the
        // delegation service is counted
        const TVector<TString> secretPath = SplitPath("/Root/sa-secret");
        ui32 dropped = 0;
        ui32 delegationRequests = 0;
        t.Runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvTxProxySchemeCache::EvNavigateKeySet) {
                const auto* request = ev->Get<TEvTxProxySchemeCache::TEvNavigateKeySet>()->Request.Get();
                if (request && request->ResultSet.size() == 1 && request->ResultSet.front().Path == secretPath) {
                    ++dropped;
                    return TTestActorRuntimeBase::EEventAction::DROP;
                }
            }
            if (ev->GetTypeRewrite() == NIamDelegation::TEvIamDelegation::EvSetupDelegation
                || ev->GetTypeRewrite() == NIamDelegation::TEvIamDelegation::EvRevokeDelegation)
            {
                ++delegationRequests;
            }
            return TTestActorRuntimeBase::EEventAction::PROCESS;
        });

        auto userToken = MakeIntrusiveConst<NACLib::TUserToken>("bob@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        auto op = t.CreateOperation("/Root", "sa-secret", "aje-sa", "b1g-cloud", userToken);
        op.Timeouts.Navigate = TDuration::Seconds(1); // what makes the timeout inevitable
        auto future = op.Promise.GetFuture();
        t.Runtime.Register(CreateIamDelegationSecretCreator(std::move(op)));

        // hang guard: the timeout of the orchestrator makes the completion inevitable
        t.Runtime.WaitFor("the promise of the orchestrator", [&]() { return future.HasValue(); }, TDuration::Seconds(120));
        const auto& result = future.GetValue();
        UNIT_ASSERT(!result.Success());
        UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(result.Status()), static_cast<int>(NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::UNAVAILABLE)));
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "timeout while resolving /Root/sa-secret");
        UNIT_ASSERT(dropped >= 1);
        UNIT_ASSERT_VALUES_EQUAL(delegationRequests, 0u);
        t.Runtime.SetObserverFunc(TTestActorRuntime::DefaultObserverFunc);
        UNIT_ASSERT(!t.Exists("/Root/sa-secret"));
    }

    // The user is a cloud subject but presented no bearer token to forward (an empty original token): the cloud
    // lookup is skipped, the cloud of the database is used with a warning, and no ServiceAccountService /
    // FolderService request is sent although both are configured and reachable.
    Y_UNIT_TEST(NoBearerFallsBackToDatabaseCloud) {
        // the lookup services of IAM, reachable and recording
        TPortManager portManager;
        const ui16 iamPort = portManager.GetPort();
        TServiceAccountServiceMock serviceAccountMock;
        TFolderServiceMock folderMock;
        serviceAccountMock.ServiceAccountData["aje-sa"].set_id("aje-sa");
        serviceAccountMock.ServiceAccountData["aje-sa"].set_folder_id("folder-1");
        folderMock.Folders["folder-1"].set_id("folder-1");
        folderMock.Folders["folder-1"].set_cloud_id("b1g-sa-cloud");
        grpc::ServerBuilder builder;
        builder.AddListeningPort("[::]:" + ToString(iamPort), grpc::InsecureServerCredentials());
        builder.RegisterService(&serviceAccountMock);
        builder.RegisterService(&folderMock);
        auto iamServer = builder.BuildAndStart();

        auto appConfig = TOrchestratorRunner::MakeAppConfig();
        appConfig.MutableIamConfig()->SetServiceControlEndpoint("localhost:" + ToString(iamPort));
        appConfig.MutableIamConfig()->SetResourceManagerEndpoint("localhost:" + ToString(iamPort));
        appConfig.MutableIamConfig()->SetEnableSsl(false);
        TOrchestratorRunner t(TOrchestratorRunner::MakeSettings(appConfig).SetDynamicNodeCount(1).SetStoragePoolTypes({"hdd"}));
        const TString databasePath = t.Kikimr.CreateDatabase("CloudDb", "hdd", {{"cloud_id", "b1g-db-cloud"}});

        // a cloud subject without an original token (the SID alone)
        auto userToken = MakeIntrusiveConst<NACLib::TUserToken>("bob@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        UNIT_ASSERT_VALUES_EQUAL(userToken->GetOriginalUserToken(), "");
        auto op = t.CreateOperation(databasePath, "db-secret", "aje-sa", "", userToken);
        auto promise = op.Promise;
        t.Runtime.Register(CreateIamDelegationSecretCreator(std::move(op)));

        const auto result = TOrchestratorRunner::WaitResult(promise);
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(),
            "RESOURCE b1g-db-cloud was taken from the database, the cloud of service account aje-sa is unknown: the user has no IAM token to look the service account up with");
        const auto calls = t.Calls->Snapshot();
        UNIT_ASSERT_VALUES_EQUAL(calls.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(calls[0].Method, "Setup");
        UNIT_ASSERT_VALUES_EQUAL(calls[0].Spec.CloudId, "b1g-db-cloud");
        UNIT_ASSERT_VALUES_EQUAL(calls[0].SubjectId, "bob");
        UNIT_ASSERT_VALUES_EQUAL(t.Describe(databasePath + "/db-secret").GetIamDelegation().GetCloudId(), "b1g-db-cloud");
        // the lookup services saw no request at all (the mock records the user agent of every call it serves)
        with_lock (serviceAccountMock.MetadataMutex) {
            UNIT_ASSERT_VALUES_EQUAL(serviceAccountMock.CapturedUserAgent, "");
        }

        // with a bearer the lookup runs and wins over the database
        auto tokenWithBearer = MakeIntrusiveConst<NACLib::TUserToken>(NACLib::TUserToken::TUserTokenInitFields{
            .OriginalUserToken = "cloud-user-bearer", .UserSID = "bob@" BUILTIN_ACL_DOMAIN});
        auto op2 = t.CreateOperation(databasePath, "db-secret-2", "aje-sa", "", tokenWithBearer);
        auto promise2 = op2.Promise;
        t.Runtime.Register(CreateIamDelegationSecretCreator(std::move(op2)));
        const auto result2 = TOrchestratorRunner::WaitResult(promise2);
        UNIT_ASSERT_C(result2.Success(), result2.Issues().ToString());
        UNIT_ASSERT_C(result2.Issues().Empty(), result2.Issues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(t.Describe(databasePath + "/db-secret-2").GetIamDelegation().GetCloudId(), "b1g-sa-cloud");
        with_lock (serviceAccountMock.MetadataMutex) {
            UNIT_ASSERT_VALUES_UNEQUAL(serviceAccountMock.CapturedUserAgent, "");
        }
        iamServer->Shutdown();
    }

    // The cloud lookup never answers (the IAM control plane accepts the connection and stays silent): after the
    // delegation timeout of the operation the statement falls back to the cloud of the database with a warning;
    // an explicit RESOURCE never looks anything up; a database without a cloud fails before any IAM call.
    Y_UNIT_TEST(CloudLookupTimeoutFallsBackToDatabaseCloud) {
        // a listener that never answers, standing in for the IAM control plane and Resource Manager
        TPortManager portManager;
        const ui16 silentPort = portManager.GetPort();
        TInetStreamSocket silentListener;
        {
            TSockAddrInet addr("127.0.0.1", silentPort);
            UNIT_ASSERT_VALUES_EQUAL(silentListener.Bind(&addr), 0);
            UNIT_ASSERT_VALUES_EQUAL(silentListener.Listen(16), 0);
        }
        auto appConfig = TOrchestratorRunner::MakeAppConfig();
        appConfig.MutableIamConfig()->SetServiceControlEndpoint("127.0.0.1:" + ToString(silentPort));
        appConfig.MutableIamConfig()->SetResourceManagerEndpoint("127.0.0.1:" + ToString(silentPort));
        appConfig.MutableIamConfig()->SetEnableSsl(false);
        TOrchestratorRunner t(TOrchestratorRunner::MakeSettings(appConfig).SetDynamicNodeCount(1).SetStoragePoolTypes({"hdd"}));
        const TString databasePath = t.Kikimr.CreateDatabase("CloudDb", "hdd", {{"cloud_id", "b1g-db-cloud"}});
        auto userToken = MakeIntrusiveConst<NACLib::TUserToken>(NACLib::TUserToken::TUserTokenInitFields{
            .OriginalUserToken = "cloud-user-bearer", .UserSID = "bob@" BUILTIN_ACL_DOMAIN});
        const auto run = [&](const TString& database, const TString& name, const TString& serviceAccountId, const TString& cloudId) {
            auto op = t.CreateOperation(database, name, serviceAccountId, cloudId, userToken);
            op.Timeouts.CloudLookup = TDuration::Seconds(1); // what makes the timeout of the lookup inevitable (the delegation call keeps its own)
            auto promise = op.Promise;
            t.Runtime.Register(CreateIamDelegationSecretCreator(std::move(op)));
            return TOrchestratorRunner::WaitResult(promise);
        };

        // RESOURCE omitted: the lookup times out, the database cloud is taken
        {
            const auto result = run(databasePath, "db-secret", "aje-sa", "");
            UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
            UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(),
                "RESOURCE b1g-db-cloud was taken from the database, the cloud of service account aje-sa is unknown: timeout while looking the service account up");
            UNIT_ASSERT_VALUES_EQUAL(t.Describe(databasePath + "/db-secret").GetIamDelegation().GetCloudId(), "b1g-db-cloud");
            const auto calls = t.Calls->Snapshot();
            UNIT_ASSERT_VALUES_EQUAL(calls.size(), 1u);
            UNIT_ASSERT_VALUES_EQUAL(calls[0].Method, "Setup");
            UNIT_ASSERT_VALUES_EQUAL(calls[0].Spec.CloudId, "b1g-db-cloud");
        }

        // an explicit RESOURCE is unaffected: no lookup, no warning
        {
            const auto result = run(databasePath, "db-secret-2", "aje-sa", "b1g-other-cloud");
            UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
            UNIT_ASSERT_C(result.Issues().Empty(), result.Issues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(t.Describe(databasePath + "/db-secret-2").GetIamDelegation().GetCloudId(), "b1g-other-cloud");
        }

        // a database without a cloud: the timeout is the reason of the failure, before any IAM call
        {
            const auto result = run("/Root", "nocloud-secret", "aje-sa-2", "");
            UNIT_ASSERT(!result.Success());
            UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(result.Status()), static_cast<int>(NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::BAD_REQUEST)));
            UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(),
                "database /Root has no cloud_id attribute and the cloud of service account aje-sa-2 is unknown: timeout while looking the service account up; specify RESOURCE explicitly");
            UNIT_ASSERT_VALUES_EQUAL(t.Calls->Snapshot().size(), 2u);
            UNIT_ASSERT(!t.Exists("/Root/nocloud-secret"));
        }
    }

    // The orchestrator is poisoned while it waits for IAM (the crash window between the schema write and the
    // answer of SetupDelegation): it must complete its promise, so that the executer waiting on it does not
    // hang. It compensates nothing itself: the secret names the delegation, so whatever IAM did with it is
    // revoked by the schemeshard when the secret is dropped or altered.
    Y_UNIT_TEST(PoisonedAfterSetupCompletesPromise) {
        NKikimrConfig::TAppConfig appConfig;
        auto& iamConfig = *appConfig.MutableIamConfig();
        iamConfig.SetTokenServiceEndpoint("localhost:1"); // never called: the delegation service is a fake
        iamConfig.SetServiceControlEndpoint("localhost:1");
        iamConfig.SetServiceId("ydb");
        iamConfig.SetMicroserviceId("data-plane");
        iamConfig.SetResourceType("resource-manager.cloud");
        appConfig.MutableAuthConfig()->SetAccessServiceDomain(BUILTIN_ACL_DOMAIN); // builtin logins stand for cloud subjects
        NKikimrConfig::TFeatureFlags featureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        TKikimrRunner kikimr(TKikimrSettings(appConfig).SetWithSampleTables(false).SetFeatureFlags(featureFlags));
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        runtime.SetLogPriority(NKikimrServices::IAM_DELEGATION, NLog::PRI_DEBUG);
        {
            // the KQP proxy registers the real IAM services when it bootstraps, which has happened once it has
            // served a query: the fake registered afterwards replaces them
            const auto result = kikimr.GetQueryClient().ExecuteQuery("SELECT 1;", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        GrantAllToBob(kikimr);
        auto calls = NSecret::RegisterFakeIamDelegationService(runtime);
        with_lock (calls->Mutex) {
            calls->HoldSetups = true;
        }

        auto userToken = MakeIntrusiveConst<NACLib::TUserToken>("bob@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetDatabaseName("/Root");
        request->Record.SetUserToken(userToken->GetSerializedToken());
        auto& scheme = *request->Record.MutableTransaction()->MutableModifyScheme();
        scheme.SetWorkingDir("/Root");
        scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
        scheme.SetFailOnExist(true);
        scheme.SetFailedOnAlreadyExists(true);
        auto& op = *scheme.MutableCreateSecret();
        op.SetName("sa-secret");
        op.MutableIamDelegation()->SetServiceAccountId("aje-sa");
        op.MutableIamDelegation()->SetCloudId("b1g-cloud");

        auto promise = NThreading::NewPromise<NYql::IKikimrGateway::TGenericResult>();
        const TActorId orchestrator = runtime.Register(CreateIamDelegationSecretCreator({
            .Request = std::move(request),
            .Database = "/Root",
            .UserToken = userToken,
            .Promise = promise,
            .FailedOnAlreadyExists = true,
            .SuccessOnNotExist = false,
        }));
        calls->WaitCalls("Setup", 1);
        UNIT_ASSERT(!promise.HasValue());

        runtime.Send(new IEventHandle(orchestrator, runtime.AllocateEdgeActor(), new TEvents::TEvPoison()));
        auto future = promise.GetFuture();
        UNIT_ASSERT_C(future.Wait(TDuration::Seconds(120)), "the promise was not completed after the poison (hang guard)");
        const auto& result = future.GetValue();
        UNIT_ASSERT(!result.Success());
        UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(result.Status()), static_cast<int>(NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::CANCELLED)));
        UNIT_ASSERT_STRING_CONTAINS(result.Issues().ToString(), "cancelled");

        // the orchestrator died in the same turn that completed the promise, so the late IAM reply reaches
        // nobody: no drop, no revoke; the secret stays and names the delegation IAM has set up
        NSecret::ReleaseHeldDelegationReplies(runtime);
        {
            // a tracked event to the orchestrator comes back undelivered: it is gone
            const TActorId probe = runtime.AllocateEdgeActor();
            runtime.Send(new IEventHandle(orchestrator, probe, new TEvents::TEvWakeup(), IEventHandle::FlagTrackDelivery));
            const auto undelivered = runtime.GrabEdgeEvent<TEvents::TEvUndelivered>(probe, TDuration::Seconds(120));
            UNIT_ASSERT(undelivered);
        }
        const auto recorded = calls->Snapshot();
        UNIT_ASSERT_VALUES_EQUAL(recorded.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(recorded[0].Method, "Setup");
        UNIT_ASSERT_VALUES_EQUAL(recorded[0].SubjectId, "bob");
        const auto navigate = Navigate(runtime, runtime.AllocateEdgeActor(), "/Root/sa-secret", NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
        const auto& entry = navigate->ResultSet.at(0);
        UNIT_ASSERT_VALUES_EQUAL(entry.Status, NSchemeCache::TSchemeCacheNavigate::EStatus::Ok);
        UNIT_ASSERT(entry.SecretInfo);
        UNIT_ASSERT_VALUES_EQUAL(entry.SecretInfo->Description.GetIamDelegation().GetReferrerId(), recorded[0].Spec.ReferrerId);
        UNIT_ASSERT(!entry.SecretInfo->Description.HasPendingIamDelegation());
    }

    Y_UNIT_TEST(NewReferrerId) {
        const TString prefix = "ydb.delegation.";
        const TString first = NKqp::NewReferrerId();
        const TString second = NKqp::NewReferrerId();
        UNIT_ASSERT_VALUES_UNEQUAL(first, second);
        UNIT_ASSERT_C(first.StartsWith(prefix), first);
        // IAM enforces a limit of 50 characters on referrer.id
        UNIT_ASSERT_VALUES_EQUAL_C(first.size(), prefix.size() + 32, first);
        UNIT_ASSERT_C(first.size() <= 50, first);
        for (const char c : TStringBuf(first).Skip(prefix.size())) {
            UNIT_ASSERT_C(IsAsciiHex(c) && !IsAsciiUpper(c), first);
        }
    }

    // runs a scheme request through the regular handler (no user token: no rights check)
    NYql::IKikimrGateway::TGenericResult RunScheme(TTestActorRuntime& runtime, std::function<void(NKikimrSchemeOp::TModifyScheme&)> fill) {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetDatabaseName("/Root");
        fill(*request->Record.MutableTransaction()->MutableModifyScheme());
        auto promise = NThreading::NewPromise<NYql::IKikimrGateway::TGenericResult>();
        runtime.Register(new TSchemeOpRequestHandler(request.Release(), promise, true));
        auto future = promise.GetFuture();
        runtime.WaitFor("scheme request", [&]() { return future.HasValue(); }, TDuration::Seconds(120)); // hang guard
        return future.GetValue();
    }

    // CREATE SECRET ... IF NOT EXISTS of a delegation secret, the way the gateway prepares it (FailOnExist = false)
    TIamDelegationSecretOperation IfNotExistsOperation(const TString& name, const TIntrusiveConstPtr<NACLib::TUserToken>& userToken) {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetDatabaseName("/Root");
        request->Record.SetUserToken(userToken->GetSerializedToken());
        auto& scheme = *request->Record.MutableTransaction()->MutableModifyScheme();
        scheme.SetWorkingDir("/Root");
        scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
        scheme.SetFailOnExist(false);
        scheme.SetFailedOnAlreadyExists(false);
        auto& op = *scheme.MutableCreateSecret();
        op.SetName(name);
        op.MutableIamDelegation()->SetServiceAccountId("aje-sa");
        op.MutableIamDelegation()->SetCloudId("b1g-cloud");
        return {
            .Request = std::move(request),
            .Database = "/Root",
            .UserToken = userToken,
            .Promise = NThreading::NewPromise<NYql::IKikimrGateway::TGenericResult>(),
            .FailedOnAlreadyExists = false,
            .SuccessOnNotExist = false,
        };
    }

    // CREATE SECRET ... IF NOT EXISTS racing another CREATE of the same name: the navigate of the orchestrator
    // (forged here to answer "absent", which is what it answers when the other CREATE lands between the navigate
    // and the propose) is followed by the statement's own propose, which must not accept the existing secret as
    // this statement's: no delegation is set up for a secret that does not name it, the other statement's secret
    // is not dropped, and the statement succeeds as IF NOT EXISTS says.
    Y_UNIT_TEST(IfNotExistsRaceSetsNothingUp) {
        TOrchestratorRunner t(TOrchestratorRunner::MakeSettings(TOrchestratorRunner::MakeAppConfig()).SetUseRealThreads(false), TOrchestratorRunner::EFakeDelegationService::Keep);
        auto& runtime = t.Runtime;
        {
            // the user needs the rights to create in /Root; a scheme request grants them (no query service in this mode)
            const auto result = RunScheme(runtime, [](auto& scheme) {
                scheme.SetWorkingDir("/");
                scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpModifyACL);
                NACLib::TDiffACL diff;
                diff.AddAccess(NACLib::EAccessType::Allow, NACLib::GenericFull, "bob@" BUILTIN_ACL_DOMAIN);
                scheme.MutableModifyACL()->SetName("Root");
                scheme.MutableModifyACL()->SetDiffACL(diff.SerializeAsString());
            });
            UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        }
        {
            // the secret of the other statement (a VALUE secret, created without any IAM involvement)
            const auto result = RunScheme(runtime, [](auto& scheme) {
                scheme.SetWorkingDir("/Root");
                scheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateSecret);
                scheme.MutableCreateSecret()->SetName("race-secret");
                scheme.MutableCreateSecret()->SetValue("the other statement's value");
            });
            UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        }
        UNIT_ASSERT(t.Exists("/Root/race-secret"));

        const TVector<TString> secretPath = SplitPath("/Root/race-secret");
        TActorId orchestrator;
        ui32 forged = 0;
        ui32 setups = 0;
        ui32 drops = 0;
        runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvTxProxySchemeCache::EvNavigateKeySetResult && orchestrator && ev->Recipient == orchestrator && !forged) {
                auto* request = ev->Get<TEvTxProxySchemeCache::TEvNavigateKeySetResult>()->Request.Get();
                if (request->ResultSet.size() == 1 && request->ResultSet.front().Path == secretPath) {
                    request->ResultSet.front().Status = NSchemeCache::TSchemeCacheNavigate::EStatus::PathErrorUnknown;
                    request->ResultSet.front().SecretInfo.Reset();
                    ++forged;
                }
            }
            if (ev->GetTypeRewrite() == NIamDelegation::TEvIamDelegation::EvSetupDelegation) {
                ++setups;
            }
            if (ev->GetTypeRewrite() == TEvTxUserProxy::EvProposeTransaction
                && ev->Get<TEvTxUserProxy::TEvProposeTransaction>()->Record.GetTransaction().GetModifyScheme().GetOperationType() == NKikimrSchemeOp::ESchemeOpDropSecret)
            {
                ++drops;
            }
            return TTestActorRuntimeBase::EEventAction::PROCESS;
        });

        auto userToken = MakeIntrusiveConst<NACLib::TUserToken>("bob@" BUILTIN_ACL_DOMAIN, TVector<NACLib::TSID>{});
        auto op = IfNotExistsOperation("race-secret", userToken);
        auto future = op.Promise.GetFuture();
        orchestrator = runtime.Register(CreateIamDelegationSecretCreator(std::move(op)));
        runtime.WaitFor("the promise of the orchestrator", [&]() { return future.HasValue(); }, TDuration::Seconds(120)); // hang guard
        const auto& result = future.GetValue();
        runtime.SetObserverFunc(TTestActorRuntime::DefaultObserverFunc);

        UNIT_ASSERT_VALUES_EQUAL(forged, 1u);
        UNIT_ASSERT_C(result.Success(), result.Issues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(setups, 0u);
        UNIT_ASSERT_VALUES_EQUAL(drops, 0u);
        UNIT_ASSERT(t.Exists("/Root/race-secret"));
        UNIT_ASSERT(!t.Describe("/Root/race-secret").HasIamDelegation()); // still the other statement's stored-value secret
    }
}

} // namespace NKikimr::NKqp
