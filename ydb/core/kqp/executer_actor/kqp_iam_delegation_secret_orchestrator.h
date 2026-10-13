#pragma once

#include <ydb/core/kqp/provider/yql_kikimr_gateway.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/library/aclib/aclib.h>
#include <library/cpp/threading/future/future.h>

namespace NKikimr::NKqp {

TMaybe<TString> ExtractCloudSubjectId(const NACLib::TUserToken& userToken, TStringBuf accessServiceDomain);
TString NewReferrerId();

struct TIamDelegationSecretOperation {
    THolder<TEvTxUserProxy::TEvProposeTransaction> Request;
    TString Database;
    TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    NThreading::TPromise<NYql::IKikimrGateway::TGenericResult> Promise;
    bool FailedOnAlreadyExists = false;
    bool SuccessOnNotExist = false;

    struct TTimeouts {
        static constexpr TDuration Navigate = TDuration::Seconds(30);
        static constexpr TDuration Delegation = TDuration::Minutes(5);
        static constexpr TDuration CloudLookup = TDuration::Minutes(5);
    };
    TTimeouts Timeouts;
};

// Schema stores an immutable locator. All mutable lifecycle state and cleanup
// obligations are committed in the independent tablet before contacting IAM.
NActors::IActor* CreateIamDelegationSecretCreator(TIamDelegationSecretOperation op);
NActors::IActor* CreateIamDelegationSecretAlterer(TIamDelegationSecretOperation op);

} // namespace NKikimr::NKqp
