#pragma once

#include <ydb/core/base/feature_flags.h>
#include <ydb/core/kqp/common/events/script_executions.h>

#include <ydb/library/aclib/aclib.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/async/async.h>

#include <library/cpp/retry/retry_policy.h>
#include <library/cpp/threading/future/future.h>

#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <optional>

namespace NKikimr::NSecret {

struct TDescribeSecretSettings {
    IRetryPolicy<>::TPtr RetryPolicy;
    std::optional<NKqp::TEvDescribeSecretsResponse::TSecretReference> ExpectedReference;
};

IRetryPolicy<>::TPtr MakeShortRetryPolicy();
IRetryPolicy<>::TPtr MakeLongRetryPolicy();

NThreading::TFuture<NKqp::TEvDescribeSecretsResponse::TDescription> DescribeSecret(
    const TVector<TString>& secretNames,
    const TIntrusiveConstPtr<NACLib::TUserToken> userToken,
    const TString& database,
    NActors::TActorSystem* actorSystem,
    TDescribeSecretSettings settings = {}
);

// Owned acquisition for actor callers. Cancellation releases its resolver request
// and delegated readers while leaving account-scoped token producers alive.
NActors::async<NKqp::TEvDescribeSecretsResponse::TDescription> DescribeSecretAsync(
    TVector<TString> secretNames, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
    TString database, TDescribeSecretSettings settings = {});

// A future adapter for consumers already holding a schema-secret identity.
NThreading::TFuture<NKqp::TEvDescribeSecretsResponse::TDescription> DescribeSchemaSecret(
    TVector<TString> secretNames, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
    TString database, NActors::TActorSystem* actorSystem, TDescribeSecretSettings settings = {});

bool IsSchemeSecret(const TString& secretName);

bool UseSchemaSecrets(const NKikimr::TFeatureFlags& flags, const TVector<TString>& secretNames);
bool UseSchemaSecrets(const NKikimr::TFeatureFlags& flags, const TString& secretName);

}  // namespace NKikimr::NSecret
