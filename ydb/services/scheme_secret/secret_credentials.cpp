#include "secret_credentials.h"
#include "resolver.h"
#include "service.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/counters.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/library/yql/providers/common/token_accessor/client/bearer_credentials_provider.h>
#include <yql/essentials/providers/common/structured_token/yql_structured_token.h>

#include <library/cpp/threading/future/future.h>

#include <util/system/spinlock.h>

#include <optional>

namespace NKikimr::NSecret {

namespace {

// Sensors of the re-reading credentials, under the schema secrets service counters.
struct TSensors : TThrRefBase {
    explicit TSensors(NActors::TActorSystem* actorSystem) {
        const auto group = GetServiceCounters(AppData(actorSystem)->Counters, "schema_secrets")->GetSubgroup("component", "refreshing_credentials");
        Providers = group->GetCounter("ProvidersCreated", true);
        ReReads = group->GetCounter("ReReads", true);
        ReReadErrors = group->GetCounter("ReReadErrors", true);
    }

    ::NMonitoring::TDynamicCounters::TCounterPtr Providers;
    ::NMonitoring::TDynamicCounters::TCounterPtr ReReads;
    ::NMonitoring::TDynamicCounters::TCounterPtr ReReadErrors;
};

// Shared between the provider and the callbacks of its re-reads (a callback may outlive the provider).
struct TRefreshState : TThrRefBase {
    TAdaptiveLock Lock;
    TString Value;
    // After this moment Value must not be handed out (a delegation token below its minimum remaining lifetime).
    // The value the task started with was read just before, so it counts as usable until the first re-read.
    TInstant UsableUntil = TInstant::Max();
    bool Refreshing = false; // a re-read is in flight: the next uses take the current value while it is usable
    // uses that found no usable value: completed by the re-read in flight
    std::optional<NThreading::TPromise<std::string>> Waiting;
};

class TRefreshingSecretCredentialsProvider : public NYdb::ICredentialsProvider {
public:
    TRefreshingSecretCredentialsProvider(NActors::TActorSystem* actorSystem, TString secretPath, TString database, const TString& initialToken,
            TIntrusivePtr<TSensors> sensors)
        : ActorSystem(actorSystem)
        , SecretPath(std::move(secretPath))
        , Database(std::move(database))
        , State(MakeIntrusive<TRefreshState>())
        , Sensors(std::move(sensors))
    {
        State->Value = initialToken;
    }

    std::string GetAuthInfo() const override {
        return GetAuthInfoAsync().GetValueSync();
    }

    // Returns the value of the last read and starts the next one, so that the value handed out is never older
    // than one read of the node's secret cache. Only one read is in flight at a time, so a busy task does not
    // multiply reads; a failed read keeps the current value and is retried on the next use. A value past its
    // usable moment is never handed out: the use waits for the read in flight and gets its result, a fresh
    // value or the error the read ended with.
    NThreading::TFuture<std::string> GetAuthInfoAsync() const override {
        bool refresh = false;
        std::optional<NThreading::TFuture<std::string>> wait;
        std::string value;
        with_lock (State->Lock) {
            if (!State->Refreshing) {
                State->Refreshing = true;
                refresh = true;
            }
            if (!State->Value.empty() && TInstant::Now() < State->UsableUntil) {
                value = State->Value;
            } else {
                if (!State->Waiting) {
                    State->Waiting = NThreading::NewPromise<std::string>();
                }
                wait = State->Waiting->GetFuture();
            }
        }
        if (refresh) {
            StartRefresh();
        }
        if (wait) {
            return *wait;
        }
        return NThreading::MakeFuture(std::move(value));
    }

    bool IsValid() const override {
        return true;
    }

private:
    // Called from any thread (SDK sessions ask for tokens on their own threads): the secret service is
    // reached through the actor system only. No user token: the rights were checked when the execution started.
    void StartRefresh() const {
        auto state = State;
        auto sensors = Sensors;
        auto* actorSystem = ActorSystem;
        const TString secretPath = SecretPath;
        auto promise = NThreading::NewPromise<NKqp::TEvDescribeSecretsResponse::TDescription>();
        auto future = promise.GetFuture();
        actorSystem->Send(MakeDescribeSchemaSecretServiceId(actorSystem->NodeId),
            new TDescribeSchemaSecretsService::TEvResolveSecret(nullptr, Database, {SecretPath}, std::move(promise)));
        future.Subscribe(
            [state, sensors, actorSystem, secretPath](const NThreading::TFuture<NKqp::TEvDescribeSecretsResponse::TDescription>& future) {
                TString value;
                TInstant usableUntil = TInstant::Max();
                TString error;
                try {
                    const auto& description = future.GetValue();
                    if (description.Status == Ydb::StatusIds::SUCCESS && description.SecretValues.size() == 1) {
                        value = description.SecretValues[0];
                        if (!description.UsableUntil.empty()) {
                            usableUntil = description.UsableUntil[0];
                        }
                    } else {
                        error = TStringBuilder() << description.Status << ": " << description.Issues.ToOneLineString();
                    }
                } catch (const std::exception& e) {
                    error = e.what();
                }
                if (value.empty()) {
                    sensors->ReReadErrors->Inc();
                    YDB_LOG_CTX_COMP(*actorSystem, NActors::NLog::PRI_WARN, NKikimrServices::SCHEMA_SECRET_CACHE,
                        "Cannot re-read the secret for a running task, the previous value is kept",
                        {"secret", secretPath}, {"error", error});
                } else {
                    sensors->ReReads->Inc();
                }
                std::optional<NThreading::TPromise<std::string>> waiting;
                with_lock (state->Lock) {
                    state->Refreshing = false;
                    if (!value.empty()) {
                        state->Value = value;
                        state->UsableUntil = usableUntil;
                    }
                    waiting.swap(state->Waiting);
                }
                // outside the lock: the waiters' continuations run here
                if (waiting) {
                    if (!value.empty()) {
                        waiting->SetValue(value);
                    } else {
                        waiting->SetException(TStringBuilder() << "cannot read secret " << secretPath << ": " << error);
                    }
                }
            });
    }

