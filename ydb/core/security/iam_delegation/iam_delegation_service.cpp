#include "iam_delegation_service.h"
#include "iam_actor_base.h"

#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/ycloud/api/operation_service.h>
#include <ydb/library/ycloud/api/service_control_service.h>
#include <ydb/library/ycloud/impl/operation_service.h>
#include <ydb/library/ycloud/impl/service_control_service.h>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::IAM_DELEGATION

namespace NKikimr::NIamDelegation {

using namespace NActors;
using TOperation = ydb::yc::priv::operation::Operation;

class TIamDelegationService : public TIamActorBase<TIamDelegationService> {
private:
    using TBase = TIamActorBase<TIamDelegationService>;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::IAM_DELEGATION_SERVICE_ACTOR;
    }

    explicit TIamDelegationService(const TIamDelegationSettings& settings)
        : TBase(settings)
    {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        // Token manager has no unsubscribe API: this node service is the sole subscriber, regardless
        // of the number of concurrent requests or retries. The initializer selects its factory.
        Send(MakeTokenManagerID(), new TEvTokenManager::TEvSubscribeUpdateToken(Settings.Config.GetSystemTokenName()),
            IEventHandle::FlagTrackDelivery);

        {
            NCloud::TServiceControlServiceSettings clientSettings(Settings.Config.GetServiceControlEndpoint(), "ydb-iam-delegation");
            clientSettings.EnableSsl = Settings.Config.GetEnableSsl();
            clientSettings.RequestTimeoutMs = Settings.RequestTimeout.MilliSeconds();
            ServiceControlClient = Register(NCloud::CreateServiceControlService(clientSettings));
        }
        {
            NCloud::TOperationServiceSettings clientSettings(Settings.Config.GetServiceControlEndpoint(), "ydb-iam-delegation");
            clientSettings.EnableSsl = Settings.Config.GetEnableSsl();
            clientSettings.RequestTimeoutMs = Settings.RequestTimeout.MilliSeconds();
            OperationClient = Register(NCloud::CreateOperationService(clientSettings));
        }

        YDB_LOG_INFO("Delegation service started",
            {"serviceId", Settings.Config.GetServiceId()},
            {"microserviceId", Settings.Config.GetMicroserviceId()},
            {"serviceControlEndpoint", Settings.Config.GetServiceControlEndpoint()}
        );
    }

    STRICT_STFUNC(StateWork,
        hFunc(TEvIamDelegation::TEvSetupDelegation, HandleSetup);
        hFunc(TEvIamDelegation::TEvRevokeDelegation, HandleRevoke);
        hFunc(TEvIamDelegation::TEvSubmitDelegationOperation, HandleSubmitOperation);
        hFunc(TEvIamDelegation::TEvPollDelegationOperation, HandlePollOperation);
        // late replies after a timeout
        IgnoreFunc(NCloud::TEvServiceControlService::TEvSetupDelegationResponse);
        IgnoreFunc(NCloud::TEvServiceControlService::TEvRevokeDelegationResponse);
        IgnoreFunc(NCloud::TEvOperationService::TEvGetOperationResponse);
        hFunc(TEvTokenManager::TEvUpdateToken, HandleToken);
        hFunc(TEvents::TEvUndelivered, HandleUndelivered);
        cFunc(TEvents::TEvPoison::EventType, BeginShutdown);
    )

    STFUNC(StateDying) {
        Y_UNUSED(ev); // PassAway may be waiting for coroutine tasks
    }

private:
    void HandleToken(TEvTokenManager::TEvUpdateToken::TPtr& ev) {
        const auto& update = *ev->Get();
        if (update.Id != Settings.Config.GetSystemTokenName()) {
            return;
        }
        Credentials.Status = update.Status;
        Credentials.Token = update.Status.Code == TEvTokenManager::TStatus::ECode::SUCCESS ? update.Token : TString();
        Credentials.Updated.NotifyAll();
    }

