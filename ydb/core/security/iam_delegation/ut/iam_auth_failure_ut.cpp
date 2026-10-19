#include <ydb/core/security/iam_delegation/iam_actor_base.h>
#include <ydb/core/security/iam_delegation/tablet/public/setup_failure.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NIamDelegation {
namespace {

constexpr TStringBuf Sentinel = "credential-sentinel-never-expose";

void CheckAuthenticationFailure(const NYdbGrpc::TGrpcStatus& status, TStringBuf expected) {
    UNIT_ASSERT(!IsRetryableGrpcStatus(status));
    const auto reasons = ExtractIamFailureReasons(status);
    UNIT_ASSERT_VALUES_EQUAL(reasons.size(), 1);
    UNIT_ASSERT_STRING_CONTAINS(ExplainIamFailure(status), expected);
    UNIT_ASSERT(!ExplainIamFailure(status).Contains(Sentinel));

    TDelegationOperationResult result;
    result.Outcome = EDelegationOperationOutcome::Failed;
    result.Status = MapGrpcStatus(status.GRpcStatusCode);
    result.FailureReasons = reasons;
    result.Issues.AddIssue(TString(Sentinel));
    const auto durable = MakeSetupFailure(result);
    NKikimrIamDelegation::TSetupFailure restored;
    UNIT_ASSERT(restored.ParseFromString(durable.SerializeAsString()));
    UNIT_ASSERT(IsValidSetupFailure(restored));
    UNIT_ASSERT_VALUES_EQUAL(restored.ReasonsSize(), 1);
    UNIT_ASSERT_VALUES_EQUAL(restored.GetStatus(), Ydb::StatusIds::UNAUTHORIZED);
    UNIT_ASSERT_STRING_CONTAINS(DescribeSetupFailure(restored), expected);
    UNIT_ASSERT(!DescribeSetupFailure(restored).Contains(Sentinel));
}

} // namespace

Y_UNIT_TEST_SUITE(IamAuthenticationFailures) {
    Y_UNIT_TEST(AuthenticationAndPermissionFailuresHaveDifferentDurableReasons) {
        NYdbGrpc::TGrpcStatus status;
        status.GRpcStatusCode = grpc::StatusCode::UNAUTHENTICATED;
        status.Msg = TString(Sentinel);
        CheckAuthenticationFailure(status, "authentication failed");
        const auto authentication = ExtractIamFailureReasons(status);
        status.GRpcStatusCode = grpc::StatusCode::PERMISSION_DENIED;
        CheckAuthenticationFailure(status, "permission denied");
        UNIT_ASSERT(authentication != ExtractIamFailureReasons(status));
    }

    Y_UNIT_TEST(TransportCodeTakesPrecedenceOverContradictoryDetails) {
        for (auto code : {grpc::StatusCode::UNAUTHENTICATED, grpc::StatusCode::PERMISSION_DENIED}) {
            NYdbGrpc::TGrpcStatus status;
            status.GRpcStatusCode = code;
            google::rpc::Status details;
            details.set_code(code == grpc::StatusCode::UNAUTHENTICATED
                ? grpc::StatusCode::PERMISSION_DENIED : grpc::StatusCode::UNAUTHENTICATED);
            details.set_message(TString(Sentinel));
            status.Details = details.SerializeAsString();
            CheckAuthenticationFailure(status, code == grpc::StatusCode::UNAUTHENTICATED
                ? "authentication failed" : "permission denied");
        }
    }

    Y_UNIT_TEST(MalformedAndOversizedDetailsCannotHideAuthenticationKind) {
        for (auto code : {grpc::StatusCode::UNAUTHENTICATED, grpc::StatusCode::PERMISSION_DENIED}) {
            for (const auto& details : {TString("malformed"), TString(65537, 'x')}) {
                NYdbGrpc::TGrpcStatus status;
                status.GRpcStatusCode = code;
                status.Details = details;
                CheckAuthenticationFailure(status, code == grpc::StatusCode::UNAUTHENTICATED
                    ? "authentication failed" : "permission denied");
            }
        }
    }

    Y_UNIT_TEST(InternalTransportErrorsDoNotBlameCredentialsOrPermissions) {
        for (auto code : {grpc::StatusCode::UNAUTHENTICATED, grpc::StatusCode::PERMISSION_DENIED}) {
            NYdbGrpc::TGrpcStatus status;
            status.InternalError = true;
            status.GRpcStatusCode = code;
            google::rpc::Status details;
            details.set_code(code);
            status.Details = details.SerializeAsString();
            UNIT_ASSERT(IsRetryableGrpcStatus(status));
            UNIT_ASSERT(ExtractIamFailureReasons(status).empty());
            UNIT_ASSERT(ExplainIamFailure(status).empty());
        }
    }

    Y_UNIT_TEST(TerminalOperationStatusKeepsAuthenticationKind) {
        for (auto code : {grpc::StatusCode::UNAUTHENTICATED, grpc::StatusCode::PERMISSION_DENIED}) {
            google::rpc::Status status;
            status.set_code(code);
            status.set_message(TString(Sentinel));
            UNIT_ASSERT_VALUES_EQUAL(ExtractIamFailureReasons(status).size(), 1);
            UNIT_ASSERT_STRING_CONTAINS(ExplainIamFailure(status), code == grpc::StatusCode::UNAUTHENTICATED
                ? "authentication failed" : "permission denied");
            UNIT_ASSERT(!ExplainIamFailure(status).Contains(Sentinel));
        }
    }
}

} // namespace NKikimr::NIamDelegation
