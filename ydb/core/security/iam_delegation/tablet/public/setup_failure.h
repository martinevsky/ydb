#pragma once

#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/tablet/protos/iam_delegation.pb.h>

namespace NKikimr::NIamDelegation {

// The durable diagnostic format accepts only known non-success statuses and
// unique allowlisted reasons. Unknown protobuf fields are deliberately rejected.
bool IsValidSetupFailure(const NKikimrIamDelegation::TSetupFailure& failure);

// Converts an authoritative local failure, discarding all remote free text.
NKikimrIamDelegation::TSetupFailure MakeSetupFailure(const TDelegationOperationResult& result);

// Fixed, actionable text reconstructed from validated durable reason codes.
TString DescribeSetupFailure(const NKikimrIamDelegation::TSetupFailure& failure);

} // namespace NKikimr::NIamDelegation
