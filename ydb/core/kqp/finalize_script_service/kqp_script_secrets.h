#pragma once

#include <ydb/core/kqp/common/events/script_executions.h>
#include <ydb/library/actors/core/actor.h>

#include <optional>

namespace NKikimr::NKqp {

// Recovered effects must resolve the same secret incarnation that authorized
// their execution. The result keeps the input secret order for sink substitution.
IActor* CreateScriptSecretsResolverActor(const TActorId& replyTo,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, TString database,
    std::vector<TString> secretNames, std::vector<NKqpProto::TKqpExternalSink> sinks,
    std::optional<TInstant> deadline = std::nullopt);

} // namespace NKikimr::NKqp
