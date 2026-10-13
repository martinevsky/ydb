#pragma once

#include "settings.h"

#include <ydb/core/base/iam_delegation.h>

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/actorid.h>

namespace NKikimr::NIamDelegation {

// Node-local service that sets up and revokes IAM delegations (ServiceControlService.SetupDelegation /
// RevokeDelegation) on behalf of YDB and waits for the resulting operations.
// Legacy requests: TEvIamDelegation::TEvSetupDelegation / TEvRevokeDelegation.
// Durable owners use TEvSubmitDelegationOperation (one mutation, immediate operation ID) and
// TEvPollDelegationOperation (one Get, no mutation). Both reply TEvDelegationOperationResult.
// All replies carry the request cookie. Unknown outcomes are not terminal operation failures.
// The actor subscribes once to Config.SystemTokenName in the node token manager.
// Setup/revoke use bounded attempts; OperationPollTimeout starts after their pending operation
// reply and bounds all subsequent credential waits, poll calls and retry delays. A local timeout
// does not cancel an IAM operation or a gRPC request already sent.
NActors::IActor* CreateIamDelegationService(const TIamDelegationSettings& settings);

} // namespace NKikimr::NIamDelegation
