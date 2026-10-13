#include <ydb/core/kqp/common/kqp_external_effect_secrets.h>
#include <ydb/core/kqp/finalize_script_service/kqp_script_secrets.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/tx/iam_delegation/ut/requests.h>
#include <ydb/services/scheme_secret/service.h>

namespace NKikimr::NKqp {
namespace {
using namespace NIamDelegation::NTests;
using TReference = TEvDescribeSecretsResponse::TSecretReference;

struct TFixture {
    TTestContext Context;
    TReference SchemaReference{TTestContext::TabletId, "database-incarnation-1", "create-1", 100, 42};
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
                auto self = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TDirEntryInfo>();
                self->Info.SetSchemeshardId(Fixture.SchemaReference.PathOwnerId);
                self->Info.SetPathId(Fixture.SchemaReference.PathLocalId);
                entry.Self = self;
                auto secret = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TSecretInfo>();
                auto* locator = secret->Description.MutableIamDelegation();
                locator->SetTabletId(Fixture.SchemaReference.TabletId);
                locator->SetDatabaseIncarnation(Fixture.SchemaReference.DatabaseIncarnation);
                locator->SetCreateOperationId(Fixture.SchemaReference.CreateOperationId);
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
}
} // namespace NKikimr::NKqp
