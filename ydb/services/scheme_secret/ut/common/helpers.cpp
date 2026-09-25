#include "helpers.h"

#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/common/events/script_executions.h>
#include <ydb/services/scheme_secret/service.h>
#include <ydb/core/base/path.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

#include <util/generic/algorithm.h>

namespace NKikimr::NSecret {
    using TDescriptionPromise = NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription>;

    namespace {
        // alter: what an AlterSecret does with the delegation (STAGE names it as the pending replacement, PROMOTE makes it current)
        void ProposeIamDelegationSecret(NActors::TTestActorRuntime& runtime, const TString& path, NKikimrSchemeOp::EOperationType operationType,
            const TString& serviceAccountId, const TString& cloudId, const TString& referrerId,
            NKikimrSchemeOp::EIamDelegationAlter alter = NKikimrSchemeOp::IAM_DELEGATION_ALTER_NONE)
        {
            const auto parts = SplitPath(path);
            UNIT_ASSERT_C(parts.size() >= 2, path);
            const TString name = parts.back();
            const TString workingDir = CanonizePath(TVector<TString>(parts.begin(), parts.end() - 1));

            auto request = std::make_unique<TEvTxUserProxy::TEvProposeTransaction>();
            request->Record.SetDatabaseName(CanonizePath(parts.front()));
            auto& schemeTx = *request->Record.MutableTransaction()->MutableModifyScheme();
            schemeTx.SetWorkingDir(workingDir);
            schemeTx.SetOperationType(operationType);
            auto& secret = operationType == NKikimrSchemeOp::ESchemeOpCreateSecret ? *schemeTx.MutableCreateSecret() : *schemeTx.MutableAlterSecret();
            secret.SetName(name);
            secret.SetType(NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION);
            auto& delegation = *secret.MutableIamDelegation();
            delegation.SetServiceAccountId(serviceAccountId);
            delegation.SetCloudId(cloudId);
            delegation.SetReferrerId(referrerId);
            if (operationType == NKikimrSchemeOp::ESchemeOpAlterSecret) {
                secret.SetIamDelegationAlter(alter);
            }
            const bool staged = alter == NKikimrSchemeOp::IAM_DELEGATION_ALTER_STAGE;

            const TActorId sender = runtime.AllocateEdgeActor();
            runtime.Send(new IEventHandle(MakeTxProxyID(), sender, request.release()));
            const auto status = runtime.GrabEdgeEventRethrow<TEvTxUserProxy::TEvProposeTransactionStatus>(sender);
            UNIT_ASSERT(status);
            const auto statusCode = static_cast<TEvTxUserProxy::TEvProposeTransactionStatus::EStatus>(status->Get()->Record.GetStatus());
            UNIT_ASSERT_C(statusCode == TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecComplete
                || statusCode == TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecInProgress,
                "unexpected status " << status->Get()->Record.ShortDebugString());

            // hang guard: wait until the scheme cache serves the requested description (inevitable after the accepted proposal)
            const TInstant deadline = TInstant::Now() + TDuration::Seconds(120);
            for (;;) {
                const auto navigate = NKqp::Navigate(runtime, sender, path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
                const auto& entry = navigate->ResultSet.at(0);
                if (entry.Status == NSchemeCache::TSchemeCacheNavigate::EStatus::Ok
                    && entry.Kind == NSchemeCache::TSchemeCacheNavigate::EKind::KindSecret
                    && entry.SecretInfo)
                {
                    const auto& described = staged
                        ? entry.SecretInfo->Description.GetPendingIamDelegation()
                        : entry.SecretInfo->Description.GetIamDelegation();
                    if (described.GetServiceAccountId() == serviceAccountId
                        && described.GetCloudId() == cloudId
                        && described.GetReferrerId() == referrerId)
                    {
                        return;
                    }
                }
                UNIT_ASSERT_C(TInstant::Now() < deadline, "secret " << path << " was not updated in time, status " << static_cast<int>(entry.Status));
                Sleep(TDuration::MilliSeconds(100));
            }
        }
    }

    namespace {
        class TFakeIamDelegatedTokenService : public NActors::TActorBootstrapped<TFakeIamDelegatedTokenService> {
        public:
            explicit TFakeIamDelegatedTokenService(TIntrusivePtr<TFakeDelegatedTokens> tokens)
                : Tokens(std::move(tokens))
            {}

