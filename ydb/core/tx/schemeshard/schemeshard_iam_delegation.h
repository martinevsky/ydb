#pragma once

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/library/actors/core/actor.h>

#include <util/generic/string.h>

namespace NKikimr::NSchemeShard {

// An IAM delegation the schemeshard has to revoke: the secret that named it no longer does (the secret was
// dropped, or the delegation was a replacement that was promoted or cancelled by ALTER SECRET). The record
// is written in the transaction that makes the change and lives in the local database until IAM accepts
// the revocation, so that a restart of the tablet or a failure of IAM never leaves a delegation nobody
// names: the outbox of delegation revocations.
struct TIamDelegationRevocation {
    TString ReferrerId;
    TString ServiceAccountId;
    TString CloudId;
    TPathId PathId; // the secret that named the delegation, for logs

    TString ToString() const {
        return TStringBuilder() << "{referrer: " << ReferrerId << ", sa: " << ServiceAccountId << ", cloud: " << CloudId << ", secret: " << PathId << "}";
    }
};

// The delegations a secret names: the one its readers use and the replacement staged by ALTER SECRET, if any.
TVector<NKikimrSchemeOp::TIamDelegation> NamedIamDelegations(const NKikimrSchemeOp::TSecretDescription& secret);

// Revokes one delegation through the IAM delegation service of the node (ydb/core/security/iam_delegation)
// and answers the schemeshard with TEvPrivate::TEvIamDelegationRevoked. It retries until the service accepts
// the revocation (a delegation IAM does not know counts as revoked) or the schemeshard poisons it.
NActors::IActor* CreateIamDelegationRevoker(const NActors::TActorId& schemeShard, const TIamDelegationRevocation& revocation);

} // namespace NKikimr::NSchemeShard
