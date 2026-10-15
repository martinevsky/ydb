#include <ydb/core/kqp/common/kqp_external_effect_secrets.h>
#include <ydb/core/kqp/finalize_script_service/kqp_script_secrets.h>
#include <ydb/core/kqp/finalize_script_service/kqp_finalize_script_actor.h>
#include <ydb/core/protos/config.pb.h>
#include <util/generic/scope.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/iam_delegated_token_service.h>
#include <ydb/core/security/token_manager/token_manager.h>
#include <ydb/library/ycloud/api/iam_token_service.h>
#include <util/random/random.h>
#include <ydb/core/base/path.h>
#include <ydb/core/security/iam_delegation/tablet/ut/requests.h>
#include <ydb/services/scheme_secret/service.h>

namespace NKikimr::NKqp {
namespace {
using namespace NIamDelegation::NTests;
using TReference = TEvDescribeSecretsResponse::TSecretReference;

struct TFixture {
    TTestContext Context;
    TReference SchemaReference{TTestContext::TabletId, "database-incarnation-1", "create-1", 100, 42};
    THashMap<TString, TReference> AdditionalSchemaReferences;
    bool HoldSchema = false;
    TEvTxProxySchemeCache::TEvNavigateKeySet::TPtr HeldSchema;
    ui32 MintCalls = 0;

    class TServices final : public TActorBootstrapped<TServices> {
    public:
        explicit TServices(TFixture& fixture) : Fixture(fixture) {}
        void Bootstrap() { Become(&TServices::StateWork); }
        STRICT_STFUNC(StateWork,
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySet, HandleSchema);
            hFunc(NIamDelegation::TEvIamDelegation::TEvGetToken, HandleToken);
        )
        void HandleSchema(TEvTxProxySchemeCache::TEvNavigateKeySet::TPtr& ev) {
            if (Fixture.HoldSchema) {
                Fixture.HeldSchema = std::move(ev);
                return;
            }
            auto request = std::move(ev->Get()->Request);
            for (auto& entry : request->ResultSet) {
                UNIT_ASSERT(entry.SyncVersion);
                UNIT_ASSERT_VALUES_EQUAL(entry.Access, ui32(NACLib::SelectRow));
                entry.Status = NSchemeCache::TSchemeCacheNavigate::EStatus::Ok;
                entry.Kind = NSchemeCache::TSchemeCacheNavigate::KindSecret;
                const auto found = Fixture.AdditionalSchemaReferences.find(CanonizePath(entry.Path));
                const auto& reference = found == Fixture.AdditionalSchemaReferences.end()
                    ? Fixture.SchemaReference : found->second;
                auto self = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TDirEntryInfo>();
                self->Info.SetSchemeshardId(reference.PathOwnerId);
                self->Info.SetPathId(reference.PathLocalId);
                entry.Self = self;
                auto secret = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TSecretInfo>();
                auto* locator = secret->Description.MutableIamDelegation();
                locator->SetTabletId(reference.TabletId);
                locator->SetDatabaseIncarnation(reference.DatabaseIncarnation);
                locator->SetCreateOperationId(reference.CreateOperationId);
                entry.SecretInfo = secret;
            }
            Send(ev->Sender, new TEvTxProxySchemeCache::TEvNavigateKeySetResult(request.Release()), 0, ev->Cookie);
        }
        void HandleToken(NIamDelegation::TEvIamDelegation::TEvGetToken::TPtr& ev) {
            ++Fixture.MintCalls;
            auto result = MakeHolder<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>();
            result->Key = ev->Get()->Key;
            result->Token = "delegated-token-for-" + result->Key.ServiceAccountId;
            result->UsableUntil = Now() + TDuration::Hours(1);
            result->ExpiresAt = result->UsableUntil + TDuration::Minutes(5);
            Send(ev->Sender, result.Release(), 0, ev->Cookie);
        }
    private:
        TFixture& Fixture;
    };

    TFixture() {
        CreateActive(Context);
        auto& runtime = Context.GetRuntime();
        runtime.GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        runtime.GetAppData(0).FeatureFlags.SetEnableIamDelegationSecrets(true);
        const auto services = runtime.Register(new TServices(*this));
        runtime.RegisterService(MakeSchemeCacheID(), services);
        runtime.RegisterService(NIamDelegation::MakeIamDelegatedTokenServiceId(), services);
        runtime.RegisterService(NSecret::MakeDescribeSchemaSecretServiceId(runtime.GetNodeId()),
            runtime.Register(new NSecret::TDescribeSchemaSecretsService()));
    }