            void Bootstrap() {
                Become(&TThis::StateWork);
            }

            STRICT_STFUNC(StateWork,
                hFunc(NIamDelegation::TEvIamDelegation::TEvGetToken, Handle);
                cFunc(NActors::TEvents::TEvWakeup::EventType, ReleaseHeld);
                cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
            )

        private:
            void Handle(NIamDelegation::TEvIamDelegation::TEvGetToken::TPtr& ev) {
                auto result = MakeHolder<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>();
                result->Key = ev->Get()->Key;
                ++Tokens->Calls;
                if (Tokens->UnavailableAnswers > 0) {
                    --Tokens->UnavailableAnswers;
                    result->Status = Ydb::StatusIds::UNAVAILABLE;
                    result->Issues.AddIssue("IAM is unavailable (fake)");
                } else if (const auto it = Tokens->Tokens.find(result->Key.ToString()); it != Tokens->Tokens.end()) {
                    with_lock (Tokens->Mutex) {
                        result->Token = Tokens->UniqueTokens ? TStringBuilder() << it->second << "-" << Tokens->Minted.size() + 1 : it->second;
                        Tokens->Minted.push_back(result->Token);
                    }
                    result->ExpiresAt = TInstant::Now() + TDuration::Hours(1);
                } else {
                    result->Status = Ydb::StatusIds::UNAUTHORIZED;
                    result->Issues.AddIssue(TStringBuilder() << "No delegation for service account " << result->Key.ServiceAccountId);
                }
                if (Tokens->HoldReplies.load()) {
                    Held.emplace_back(ev->Sender, ev->Cookie, std::move(result));
                    return;
                }
                Send(ev->Sender, result.Release(), 0, ev->Cookie);
            }

            void ReleaseHeld() {
                auto held = std::move(Held);
                Held.clear();
                for (auto& [sender, cookie, result] : held) {
                    Send(sender, result.Release(), 0, cookie);
                }
            }

            const TIntrusivePtr<TFakeDelegatedTokens> Tokens;
            TVector<std::tuple<NActors::TActorId, ui64, THolder<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>>> Held;
        };
    }

    TVector<TString> TFakeDelegatedTokens::MintedTokens() {
        with_lock (Mutex) {
            return Minted;
        }
    }

    namespace {
        class TFakeIamDelegationService : public NActors::TActorBootstrapped<TFakeIamDelegationService> {
        public:
            explicit TFakeIamDelegationService(TIntrusivePtr<TFakeDelegationCalls> calls)
                : Calls(std::move(calls))
            {}

            void Bootstrap() {
                Become(&TThis::StateWork);
            }

            STRICT_STFUNC(StateWork,
                hFunc(NIamDelegation::TEvIamDelegation::TEvSetupDelegation, HandleSetup);
                hFunc(NIamDelegation::TEvIamDelegation::TEvRevokeDelegation, HandleRevoke);
                cFunc(NActors::TEvents::TEvWakeup::EventType, ReleaseHeld);
                cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
            )

        private:
            struct THeld {
                NActors::TActorId Sender;
                ui64 Cookie;
                bool Revoke;
                Ydb::StatusIds::StatusCode Status;
            };

            void HandleSetup(NIamDelegation::TEvIamDelegation::TEvSetupDelegation::TPtr& ev) {
                TFakeDelegationCalls::TCall call{.Method = "Setup", .Spec = ev->Get()->Spec, .SubjectId = ev->Get()->SubjectId};
                bool hold;
                Ydb::StatusIds::StatusCode status;
                with_lock (Calls->Mutex) {
                    Calls->Calls.push_back(std::move(call));
                    hold = Calls->HoldSetups;
                    status = Calls->SetupStatus;
                }
                Answer({.Sender = ev->Sender, .Cookie = ev->Cookie, .Revoke = false, .Status = status}, hold);
            }

