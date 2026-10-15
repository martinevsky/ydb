#include "secret_credentials.h"
#include "service.h"

#include <ydb/library/yql/providers/common/token_accessor/client/bearer_credentials_provider.h>
#include <yql/essentials/providers/common/structured_token/yql_structured_token.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>
#include <util/string/cast.h>
#include <util/system/spinlock.h>

namespace NKikimr::NSecret {
namespace {
using TReference = NKqp::TEvDescribeSecretsResponse::TSecretReference;
using TDescription = NKqp::TEvDescribeSecretsResponse::TDescription;

struct TRefreshAttempt : TThrRefBase {
    NThreading::TPromise<std::string> Promise = NThreading::NewPromise<std::string>();
};

struct TRefreshState : TThrRefBase {
    TAdaptiveLock Lock;
    TIntrusivePtr<TRefreshAttempt> Waiting;
};

class TRefreshingProvider final : public NYdb::ICredentialsProvider {
public:
    TRefreshingProvider(NActors::TActorSystem* actorSystem, TString path, TString database,
        TReference reference, TIntrusiveConstPtr<NACLib::TUserToken> userToken)
        : ActorSystem(actorSystem), Path(std::move(path)), Database(std::move(database))
        , Reference(std::move(reference)), UserToken(std::move(userToken)), State(MakeIntrusive<TRefreshState>()) {}

    std::string GetAuthInfo() const override {
        // Blocking an actor prevents the resolver from progressing on that executor.
        // A caller that cannot wait asynchronously must fail, never reuse a previous token.
        Y_ENSURE(!NActors::TlsActivationContext,
            "IAM delegation credentials require asynchronous authorization on actor threads");
        return GetAuthInfoAsync().GetValueSync();
    }

