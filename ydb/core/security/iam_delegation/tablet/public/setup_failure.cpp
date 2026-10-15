#include "setup_failure.h"

#include <util/string/builder.h>

#include <array>

namespace NKikimr::NIamDelegation {
namespace {
namespace P = NKikimrIamDelegation;

struct TFailureReason {
    EDelegationFailureReason Native;
    P::ESetupFailureReason Durable;
    TStringBuf Hint;
};

constexpr std::array<TFailureReason, 5> Reasons{{
    {EDelegationFailureReason::WrongCloud, P::SETUP_FAILURE_WRONG_CLOUD,
        "the service account does not belong to the cloud of the delegation, set RESOURCE to the cloud of the service account"},
    {EDelegationFailureReason::ServiceNotEnabled, P::SETUP_FAILURE_SERVICE_NOT_ENABLED,
        "the YDB service is not enabled in the cloud of the delegation, it must be enabled in the cloud of the service account (RESOURCE)"},
    {EDelegationFailureReason::CloudNotFound, P::SETUP_FAILURE_CLOUD_NOT_FOUND,
        "the cloud of the delegation does not exist, check RESOURCE"},
    {EDelegationFailureReason::ServiceAccountNotFound, P::SETUP_FAILURE_SERVICE_ACCOUNT_NOT_FOUND,
        "check SERVICE_ACCOUNT_ID"},
    {EDelegationFailureReason::SubjectNotDelegatable, P::SETUP_FAILURE_SUBJECT_NOT_DELEGATABLE,
        "the user running the statement is not a subject a delegation can be made on behalf of"},
}};

bool IsFailureStatus(Ydb::StatusIds::StatusCode status) {
    return Ydb::StatusIds::StatusCode_IsValid(status)
        && status != Ydb::StatusIds::STATUS_CODE_UNSPECIFIED
        && status != Ydb::StatusIds::SUCCESS;
}

} // namespace

bool IsValidSetupFailure(const P::TSetupFailure& failure) {
    // Five enums and a status fit well below this bound. Reject unknown fields
    // even when small: they can carry arbitrary remote text through a replay.
    if (!failure.HasStatus() || !IsFailureStatus(failure.GetStatus())
        || static_cast<size_t>(failure.ReasonsSize()) > Reasons.size() || failure.ByteSizeLong() > 128
        || !failure.GetReflection()->GetUnknownFields(failure).empty()) {
        return false;
    }
    std::array<bool, Reasons.size()> seen{};
    for (const auto reason : failure.GetReasons()) {
        bool found = false;
        for (size_t index = 0; index < Reasons.size(); ++index) {
            if (reason == Reasons[index].Durable) {
                if (seen[index]) {
                    return false;
                }
                seen[index] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

P::TSetupFailure MakeSetupFailure(const TDelegationOperationResult& result) {
    P::TSetupFailure failure;
    failure.SetStatus(IsFailureStatus(result.Status) ? result.Status : Ydb::StatusIds::INTERNAL_ERROR);
    // Canonical order and set membership bound output even if a native caller
    // supplied duplicates or an unrecognized enum value.
    for (const auto& reason : Reasons) {
        for (const auto native : result.FailureReasons) {
            if (native == reason.Native) {
                failure.AddReasons(reason.Durable);
                break;
            }
        }
    }
    return failure;
}

TString DescribeSetupFailure(const P::TSetupFailure& failure) {
    TStringBuilder message;
    message << "IAM refused delegation setup";
    if (IsValidSetupFailure(failure)) {
        for (const auto reason : failure.GetReasons()) {
            for (const auto& known : Reasons) {
                if (reason == known.Durable) {
                    message << "; " << known.Hint;
                    break;
                }
            }
        }
    }
    return message;
}

} // namespace NKikimr::NIamDelegation