            void HandleRevoke(NIamDelegation::TEvIamDelegation::TEvRevokeDelegation::TPtr& ev) {
                TFakeDelegationCalls::TCall call{.Method = "Revoke", .Spec = ev->Get()->Spec};
                bool hold;
                Ydb::StatusIds::StatusCode status;
                with_lock (Calls->Mutex) {
                    Calls->Calls.push_back(std::move(call));
                    hold = Calls->HoldRevokes;
                    status = Calls->RevokeStatus;
                }
                Answer({.Sender = ev->Sender, .Cookie = ev->Cookie, .Revoke = true, .Status = status}, hold);
            }

            void Answer(THeld reply, bool hold) {
                if (hold) {
                    Held.push_back(reply);
                    return;
                }
                auto result = reply.Status == Ydb::StatusIds::SUCCESS
                    ? NIamDelegation::TDelegationResult::Success()
                    : NIamDelegation::TDelegationResult::Error(reply.Status, TStringBuilder() << "injected " << (reply.Revoke ? "revoke" : "setup") << " failure (fake)");
                if (reply.Revoke) {
                    Send(reply.Sender, new NIamDelegation::TEvIamDelegation::TEvRevokeDelegationResult(std::move(result)), 0, reply.Cookie);
                } else {
                    Send(reply.Sender, new NIamDelegation::TEvIamDelegation::TEvSetupDelegationResult(std::move(result)), 0, reply.Cookie);
                }
            }

            void ReleaseHeld() {
                auto held = std::move(Held);
                Held.clear();
                for (const auto& reply : held) {
                    Answer(reply, false);
                }
            }

            const TIntrusivePtr<TFakeDelegationCalls> Calls;
            TVector<THeld> Held;
        };
    }

    ui32 TFakeDelegationCalls::SetupCalls() {
        with_lock (Mutex) {
            return CountIf(Calls, [](const TCall& c) { return c.Method == "Setup"; });
        }
    }

    ui32 TFakeDelegationCalls::RevokeCalls() {
        with_lock (Mutex) {
            return CountIf(Calls, [](const TCall& c) { return c.Method == "Revoke"; });
        }
    }

    TVector<TFakeDelegationCalls::TCall> TFakeDelegationCalls::Snapshot() {
        with_lock (Mutex) {
            return Calls;
        }
    }

    void TFakeDelegationCalls::WaitCalls(const TString& method, ui32 count, TDuration timeout) {
        const TInstant deadline = TInstant::Now() + timeout;
        for (;;) {
            const ui32 calls = method == "Setup" ? SetupCalls() : RevokeCalls();
            if (calls >= count) {
                return;
            }
            UNIT_ASSERT_C(TInstant::Now() < deadline, "only " << calls << " " << method << " calls were received in " << timeout);
            Sleep(TDuration::MilliSeconds(20));
        }
    }

    TIntrusivePtr<TFakeDelegationCalls> RegisterFakeIamDelegationService(NActors::TTestActorRuntime& runtime, ui32 nodeIndex,
        TIntrusivePtr<TFakeDelegationCalls> calls)
    {
        if (!calls) {
            calls = MakeIntrusive<TFakeDelegationCalls>();
        }
        const auto actorId = runtime.Register(new TFakeIamDelegationService(calls), nodeIndex);
        runtime.RegisterService(NIamDelegation::MakeIamDelegationServiceId(), actorId, nodeIndex);
        return calls;
    }

    void ReleaseHeldDelegationReplies(NActors::TTestActorRuntime& runtime, ui32 nodeIndex) {
        runtime.Send(new IEventHandle(NIamDelegation::MakeIamDelegationServiceId(), runtime.AllocateEdgeActor(nodeIndex), new NActors::TEvents::TEvWakeup()), nodeIndex);
    }

    TIntrusivePtr<TFakeDelegatedTokens> RegisterFakeIamDelegatedTokenService(NActors::TTestActorRuntime& runtime,
        const TVector<std::pair<TString, TString>>& delegations, const TString& prefix, ui32 nodeIndex, TIntrusivePtr<TFakeDelegatedTokens> tokens)
    {
        if (!tokens) {
            tokens = MakeIntrusive<TFakeDelegatedTokens>();
            for (const auto& [serviceAccountId, cloudId] : delegations) {
                const NIamDelegation::TTokenKey key{.ServiceAccountId = serviceAccountId, .CloudId = cloudId};
                tokens->Tokens[key.ToString()] = prefix + key.ToString();
            }
        }
        const auto actorId = runtime.Register(new TFakeIamDelegatedTokenService(tokens), nodeIndex);
        runtime.RegisterService(NIamDelegation::MakeIamDelegatedTokenServiceId(), actorId, nodeIndex);
        return tokens;
    }