    NActors::TActorSystem* const ActorSystem;
    const TString SecretPath;
    const TString Database;
    const TIntrusivePtr<TRefreshState> State;
    const TIntrusivePtr<TSensors> Sensors;
};

class TRefreshingSecretCredentialsProviderFactory : public NYdb::ICredentialsProviderFactory {
public:
    TRefreshingSecretCredentialsProviderFactory(NActors::TActorSystem* actorSystem, TString secretPath, TString database, TString initialToken)
        : ActorSystem(actorSystem)
        , SecretPath(std::move(secretPath))
        , Database(std::move(database))
        , InitialToken(std::move(initialToken))
        , Sensors(MakeIntrusive<TSensors>(actorSystem))
    {}

    NYdb::TCredentialsProviderPtr CreateProvider() const override {
        Sensors->Providers->Inc();
        if (NActors::TlsActivationContext) {
            YDB_LOG_DEBUG_COMP(NKikimrServices::SCHEMA_SECRET_CACHE, "Created a credentials provider over a secret", {"secret", SecretPath});
        }
        return std::make_shared<TRefreshingSecretCredentialsProvider>(ActorSystem, SecretPath, Database, InitialToken, Sensors);
    }

    std::string GetClientIdentity() const override {
        return TStringBuilder() << "SecretRef:" << SecretPath;
    }

private:
    NActors::TActorSystem* const ActorSystem;
    const TString SecretPath;
    const TString Database;
    const TString InitialToken;
    const TIntrusivePtr<TSensors> Sensors;
};

class TRefreshingSecretCredentialsFactoryOverFactory : public NYql::IStructuredTokenCredentialsFactory {
public:
    explicit TRefreshingSecretCredentialsFactoryOverFactory(NYql::IStructuredTokenCredentialsFactory::TPtr factory)
        : Inner(std::move(factory))
    {}

    std::shared_ptr<NYdb::ICredentialsProviderFactory> Create(const TString& structuredTokenJson, bool addBearerToToken) override {
        if (NYql::IsStructuredTokenJson(structuredTokenJson) && NActors::TlsActivationContext) {
            const auto token = NYql::ParseStructuredToken(structuredTokenJson);
            if (token.HasField(TString(SecretReferenceField)) && token.HasField("token")) {
                return CreateRefreshingSecretCredentialsProviderFactory(
                    NActors::TActivationContext::ActorSystem(),
                    token.GetField(TString(SecretReferenceField)),
                    token.GetField(TString(SecretDatabaseField)),
                    token.GetField("token"),
                    addBearerToToken ? EBearerPrefix::Add : EBearerPrefix::None);
            }
        }
        return Inner->Create(structuredTokenJson, addBearerToToken);
    }

private:
    const NYql::IStructuredTokenCredentialsFactory::TPtr Inner;
};

} // namespace

TString KeepTokenSecretReference(const TString& structuredTokenJson, const TString& tokenReference, const TString& database) {
    if (!IsSchemeSecret(tokenReference) || !NYql::IsStructuredTokenJson(structuredTokenJson)) {
        return structuredTokenJson;
    }
    auto token = NYql::ParseStructuredToken(structuredTokenJson);
    token.SetField(TString(SecretReferenceField), tokenReference);
    token.SetField(TString(SecretDatabaseField), database);
    return token.ToJson();
}

std::shared_ptr<NYdb::ICredentialsProviderFactory> CreateRefreshingSecretCredentialsProviderFactory(
    NActors::TActorSystem* actorSystem, const TString& secretPath, const TString& database,
    const TString& initialToken, EBearerPrefix bearerPrefix)
{
    Y_ENSURE(actorSystem, "actor system is required to re-read secrets");
    std::shared_ptr<NYdb::ICredentialsProviderFactory> factory =
        std::make_shared<TRefreshingSecretCredentialsProviderFactory>(actorSystem, secretPath, database, initialToken);
    return bearerPrefix == EBearerPrefix::Add ? NYql::WrapCredentialsProviderFactoryWithBearer(std::move(factory)) : factory;
}

NYql::IStructuredTokenCredentialsFactory::TPtr CreateRefreshingSecretCredentialsFactoryOverFactory(
    NYql::IStructuredTokenCredentialsFactory::TPtr factory)
{
    return std::make_shared<TRefreshingSecretCredentialsFactoryOverFactory>(std::move(factory));
}

} // namespace NKikimr::NSecret
