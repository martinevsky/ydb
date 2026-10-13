#include "kqp_script_secrets.h"

#include <ydb/core/kqp/common/kqp_external_effect_secrets.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/services/scheme_secret/resolver.h>
#include <yql/essentials/providers/common/structured_token/yql_token_builder.h>

#include <algorithm>

namespace NKikimr::NKqp {
namespace {

class TScriptSecretsResolver final : public TActorBootstrapped<TScriptSecretsResolver> {
    using TDescription = TEvDescribeSecretsResponse::TDescription;
public:
    TScriptSecretsResolver(const TActorId& replyTo, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
        TString database, std::vector<TString> secretNames, std::vector<NKqpProto::TKqpExternalSink> sinks)
        : ReplyTo(replyTo), UserToken(std::move(userToken)), Database(std::move(database))
        , SecretNames(std::move(secretNames)), Sinks(std::move(sinks))
        , Values(SecretNames.size()), References(SecretNames.size())
        , UsableUntil(SecretNames.size(), TInstant::Max()), Received(SecretNames.size(), false)
    {}

    void Bootstrap() {
        try {
            Expected = ExtractScriptExternalEffectSecretPins(Sinks, Database);
            TSet<TString> required;
            for (const auto& sink : Sinks) {
                if (NYql::IsStructuredTokenJson(sink.GetAuthInfo())) {
                    NYql::CreateStructuredTokenParser(sink.GetAuthInfo()).ListReferences(required);
                }
            }
            for (const auto& path : required) {
                Y_ENSURE(std::find(SecretNames.begin(), SecretNames.end(), path) != SecretNames.end(),
                    "Persisted external effect secret is missing from the secret list");
            }
            for (size_t i = 0; i < SecretNames.size(); ++i) {
                // SecretNames also contains source credentials. Finalizing a sink
                // must not require or adopt identities belonging only to reads.
                Received[i] = !required.contains(SecretNames[i]);
                Remaining += !Received[i];
            }
        } catch (const std::exception&) {
            Fail(Ydb::StatusIds::PRECONDITION_FAILED, "Invalid persisted external effect secret identities");
            return;
        }
        if (!Remaining) {
            Complete();
            return;
        }
        Become(&TScriptSecretsResolver::StateWork);
        Schedule(TDuration::Minutes(1), new TEvents::TEvWakeup());
        auto* actorSystem = TActivationContext::ActorSystem();
        for (size_t i = 0; i < SecretNames.size(); ++i) {
            if (Received[i]) {
                continue;
            }
            NSecret::TDescribeSecretSettings settings;
            if (const auto it = Expected.find(SecretNames[i]); it != Expected.end()) {
                settings.ExpectedReference = it->second;
            }
            NSecret::DescribeSecret({SecretNames[i]}, UserToken, Database, actorSystem, std::move(settings))
                .Subscribe([actorSystem, self = SelfId(), i](const NThreading::TFuture<TDescription>& ready) {
                    THolder<TEvDescribeSecretsResponse> response;
                    try {
                        response = MakeHolder<TEvDescribeSecretsResponse>(ready.GetValue());
                    } catch (const std::exception&) {
                        response = MakeHolder<TEvDescribeSecretsResponse>(TDescription(Ydb::StatusIds::UNAVAILABLE,
                            {NYql::TIssue("Cannot resolve external effect secret")}));
                    }
                    actorSystem->Send(new IEventHandle(self, {}, response.Release(), 0, i));
                });
        }
    }

private:
    STRICT_STFUNC(StateWork,
        hFunc(TEvDescribeSecretsResponse, Handle);
        cFunc(TEvents::TEvWakeup::EventType, Timeout);
    )

    void Handle(TEvDescribeSecretsResponse::TPtr& ev) {
        const size_t index = ev->Cookie;
        if (index >= SecretNames.size() || Received[index]) {
            return;
        }
        const auto& description = ev->Get()->Description;
        if (description.Status != Ydb::StatusIds::SUCCESS) {
            Send(ReplyTo, new TEvDescribeSecretsResponse(description));
            PassAway();
            return;
        }
        if (description.SecretValues.size() != 1 || description.References.size() > 1
            || description.UsableUntil.size() > 1) {
            Fail(Ydb::StatusIds::INTERNAL_ERROR, "Unexpected external effect secret response");
            return;
        }
        const auto reference = description.References.empty()
            ? TEvDescribeSecretsResponse::TSecretReference{} : description.References.front();
        const auto expected = Expected.find(SecretNames[index]);
        if (expected != Expected.end() && expected->second != reference) {
            Fail(Ydb::StatusIds::NOT_FOUND, "External effect secret incarnation changed");
            return;
        }
        if (reference.TabletId && expected == Expected.end()) {
            // Legacy effects have only a pathname. It cannot prove that the
            // current delegated secret authorized the original upload.
            Fail(Ydb::StatusIds::PRECONDITION_FAILED, "External effect has no persisted IAM secret identity");
            return;
        }
        if (reference.TabletId && description.UsableUntil.size() != 1) {
            Fail(Ydb::StatusIds::UNAVAILABLE, "Missing delegated token expiry");
            return;
        }
        Values[index] = description.SecretValues.front();
        References[index] = reference;
        if (!description.UsableUntil.empty()) {
            UsableUntil[index] = description.UsableUntil.front();
        }
        Received[index] = true;
        if (!--Remaining) {
            Complete();
        }
    }

    void Complete() {
        for (size_t i = 0; i < References.size(); ++i) {
            if (References[i].TabletId && NActors::TActivationContext::Now() >= UsableUntil[i]) {
                Fail(Ydb::StatusIds::UNAVAILABLE, "Delegated token expired during external effect preparation");
                return;
            }
        }
        Send(ReplyTo, new TEvDescribeSecretsResponse(TDescription(
            std::move(Values), std::move(References), std::move(UsableUntil))));
        PassAway();
    }

    void Timeout() { Fail(Ydb::StatusIds::UNAVAILABLE, "External effect secret resolution timed out"); }

    void Fail(Ydb::StatusIds::StatusCode status, const TString& issue) {
        Send(ReplyTo, new TEvDescribeSecretsResponse(TDescription(status, {NYql::TIssue(issue)})));
        PassAway();
    }

    const TActorId ReplyTo;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const TString Database;
    const std::vector<TString> SecretNames;
    const std::vector<NKqpProto::TKqpExternalSink> Sinks;
    TScriptSecretReferences Expected;
    std::vector<TString> Values;
    std::vector<TEvDescribeSecretsResponse::TSecretReference> References;
    std::vector<TInstant> UsableUntil;
    std::vector<bool> Received;
    size_t Remaining = 0;
};
} // namespace

IActor* CreateScriptSecretsResolverActor(const TActorId& replyTo,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, TString database,
    std::vector<TString> secretNames, std::vector<NKqpProto::TKqpExternalSink> sinks)
{
    return new TScriptSecretsResolver(replyTo, std::move(userToken), std::move(database),
        std::move(secretNames), std::move(sinks));
}

} // namespace NKikimr::NKqp
