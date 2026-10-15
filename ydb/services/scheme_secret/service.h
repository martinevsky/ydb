#pragma once

#include <ydb/services/scheme_secret/resolver.h>
#include <ydb/services/scheme_secret/iam_delegation_reader.h>

#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/scheme_board/events.h>

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

#include <util/generic/hash_multi_map.h>
#include <util/generic/strbuf.h>

#include <memory>

namespace NKikimr::NSecret {

inline NActors::TActorId MakeDescribeSchemaSecretServiceId(ui32 nodeId) {
    constexpr char name[NActors::TActorId::MaxServiceIDLength] = "desc_secret";
    return NActors::TActorId(nodeId, TStringBuf(name, NActors::TActorId::MaxServiceIDLength));
}

class TDescribeSchemaSecretsService: public NActors::TActorBootstrapped<TDescribeSchemaSecretsService> {
public:
    using TRetryPolicy = IRetryPolicy<>;

    enum ESecretEvents {
        EvResolveSecret = EventSpaceBegin(TKikimrEvents::ES_PRIVATE),
        EvResolveSecretSchemeCacheRetry,
        EvResolveSecretSchemeShardRetry,
        EvCancelResolveSecret,
        EvEnd,
    };

    enum class EResolveOwnership { Promise, Caller };

    struct TEvResolveSecret : public NActors::TEventLocal<TEvResolveSecret, EvResolveSecret> {
    public:
        TEvResolveSecret(
            const TIntrusiveConstPtr<NACLib::TUserToken> userToken,
            const TString& database,
            const TVector<TString>& secretNames,
            NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription> promise,
            TDescribeSecretSettings settings = {},
            EResolveOwnership ownership = EResolveOwnership::Promise
        )
            : UserToken(userToken)
            , Database(database)
            , SecretNames(secretNames)
            , Promise(promise)
            , Settings(std::move(settings))
            , Ownership(ownership)
        {
            Y_ENSURE(!Database.empty(), "Database name must be set in secret requests");
        }

        THolder<TEvResolveSecret> MakeCopy() const {
            return MakeHolder<TEvResolveSecret>(UserToken, Database, SecretNames, Promise, Settings, Ownership);
        }

    public:
        const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
        const TString Database;
        const TVector<TString> SecretNames;
        NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription> Promise;
        const TDescribeSecretSettings Settings;
        const EResolveOwnership Ownership;
    };

    struct TEvCancelResolveSecret :
        public NActors::TEventLocal<TEvCancelResolveSecret, EvCancelResolveSecret> {};

private:
    struct TVersionedSecret {
        ui64 SecretVersion = 0;
        ui64 PathId = 0;
        ui64 PathOwnerId = 0;
        TString Name;
        TString Value;
        NKqp::TEvDescribeSecretsResponse::TSecretReference Reference;
        TInstant UsableUntil;
    };

    using TDescription = NKqp::TEvDescribeSecretsResponse::TDescription;
    using TRequestKey = std::pair<TActorId, ui64>;

    STRICT_STFUNC(StateWait,
        hFunc(TEvResolveSecret, HandleIncomingRequest);
        hFunc(TEvCancelResolveSecret, HandleCancellation);
        IgnoreFunc(TEvIamDelegationSecretReadResult);
        IgnoreFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult);
        IgnoreFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult);
        hFunc(TSchemeBoardEvents::TEvNotifyDelete, HandleNotifyDelete);
        hFunc(TSchemeBoardEvents::TEvNotifyUpdate, HandleNotifyUpdate);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
    )

    void HandleIncomingRequest(TEvResolveSecret::TPtr ev);
    void HandleCancellation(TEvCancelResolveSecret::TPtr& ev);
    void HandleNotifyDelete(TSchemeBoardEvents::TEvNotifyDelete::TPtr& ev);
    void HandleNotifyUpdate(TSchemeBoardEvents::TEvNotifyUpdate::TPtr& ev);
    NActors::async<TDescription> Resolve(const TEvResolveSecret& request);
    NActors::async<TAutoPtr<NSchemeCache::TSchemeCacheNavigate>> Navigate(
        const TEvResolveSecret& request, const TVector<TString>& paths);
    NActors::async<TVersionedSecret> ResolveOne(
        const TEvResolveSecret& request, NSchemeCache::TSchemeCacheNavigate::TEntry entry);

public:
    TDescribeSchemaSecretsService() = default;

    void Bootstrap();
    void PassAway() override;

public:
    // For tests only
    class ISecretUpdateListener : public TThrRefBase {
    public:
        virtual void HandleNotifyDelete(const TString& secretName) = 0;
        virtual ~ISecretUpdateListener() = default;
    };
    // For tests only
    void SetSecretUpdateListener(ISecretUpdateListener* secretUpdateListener) {
        SecretUpdateListener = secretUpdateListener;
    }

    // For tests only
    class ISchemeCacheStatusGetter : public TThrRefBase {
    public:
        virtual NSchemeCache::TSchemeCacheNavigate::EStatus GetStatus(
            NSchemeCache::TSchemeCacheNavigate::TEntry& entry) const = 0;
        virtual ~ISchemeCacheStatusGetter() = default;
    };
    // For tests only
    void SetSchemeCacheStatusGetter(ISchemeCacheStatusGetter* schemeCacheStatusGetter) {
        SchemeCacheStatusGetter = schemeCacheStatusGetter;
    }

    // For tests only
    class ISchemeShardStatusGetter : public TThrRefBase {
    public:
        virtual NKikimrScheme::EStatus GetStatus(
            const NKikimrScheme::TEvDescribeSchemeResult& record) const = 0;
        virtual ~ISchemeShardStatusGetter() = default;
    };
    // For tests only
    void SetSchemeShardStatusGetter(ISchemeShardStatusGetter* schemeShardStatusGetter) {
        SchemeShardStatusGetter = schemeShardStatusGetter;
    }

private:
    // Only cancellation routing is shared; operation state lives in its coroutine.
    THashMap<TRequestKey, NActors::TAsyncCancellationScope*> RunningRequests;
    THashMap<TString, TVersionedSecret> VersionedSecrets;
    THashMap<TString, TActorId> SchemeBoardSubscribers;
    ISecretUpdateListener* SecretUpdateListener = nullptr;
    ISchemeCacheStatusGetter* SchemeCacheStatusGetter = nullptr;
    ISchemeShardStatusGetter* SchemeShardStatusGetter = nullptr;
};

class IDescribeSchemaSecretsServiceFactory {
public:
    using TPtr = std::shared_ptr<IDescribeSchemaSecretsServiceFactory>;

    virtual NActors::IActor* CreateService() = 0;
    virtual ~IDescribeSchemaSecretsServiceFactory() = default;
};

class TDescribeSchemaSecretsServiceFactory : public IDescribeSchemaSecretsServiceFactory {
public:
    NActors::IActor* CreateService() override;
};

}  // namespace NKikimr::NSecret