    std::vector<NKqpProto::TKqpExternalSink> PersistSinks(bool pin = true) {
        std::vector<NKqpProto::TKqpExternalSink> sinks(1);
        sinks[0].SetSinkName("s3-sink");
        sinks[0].SetAuthInfo("{\"token_ref\":\"/Root/database/secret\"}");
        if (pin) {
            PinScriptExternalEffectSecrets(sinks, "/Root/database", {{"/Root/database/secret", SchemaReference}});
        }
        // script_sinks persists the complete binary sink proto. Reload it before
        // finalization so only the durable identity can protect the old effect.
        const auto persisted = sinks[0].SerializeAsString();
        UNIT_ASSERT(!TString(persisted).Contains("delegated-token"));
        sinks[0].Clear();
        UNIT_ASSERT(sinks[0].ParseFromString(persisted));
        return sinks;
    }

    TActorId Start(std::vector<NKqpProto::TKqpExternalSink> sinks,
        std::vector<TString> names = {"/Root/database/secret"}) {
        auto& runtime = Context.GetRuntime();
        const auto edge = runtime.AllocateEdgeActor();
        runtime.Register(CreateScriptSecretsResolverActor(edge,
            MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"}),
            "/Root/database", std::move(names), std::move(sinks)));
        return edge;
    }

    THolder<TEvDescribeSecretsResponse> Finish(const TActorId& edge) {
        return THolder<TEvDescribeSecretsResponse>(
            Context.GetRuntime().GrabEdgeEvent<TEvDescribeSecretsResponse>(edge)->Release());
    }

    void Recreate() {
        Context.Call(DropRequest(GetSecret(Context)));
        auto binding = Binding("replacement-referrer");
        binding.SetServiceAccountId("replacement-account");
        const auto staged = Context.Call(StageCreateRequest("create-2", binding,
            "/Root/database/secret", "database-incarnation-1", 20));
        const auto bound = Context.Call(BindRequest(staged.GetDelegation(), SecretIdentity(43)));
        const auto setup = CompleteSetup(Context, bound.GetDelegation());
        Context.Call(PromoteRequest(setup.GetDelegation(), GetSecret(Context, SecretIdentity(43)).GetRevision()));
        SchemaReference.CreateOperationId = "create-2";
        SchemaReference.PathLocalId = 43;
    }
};
} // namespace

