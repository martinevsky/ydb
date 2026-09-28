#pragma once

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/library/actors/core/actor.h>

#include <util/generic/string.h>

namespace NKikimr::NSchemeShard {

// An IAM delegation the schemeshard has to revoke: the secret that named it no longer does (the secret was
// dropped, alone or with its directory or subdomain, or the delegation was a replacement that was promoted
// or cancelled by ALTER SECRET). The record is written in the transaction that makes the change and lives
// in the local database until IAM accepts the revocation, so that a restart of the tablet or a failure of
// IAM never leaves a delegation nobody names: the outbox of delegation revocations.
//
// The one exception is the drop of a database (an extsubdomain): its schemeshard is deleted with the
// database, together with the secrets and any revocation still in its outbox. Those delegations stay in
// IAM; the cloud revokes them with the other resources of the database.
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

// How long a staged replacement is considered to be set up by the ALTER SECRET that staged it. A second ALTER
// may replace a staged delegation only after that: a replacement whose setup could still be in flight must
// not be revoked, or IAM would create it after the revocation and nothing would name it. Twice the time the
// orchestrator gives one delegation call (5 minutes, ydb/core/kqp/executer_actor).
constexpr TDuration StagedIamDelegationLease = TDuration::Minutes(10);

// Revokes one delegation through the IAM delegation service of the node (ydb/core/security/iam_delegation)
// and answers the schemeshard with TEvPrivate::TEvIamDelegationRevoked. It retries until the service accepts
// the revocation (a delegation IAM does not know counts as revoked) or the schemeshard poisons it.
NActors::IActor* CreateIamDelegationRevoker(const NActors::TActorId& schemeShard, const TIamDelegationRevocation& revocation);

} // namespace NKikimr::NSchemeShard