    void HandleUndelivered(TEvents::TEvUndelivered::TPtr& ev) {
        if (ev->Get()->SourceType == TEvTokenManager::TEvSubscribeUpdateToken::EventType) {
            Credentials.Token.clear();
            Credentials.Status = {TEvTokenManager::TStatus::ECode::ERROR, "token manager is not available"};
            Credentials.Updated.NotifyAll();
        }
        // Timed-out client waits no longer intercept their undelivered replies.
    }

    void BeginShutdown() {
        Become(&TThis::StateDying);
        Send(ServiceControlClient, new TEvents::TEvPoison());
        Send(OperationClient, new TEvents::TEvPoison());
        PassAway();
    }

    static void SetOperationError(TDelegationOperationResult& result, Ydb::StatusIds::StatusCode status, const TString& message) {
        result.Status = status;
        result.Issues.AddIssue(message);
    }

    static bool ValidBinding(const TDelegationBinding& binding) {
        return !binding.ServiceAccountId.empty() && !binding.CloudId.empty() && !binding.ReferrerId.empty()
            && !binding.ServiceId.empty() && !binding.MicroserviceId.empty()
            && !binding.ResourceType.empty() && !binding.ReferrerType.empty();
    }

    template<class TRequest>
    static void FillBinding(TRequest& request, const TDelegationBinding& binding) {
        request.set_service_id(binding.ServiceId);
        request.set_microservice_id(binding.MicroserviceId);
        request.mutable_resource()->set_id(binding.CloudId);
        request.mutable_resource()->set_type(binding.ResourceType);
        request.set_target_service_account_id(binding.ServiceAccountId);
        request.mutable_referrer()->set_id(binding.ReferrerId);
        request.mutable_referrer()->set_type(binding.ReferrerType);
        request.set_with_references(binding.WithReferences);
    }