Y_UNIT_TEST_SUITE(ScriptExternalEffectSecrets) {

    Y_UNIT_TEST(TwoPersistedPinsKeepIndependentIdentityAfterOnePathIsRecreated) {
        TFixture fixture;
        const TString firstPath = "/Root/database/secret";
        const TString secondPath = "/Root/database/second";
        auto binding = Binding("second-referrer");
        binding.SetServiceAccountId("second-account");
        const auto staged = fixture.Context.Call(StageCreateRequest("second-create", binding,
            secondPath, "database-incarnation-1", 30)).GetDelegation();
        const auto bound = fixture.Context.Call(BindRequest(staged, SecretIdentity(44))).GetDelegation();
        const auto ready = CompleteSetup(fixture.Context, bound).GetDelegation();
        fixture.Context.Call(PromoteRequest(ready, GetSecret(fixture.Context, SecretIdentity(44)).GetRevision()));
        const TReference first = fixture.SchemaReference;
        const TReference second{TTestContext::TabletId, "database-incarnation-1", "second-create", 100, 44};
        fixture.AdditionalSchemaReferences[secondPath] = second;

        std::vector<NKqpProto::TKqpExternalSink> sinks(2);
        sinks[0].SetSinkName("first-sink");
        sinks[0].SetAuthInfo("{\"token_ref\":\"/Root/database/secret\"}");
        sinks[1].SetSinkName("second-sink");
        sinks[1].SetAuthInfo("{\"token_ref\":\"/Root/database/second\"}");
        PinScriptExternalEffectSecrets(sinks, "/Root/database", {{firstPath, first}, {secondPath, second}});
        for (auto& sink : sinks) {
            const auto bytes = sink.SerializeAsString();
            UNIT_ASSERT(!TString(bytes).Contains("delegated-token"));
            sink.Clear();
            UNIT_ASSERT(sink.ParseFromString(bytes));
        }
        const auto original = fixture.Finish(fixture.Start(sinks, {firstPath, secondPath}));
        UNIT_ASSERT_VALUES_EQUAL_C(original->Description.Status, Ydb::StatusIds::SUCCESS,
            original->Description.Issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(original->Description.SecretValues.size(), 2);
        UNIT_ASSERT(original->Description.References.at(0) == first);
        UNIT_ASSERT(original->Description.References.at(1) == second);

        fixture.Recreate();
        const auto rejected = fixture.Finish(fixture.Start(sinks, {firstPath, secondPath}));
        UNIT_ASSERT_VALUES_EQUAL(rejected->Description.Status, Ydb::StatusIds::NOT_FOUND);
        UNIT_ASSERT(rejected->Description.SecretValues.empty());
        const auto unchanged = fixture.Finish(fixture.Start({sinks[1]}, {secondPath}));
        UNIT_ASSERT_VALUES_EQUAL(unchanged->Description.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(unchanged->Description.SecretValues.at(0), "delegated-token-for-second-account");
        UNIT_ASSERT(unchanged->Description.References.at(0) == second);
    }

    Y_UNIT_TEST(HeldFinalizationCannotUseRecreatedSecretAfterPersistedSinkReload) {
        TFixture fixture;
        auto sinks = fixture.PersistSinks();
        fixture.HoldSchema = true;
        const auto edge = fixture.Start(std::move(sinks));
        TDispatchOptions options;
        options.CustomFinalCondition = [&] { return bool(fixture.HeldSchema); };
        fixture.Context.GetRuntime().DispatchEvents(options);
        UNIT_ASSERT(fixture.HeldSchema);

        fixture.Recreate();
        fixture.HoldSchema = false;
        fixture.Context.GetRuntime().Send(fixture.HeldSchema.Release(), 0, true);

        const auto response = fixture.Finish(edge);
        UNIT_ASSERT_VALUES_EQUAL(response->Description.Status, Ydb::StatusIds::NOT_FOUND);
        UNIT_ASSERT(response->Description.SecretValues.empty());
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 0);
    }

    Y_UNIT_TEST(PersistedIdentityAllowsAlterToRefreshTheCurrentBinding) {
        TFixture fixture;
        const auto original = fixture.SchemaReference;
        auto sinks = fixture.PersistSinks();
        auto binding = Binding("alter-referrer");
        binding.SetServiceAccountId("altered-account");
        const auto staged = fixture.Context.Call(StageAlterRequest(GetSecret(fixture.Context), "alter-1", binding));
        const auto setup = CompleteSetup(fixture.Context, staged.GetDelegation());
        fixture.Context.Call(PromoteRequest(setup.GetDelegation(), GetSecret(fixture.Context).GetRevision()));

        const auto response = fixture.Finish(fixture.Start(std::move(sinks)));
        UNIT_ASSERT_VALUES_EQUAL(response->Description.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response->Description.SecretValues.at(0), "delegated-token-for-altered-account");
        UNIT_ASSERT(response->Description.References.at(0) == original);
    }

    Y_UNIT_TEST(LegacyDelegatedEffectsWithoutIdentityFailClosed) {
        TFixture fixture;
        const auto response = fixture.Finish(fixture.Start(fixture.PersistSinks(false)));
        UNIT_ASSERT_VALUES_EQUAL(response->Description.Status, Ydb::StatusIds::PRECONDITION_FAILED);
        UNIT_ASSERT(response->Description.SecretValues.empty());
    }

    Y_UNIT_TEST(LegacyPlainSecretEffectsRemainSupported) {
        TFixture fixture;
        class TPlainSecrets final : public TActorBootstrapped<TPlainSecrets> {
        public:
            void Bootstrap() { Become(&TPlainSecrets::StateWork); }
            STRICT_STFUNC(StateWork, hFunc(NSecret::TDescribeSchemaSecretsService::TEvResolveSecret, Handle);)
            void Handle(NSecret::TDescribeSchemaSecretsService::TEvResolveSecret::TPtr& ev) {
                ev->Get()->Promise.SetValue(TEvDescribeSecretsResponse::TDescription({"plain-value"}));
            }
        };
        auto& runtime = fixture.Context.GetRuntime();
        runtime.RegisterService(NSecret::MakeDescribeSchemaSecretServiceId(runtime.GetNodeId()),
            runtime.Register(new TPlainSecrets()));
        const auto response = fixture.Finish(fixture.Start(fixture.PersistSinks(false)));
        UNIT_ASSERT_VALUES_EQUAL(response->Description.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response->Description.SecretValues.at(0), "plain-value");
    }

    Y_UNIT_TEST(UnusedDelegatedSourceDoesNotRequireAnEffectPin) {
        TFixture fixture;
        auto sinks = fixture.PersistSinks(false);
        sinks[0].SetAuthInfo("{}");
        const auto response = fixture.Finish(fixture.Start(std::move(sinks)));
        UNIT_ASSERT_VALUES_EQUAL(response->Description.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 0);
    }

    Y_UNIT_TEST(NoAuthAndNoSecretEffectsDoNotRequireStructuredJson) {
        TFixture fixture;
        std::vector<NKqpProto::TKqpExternalSink> sinks(1);
        sinks[0].SetSinkName("anonymous-s3-sink");
        const auto response = fixture.Finish(fixture.Start(sinks, {}));
        UNIT_ASSERT_VALUES_EQUAL(response->Description.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT(response->Description.SecretValues.empty());
        const auto auth = ResolveScriptExternalEffectAuth(sinks[0].GetAuthInfo(), {});
        UNIT_ASSERT(auth.Token.empty());
        UNIT_ASSERT(auth.SecretPath.empty());
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 0);

        sinks[0].SetAuthInfo("opaque-test-credential");
        const auto opaque = fixture.Finish(fixture.Start(sinks, {}));
        UNIT_ASSERT_VALUES_EQUAL(opaque->Description.Status, Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(ResolveScriptExternalEffectAuth(sinks[0].GetAuthInfo(), {}).Token,
            "opaque-test-credential");
    }
    Y_UNIT_TEST(ConfiguredFinalizerBudgetReachesSecretAcquisition) {
        TFixture fixture;
        auto& runtime = fixture.Context.GetRuntime();
        auto sinks = fixture.PersistSinks();
        const auto edge = runtime.AllocateEdgeActor();
        runtime.Send(new IEventHandle(edge, edge, new TEvScriptFinalizeRequest(
            EFinalizationStatus::FS_COMMIT, "deadline-execution", "/Root/database",
            Ydb::StatusIds::SUCCESS, Ydb::Query::EXEC_STATUS_COMPLETED, {},
            std::nullopt, std::nullopt, std::nullopt, 1, false)), 0, true);
        auto request = runtime.GrabEdgeEvent<TEvScriptFinalizeRequest>(edge);

        TActorId parent;
        TActorId metadataSaver;
        auto previousRegistration = runtime.SetRegistrationObserverFunc(
            [&](TTestActorRuntimeBase& rt, const TActorId& owner, const TActorId& actor) {
                TTestActorRuntimeBase::DefaultRegistrationObserver(rt, owner, actor);
                if (parent && owner == parent && !metadataSaver) {
                    metadataSaver = actor;
                }
            });
        Y_DEFER { runtime.SetRegistrationObserverFunc(previousRegistration); };
        auto metadataBoundary = runtime.AddObserver([&](IEventHandle::TPtr& event) {
            // The first child is the metadata saver. It remains suspended while its
            // completed metadata response below supplies the persisted sink records.
            if (metadataSaver && event->GetRecipientRewrite() == metadataSaver) {
                event.Reset();
            }
        });
        auto previousTimers = runtime.SetScheduledEventFilter(
            [](auto&, auto&, auto, auto&) { return false; });
        Y_DEFER { runtime.SetScheduledEventFilter(previousTimers); };

        NKikimrConfig::TQueryServiceConfig config;
        config.MutableFinalizeScriptServiceConfig()->SetScriptFinalizationTimeoutSeconds(300);
        parent = runtime.Register(CreateScriptFinalizerActor(std::move(request), config, std::nullopt, {}));
        TDispatchOptions started;
        started.CustomFinalCondition = [&] { return bool(metadataSaver); };
        runtime.DispatchEvents(started);
        UNIT_ASSERT(metadataSaver);

        THolder<TEvSaveScriptFinalStatusResponse> saved = MakeHolder<TEvSaveScriptFinalStatusResponse>();
        saved->Status = Ydb::StatusIds::SUCCESS;
        saved->ApplicateScriptExternalEffectRequired = true;
        saved->CustomerSuppliedId = "deadline-test";
        saved->UserToken = MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"});
        saved->Sinks = std::move(sinks);
        saved->SecretNames = {"/Root/database/secret"};

        NIamDelegation::TEvIamDelegation::TEvGetTokenResult::TPtr heldToken;
        auto holdToken = runtime.AddObserver<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>(
            [&](auto& event) { heldToken = std::move(event); });
        std::vector<TEvDescribeSecretsResponse::TDescription> resolutions;
        auto resolved = runtime.AddObserver<TEvDescribeSecretsResponse>([&](auto& event) {
            if (event->GetRecipientRewrite() == parent) {
                resolutions.push_back(event->Get()->Description);
                // Stop at the real parent's preparation boundary: S3 commit and
                // final metadata persistence are outside this deadline regression.
                event.Reset();
            }
        });
        runtime.Send(new IEventHandle(parent, metadataSaver, saved.Release()), 0, true);
        TDispatchOptions mintStarted;
        mintStarted.CustomFinalCondition = [&] { return bool(heldToken); };
        runtime.DispatchEvents(mintStarted);
        UNIT_ASSERT(heldToken);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 1);

        runtime.SimulateSleep(TDuration::Seconds(61));
        UNIT_ASSERT_C(resolutions.empty(),
            "A caller allowing 300 seconds must not fail secret preparation at 60 seconds");
        holdToken.Remove();
        runtime.Send(heldToken.Release(), 0, true);
        TDispatchOptions completed;
        completed.CustomFinalCondition = [&] { return !resolutions.empty(); };
        runtime.DispatchEvents(completed);
        UNIT_ASSERT_VALUES_EQUAL(resolutions.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL_C(resolutions.front().Status, Ydb::StatusIds::SUCCESS,
            resolutions.front().Issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(resolutions.front().SecretValues.size(), 1);
        UNIT_ASSERT(resolutions.front().References.at(0) == fixture.SchemaReference);
    }

    Y_UNIT_TEST(ConfiguredFinalizerWaitsThroughRealProducerBackoff) {
        TFixture fixture;
        auto& runtime = fixture.Context.GetRuntime();
        SetRandomSeed(42);
        Y_DEFER { ResetRandomState(); };
        using TCloudToken = NCloud::TEvIamTokenService;
        TActorId producer;
        TDuration longBackoff;
        ui32 subscriptions = 0;
        ui32 mintCalls = 0;
        ui32 getCalls = 0;
        bool recover = false;
        const auto until = [&](auto condition) {
            if (!condition()) {
                TDispatchOptions options;
                options.CustomFinalCondition = condition;
                options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
                runtime.DispatchEvents(options, TDuration::Minutes(10));
            }
            UNIT_ASSERT(condition());
        };
        auto sinks = fixture.PersistSinks();
        const auto edge = runtime.AllocateEdgeActor();
        runtime.Send(new IEventHandle(edge, edge, new TEvScriptFinalizeRequest(
            EFinalizationStatus::FS_COMMIT, "deadline-execution", "/Root/database",
            Ydb::StatusIds::SUCCESS, Ydb::Query::EXEC_STATUS_COMPLETED, {},
            std::nullopt, std::nullopt, std::nullopt, 1, false)), 0, true);
        auto request = runtime.GrabEdgeEvent<TEvScriptFinalizeRequest>(edge);

        TActorId parent;
        TActorId metadataSaver;
        auto previousRegistration = runtime.SetRegistrationObserverFunc(
            [&](TTestActorRuntimeBase& rt, const TActorId& owner, const TActorId& actor) {
                TTestActorRuntimeBase::DefaultRegistrationObserver(rt, owner, actor);
                if (parent && owner == parent && !metadataSaver) {
                    metadataSaver = actor;
                }
            });
        Y_DEFER { runtime.SetRegistrationObserverFunc(previousRegistration); };
        auto metadataBoundary = runtime.AddObserver([&](IEventHandle::TPtr& event) {
            // The first child is the metadata saver. It remains suspended while its
            // completed metadata response below supplies the persisted sink records.
            if (metadataSaver && event->GetRecipientRewrite() == metadataSaver) {
                event.Reset();
            }
        });
        auto previousTimers = runtime.SetScheduledEventFilter(
            [&](auto&, auto& event, TDuration delay, auto&) {
                if (event->Recipient == producer && delay > TDuration::Seconds(65)
                    && delay <= TDuration::Seconds(75)) {
                    longBackoff = delay;
                }
                return false;
            });
        Y_DEFER { runtime.SetScheduledEventFilter(previousTimers); };
        auto subscribe = runtime.AddObserver<TEvTokenManager::TEvSubscribeUpdateToken>([&](auto& event) {
            ++subscriptions;
            event.Reset();
        });
        auto mints = runtime.AddObserver<TCloudToken::TEvCreateForServiceRequest>([&](auto& event) {
            ++mintCalls;
            UNIT_ASSERT_C(mintCalls < 200, "Real producer never reached its capped backoff");
            UNIT_ASSERT_VALUES_EQUAL(event->Get()->Request.target_service_account_id(), "service-account-1");
            UNIT_ASSERT_VALUES_EQUAL(event->Get()->Token, "platform-token");
            auto reply = MakeHolder<TCloudToken::TEvCreateForServiceResponse>();
            reply->Status.GRpcStatusCode = recover ? grpc::StatusCode::OK : grpc::StatusCode::UNAVAILABLE;
            if (recover) {
                reply->Response.set_iam_token("recovered-finalizer-token");
                reply->Response.mutable_expires_at()->set_seconds(
                    (runtime.GetCurrentTime() + TDuration::Hours(1)).Seconds());
            }
            runtime.Send(new IEventHandle(event->Sender, event->Recipient, reply.Release(), 0, event->Cookie));
            event.Reset();
        });
        auto gets = runtime.AddObserver<NIamDelegation::TEvIamDelegation::TEvGetToken>([&](auto&) { ++getCalls; });
        NIamDelegation::TIamDelegationSettings settings;
        settings.Config.SetTokenServiceEndpoint("localhost:1");
        settings.Config.SetSystemTokenName("system-account");
        settings.Config.SetEnableSsl(false);
        const auto manager = runtime.AllocateEdgeActor();
        runtime.RegisterService(MakeTokenManagerID(), manager);
        producer = runtime.Register(NIamDelegation::CreateIamDelegatedTokenService(settings));
        runtime.RegisterService(NIamDelegation::MakeIamDelegatedTokenServiceId(), producer);
        until([&] { return subscriptions == 1; });
        runtime.Send(new IEventHandle(producer, manager, new TEvTokenManager::TEvUpdateToken(
            "system-account", "platform-token", {TEvTokenManager::TStatus::ECode::SUCCESS, {}})));
        runtime.Send(new IEventHandle(producer, runtime.AllocateEdgeActor(),
            new NIamDelegation::TEvIamDelegation::TEvGetToken(
                {"service-account-1", "cloud-1", "ydb", "data-plane", "resource-manager.cloud"})));
        until([&] { return longBackoff != TDuration::Zero(); });
        recover = true;
        const auto mintsBefore = mintCalls;
        const auto getsBefore = getCalls;

        NKikimrConfig::TQueryServiceConfig config;
        config.MutableFinalizeScriptServiceConfig()->SetScriptFinalizationTimeoutSeconds(300);
        parent = runtime.Register(CreateScriptFinalizerActor(std::move(request), config, std::nullopt, {}));
        TDispatchOptions started;
        started.CustomFinalCondition = [&] { return bool(metadataSaver); };
        runtime.DispatchEvents(started);
        UNIT_ASSERT(metadataSaver);

        THolder<TEvSaveScriptFinalStatusResponse> saved = MakeHolder<TEvSaveScriptFinalStatusResponse>();
        saved->Status = Ydb::StatusIds::SUCCESS;
        saved->ApplicateScriptExternalEffectRequired = true;
        saved->CustomerSuppliedId = "deadline-test";
        saved->UserToken = MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"});
        saved->Sinks = std::move(sinks);
        saved->SecretNames = {"/Root/database/secret"};

        std::vector<TEvDescribeSecretsResponse::TDescription> resolutions;
        auto resolved = runtime.AddObserver<TEvDescribeSecretsResponse>([&](auto& event) {
            if (event->GetRecipientRewrite() == parent) {
                resolutions.push_back(event->Get()->Description);
                // Stop at the real parent's preparation boundary: S3 commit and
                // final metadata persistence are outside this deadline regression.
                event.Reset();
            }
        });
        runtime.Send(new IEventHandle(parent, metadataSaver, saved.Release()), 0, true);
        until([&] { return getCalls == getsBefore + 1; });

        runtime.SimulateSleep(TDuration::Seconds(61));
        UNIT_ASSERT_C(resolutions.empty(),
            "A caller allowing 300 seconds must not fail secret preparation at 60 seconds");
        UNIT_ASSERT_VALUES_EQUAL(mintCalls, mintsBefore);
        UNIT_ASSERT_VALUES_EQUAL(getCalls, getsBefore + 1);
        TDispatchOptions completed;
        completed.CustomFinalCondition = [&] { return !resolutions.empty(); };
        runtime.DispatchEvents(completed);
        UNIT_ASSERT_VALUES_EQUAL(resolutions.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL_C(resolutions.front().Status, Ydb::StatusIds::SUCCESS,
            resolutions.front().Issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(resolutions.front().SecretValues.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(resolutions.front().SecretValues.front(), "recovered-finalizer-token");
        UNIT_ASSERT(resolutions.front().References.at(0) == fixture.SchemaReference);
        UNIT_ASSERT_VALUES_EQUAL(mintCalls, mintsBefore + 1);
        UNIT_ASSERT_VALUES_EQUAL(getCalls, getsBefore + 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 0);
    }

    Y_UNIT_TEST(FinalizerTimeoutCancelsPendingSecretAcquisition) {
        TFixture fixture;
        auto& runtime = fixture.Context.GetRuntime();
        auto sinks = fixture.PersistSinks();
        const auto edge = runtime.AllocateEdgeActor();
        runtime.Send(new IEventHandle(edge, edge, new TEvScriptFinalizeRequest(
            EFinalizationStatus::FS_COMMIT, "deadline-execution", "/Root/database",
            Ydb::StatusIds::SUCCESS, Ydb::Query::EXEC_STATUS_COMPLETED, {},
            std::nullopt, std::nullopt, std::nullopt, 1, false)), 0, true);
        auto request = runtime.GrabEdgeEvent<TEvScriptFinalizeRequest>(edge);

        TActorId parent;
        TActorId metadataSaver;
        TActorId metadataFinisher;
        bool awaitingFinalization = false;
        auto previousRegistration = runtime.SetRegistrationObserverFunc(
            [&](TTestActorRuntimeBase& rt, const TActorId& owner, const TActorId& actor) {
                TTestActorRuntimeBase::DefaultRegistrationObserver(rt, owner, actor);
                if (parent && owner == parent) {
                    if (!metadataSaver) {
                        metadataSaver = actor;
                    } else if (awaitingFinalization) {
                        metadataFinisher = actor;
                    }
                }
            });
        Y_DEFER { runtime.SetRegistrationObserverFunc(previousRegistration); };
        auto metadataBoundary = runtime.AddObserver([&](IEventHandle::TPtr& event) {
            // The first child is the metadata saver. It remains suspended while its
            // completed metadata response below supplies the persisted sink records.
            if ((metadataSaver && event->GetRecipientRewrite() == metadataSaver)
                || (metadataFinisher && event->GetRecipientRewrite() == metadataFinisher)) {
                event.Reset();
            }
        });
        auto previousTimers = runtime.SetScheduledEventFilter(
            [](auto&, auto&, auto, auto&) { return false; });
        Y_DEFER { runtime.SetScheduledEventFilter(previousTimers); };

        NKikimrConfig::TQueryServiceConfig config;
        config.MutableFinalizeScriptServiceConfig()->SetScriptFinalizationTimeoutSeconds(10);
        parent = runtime.Register(CreateScriptFinalizerActor(std::move(request), config, std::nullopt, {}));
        TDispatchOptions started;
        started.CustomFinalCondition = [&] { return bool(metadataSaver); };
        runtime.DispatchEvents(started);
        UNIT_ASSERT(metadataSaver);

        THolder<TEvSaveScriptFinalStatusResponse> saved = MakeHolder<TEvSaveScriptFinalStatusResponse>();
        saved->Status = Ydb::StatusIds::SUCCESS;
        saved->ApplicateScriptExternalEffectRequired = true;
        saved->CustomerSuppliedId = "deadline-test";
        saved->UserToken = MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"});
        saved->Sinks = std::move(sinks);
        saved->SecretNames = {"/Root/database/secret"};

        NIamDelegation::TEvIamDelegation::TEvGetTokenResult::TPtr heldToken;
        auto holdToken = runtime.AddObserver<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>(
            [&](auto& event) { heldToken = std::move(event); });
        std::vector<TEvDescribeSecretsResponse::TDescription> resolutions;
        auto resolved = runtime.AddObserver<TEvDescribeSecretsResponse>([&](auto& event) {
            if (event->GetRecipientRewrite() == parent) {
                resolutions.push_back(event->Get()->Description);
                // Stop at the real parent's preparation boundary: S3 commit and
                // final metadata persistence are outside this deadline regression.
                event.Reset();
            }
        });
        runtime.Send(new IEventHandle(parent, metadataSaver, saved.Release()), 0, true);
        TDispatchOptions mintStarted;
        mintStarted.CustomFinalCondition = [&] { return bool(heldToken); };
        runtime.DispatchEvents(mintStarted);
        UNIT_ASSERT(heldToken);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 1);

        ui32 schemaAfterCancellation = 0;
        auto schemas = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySet>(
            [&](auto&) { ++schemaAfterCancellation; });
        std::vector<Ydb::StatusIds::StatusCode> finalStatuses;
        auto finalized = runtime.AddObserver<TEvScriptExecutionFinished>([&](auto& event) {
            if (event->GetRecipientRewrite() == edge) {
                finalStatuses.push_back(event->Get()->Status);
                event.Reset();
            }
        });
        awaitingFinalization = true;
        runtime.SimulateSleep(TDuration::Seconds(11));
        UNIT_ASSERT(metadataFinisher);
        // The real parent's timeout reaches the final-metadata boundary. Supply its
        // result without claiming a database or S3 finalization integration here.
        runtime.Send(new IEventHandle(parent, metadataFinisher,
            new TEvScriptExecutionFinished(Ydb::StatusIds::TIMEOUT,
                {.ExecutionEntryExists = true, .AlreadyStopped = false}, {})), 0, true);
        TDispatchOptions finished;
        finished.CustomFinalCondition = [&] { return !finalStatuses.empty(); };
        runtime.DispatchEvents(finished);
        UNIT_ASSERT_VALUES_EQUAL(finalStatuses.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(finalStatuses.front(), Ydb::StatusIds::TIMEOUT);
        const auto resolutionsAtCompletion = resolutions.size();
        holdToken.Remove();
        runtime.Send(heldToken.Release(), 0, true);
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(schemaAfterCancellation, 0);
        UNIT_ASSERT_VALUES_EQUAL(resolutions.size(), resolutionsAtCompletion);
        UNIT_ASSERT_VALUES_EQUAL(finalStatuses.size(), 1);
    }

    // The following case needs the additive deadline argument on
    // CreateScriptSecretsResolverActor. For RED, add the interface with an unused
    // argument first; do not implement its behavior before running this regression.
    Y_UNIT_TEST(ExplicitShortDeadlineCancelsResolutionAndIgnoresLateToken) {
        TFixture fixture;
        auto& runtime = fixture.Context.GetRuntime();
        auto previousTimers = runtime.SetScheduledEventFilter(
            [](auto&, auto&, auto, auto&) { return false; });
        Y_DEFER { runtime.SetScheduledEventFilter(previousTimers); };
        const auto edge = runtime.AllocateEdgeActor();
        const auto deadline = runtime.GetCurrentTime() + TDuration::Seconds(10);
        NIamDelegation::TEvIamDelegation::TEvGetTokenResult::TPtr heldToken;
        auto holdToken = runtime.AddObserver<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>(
            [&](auto& event) { heldToken = std::move(event); });
        std::vector<TEvDescribeSecretsResponse::TDescription> resolutions;
        auto resolved = runtime.AddObserver<TEvDescribeSecretsResponse>([&](auto& event) {
            if (event->GetRecipientRewrite() == edge) {
                resolutions.push_back(event->Get()->Description);
                event.Reset();
            }
        });
        runtime.Register(CreateScriptSecretsResolverActor(edge,
            MakeIntrusive<NACLib::TUserToken>("reader", TVector<TString>{"readers"}),
            "/Root/database", {"/Root/database/secret"}, fixture.PersistSinks(), deadline));
        TDispatchOptions mintStarted;
        mintStarted.CustomFinalCondition = [&] { return bool(heldToken); };
        runtime.DispatchEvents(mintStarted);
        UNIT_ASSERT(heldToken);
        runtime.SimulateSleep(TDuration::Seconds(11));
        UNIT_ASSERT_VALUES_EQUAL(resolutions.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(resolutions.front().Status, Ydb::StatusIds::TIMEOUT);
        UNIT_ASSERT(resolutions.front().SecretValues.empty());

        ui32 laterSchemaRequests = 0;
        auto navigation = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySet>(
            [&](auto&) { ++laterSchemaRequests; });
        holdToken.Remove();
        runtime.Send(heldToken.Release(), 0, true);
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(resolutions.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(laterSchemaRequests, 0);
        UNIT_ASSERT_VALUES_EQUAL(fixture.MintCalls, 1);
    }

}
} // namespace NKikimr::NKqp
