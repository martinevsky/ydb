#pragma once

#include <ydb/core/kqp/common/events/script_executions.h>

#include <atomic>
#include <ydb/services/scheme_secret/service.h>
#include <ydb/services/scheme_secret/resolver.h>
#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <util/generic/hash.h>
#include <util/generic/vector.h>
#include <util/generic/string.h>
#include <util/generic/ptr.h>

namespace NKikimr::NSecret {
    using TDescriptionPromise = NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription>;

    void CreateSchemaSecret(const TString& secretName, const TString& secretValue, NYdb::NTable::TSession& session);
    void AlterSchemaSecret(const TString& secretName, const TString& secretValue, NYdb::NTable::TSession& session);
    void DropSchemaSecret(const TString& secretName, NYdb::NTable::TSession& session);

    // Creates / alters a secret of type IAM_DELEGATION directly through the tx proxy (no IAM delegation is set up)
    // and waits until the scheme cache serves the new description. FeatureFlags.EnableIamDelegationSecrets must be on.
    void CreateIamDelegationSecretDirect(NActors::TTestActorRuntime& runtime, const TString& path,
        const TString& serviceAccountId, const TString& cloudId, const TString& referrerId);
    void AlterIamDelegationSecretDirect(NActors::TTestActorRuntime& runtime, const TString& path,
        const TString& serviceAccountId, const TString& cloudId, const TString& referrerId);

    // Registers a stand-in for the node-local IAM delegated token service: it answers TEvGetToken with
    // "<prefix><resource>/<service account>" (no IAM is involved) for keys in `tokens` and with UNAUTHORIZED
    // for the others. Returns the token for a key.
    struct TFakeDelegatedTokens : TThrRefBase {
        THashMap<TString, TString> Tokens; // TTokenKey::ToString() -> token
        ui32 UnavailableAnswers = 0;       // answer UNAVAILABLE this many times before serving tokens
        std::atomic<ui32> Calls = 0;       // TEvGetToken requests received
        // every answer carries a distinct token "<token>-<n>" (a mint per request), recorded in Minted
        bool UniqueTokens = false;
        // hold the answers until ReleaseHeldDelegatedTokenReplies (the requests are still counted in Calls)
        std::atomic<bool> HoldReplies = false;
        TMutex Mutex;
        TVector<TString> Minted; // tokens handed out, in order (under Mutex)

        TVector<TString> MintedTokens();
    };
    // `tokens` lets several nodes share one record; the service is registered on node `nodeIndex`.
    TIntrusivePtr<TFakeDelegatedTokens> RegisterFakeIamDelegatedTokenService(NActors::TTestActorRuntime& runtime,
        const TVector<std::pair<TString, TString>>& delegations /* {service account, resource} */, const TString& prefix = "delegated-token-",
        ui32 nodeIndex = 0, TIntrusivePtr<TFakeDelegatedTokens> tokens = nullptr);
    void ReleaseHeldDelegatedTokenReplies(NActors::TTestActorRuntime& runtime, ui32 nodeIndex = 0);

    // Registers a stand-in for the node-local IAM delegation service: it records every TEvSetupDelegation /
    // TEvRevokeDelegation it receives (no IAM is involved) and answers with the configured statuses. Replies can
    // be held back (HoldSetups / HoldRevokes) and released later with ReleaseHeldDelegationReplies, so that a test
    // can observe the state of the schema while the orchestrator waits for IAM.
    struct TFakeDelegationCalls : TThrRefBase {
        struct TCall {
            TString Method; // "Setup" | "Revoke"
            NIamDelegation::TDelegationSpec Spec;
            TString SubjectId; // Setup only
        };

        TMutex Mutex;
        TVector<TCall> Calls;
        Ydb::StatusIds::StatusCode SetupStatus = Ydb::StatusIds::SUCCESS;
        Ydb::StatusIds::StatusCode RevokeStatus = Ydb::StatusIds::SUCCESS;
        bool HoldSetups = false;
        bool HoldRevokes = false;

