#pragma once

#include "resolver.h"
#include <ydb/library/actors/core/event_local.h>

namespace NKikimr::NSecret {

struct TEvIamDelegationSecretReadResult : NActors::TEventLocal<TEvIamDelegationSecretReadResult,
    EventSpaceBegin(TKikimrEvents::ES_PRIVATE) + 0x6900> {
    TString SecretPath;
    NKqp::TEvDescribeSecretsResponse::TSecretReference Reference;
    Ydb::StatusIds::StatusCode Status = Ydb::StatusIds::SUCCESS;
    NYql::TIssues Issues;
    TString Token;
    TInstant UsableUntil;
};

NActors::IActor* CreateIamDelegationSecretReaderActor(const NActors::TActorId& replyTo, ui64 cookie,
    TString secretPath, TString database, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
    NKqp::TEvDescribeSecretsResponse::TSecretReference reference);

} // namespace NKikimr::NSecret