    void ReleaseHeldDelegatedTokenReplies(NActors::TTestActorRuntime& runtime, ui32 nodeIndex) {
        runtime.Send(new IEventHandle(NIamDelegation::MakeIamDelegatedTokenServiceId(), runtime.AllocateEdgeActor(nodeIndex), new NActors::TEvents::TEvWakeup()), nodeIndex);
    }

    void CreateIamDelegationSecretDirect(NActors::TTestActorRuntime& runtime, const TString& path,
        const TString& serviceAccountId, const TString& cloudId, const TString& referrerId)
    {
        ProposeIamDelegationSecret(runtime, path, NKikimrSchemeOp::ESchemeOpCreateSecret, serviceAccountId, cloudId, referrerId);
    }

    void AlterIamDelegationSecretDirect(NActors::TTestActorRuntime& runtime, const TString& path,
        const TString& serviceAccountId, const TString& cloudId, const TString& referrerId)
    {
        // the way the orchestrator replaces a delegation: staged first, promoted once set up
        ProposeIamDelegationSecret(runtime, path, NKikimrSchemeOp::ESchemeOpAlterSecret, serviceAccountId, cloudId, referrerId,
            NKikimrSchemeOp::IAM_DELEGATION_ALTER_STAGE);
        ProposeIamDelegationSecret(runtime, path, NKikimrSchemeOp::ESchemeOpAlterSecret, serviceAccountId, cloudId, referrerId,
            NKikimrSchemeOp::IAM_DELEGATION_ALTER_PROMOTE);
    }

    void CreateSchemaSecret(const TString& secretName, const TString& secretValue, NYdb::NTable::TSession& session) {
        auto query = "CREATE SECRET `" + secretName + "` WITH (value = \"" + secretValue + "\");";
        auto queryResult = session.ExecuteSchemeQuery(query).GetValueSync();
        UNIT_ASSERT_C(queryResult.GetStatus() == NYdb::EStatus::SUCCESS, queryResult.GetIssues().ToString());
    }

    void AlterSchemaSecret(const TString& secretName, const TString& secretValue, NYdb::NTable::TSession& session) {
        auto query = "ALTER SECRET `" + secretName + "` WITH (value = \"" + secretValue + "\");";
        auto queryResult = session.ExecuteSchemeQuery(query).GetValueSync();
        UNIT_ASSERT_C(queryResult.GetStatus() == NYdb::EStatus::SUCCESS, queryResult.GetIssues().ToString());
    }

    void DropSchemaSecret(const TString& secretName, NYdb::NTable::TSession& session) {
        auto query = "DROP SECRET `" + secretName + "`;";
        auto queryResult = session.ExecuteSchemeQuery(query).GetValueSync();
        UNIT_ASSERT_C(queryResult.GetStatus() == NYdb::EStatus::SUCCESS, queryResult.GetIssues().ToString());
    }

    TDescriptionPromise
    ResolveSecrets(
        const TVector<TString>& secretNames,
        NKqp::TKikimrRunner& kikimr,
        const TIntrusiveConstPtr<NACLib::TUserToken> userToken,
        TDescribeSecretSettings settings)
    {
        auto promise = NThreading::NewPromise<NKqp::TEvDescribeSecretsResponse::TDescription>();
        const auto evResolveSecret = new TDescribeSchemaSecretsService::TEvResolveSecret(
            userToken, "/Root", secretNames, promise, std::move(settings));
        auto actorSystem = kikimr.GetTestServer().GetRuntime()->GetActorSystem(0);
        actorSystem->Send(MakeDescribeSchemaSecretServiceId(actorSystem->NodeId), evResolveSecret);
        return promise;
    }

    TDescriptionPromise
    ResolveSecret(
        const TString& secretName,
        NKqp::TKikimrRunner& kikimr,
        const TIntrusiveConstPtr<NACLib::TUserToken> userToken,
        TDescribeSecretSettings settings)
    {
        return ResolveSecrets(TVector<TString>{secretName}, kikimr, userToken, std::move(settings));
    }

