#include "iam_delegation_service.h"
#include "iam_errors.h"

#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/token_manager/token_manager.h>
#include <ydb/core/util/shared_backoff.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/actors/core/scheduler_cookie.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/library/ycloud/api/operation_service.h>
#include <ydb/library/ycloud/api/service_control_service.h>
#include <ydb/library/ycloud/impl/operation_service.h>
#include <ydb/library/ycloud/impl/service_control_service.h>

#include <util/generic/guid.h>
#include <util/generic/intrlist.h>
#include <util/string/builder.h>
#include <list>
#include <map>
#include <memory>
#include <optional>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::IAM_DELEGATION

namespace NKikimr::NIamDelegation {

using namespace NActors;
using TOperation = ydb::yc::priv::operation::Operation;
using TControlEvents = NCloud::TEvServiceControlService;
using TOperationEvents = NCloud::TEvOperationService;

class TIamDelegationService : public TActorBootstrapped<TIamDelegationService>, public IActorExceptionHandler {
    enum class ECall { Setup, Revoke };
    enum class EPhase { Credentials, Backoff, DispatchCredentials, Reply, Poll };
    enum class ETrigger { Event, Timer };

    using TWaiters = std::list<ui64>;

    struct TRequest : TIntrusiveListItem<TRequest> {
        ui64 Id = 0;
        ECall Call = ECall::Setup;
        EPhase Phase = EPhase::Credentials;
        TActorId ReplyTo;
        ui64 ReplyCookie = 0;
        TDelegationSpec Spec;
        TString SubjectId;
        TString OperationId;
        TMonotonic PollDeadline = TMonotonic::Max();
        TMonotonic AttemptDeadline = TMonotonic::Max();
        TMonotonic DispatchDeadline = TMonotonic::Max();
        ui64 TimerGeneration = 0;
        TSchedulerCookieHolder TimerCookie;
        std::optional<TWaiters::iterator> CredentialWaiter;
        std::optional<TWaiters::iterator> BackoffWaiter;
        ui64 RpcCookie = 0;
        ui64 BackoffVersion = 0;

        TStringBuf MutationMethod() const {
            return Call == ECall::Setup ? "ServiceControl.SetupDelegation" : "ServiceControl.RevokeDelegation";
        }
        TStringBuf Method() const {
            return OperationId.empty() ? MutationMethod() : "OperationService.Get";
        }
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::IAM_DELEGATION_SERVICE_ACTOR;
    }

    explicit TIamDelegationService(const TIamDelegationSettings& settings)
        : Settings(settings)
        , RetryBackoff(TDuration::Seconds(1), TDuration::Minutes(1))
    {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        // One subscriber for the service lifetime; token manager has no unsubscribe API.
        Send(MakeTokenManagerID(), new TEvTokenManager::TEvSubscribeUpdateToken(Settings.Config.GetSystemTokenName()),
            IEventHandle::FlagTrackDelivery);
        NCloud::TServiceControlServiceSettings control(Settings.Config.GetServiceControlEndpoint(), "ydb-iam-delegation");
        control.EnableSsl = Settings.Config.GetEnableSsl();
        control.RequestTimeoutMs = Settings.RequestTimeout.MilliSeconds();
        ServiceControlClient = Register(NCloud::CreateServiceControlService(control));
        NCloud::TOperationServiceSettings operations(Settings.Config.GetServiceControlEndpoint(), "ydb-iam-delegation");
        operations.EnableSsl = Settings.Config.GetEnableSsl();
        operations.RequestTimeoutMs = Settings.RequestTimeout.MilliSeconds();
        OperationClient = Register(NCloud::CreateOperationService(operations));
        YDB_LOG_INFO("Delegation service started", {"serviceId", Settings.Config.GetServiceId()},
            {"microserviceId", Settings.Config.GetMicroserviceId()},
            {"serviceControlEndpoint", Settings.Config.GetServiceControlEndpoint()});
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvIamDelegation::TEvSetupDelegation, HandleSetup);
            hFunc(TEvIamDelegation::TEvRevokeDelegation, HandleRevoke);
            hFunc(TControlEvents::TEvSetupDelegationResponse, HandleResponse);
            hFunc(TControlEvents::TEvRevokeDelegationResponse, HandleResponse);
            hFunc(TOperationEvents::TEvGetOperationResponse, HandleResponse);
            hFunc(TEvTokenManager::TEvUpdateToken, HandleToken);
            hFunc(TEvents::TEvUndelivered, HandleUndelivered);
            hFunc(TEvents::TEvWakeup, HandleWakeup);
            case TEvents::TEvPoison::EventType:
                BeginShutdown();
                return;
            default:
                if (const auto it = RpcRequests.find(ev->Cookie); it != RpcRequests.end()) {
                    ProcessRequest(it->second, [&](TRequest& request) {
                        if (Now() >= request.AttemptDeadline) {
                            Fail(request, Ydb::StatusIds::TIMEOUT, "IAM operation polling deadline exceeded");
                        } else {
                            Fail(request, Ydb::StatusIds::INTERNAL_ERROR, "Unexpected IAM response; outcome is unknown");
                        }
                    });
                }
                break;
        }
        DrainBackoffNotifications();
        ArmTimer();
    }