    // No mutation retry here: a missing response can hide an accepted operation. The owner
    // persists the accepted ID and uses PollOperation for every subsequent request.
    template<class TRequest, class TResponse>
    async<TDelegationOperationResult> CallOperationOnce(TActorId client, THolder<TRequest> request,
            EDelegationOperationKind kind, TString operationId = {})
    {
        TDelegationOperationResult result;
        result.OperationId = operationId;
        const bool polling = !operationId.empty();
        bool dispatched = false;
        try {
            const auto deadline = TActivationContext::Monotonic() + Settings.RequestTimeout * 2 + TDuration::Seconds(1);
            request->Token = co_await GetIamCallToken(Credentials, deadline);
            if (request->RequestId.empty()) {
                request->RequestId = CreateGuidAsString();
            }
            const auto responseDeadline = Min(deadline, TActivationContext::Monotonic() + Settings.RequestTimeout + TDuration::Seconds(1));
            dispatched = true;
            auto response = co_await WithDeadline(responseDeadline, &IamClientRequest, client, request.Release());
            if (!response) {
                SetOperationError(result, Ydb::StatusIds::TIMEOUT, "IAM operation response was not received; outcome is unknown");
                co_return result;
            }
            if ((*response)->GetTypeRewrite() == TEvents::TEvUndelivered::EventType) {
                const auto* undelivered = (*response)->template Get<TEvents::TEvUndelivered>();
                if (!polling && !undelivered->Unsure
                    && undelivered->Reason == TEvents::TEvUndelivered::ReasonActorUnknown) {
                    result.Outcome = EDelegationOperationOutcome::Failed;
                }
                SetOperationError(result, Ydb::StatusIds::UNAVAILABLE,
                    result.Outcome == EDelegationOperationOutcome::Failed
                        ? "IAM client is unavailable; request was not delivered"
                        : "IAM client is unavailable; outcome is unknown");
                co_return result;
            }
            if ((*response)->GetTypeRewrite() != TResponse::EventType) {
                SetOperationError(result, Ydb::StatusIds::INTERNAL_ERROR, "unexpected IAM operation response");
                co_return result;
            }
            typename TResponse::TPtr typed(reinterpret_cast<TEventHandle<TResponse>*>(response->Release()));
            const auto& status = typed->Get()->Status;
            if (!status.Ok()) {
                SetOperationError(result, status.InternalError ? Ydb::StatusIds::UNAVAILABLE : MapGrpcStatus(status.GRpcStatusCode), TStringBuilder() << status.Msg << ExplainIamFailure(status));
                if (!polling && !status.InternalError) {
                    const auto code = static_cast<grpc::StatusCode>(status.GRpcStatusCode);
                    if (kind == EDelegationOperationKind::Revoke && code == grpc::StatusCode::NOT_FOUND) {
                        result.Outcome = EDelegationOperationOutcome::Succeeded;
                        result.Status = Ydb::StatusIds::SUCCESS;
                        result.Issues.Clear();
                    } else if (code == grpc::StatusCode::PERMISSION_DENIED || code == grpc::StatusCode::UNAUTHENTICATED
                            || code == grpc::StatusCode::INVALID_ARGUMENT || code == grpc::StatusCode::OUT_OF_RANGE
                            || code == grpc::StatusCode::NOT_FOUND || code == grpc::StatusCode::UNIMPLEMENTED) {
                        result.Outcome = EDelegationOperationOutcome::Failed;
                    }
                    // ALREADY_EXISTS and FAILED_PRECONDITION can describe an earlier, still
                    // unfinished delegation. They do not establish that setup failed.
                }
                co_return result;
            }
            const auto& operation = typed->Get()->Response;
            if (operation.id().empty() || (polling && operation.id() != operationId)) {
                SetOperationError(result, Ydb::StatusIds::INTERNAL_ERROR, "IAM returned a missing or mismatched operation ID");
                co_return result;
            }
            result.OperationId = operation.id();
            result.Status = Ydb::StatusIds::SUCCESS;
            if (!operation.done()) {
                result.Outcome = EDelegationOperationOutcome::Accepted;
            } else if (operation.has_error()) {
                result.Outcome = EDelegationOperationOutcome::Failed;
                SetOperationError(result, MapGrpcStatus(operation.error().code()),
                    TStringBuilder() << operation.error().message() << ExplainIamFailure(operation.error()));
            } else {
                result.Outcome = EDelegationOperationOutcome::Succeeded;
            }
        } catch (const TIamCallError& error) {
            // Credential acquisition failed before a mutation could reach IAM.
            // A poll failure never changes the known remote operation's outcome.
            if (!polling && !dispatched) {
                result.Outcome = EDelegationOperationOutcome::Failed;
            }
            SetOperationError(result, error.Status, error.what());
        } catch (const std::exception& error) {
            if (!polling && !dispatched) {
                result.Outcome = EDelegationOperationOutcome::Failed;
            }
            SetOperationError(result, Ydb::StatusIds::INTERNAL_ERROR, error.what());
        }
        co_return result;
    }

    void HandleSubmitOperation(TEvIamDelegation::TEvSubmitDelegationOperation::TPtr ev) {
        const auto recipient = ev->Sender;
        const auto cookie = ev->Cookie;
        const auto kind = ev->Get()->Kind;
        const auto binding = ev->Get()->Binding;
        const auto subjectId = ev->Get()->SubjectId;
        const auto requestId = ev->Get()->RequestId;
        ev.Reset();
        TDelegationOperationResult result;
        if (!ValidBinding(binding) || (kind == EDelegationOperationKind::Setup && subjectId.empty())
                || (kind != EDelegationOperationKind::Setup && kind != EDelegationOperationKind::Revoke)) {
            result.Outcome = EDelegationOperationOutcome::Failed;
            SetOperationError(result, Ydb::StatusIds::BAD_REQUEST, "complete IAM binding and setup subject are required");
        } else if (kind == EDelegationOperationKind::Setup) {
            auto request = MakeHolder<NCloud::TEvServiceControlService::TEvSetupDelegationRequest>();
            FillBinding(request->Request, binding);
            request->Request.set_on_behalf_of_subject_id(subjectId);
            request->RequestId = requestId;
            result = co_await CallOperationOnce<NCloud::TEvServiceControlService::TEvSetupDelegationRequest,
                NCloud::TEvServiceControlService::TEvSetupDelegationResponse>(ServiceControlClient, std::move(request), kind);
        } else {
            auto request = MakeHolder<NCloud::TEvServiceControlService::TEvRevokeDelegationRequest>();
            FillBinding(request->Request, binding);
            request->RequestId = requestId;
            result = co_await CallOperationOnce<NCloud::TEvServiceControlService::TEvRevokeDelegationRequest,
                NCloud::TEvServiceControlService::TEvRevokeDelegationResponse>(ServiceControlClient, std::move(request), kind);
        }
        Send(recipient, new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, cookie);
    }

