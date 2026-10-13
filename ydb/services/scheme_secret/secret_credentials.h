#pragma once

#include "resolver.h"
#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/credentials/credentials.h>

namespace NKikimr::NSecret {

// Trusted query-task metadata: immutable secret identity and ACL subject only.
// The original authentication bearer is never copied into this metadata.
TString KeepTokenSecretReference(const TString& structuredTokenJson, const TString& secretPath,
    const TString& database, const NKqp::TEvDescribeSecretsResponse::TSecretReference& reference,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken);

enum class EBearerPrefix { None, Add };

std::shared_ptr<NYdb::ICredentialsProviderFactory> CreateRefreshingSecretCredentialsProviderFactory(
    NActors::TActorSystem* actorSystem, const TString& secretPath, const TString& database,
    const NKqp::TEvDescribeSecretsResponse::TSecretReference& reference,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, EBearerPrefix bearerPrefix);

NYql::IStructuredTokenCredentialsFactory::TPtr CreateRefreshingSecretCredentialsFactoryOverFactory(
    NYql::IStructuredTokenCredentialsFactory::TPtr factory);

} // namespace NKikimr::NSecret
