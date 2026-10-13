#pragma once

#include <ydb/core/kqp/common/events/script_executions.h>
#include <yql/essentials/providers/common/structured_token/yql_structured_token.h>
#include <yql/essentials/providers/common/structured_token/yql_token_builder.h>

#include <util/generic/hash.h>
#include <util/string/cast.h>

namespace NKikimr::NKqp {

using TScriptSecretReferences = THashMap<TString, TEvDescribeSecretsResponse::TSecretReference>;

struct TScriptExternalEffectAuth {
    TString Token;
    TString SecretPath;
};

inline TScriptExternalEffectAuth ResolveScriptExternalEffectAuth(const TString& authInfo,
    const std::map<TString, TString>& secrets)
{
    // Empty authorization and legacy opaque tokens have no references to
    // substitute. ParseStructuredToken aborts for either representation.
    if (!NYql::IsStructuredTokenJson(authInfo)) {
        return {authInfo, {}};
    }
    return {NYql::CreateStructuredTokenParser(authInfo).ToBuilder().ReplaceReferences(secrets).ToJson(),
        NYql::ParseStructuredToken(authInfo).GetFieldOrDefault("token_ref", "")};
}

// These fields travel in the already-persisted sink proto. Keep the original
// token_ref: neither the delegated token nor query bearer belongs in this pin.
inline void PinScriptExternalEffectSecrets(std::vector<NKqpProto::TKqpExternalSink>& sinks,
    const TString& database, const TScriptSecretReferences& references)
{
    for (auto& sink : sinks) {
        if (!NYql::IsStructuredTokenJson(sink.GetAuthInfo())) {
            continue;
        }
        auto token = NYql::ParseStructuredToken(sink.GetAuthInfo());
        const auto path = token.GetFieldOrDefault("token_ref", "");
        const auto it = references.find(path);
        if (it == references.end()) {
            continue;
        }
        const auto& reference = it->second;
        Y_ENSURE(reference.TabletId && reference.PathOwnerId && reference.PathLocalId
            && !reference.DatabaseIncarnation.empty() && !reference.CreateOperationId.empty(),
            "Incomplete external effect secret identity");
        token.SetField("script_secret_path", path)
            .SetField("script_secret_database", database)
            .SetField("script_secret_tablet", ToString(reference.TabletId))
            .SetField("script_secret_incarnation", reference.DatabaseIncarnation)
            .SetField("script_secret_create_operation", reference.CreateOperationId)
            .SetField("script_secret_owner", ToString(reference.PathOwnerId))
            .SetField("script_secret_local", ToString(reference.PathLocalId));
        sink.SetAuthInfo(token.ToJson());
    }
}

inline TScriptSecretReferences ExtractScriptExternalEffectSecretPins(
    const std::vector<NKqpProto::TKqpExternalSink>& sinks, const TString& database)
{
    TScriptSecretReferences references;
    for (const auto& sink : sinks) {
        if (!NYql::IsStructuredTokenJson(sink.GetAuthInfo())) {
            continue;
        }
        const auto token = NYql::ParseStructuredToken(sink.GetAuthInfo());
        if (!token.HasField("script_secret_path")) {
            continue;
        }
        const auto path = token.GetField("script_secret_path");
        Y_ENSURE(!path.empty() && path == token.GetField("token_ref")
            && database == token.GetField("script_secret_database"),
            "External effect secret path or database changed");
        TEvDescribeSecretsResponse::TSecretReference reference{
            FromString<ui64>(token.GetField("script_secret_tablet")),
            token.GetField("script_secret_incarnation"), token.GetField("script_secret_create_operation"),
            FromString<ui64>(token.GetField("script_secret_owner")),
            FromString<ui64>(token.GetField("script_secret_local"))};
        Y_ENSURE(reference.TabletId && reference.PathOwnerId && reference.PathLocalId
            && !reference.DatabaseIncarnation.empty() && !reference.CreateOperationId.empty(),
            "Incomplete persisted external effect secret identity");
        const auto [it, inserted] = references.emplace(path, reference);
        Y_ENSURE(inserted || it->second == reference, "Conflicting external effect secret identities");
    }
    return references;
}

} // namespace NKikimr::NKqp
