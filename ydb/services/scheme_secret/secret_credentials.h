#pragma once

#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/credentials/credentials.h>

#include <util/datetime/base.h>
#include <util/generic/string.h>

#include <memory>

namespace NKikimr::NSecret {

// Fields the executer adds to the structured token of a task next to the resolved token: the path of the
// secret the token came from and the database of the query. Credentials built from such a token do not keep
// the value: every use takes the current one from the node's secret cache (with no user token — the rights
// were checked when the execution started), so a long-running execution never uses a stale value of a
// rotating secret. Reads are local and at most one of them is in flight per credentials object.
// Readers of the secret see nothing but a credentials provider.
constexpr TStringBuf SecretReferenceField = "secret_ref";
constexpr TStringBuf SecretDatabaseField = "secret_database";

// Adds the fields to a structured token whose token reference was just resolved into a token.
// The executer adds them only for secrets whose value changes over time (IAM delegations); for other
// references the token is returned unchanged.
TString KeepTokenSecretReference(const TString& structuredTokenJson, const TString& tokenReference, const TString& database);

// Sensors: schema_secrets/component=refreshing_credentials/{ProvidersCreated, ReReads, ReReadErrors}.
// A credentials provider factory over the secret at secretPath: providers start with initialToken and
// take the current value of the secret on every use.
enum class EBearerPrefix {
    None, // the value of the secret is handed out as is
    Add,  // "Bearer " is prepended, as IStructuredTokenCredentialsFactory::Create does when asked
};

std::shared_ptr<NYdb::ICredentialsProviderFactory> CreateRefreshingSecretCredentialsProviderFactory(
    NActors::TActorSystem* actorSystem, const TString& secretPath, const TString& database,
    const TString& initialToken, EBearerPrefix bearerPrefix);

// Wraps a structured token credentials factory: structured tokens that carry SecretReferenceField get a
// refreshing provider (when created from an actor), everything else goes to the wrapped factory.
NYql::IStructuredTokenCredentialsFactory::TPtr CreateRefreshingSecretCredentialsFactoryOverFactory(
    NYql::IStructuredTokenCredentialsFactory::TPtr factory);

} // namespace NKikimr::NSecret
