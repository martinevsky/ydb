#pragma once

#include <ydb/core/base/iam_delegation.h>
#include "settings.h"

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/actorid.h>

namespace NKikimr::NIamDelegation {

// Node-local cache of IAM tokens of delegated service accounts (IamTokenService.CreateForService
// authorized with the system service account). A token is minted on the first request for its key and
// refreshed ahead of expiry for as long as the key is in use; TEvGetToken always answers with the
// current one (or the error of the last attempt). Keys nobody asks for are dropped after IdleKeyTtl.
// The system token comes from the configured node token-manager subscription.
NActors::IActor* CreateIamDelegatedTokenService(const TIamDelegationSettings& settings);

} // namespace NKikimr::NIamDelegation
