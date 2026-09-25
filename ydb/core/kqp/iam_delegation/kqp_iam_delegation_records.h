#pragma once

#include <ydb/core/security/iam_delegation/events.h>

#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/actorid.h>

#include <vector>

namespace NKikimr::NKqp {

// Durable records of the delegations whose outcome is not final yet. A record is written before any IAM call
// that sets up or revokes a delegation and removed once the outcome is final: the schema change committed with
// that referrer, or IAM accepted the revocation. The reconciliation service revokes the recorded delegations
// that no secret names once their lease has passed, so that a crash or a failed revocation never leaves a
// delegation YDB does not know about.
//
// The records live in a table of the node's database (.metadata/iam_delegation/delegations), one row per
// (database, referrer). The coroutines below are nested coroutines of the calling actor; they throw
// yexception when the table cannot be written or read.

// How long an operation that wrote a record is considered in progress: the reconciliation leaves the record
// alone until then.
constexpr TDuration DelegationRecordLease = TDuration::Minutes(30);

NActors::async<void> EnsureDelegationRecordsTable();
NActors::async<void> WriteDelegationRecord(NIamDelegation::TEvIamDelegation::TDelegationRecord record);
NActors::async<void> RemoveDelegationRecord(TString database, TString referrerId);
NActors::async<std::vector<NIamDelegation::TEvIamDelegation::TDelegationRecord>> ListExpiredDelegationRecords(TInstant now, ui32 limit);

// Node-local service that periodically revokes the recorded delegations that no secret names, through the
// delegation service (NIamDelegation::MakeIamDelegationServiceId()), and removes the records whose outcome is final.
NActors::IActor* CreateIamDelegationReconciliationService();

inline NActors::TActorId MakeIamDelegationReconciliationServiceId(ui32 nodeId = 0) {
    return NActors::TActorId(nodeId, "iam_dlg_rec");
}

} // namespace NKikimr::NKqp
