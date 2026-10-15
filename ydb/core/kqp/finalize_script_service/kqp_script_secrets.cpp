#include "kqp_script_secrets.h"

#include <ydb/core/kqp/common/kqp_external_effect_secrets.h>
#include <ydb/library/actors/async/task_group.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/services/scheme_secret/resolver.h>
#include <yql/essentials/providers/common/structured_token/yql_token_builder.h>

#include <algorithm>

namespace NKikimr::NKqp {
namespace {
using namespace NActors;
using TDescription = TEvDescribeSecretsResponse::TDescription;
using TReference = TEvDescribeSecretsResponse::TSecretReference;

TDescription Failure(Ydb::StatusIds::StatusCode status, TString issue) {
    return TDescription(status, {NYql::TIssue(std::move(issue))});
}

class TScriptSecretsResolver final : public TActorBootstrapped<TScriptSecretsResolver> {
public:
    TScriptSecretsResolver(const TActorId& replyTo, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
        TString database, std::vector<TString> secretNames, std::vector<NKqpProto::TKqpExternalSink> sinks,
        std::optional<TInstant> deadline)
        : ReplyTo(replyTo), UserToken(std::move(userToken)), Database(std::move(database))
        , SecretNames(std::move(secretNames)), Sinks(std::move(sinks)), Deadline(deadline) {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        auto result = co_await WithDeadline(Deadline.value_or(TInstant::Max()), &TThis::Resolve, this);
        Send(ReplyTo, new TEvDescribeSecretsResponse(result ? std::move(*result)
            : Failure(Ydb::StatusIds::TIMEOUT, "External effect secret resolution timed out")));
        PassAway();
    }

private:
    STRICT_STFUNC(StateWork,
        IgnoreFunc(TEvDescribeSecretsResponse);
        IgnoreFunc(TEvents::TEvUndelivered);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )

    async<TDescription> ReadSecret(TString path, std::optional<TReference> expected) {
        NSecret::TDescribeSecretSettings settings;
        settings.ExpectedReference = expected;
        auto description = co_await NSecret::DescribeSecretAsync({path}, UserToken, Database, std::move(settings));
        if (description.Status != Ydb::StatusIds::SUCCESS) co_return description;
        if (description.SecretValues.size() != 1 || description.References.size() > 1
            || description.UsableUntil.size() > 1) {
            co_return Failure(Ydb::StatusIds::INTERNAL_ERROR, "Unexpected external effect secret response");
        }
        const auto reference = description.References.empty() ? TReference{} : description.References.front();
        if (expected && *expected != reference) {
            co_return Failure(Ydb::StatusIds::NOT_FOUND, "External effect secret incarnation changed");
        }
        if (reference.TabletId && !expected) {
            co_return Failure(Ydb::StatusIds::PRECONDITION_FAILED, "External effect has no persisted IAM secret identity");
        }
        if (reference.TabletId && description.UsableUntil.size() != 1) {
            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Missing delegated token expiry");
        }
        co_return description;
    }

    async<TDescription> Resolve() {
        TScriptSecretReferences expected;
        TSet<TString> required;
        try {
            expected = ExtractScriptExternalEffectSecretPins(Sinks, Database);
            for (const auto& sink : Sinks) {
                if (NYql::IsStructuredTokenJson(sink.GetAuthInfo())) {
                    NYql::CreateStructuredTokenParser(sink.GetAuthInfo()).ListReferences(required);
                }
            }
            for (const auto& path : required) {
                Y_ENSURE(std::find(SecretNames.begin(), SecretNames.end(), path) != SecretNames.end(),
                    "Persisted external effect secret is missing from the secret list");
            }
        } catch (const std::exception&) {
            co_return Failure(Ydb::StatusIds::PRECONDITION_FAILED, "Invalid persisted external effect secret identities");
        }
        co_return co_await WithTaskGroup<TDescription>([&](TTaskGroup<TDescription>& group) -> async<TDescription> {
            std::vector<size_t> indices;
            for (size_t i = 0; i < SecretNames.size(); ++i) {
                // Input also contains source credentials. Only persisted sink
                // references participate in external-effect finalization.
                if (!required.contains(SecretNames[i])) continue;
                const auto it = expected.find(SecretNames[i]);
                std::optional<TReference> pin;
                if (it != expected.end()) pin = it->second;
                group.Add([this, path = SecretNames[i], pin = std::move(pin)]() mutable {
                    return ReadSecret(std::move(path), std::move(pin));
                });
                indices.push_back(i);
            }
            std::vector<TString> values(SecretNames.size());
            std::vector<TReference> references(SecretNames.size());
            std::vector<TInstant> usableUntil(SecretNames.size(), TInstant::Max());
            for (size_t remaining = indices.size(); remaining; --remaining) {
                auto completed = co_await group.NextResult();
                const auto index = indices.at(completed.GetIndex());
                auto description = completed.ExtractValue();
                if (description.Status != Ydb::StatusIds::SUCCESS) co_return description;
                values[index] = description.SecretValues.front();
                if (!description.References.empty()) references[index] = description.References.front();
                if (!description.UsableUntil.empty()) usableUntil[index] = description.UsableUntil.front();
            }
            for (size_t i = 0; i < references.size(); ++i) {
                if (references[i].TabletId && TActivationContext::Now() >= usableUntil[i]) {
                    co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Delegated token expired during external effect preparation");
                }
            }
            co_return TDescription(std::move(values), std::move(references), std::move(usableUntil));
        });
    }

    const TActorId ReplyTo;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const TString Database;
    const std::vector<TString> SecretNames;
    const std::vector<NKqpProto::TKqpExternalSink> Sinks;
    const std::optional<TInstant> Deadline;
};
} // namespace

IActor* CreateScriptSecretsResolverActor(const TActorId& replyTo,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, TString database,
    std::vector<TString> secretNames, std::vector<NKqpProto::TKqpExternalSink> sinks,
    std::optional<TInstant> deadline)
{
    return new TScriptSecretsResolver(replyTo, std::move(userToken), std::move(database),
        std::move(secretNames), std::move(sinks), deadline);
}

} // namespace NKikimr::NKqp
