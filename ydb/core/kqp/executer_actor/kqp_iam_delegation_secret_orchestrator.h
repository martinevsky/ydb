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

// CREATE SECRET of type IAM_DELEGATION: sets up the delegation on behalf of the user, then creates the secret
// (a failed schema operation revokes the delegation again). CREATE OR REPLACE over an existing delegation
// secret behaves like ALTER.
NActors::IActor* CreateIamDelegationSecretCreator(TIamDelegationSecretOperation op);

// ALTER SECRET of type IAM_DELEGATION: sets up the new delegation, alters the secret, then revokes the old one.
NActors::IActor* CreateIamDelegationSecretAlterer(TIamDelegationSecretOperation op);

// DROP SECRET: drops the secret and, when it was a delegation secret, revokes its delegation.
NActors::IActor* CreateIamDelegationSecretDropper(TIamDelegationSecretOperation op);

} // namespace NKikimr::NKqp