    void HandlePollOperation(TEvIamDelegation::TEvPollDelegationOperation::TPtr ev) {
        const auto recipient = ev->Sender;
        const auto cookie = ev->Cookie;
        const auto operationId = ev->Get()->OperationId;
        ev.Reset();
        TDelegationOperationResult result;
        if (operationId.empty()) {
            SetOperationError(result, Ydb::StatusIds::BAD_REQUEST, "IAM operation ID is required");
        } else {
            auto request = MakeHolder<NCloud::TEvOperationService::TEvGetOperationRequest>();
            request->Request.set_operation_id(operationId);
            result = co_await CallOperationOnce<NCloud::TEvOperationService::TEvGetOperationRequest,
                NCloud::TEvOperationService::TEvGetOperationResponse>(OperationClient, std::move(request),
                    EDelegationOperationKind::Setup, operationId);
        }
        Send(recipient, new TEvIamDelegation::TEvDelegationOperationResult(std::move(result)), 0, cookie);
    }

    // top-level coroutine: one task per request
    void HandleSetup(TEvIamDelegation::TEvSetupDelegation::TPtr ev) {
        const TActorId replyTo = ev->Sender;
        const ui64 cookie = ev->Cookie;
        const TDelegationSpec spec = ev->Get()->Spec;
        const TString subjectId = ev->Get()->SubjectId;
        ev.Reset();

        YDB_LOG_INFO("SetupDelegation", {"spec", spec.ToString()}, {"subjectId", subjectId});
        TDelegationResult result = co_await Run(spec, subjectId, ECall::Setup);
        if (result.IsSuccess()) {
            YDB_LOG_INFO("SetupDelegation done", {"spec", spec.ToString()});
        } else {
            YDB_LOG_WARN("SetupDelegation failed", {"spec", spec.ToString()}, {"status", result.Status}, {"issues", result.Issues.ToOneLineString()});
        }
        Send(replyTo, new TEvIamDelegation::TEvSetupDelegationResult(std::move(result)), 0, cookie);
    }

    void HandleRevoke(TEvIamDelegation::TEvRevokeDelegation::TPtr ev) {
        const TActorId replyTo = ev->Sender;
        const ui64 cookie = ev->Cookie;
        const TDelegationSpec spec = ev->Get()->Spec;
        ev.Reset();

        YDB_LOG_INFO("RevokeDelegation", {"spec", spec.ToString()});
        TDelegationResult result = co_await Run(spec, TString(), ECall::Revoke);
        if (result.IsSuccess()) {
            YDB_LOG_INFO("RevokeDelegation done", {"spec", spec.ToString()});
        } else {
            YDB_LOG_WARN("RevokeDelegation failed", {"spec", spec.ToString()}, {"status", result.Status}, {"issues", result.Issues.ToOneLineString()});
        }
        Send(replyTo, new TEvIamDelegation::TEvRevokeDelegationResult(std::move(result)), 0, cookie);
    }

    enum class ECall {
        Setup,
        Revoke,
    };

    async<TDelegationResult> Run(TDelegationSpec spec, TString subjectId, ECall call) {
        try {
            std::optional<TOperation> operation;
            switch (call) {
                case ECall::Setup:
                    operation = co_await CallSetup(spec, subjectId);
                    break;
                case ECall::Revoke:
                    operation = co_await CallRevoke(spec);
                    break;
            }
            if (operation) {
                co_await WaitOperation(*operation, call == ECall::Revoke ? "RevokeDelegation" : "SetupDelegation");
            }
            co_return TDelegationResult::Success();
        } catch (const TIamCallError& e) {
            co_return TDelegationResult::Error(e.Status, e.what());
        } catch (const std::exception& e) {
            co_return TDelegationResult::Error(Ydb::StatusIds::INTERNAL_ERROR, TStringBuilder() << "unexpected exception: " << e.what());
        }
    }