private:
    using IActorExceptionHandler::OnUnhandledException;
    bool OnUnhandledException(const std::exception&) override {
        YDB_LOG_ERROR("Unhandled exception in an IAM delegation actor", {"actor", SelfId()});
        // Per-request failures are contained below. A one-shot failure of the
        // shared scheduler must not leave unrelated requests without a timer.
        ArmTimer();
        return true;
    }

    static TMonotonic Now() { return TActivationContext::Monotonic(); }

    template<class TAction>
    void ProcessRequest(ui64 id, TAction&& action) {
        const auto it = Requests.find(id);
        if (it == Requests.end()) {
            return;
        }
        try {
            action(it->second);
        } catch (const std::exception&) {
            // Unexpected failures complete this operation, including polls;
            // they neither replay mutations nor interrupt other callers.
            if (const auto current = Requests.find(id); current != Requests.end()) {
                Finish(current->second, TDelegationResult::Error(Ydb::StatusIds::INTERNAL_ERROR,
                    "Unexpected IAM operation failure"));
            }
        }
    }

    void NotifyBackoffOne() {
        if (!BackoffWaiters.empty()) {
            const auto id = BackoffWaiters.front();
            auto& request = Requests.at(id);
            // Handoff must not allocate after completing the previous owner.
            // Request destruction unlinks a notification still waiting to run.
            BackoffNotifications.PushBack(&request);
            request.BackoffWaiter.reset();
            BackoffWaiters.pop_front();
        }
    }

    void NotifyBackoffAll() {
        while (!BackoffWaiters.empty()) {
            NotifyBackoffOne();
        }
    }

    void ReleaseBackoffOwner() {
        BackoffTimerOwner = 0;
        TimerCookie.Detach();
        TimerAt = TMonotonic::Max();
        ++TimerGeneration;
        // Cancellation of the timer-owning wait wakes one successor in this
        // actor turn. Cancelling another waiter must not open admission.
        NotifyBackoffOne();
    }

    void ClearWaiters(TRequest& request) {
        if (request.CredentialWaiter) {
            CredentialWaiters.erase(*request.CredentialWaiter);
            request.CredentialWaiter.reset();
        }
        if (request.BackoffWaiter) {
            BackoffWaiters.erase(*request.BackoffWaiter);
            request.BackoffWaiter.reset();
        }
        if (BackoffTimerOwner == request.Id) {
            ReleaseBackoffOwner();
        }
    }

    void SetPhase(TRequest& request, EPhase phase, TMonotonic deadline) {
        ClearWaiters(request);
        request.TimerCookie.Detach();
        request.Phase = phase;
        ++request.TimerGeneration;
        if (phase == EPhase::Credentials || phase == EPhase::DispatchCredentials) {
            CredentialWaiters.push_back(request.Id);
            request.CredentialWaiter = std::prev(CredentialWaiters.end());
        }
        // Each wait arbitrates against its own mailbox timer. An earlier wake
        // belonging to another request cannot expire this request's queued reply.
        auto event = MakeHolder<TEvents::TEvWakeup>(request.TimerGeneration);
        auto handle = std::make_unique<IEventHandle>(SelfId(), SelfId(), event.Get(), 0, request.Id);
        Y_UNUSED(event.Release());
        request.TimerCookie.Reset(ISchedulerCookie::Make2Way());
        TActivationContext::Schedule(deadline, std::move(handle), request.TimerCookie.Get());
    }

    void ReportBackoffFailure(ui64 version) {
        if (RetryBackoff.OnFailure(version, Now()) == TSharedBackoff::EUpdate::Applied) {
            NotifyBackoffAll();
        }
    }

    void ReportBackoffSuccess(ui64 version) {
        const auto previous = RetryBackoff.GetVersion();
        RetryBackoff.OnSuccess(version);
        if (RetryBackoff.GetVersion() != previous) {
            NotifyBackoffAll();
        }
    }

    void ClearRpc(TRequest& request) {
        if (request.RpcCookie) {
            RpcRequests.erase(request.RpcCookie);
            request.RpcCookie = 0;
        }
    }

    void Finish(TRequest& request, TDelegationResult result) {
        if (request.Call == ECall::Setup) {
            if (result.IsSuccess()) {
                YDB_LOG_INFO("SetupDelegation done", {"spec", request.Spec.ToString()});
            } else {
                YDB_LOG_WARN("SetupDelegation failed", {"spec", request.Spec.ToString()},
                    {"status", result.Status}, {"issues", result.Issues.ToOneLineString()});
            }
        } else if (result.IsSuccess()) {
            YDB_LOG_INFO("RevokeDelegation done", {"spec", request.Spec.ToString()});
        } else {
            YDB_LOG_WARN("RevokeDelegation failed", {"spec", request.Spec.ToString()},
                {"status", result.Status}, {"issues", result.Issues.ToOneLineString()});
        }
        if (request.Call == ECall::Setup) {
            Send(request.ReplyTo, new TEvIamDelegation::TEvSetupDelegationResult(std::move(result)), 0, request.ReplyCookie);
        } else {
            Send(request.ReplyTo, new TEvIamDelegation::TEvRevokeDelegationResult(std::move(result)), 0, request.ReplyCookie);
        }
        ClearRpc(request);
        ClearWaiters(request);
        Requests.erase(request.Id);
    }

    void Poll(TRequest& request) {
        ClearRpc(request);
        if (Now() >= request.PollDeadline) {
            Finish(request, TDelegationResult::Error(Ydb::StatusIds::TIMEOUT, "IAM operation polling deadline exceeded"));
            return;
        }
        SetPhase(request, EPhase::Poll, Min(request.PollDeadline, Now() + Settings.OperationPollInterval));
    }

    void Fail(TRequest& request, Ydb::StatusIds::StatusCode status, const TString& message) {
        // A known-operation read may retry. No mutation is ever resubmitted.
        if (!request.OperationId.empty() && (status == Ydb::StatusIds::UNAVAILABLE || status == Ydb::StatusIds::TIMEOUT
                || status == Ydb::StatusIds::OVERLOADED || status == Ydb::StatusIds::INTERNAL_ERROR)) {
            Poll(request);
        } else {
            Finish(request, TDelegationResult::Error(status, message));
        }
    }

    void StartAttempt(TRequest& request) {
        request.AttemptDeadline = Min(request.PollDeadline, Now() + Settings.RequestRetryTimeout);
        SetPhase(request, EPhase::Credentials, Min(request.AttemptDeadline, Now() + Settings.RequestTimeout));
        Advance(request, ETrigger::Event);
    }

    void Start(ECall call, TActorId replyTo, ui64 cookie, const TDelegationSpec& spec, const TString& subject = {}) {
        const auto id = ++NextRequestId;
        auto& request = Requests.try_emplace(id).first->second;
        request.Id = id;
        request.Call = call;
        request.ReplyTo = replyTo;
        request.ReplyCookie = cookie;
        ProcessRequest(id, [&](TRequest& pending) {
            pending.Spec = spec;
            pending.SubjectId = subject;
            if (call == ECall::Setup) {
                YDB_LOG_INFO("SetupDelegation", {"spec", spec.ToString()}, {"subjectId", subject});
            } else {
                YDB_LOG_INFO("RevokeDelegation", {"spec", spec.ToString()});
            }
            if (spec.ServiceAccountId.empty() || spec.CloudId.empty() || spec.ReferrerId.empty()
                    || (call == ECall::Setup && subject.empty())) {
                Finish(pending, TDelegationResult::Error(Ydb::StatusIds::BAD_REQUEST, "Complete IAM binding and setup subject are required"));
            } else {
                StartAttempt(pending);
            }
        });
    }

    void HandleSetup(TEvIamDelegation::TEvSetupDelegation::TPtr& ev) {
        Start(ECall::Setup, ev->Sender, ev->Cookie, ev->Get()->Spec, ev->Get()->SubjectId);
    }

    void HandleRevoke(TEvIamDelegation::TEvRevokeDelegation::TPtr& ev) {
        Start(ECall::Revoke, ev->Sender, ev->Cookie, ev->Get()->Spec);
    }

    void Advance(TRequest& request, ETrigger trigger) {
        if (Now() >= request.PollDeadline || (request.Phase != EPhase::Poll && Now() >= request.AttemptDeadline)
                || (request.Phase == EPhase::DispatchCredentials && Now() >= request.DispatchDeadline)) {
            Fail(request, Ydb::StatusIds::TIMEOUT, request.Phase == EPhase::Backoff
                ? "IAM service retry deadline exceeded" : "IAM operation polling deadline exceeded");
            return;
        }
        switch (request.Phase) {
            case EPhase::Poll:
                StartAttempt(request);
                return;
            case EPhase::Reply:
                ReportBackoffFailure(request.BackoffVersion);
                Fail(request, Ydb::StatusIds::TIMEOUT, "IAM response was not received; outcome is unknown");
                return;
            case EPhase::Backoff:
                Fail(request, Ydb::StatusIds::TIMEOUT, "IAM service retry deadline exceeded");
                return;
            case EPhase::Credentials:
            case EPhase::DispatchCredentials:
                break;
        }
        if (trigger == ETrigger::Timer) {
            Fail(request, Ydb::StatusIds::UNAVAILABLE, "timeout while obtaining the system service account token");
            return;
        }
        if (TokenStatus.Code == TEvTokenManager::TStatus::ECode::NOT_READY) {
            return;
        }
        if (TokenStatus.Code != TEvTokenManager::TStatus::ECode::SUCCESS) {
            Fail(request, Ydb::StatusIds::UNAVAILABLE, "system service account token is unavailable");
            return;
        }
        if (Token.empty()) {
            Fail(request, Ydb::StatusIds::UNAVAILABLE, "system service account token is empty");
            return;
        }
        if (request.Phase == EPhase::Credentials) {
            SetPhase(request, EPhase::Backoff, request.AttemptDeadline);
            ResumeBackoff(request);
        } else {
            Dispatch(request);
        }
    }

    void ResumeBackoff(TRequest& request) {
        ClearWaiters(request);
        const auto version = RetryBackoff.TryAcquire(Now(), Settings.RequestTimeout * 2 + TDuration::Seconds(1));
        if (!version) {
            BackoffWaiters.push_back(request.Id);
            request.BackoffWaiter = std::prev(BackoffWaiters.end());
            if (!BackoffTimerOwner) {
                BackoffTimerOwner = request.Id;
                ArmTimer();
            }
            return;
        }
        // The gate wake may have been queued before the caller's deadline event.
        // As in the reference, acquire first, then check the absolute dispatch
        // deadline; even an expired caller has consumed this reservation.
        request.BackoffVersion = *version;
        request.DispatchDeadline = Min(request.AttemptDeadline, Now() + Settings.RequestTimeout * 2 + TDuration::Seconds(1));
        SetPhase(request, EPhase::DispatchCredentials, Min(request.DispatchDeadline, Now() + Settings.RequestTimeout));
        Advance(request, ETrigger::Event);
    }

    void DrainBackoffNotifications() {
        // These are same-turn notifications, like the reference's Changed
        // continuations. Unrelated events never wake a suspended gate waiter.
        while (!BackoffNotifications.Empty()) {
            const auto id = BackoffNotifications.PopFront()->Id;
            ProcessRequest(id, [&](TRequest& request) {
                if (request.Phase == EPhase::Backoff) {
                    ResumeBackoff(request);
                }
            });
        }
    }

    template<class TRequestProto>
    void FillBinding(TRequestProto& proto, const TDelegationSpec& spec) const {
        proto.set_service_id(Settings.Config.GetServiceId());
        proto.set_microservice_id(Settings.Config.GetMicroserviceId());
        proto.mutable_resource()->set_id(spec.CloudId);
        proto.mutable_resource()->set_type(Settings.Config.GetResourceType());
        proto.set_target_service_account_id(spec.ServiceAccountId);
        proto.mutable_referrer()->set_id(spec.ReferrerId);
        proto.mutable_referrer()->set_type(Settings.ReferrerType);
        proto.set_with_references(false);
    }

    template<class TEvent>
    void SendRpc(TRequest& request, TActorId client, THolder<TEvent> event) {
        event->Token = Token;
        event->RequestId = CreateGuidAsString();
        request.RpcCookie = ++NextRpcCookie;
        RpcRequests.emplace(request.RpcCookie, request.Id);
        SetPhase(request, EPhase::Reply, Min(request.DispatchDeadline, Now() + Settings.RequestTimeout + TDuration::Seconds(1)));
        if (Now() >= request.DispatchDeadline) {
            Fail(request, Ydb::StatusIds::TIMEOUT, "IAM operation polling deadline exceeded");
            return;
        }
        Send(client, event.Release(), IEventHandle::FlagTrackDelivery, request.RpcCookie);
    }

    void Dispatch(TRequest& request) {
        if (Now() >= request.DispatchDeadline) {
            Fail(request, Ydb::StatusIds::TIMEOUT, "IAM operation polling deadline exceeded");
            return;
        }
        if (!request.OperationId.empty()) {
            auto event = MakeHolder<TOperationEvents::TEvGetOperationRequest>();
            event->Request.set_operation_id(request.OperationId);
            SendRpc(request, OperationClient, std::move(event));
        } else if (request.Call == ECall::Setup) {
            auto event = MakeHolder<TControlEvents::TEvSetupDelegationRequest>();
            FillBinding(event->Request, request.Spec);
            event->Request.set_on_behalf_of_subject_id(request.SubjectId);
            SendRpc(request, ServiceControlClient, std::move(event));
        } else {
            auto event = MakeHolder<TControlEvents::TEvRevokeDelegationRequest>();
            FillBinding(event->Request, request.Spec);
            SendRpc(request, ServiceControlClient, std::move(event));
        }
    }

    ui32 ExpectedResponse(const TRequest& request) const {
        return !request.OperationId.empty() ? TOperationEvents::TEvGetOperationResponse::EventType
            : request.Call == ECall::Setup ? TControlEvents::TEvSetupDelegationResponse::EventType
            : TControlEvents::TEvRevokeDelegationResponse::EventType;
    }

    template<class TEvent>
    void HandleResponse(TAutoPtr<TEventHandle<TEvent>>& ev) {
        const auto it = RpcRequests.find(ev->Cookie);
        if (it == RpcRequests.end()) {
            return; // Late, duplicate or unrelated response.
        }
        ProcessRequest(it->second, [&](TRequest& request) {
            ProcessResponse(request, *ev->Get());
        });
    }

    template<class TEvent>
    void ProcessResponse(TRequest& request, const TEvent& event) {
        ClearRpc(request);
        if (Now() >= request.AttemptDeadline) {
            Fail(request, Ydb::StatusIds::TIMEOUT, "IAM operation polling deadline exceeded");
            return;
        }
        if (TEvent::EventType != ExpectedResponse(request)) {
            Fail(request, Ydb::StatusIds::INTERNAL_ERROR, "Unexpected IAM response; outcome is unknown");
            return;
        }
        const auto& status = event.Status;
        if (IamServiceHealth(status) == EIamServiceHealth::Healthy) {
            ReportBackoffSuccess(request.BackoffVersion);
        } else {
            ReportBackoffFailure(request.BackoffVersion);
        }
        if (!status.Ok()) {
            if (status.InternalError) {
                Fail(request, Ydb::StatusIds::UNAVAILABLE, "IAM client transport failed; outcome is unknown");
            } else if (request.OperationId.empty() && request.Call == ECall::Revoke && status.GRpcStatusCode == grpc::StatusCode::NOT_FOUND) {
                Finish(request, TDelegationResult::Success());
            } else {
                Fail(request, MapGrpcStatus(status.GRpcStatusCode), TStringBuilder()
                    << "IAM request failed with gRPC status " << status.GRpcStatusCode << ExplainIamFailure(status, request.Method()));
            }
            return;
        }
        const auto& operation = event.Response;
        if (operation.id().empty() || (!request.OperationId.empty() && operation.id() != request.OperationId)) {
            Fail(request, Ydb::StatusIds::INTERNAL_ERROR, "IAM returned a missing or mismatched operation ID; outcome is unknown");
            return;
        }
        if (request.OperationId.empty()) {
            request.PollDeadline = Now() + Settings.OperationPollTimeout;
        }
        request.OperationId = operation.id();
        if (!operation.done()) {
            Poll(request);
        } else if (operation.has_error()) {
            // A terminal remote failure must not become another polling retry.
            if (operation.error().code() == grpc::StatusCode::OK) {
                Finish(request, TDelegationResult::Error(Ydb::StatusIds::INTERNAL_ERROR,
                    "IAM returned a success-coded operation error; outcome is unknown"));
            } else {
                Finish(request, TDelegationResult::Error(MapGrpcStatus(operation.error().code()), TStringBuilder()
                    << "IAM operation failed with gRPC status " << operation.error().code()
                    << ExplainIamFailure(operation.error(), request.MutationMethod())));
            }
        } else {
            Finish(request, TDelegationResult::Success());
        }
    }

    void WakeCredentials() {
        if (TokenStatus.Code == TEvTokenManager::TStatus::ECode::NOT_READY) {
            return;
        }
        while (!CredentialWaiters.empty()) {
            ProcessRequest(CredentialWaiters.front(), [&](TRequest& request) {
                Advance(request, ETrigger::Event);
            });
        }
    }

    void HandleToken(TEvTokenManager::TEvUpdateToken::TPtr& ev) {
        const auto& update = *ev->Get();
        if (update.Id != Settings.Config.GetSystemTokenName()) {
            return;
        }
        TokenStatus = update.Status;
        Token = update.Status.Code == TEvTokenManager::TStatus::ECode::SUCCESS ? update.Token : TString();
        WakeCredentials();
    }

    void HandleUndelivered(TEvents::TEvUndelivered::TPtr& ev) {
        if (ev->Get()->SourceType == TEvTokenManager::TEvSubscribeUpdateToken::EventType) {
            Token.clear();
            TokenStatus = {TEvTokenManager::TStatus::ECode::ERROR, "token manager is not available"};
            WakeCredentials();
        } else if (const auto it = RpcRequests.find(ev->Cookie); it != RpcRequests.end()) {
            ProcessRequest(it->second, [&](TRequest& request) {
                if (Now() >= request.AttemptDeadline) {
                    Fail(request, Ydb::StatusIds::TIMEOUT, "IAM operation polling deadline exceeded");
                } else {
                    ReportBackoffFailure(request.BackoffVersion);
                    Fail(request, Ydb::StatusIds::UNAVAILABLE, "IAM client is unavailable; outcome is unknown");
                }
            });
        }
    }

    void ArmTimer() {
        const auto next = BackoffTimerOwner ? RetryBackoff.GetNextAllowedAt() : TMonotonic::Max();
        if (next == TimerAt) {
            return;
        }
        TimerCookie.Detach();
        TimerAt = TMonotonic::Max();
        ++TimerGeneration;
        if (next != TMonotonic::Max()) {
            auto event = MakeHolder<TEvents::TEvWakeup>(TimerGeneration);
            TimerCookie.Reset(ISchedulerCookie::Make2Way());
            Schedule(next, event.Release(), TimerCookie.Get());
            TimerAt = next;
        }
    }

    void HandleWakeup(TEvents::TEvWakeup::TPtr& ev) {
        if (ev->Cookie) {
            ProcessRequest(ev->Cookie, [&](TRequest& request) {
                if (ev->Get()->Tag != request.TimerGeneration) {
                    return;
                }
                request.TimerCookie.Detach();
                Advance(request, ETrigger::Timer);
            });
        } else if (ev->Get()->Tag == TimerGeneration && BackoffTimerOwner) {
            ProcessRequest(BackoffTimerOwner, [&](TRequest& request) {
                ResumeBackoff(request);
            });
        }
    }

    void BeginShutdown() {
        TimerCookie.Detach();
        BackoffNotifications.Clear();
        BackoffWaiters.clear();
        CredentialWaiters.clear();
        Requests.clear();
        Send(ServiceControlClient, new TEvents::TEvPoison());
        Send(OperationClient, new TEvents::TEvPoison());
        // No actor tasks: PassAway detaches immediately. Container and timer
        // RAII also release state during hard teardown without actor TLS.
        PassAway();
    }

private:
    const TIamDelegationSettings Settings;
    TSharedBackoff RetryBackoff;
    TString Token;
    TEvTokenManager::TStatus TokenStatus{TEvTokenManager::TStatus::ECode::NOT_READY, {}};
    TActorId ServiceControlClient;
    TActorId OperationClient;
    ui64 NextRequestId = 0;
    ui64 NextRpcCookie = 0;
    std::map<ui64, TRequest> Requests;
    std::map<ui64, ui64> RpcRequests;
    TWaiters CredentialWaiters;
    TWaiters BackoffWaiters;
    TIntrusiveList<TRequest> BackoffNotifications;
    ui64 BackoffTimerOwner = 0;
    TSchedulerCookieHolder TimerCookie;
    TMonotonic TimerAt = TMonotonic::Max();
    ui64 TimerGeneration = 0;
};

IActor* CreateIamDelegationService(const TIamDelegationSettings& settings) {
    return new TIamDelegationService(settings);
}

} // namespace NKikimr::NIamDelegation