    void AssertBadRequest(TDescriptionPromise promise, const TString& err, Ydb::StatusIds::StatusCode status) {
        const auto& result = promise.GetFuture().GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL(status, result.Status);
        UNIT_ASSERT_VALUES_EQUAL(err, result.Issues.ToString());
    }

    void AssertErrorContains(TDescriptionPromise promise, const TString& errPart, Ydb::StatusIds::StatusCode status) {
        const auto& result = promise.GetFuture().GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(status, result.Status, result.Issues.ToString());
        UNIT_ASSERT_STRING_CONTAINS(result.Issues.ToString(), errPart);
    }

    TIntrusiveConstPtr<NACLib::TUserToken> GetUserToken(const TString& userSid, const TVector<TString>& groupSids) {
        if (userSid.empty() && groupSids.empty()) {
            return nullptr;
        }
        return new NACLib::TUserToken(userSid, groupSids);
    }

    void AssertSecretValues(const TVector<TString>& secretValues, TDescriptionPromise promise) {
        const auto& result = promise.GetFuture().GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(secretValues.size(), result.SecretValues.size(), result.Issues.ToOneLineString());
        UNIT_ASSERT_VALUES_EQUAL(secretValues, result.SecretValues);
    }

    void AssertSecretValue(const TString& secretValue, TDescriptionPromise promise) {
        AssertSecretValues(TVector<TString>{secretValue}, promise);
    }

    TTestDescribeSchemaSecretsServiceFactory::TTestDescribeSchemaSecretsServiceFactory(
        TDescribeSchemaSecretsService::ISecretUpdateListener* secretUpdateListener,
        TDescribeSchemaSecretsService::ISchemeCacheStatusGetter* schemeCacheStatusGetter,
        TDescribeSchemaSecretsService::ISchemeShardStatusGetter* schemeShardStatusGetter
    )
        : SecretUpdateListener(secretUpdateListener)
        , SchemeCacheStatusGetter(schemeCacheStatusGetter)
        , SchemeShardStatusGetter(schemeShardStatusGetter)
    {
    }

    NActors::IActor* TTestDescribeSchemaSecretsServiceFactory::CreateService() {
        auto* service = new TDescribeSchemaSecretsService();
        service->SetSecretUpdateListener(SecretUpdateListener);
        service->SetSchemeCacheStatusGetter(SchemeCacheStatusGetter);
        service->SetSchemeShardStatusGetter(SchemeShardStatusGetter);
        return service;
    }

    TTestSchemeCacheStatusGetter::TTestSchemeCacheStatusGetter(EFailProbability failProbability)
        : FailProbability(failProbability)
    {
    }

    NSchemeCache::TSchemeCacheNavigate::EStatus TTestSchemeCacheStatusGetter::GetStatus(
        NSchemeCache::TSchemeCacheNavigate::TEntry& entry) const
    {
        switch (FailProbability) {
            case EFailProbability::None:
                return entry.Status;
            case EFailProbability::OneTenth: {
                static const int MOD = 10;
                if ((std::uniform_int_distribution<int>(0, MOD - 1))(RandomGen) == 0) {
                    return NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError;
                }
                return entry.Status;
            }
            case EFailProbability::Always:
                return NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError;
            default:
                Y_ENSURE(false, "Unexpected value");
        }
    }

    void TTestSchemeCacheStatusGetter::SetFailProbability(EFailProbability failProbability) {
        FailProbability = failProbability;
    }

    TTestSchemeShardStatusGetter::TTestSchemeShardStatusGetter(
        const ui32 statusOverwriteRemainingCount,
        const NKikimrScheme::EStatus overwrittenStatus
    )
        : OverwrittenStatus(overwrittenStatus)
        , StatusOverwriteRemainingCount(statusOverwriteRemainingCount)
    {
    }

    NKikimrScheme::EStatus TTestSchemeShardStatusGetter::GetStatus(
        const NKikimrScheme::TEvDescribeSchemeResult& record) const
    {
        if (StatusOverwriteRemainingCount > 0) {
            --StatusOverwriteRemainingCount;
            return OverwrittenStatus;
        }
        return record.GetStatus();
    }

} // NKikimr::NSecret