    async<TOperation> CallSetup(TDelegationSpec spec, TString subjectId) {
        auto response = co_await CallWithRetry<NCloud::TEvServiceControlService::TEvSetupDelegationRequest, NCloud::TEvServiceControlService::TEvSetupDelegationResponse>(
            ServiceControlClient, "ServiceControl.SetupDelegation",
            [&](yandex::cloud::priv::iam::v1::SetupDelegationRequest& request) {
                request.set_service_id(Settings.Config.GetServiceId());
                request.set_microservice_id(Settings.Config.GetMicroserviceId());
                request.mutable_resource()->set_id(spec.CloudId);
                request.mutable_resource()->set_type(Settings.Config.GetResourceType());
                request.set_target_service_account_id(spec.ServiceAccountId);
                request.mutable_referrer()->set_id(spec.ReferrerId);
                request.mutable_referrer()->set_type(Settings.ReferrerType);
                request.set_on_behalf_of_subject_id(subjectId);
                request.set_with_references(false);
            });
        co_return response->Get()->Response;
    }

    // Returns nothing when the delegation does not exist any more (NOT_FOUND).
    async<std::optional<TOperation>> CallRevoke(TDelegationSpec spec) {
        auto response = co_await CallWithRetry<NCloud::TEvServiceControlService::TEvRevokeDelegationRequest, NCloud::TEvServiceControlService::TEvRevokeDelegationResponse>(
            ServiceControlClient, "ServiceControl.RevokeDelegation",
            [&](yandex::cloud::priv::iam::v1::RevokeDelegationRequest& request) {
                request.set_service_id(Settings.Config.GetServiceId());
                request.set_microservice_id(Settings.Config.GetMicroserviceId());
                request.mutable_resource()->set_id(spec.CloudId);
                request.mutable_resource()->set_type(Settings.Config.GetResourceType());
                request.set_target_service_account_id(spec.ServiceAccountId);
                request.mutable_referrer()->set_id(spec.ReferrerId);
                request.mutable_referrer()->set_type(Settings.ReferrerType);
                request.set_with_references(false);
            },
            ENotFound::IsAbsent);
        if (!response) {
            YDB_LOG_WARN("RevokeDelegation: delegation not found, treating as revoked", {"spec", spec.ToString()});
            co_return std::nullopt;
        }
        co_return response->Get()->Response;
    }

    async<void> WaitOperation(TOperation operation, TStringBuf method) {
        const TMonotonic deadline = TActivationContext::Monotonic() + Settings.OperationPollTimeout;
        while (!operation.done()) {
            CheckIamCallDeadline(deadline);
            YDB_LOG_DEBUG("Waiting for operation", {"method", method}, {"operationId", operation.id()});
            co_await AsyncSleepUntil(Min(deadline, TActivationContext::Monotonic() + Settings.OperationPollInterval));
            CheckIamCallDeadline(deadline);
            const TString operationId = operation.id();
            auto response = co_await CallWithRetry<NCloud::TEvOperationService::TEvGetOperationRequest, NCloud::TEvOperationService::TEvGetOperationResponse>(
                OperationClient, "OperationService.Get",
                [&](yandex::cloud::priv::iam::v1::GetOperationRequest& request) {
                    request.set_operation_id(operationId);
                }, ENotFound::IsError, deadline);
            operation = response->Get()->Response;
        }
        if (operation.has_error()) {
            throw TIamCallError(MapGrpcStatus(operation.error().code())) << method << ": operation " << operation.id() << " failed: " << operation.error().message() << ExplainIamFailure(operation.error());
        }
    }

private:
    TActorId ServiceControlClient;
    TActorId OperationClient;
};

IActor* CreateIamDelegationService(const TIamDelegationSettings& settings) {
    return new TIamDelegationService(settings);
}

} // namespace NKikimr::NIamDelegation