    NThreading::TFuture<std::string> GetAuthInfoAsync() const override {
        TIntrusivePtr<TRefreshAttempt> attempt;
        with_lock (State->Lock) {
            if (State->Waiting) return State->Waiting->Promise.GetFuture();
            attempt = State->Waiting = MakeIntrusive<TRefreshAttempt>();
        }
        auto result = attempt->Promise.GetFuture();
        TDescribeSecretSettings settings;
        settings.ExpectedReference = Reference;
        DescribeSchemaSecret({Path}, UserToken, Database, ActorSystem, std::move(settings))
            .Subscribe([state = State, attempt, reference = Reference](const NThreading::TFuture<TDescription>& response) {
                TString value;
                TString error;
                try {
                    const auto& description = response.GetValue();
                    // The owned actor checks usability using actor time before
                    // completing the future; this adapter only validates its shape.
                    if (description.Status == Ydb::StatusIds::SUCCESS && description.SecretValues.size() == 1
                        && description.References.size() == 1 && description.References[0] == reference
                        && description.UsableUntil.size() == 1) {
                        value = description.SecretValues[0];
                    } else {
                        error = "Secret is unavailable, changed, expired, or access was denied";
                    }
                } catch (const std::exception&) {
                    error = "Secret resolution failed";
                }
                with_lock (state->Lock) {
                    if (state->Waiting == attempt) state->Waiting.Reset();
                }
                // Settle only this attempt, outside the shared lock. A late
                // completion must never take a newer attempt's promise.
                if (value.empty()) {
                    attempt->Promise.SetException(error.empty() ? TString("Secret returned an empty token") : error);
                } else {
                    attempt->Promise.SetValue(std::string(value));
                }
            });
        return result;
    }
    bool IsValid() const override { return true; }
private:
    NActors::TActorSystem* const ActorSystem;
    const TString Path;
    const TString Database;
    const TReference Reference;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const TIntrusivePtr<TRefreshState> State;
};

class TRefreshingFactory final : public NYdb::ICredentialsProviderFactory {
public:
    TRefreshingFactory(NActors::TActorSystem* actorSystem, TString path, TString database,
        TReference reference, TIntrusiveConstPtr<NACLib::TUserToken> userToken)
        : ActorSystem(actorSystem), Path(std::move(path)), Database(std::move(database))
        , Reference(std::move(reference)), UserToken(std::move(userToken)) {}
    NYdb::TCredentialsProviderPtr CreateProvider() const override {
        return std::make_shared<TRefreshingProvider>(ActorSystem, Path, Database, Reference, UserToken);
    }
    std::string GetClientIdentity() const override {
        // SDK clients must not share providers across path reuse or authorization contexts.
        return std::string(KeepTokenSecretReference("{}", Path, Database, Reference, UserToken));
    }
private:
    NActors::TActorSystem* const ActorSystem;
    const TString Path;
    const TString Database;
    const TReference Reference;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
};

class TFactoryWrapper final : public NYql::IStructuredTokenCredentialsFactory {
public:
    explicit TFactoryWrapper(NYql::IStructuredTokenCredentialsFactory::TPtr inner) : Inner(std::move(inner)) {}
    std::shared_ptr<NYdb::ICredentialsProviderFactory> Create(const TString& json, bool addBearer) override {
        if (NYql::IsStructuredTokenJson(json)) {
            const auto token = NYql::ParseStructuredToken(json);
            if (token.HasField("secret_ref")) {
                Y_ENSURE(NActors::TlsActivationContext, "Actor system is required for IAM secret credentials");
                TReference reference{FromString<ui64>(token.GetField("secret_tablet")),
                    token.GetField("secret_incarnation"), token.GetField("secret_create_operation"),
                    FromString<ui64>(token.GetField("secret_owner")), FromString<ui64>(token.GetField("secret_local"))};
                NJson::TJsonValue groups;
                Y_ENSURE(NJson::ReadJsonTree(token.GetField("secret_groups"), &groups) && groups.IsArray(),
                    "Invalid secret authorization context");
                TVector<TString> groupSids;
                for (const auto& group : groups.GetArray()) groupSids.push_back(group.GetStringSafe());
                auto user = MakeIntrusive<NACLib::TUserToken>(token.GetField("secret_user_sid"), groupSids);
                return CreateRefreshingSecretCredentialsProviderFactory(NActors::TActivationContext::ActorSystem(),
                    token.GetField("secret_ref"), token.GetField("secret_database"), reference, user,
                    addBearer ? EBearerPrefix::Add : EBearerPrefix::None);
            }
        }
        return Inner->Create(json, addBearer);
    }
private:
    const NYql::IStructuredTokenCredentialsFactory::TPtr Inner;
};
} // namespace

TString KeepTokenSecretReference(const TString& json, const TString& path, const TString& database,
    const TReference& reference, TIntrusiveConstPtr<NACLib::TUserToken> userToken) {
    Y_ENSURE(reference.TabletId && !reference.DatabaseIncarnation.empty() && !reference.CreateOperationId.empty(),
        "Immutable IAM secret reference is required");
    Y_ENSURE(userToken, "Authorization context is required for IAM secret credentials");
    auto token = NYql::ParseStructuredToken(json);
    token.SetField("secret_ref", path).SetField("secret_database", database)
        .SetField("secret_tablet", ToString(reference.TabletId)).SetField("secret_incarnation", reference.DatabaseIncarnation)
        .SetField("secret_create_operation", reference.CreateOperationId)
        .SetField("secret_owner", ToString(reference.PathOwnerId)).SetField("secret_local", ToString(reference.PathLocalId))
        .SetField("secret_user_sid", userToken->GetUserSID());
    NJson::TJsonValue groups(NJson::JSON_ARRAY);
    for (const auto& group : userToken->GetGroupSIDs()) groups.AppendValue(group);
    token.SetField("secret_groups", NJson::WriteJson(groups, false));
    return token.ToJson();
}

std::shared_ptr<NYdb::ICredentialsProviderFactory> CreateRefreshingSecretCredentialsProviderFactory(
    NActors::TActorSystem* actorSystem, const TString& path, const TString& database,
    const TReference& reference, TIntrusiveConstPtr<NACLib::TUserToken> userToken, EBearerPrefix bearerPrefix) {
    Y_ENSURE(actorSystem && userToken, "Actor system and authorization context are required");
    // Retain only verified ACL identity, never the bearer used to authenticate the query.
    auto identity = MakeIntrusive<NACLib::TUserToken>(userToken->GetUserSID(), userToken->GetGroupSIDs());
    std::shared_ptr<NYdb::ICredentialsProviderFactory> factory =
        std::make_shared<TRefreshingFactory>(actorSystem, path, database, reference, identity);
    return bearerPrefix == EBearerPrefix::Add ? NYql::WrapCredentialsProviderFactoryWithBearer(factory) : factory;
}

NYql::IStructuredTokenCredentialsFactory::TPtr CreateRefreshingSecretCredentialsFactoryOverFactory(
    NYql::IStructuredTokenCredentialsFactory::TPtr factory) {
    return std::make_shared<TFactoryWrapper>(std::move(factory));
}
} // namespace NKikimr::NSecret