        ui32 SetupCalls();
        ui32 RevokeCalls();
        TVector<TCall> Snapshot();
        // Hang guard: waits until at least `count` calls of the method were received (the test makes them inevitable).
        void WaitCalls(const TString& method, ui32 count, TDuration timeout = TDuration::Seconds(120));
    };
    // `calls` lets several nodes share one record (e.g. a static node and the dynamic node of a database).
    TIntrusivePtr<TFakeDelegationCalls> RegisterFakeIamDelegationService(NActors::TTestActorRuntime& runtime, ui32 nodeIndex = 0,
        TIntrusivePtr<TFakeDelegationCalls> calls = nullptr);
    void ReleaseHeldDelegationReplies(NActors::TTestActorRuntime& runtime, ui32 nodeIndex = 0);

    TDescriptionPromise
    ResolveSecrets(
        const TVector<TString>& secretNames,
        NKqp::TKikimrRunner& kikimr,
        const TIntrusiveConstPtr<NACLib::TUserToken> userToken = nullptr,
        TDescribeSecretSettings settings = {}
    );
    TDescriptionPromise
    ResolveSecret(
        const TString& secretName,
        NKqp::TKikimrRunner& kikimr,
        const TIntrusiveConstPtr<NACLib::TUserToken> userToken = nullptr,
        TDescribeSecretSettings settings = {}
    );

    void AssertBadRequest(TDescriptionPromise promise, const TString& err, Ydb::StatusIds::StatusCode status = Ydb::StatusIds::BAD_REQUEST);
    void AssertErrorContains(TDescriptionPromise promise, const TString& errPart, Ydb::StatusIds::StatusCode status);

    TIntrusiveConstPtr<NACLib::TUserToken> GetUserToken(const TString& userSid = "", const TVector<TString>& groupSids = {});

    void AssertSecretValues(const TVector<TString>& secretValues, TDescriptionPromise promise);
    void AssertSecretValue(const TString& secretValue, TDescriptionPromise promise);

    class TTestDescribeSchemaSecretsServiceFactory : public IDescribeSchemaSecretsServiceFactory {
    public:
        TTestDescribeSchemaSecretsServiceFactory(
            TDescribeSchemaSecretsService::ISecretUpdateListener* secretUpdateListener,
            TDescribeSchemaSecretsService::ISchemeCacheStatusGetter* schemeCacheStatusGetter,
            TDescribeSchemaSecretsService::ISchemeShardStatusGetter* schemeShardStatusGetter
        );

        NActors::IActor* CreateService() override;

    private:
        TDescribeSchemaSecretsService::ISecretUpdateListener* SecretUpdateListener;
        TDescribeSchemaSecretsService::ISchemeCacheStatusGetter* SchemeCacheStatusGetter;
        TDescribeSchemaSecretsService::ISchemeShardStatusGetter* SchemeShardStatusGetter;
    };

    class TTestSchemeCacheStatusGetter : public TDescribeSchemaSecretsService::ISchemeCacheStatusGetter {
    public:
        enum class EFailProbability {
            None = 0,
            OneTenth = 1,
            Always = 2,
        };

        TTestSchemeCacheStatusGetter(EFailProbability failProbability);

        NSchemeCache::TSchemeCacheNavigate::EStatus GetStatus(
            NSchemeCache::TSchemeCacheNavigate::TEntry& entry) const override;

        void SetFailProbability(EFailProbability failProbability);

    private:
        EFailProbability FailProbability;
        mutable std::mt19937 RandomGen;
    };

    class TTestSchemeShardStatusGetter : public TDescribeSchemaSecretsService::ISchemeShardStatusGetter {
    public:
        TTestSchemeShardStatusGetter(
            const ui32 statusOverwriteRemainingCount,
            const NKikimrScheme::EStatus overwrittenStatus);

        NKikimrScheme::EStatus GetStatus(
            const NKikimrScheme::TEvDescribeSchemeResult& record) const override;

    private:
        const NKikimrScheme::EStatus OverwrittenStatus;
        mutable ui32 StatusOverwriteRemainingCount = 0;
    };

} // NKikimr::NSecret
