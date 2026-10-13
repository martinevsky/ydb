#pragma once

#include <ydb/library/actors/core/actor.h>

namespace NKikimr::NIamDelegation {

// Deploy on service-owned nodes together with IAM transport; the tablet and its
// recovery workers must outlive every tenant whose bindings they retain.
NActors::IActor* CreateIamDelegationRecoveryWorker(ui64 tabletId);

inline NActors::TActorId MakeIamDelegationRecoveryWorkerId(ui32 nodeId = 0) {
    return NActors::TActorId(nodeId, "iam_dlg_work");
}

} // namespace NKikimr::NIamDelegation
