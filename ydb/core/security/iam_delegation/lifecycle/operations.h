#pragma once

#include "client.h"

#include <ydb/core/base/iam_delegation.h>
#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/library/actors/core/actorsystem.h>
#include <util/generic/scope.h>

namespace NKikimr::NIamDelegation {

inline TDelegationBinding ToIamBinding(const NKikimrIamDelegation::TIamBinding& saved) {
    return {
        .ServiceAccountId = saved.GetServiceAccountId(),
        .CloudId = saved.GetCloudId(),
        .ReferrerId = saved.GetReferrerId(),
        .ServiceId = saved.GetServiceId(),
        .MicroserviceId = saved.GetMicroserviceId(),
        .ResourceType = saved.GetResourceType(),
        .ReferrerType = saved.GetReferrerType(),
        .WithReferences = saved.GetReferencePolicy() == NKikimrIamDelegation::WITH_REFERENCES,
    };
}

inline NActors::async<NKikimrIamDelegation::TResponse> CallIamDelegationTablet(
    ui64 tabletId, NKikimrIamDelegation::TRequest request)
{
    auto event = MakeHolder<TEvIamDelegationTablet::TEvRequest>();
    event->Record.Swap(&request);
    const auto client = NActors::TActivationContext::Register(CreateIamDelegationTabletRequestActor(tabletId));
    auto reply = co_await NActors::ActorRequest<TEvIamDelegationTablet::TEvResponse>(client, event.Release());
    co_return std::move(reply->Get()->Record);
}

inline NActors::async<NActors::IEventHandle::TPtr> RequestIamOperation(NActors::IEventBase* request) {
    co_return co_await NActors::ActorRequest<NActors::IEventHandle>(MakeIamDelegationServiceId(), request,
        NActors::IEventHandle::FlagTrackDelivery);
}

inline NActors::async<TDelegationOperationResult> CallIamOperation(NActors::IEventBase* request) {
    const bool submitting = request->Type() == TEvIamDelegation::TEvSubmitDelegationOperation::EventType;
    auto reply = co_await NActors::WithTimeout(TDuration::Seconds(30), &RequestIamOperation, request);
    if (reply && (*reply)->GetTypeRewrite() == TEvIamDelegation::TEvDelegationOperationResult::EventType) {
        co_return (*reply)->Get<TEvIamDelegation::TEvDelegationOperationResult>()->Result;
    }
    TDelegationOperationResult result;
    result.Outcome = EDelegationOperationOutcome::Unknown;
    result.Status = Ydb::StatusIds::UNAVAILABLE;
    if (submitting && reply && (*reply)->GetTypeRewrite() == NActors::TEvents::TEvUndelivered::EventType) {
        const auto* undelivered = (*reply)->Get<NActors::TEvents::TEvUndelivered>();
        if (!undelivered->Unsure && undelivered->Reason == NActors::TEvents::TEvUndelivered::ReasonActorUnknown) {
            result.Outcome = EDelegationOperationOutcome::Failed;
            result.Issues.AddIssue("IAM delegation service is unavailable; request was not delivered");
            co_return result;
        }
    }
    result.Issues.AddIssue("IAM operation reply is unavailable; outcome is unknown");
    co_return result;
}

} // namespace NKikimr::NIamDelegation
