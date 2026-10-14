#pragma once

#include <ydb/core/security/iam_delegation/tablet/protos/iam_delegation.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/library/actors/async/async.h>

namespace NKikimr::NIamDelegation {

// The same saved transaction and fences are used by the statement and recovery.
THolder<TEvTxUserProxy::TEvProposeTransaction> MakeIamDelegationSchemaCreate(
    const NKikimrIamDelegation::TDelegation& delegation, ui64 tabletId);

NActors::IActor* CreateIamDelegationSchemaProxy(ui64 txId,
    THolder<TEvTxUserProxy::TEvAllocateTxIdResult> allocation);

enum class ESchemaCreateState { Unknown, Replay, Creating, Created, Retire };

struct TSchemaCreateObservation {
    ESchemaCreateState State = ESchemaCreateState::Unknown;
    NKikimrScheme::TEvDescribeSchemeResult Schema;
};

// Parent FIRST, child SECOND: a consumed parent fence prevents any delayed copy
// from creating after this observation. A matching in-flight shell is preserved.
NActors::async<TSchemaCreateObservation> ObserveIamDelegationSchemaCreate(
    const NKikimrIamDelegation::TDelegation& delegation, ui64 tabletId);

} // namespace NKikimr::NIamDelegation
