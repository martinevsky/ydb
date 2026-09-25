#pragma once

#include <ydb/core/kqp/provider/yql_kikimr_gateway.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/actors/core/actor.h>
#include <library/cpp/threading/future/future.h>

namespace NKikimr::NKqp {

// Returns the Yandex Cloud IAM subject id of the user (the SID without the "@<accessServiceDomain>" suffix,
// see AuthConfig.AccessServiceDomain) or nothing when the user is not a cloud subject (builtin, local or system users).
TMaybe<TString> ExtractCloudSubjectId(const NACLib::TUserToken& userToken, TStringBuf accessServiceDomain);

// A fresh IAM referrer id of a delegation: "ydb.delegation." followed by 32 hex digits of a random GUID,
// recognizable as YDB's on the IAM side and under the 50-character limit IAM enforces on referrer.id.
TString NewReferrerId();

struct TIamDelegationSecretOperation {
    THolder<TEvTxUserProxy::TEvProposeTransaction> Request; // prepared scheme request (database, user token, modify scheme)
    TString Database;
    TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    NThreading::TPromise<NYql::IKikimrGateway::TGenericResult> Promise;
    bool FailedOnAlreadyExists = false;
    bool SuccessOnNotExist = false;

    // Timeouts of the statement. The scheme executer keeps the defaults; unit tests shorten them.
    struct TTimeouts {
        TDuration Navigate = TDuration::Seconds(30);    // one scheme lookup
        TDuration Delegation = TDuration::Minutes(5);   // one delegation call (with its retries and the polling of the operation) or one cloud lookup
    };
    TTimeouts Timeouts;
};

// The orchestrators keep an invariant the schemeshard relies on: a secret names every delegation IAM may know
// before it is set up, and stops naming it only through the schemeshard, which then revokes it (its outbox of
// revocations, ydb/core/tx/schemeshard/schemeshard_iam_delegation.h). So the schema operation, which also
// checks the user's rights, always precedes the IAM call.

// CREATE SECRET of type IAM_DELEGATION: creates the secret naming a fresh delegation, then sets the delegation
// up on behalf of the user; when IAM refuses, the secret is dropped again. CREATE OR REPLACE over an existing
// delegation secret behaves like ALTER.
NActors::IActor* CreateIamDelegationSecretCreator(TIamDelegationSecretOperation op);

// ALTER SECRET of type IAM_DELEGATION: stages the new delegation in the secret, sets it up, then promotes it
// (the old one is revoked by the schemeshard) or, when IAM refuses, cancels it.
NActors::IActor* CreateIamDelegationSecretAlterer(TIamDelegationSecretOperation op);

// DROP SECRET needs no orchestration: the schemeshard revokes the delegations of a dropped secret itself.

} // namespace NKikimr::NKqp
