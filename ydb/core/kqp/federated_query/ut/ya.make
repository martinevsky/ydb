GTEST()

# HTTP gateway config of a KQP node (FQ transport test plan, target H: T-CFG-1..3, T-TMO-1).
# Links only the S5 library (http_gateway_config), not ydb/core/kqp/federated_query, to keep the build
# light. FORK_SUBTESTS: T-TMO-1 makes the process-wide curl gateway singleton.

SIZE(SMALL)

FORK_SUBTESTS()

SRCS(
    kqp_http_gateway_config_ut.cpp
)

PEERDIR(
    library/cpp/threading/future
    ydb/core/kqp/federated_query/http_gateway_config
    ydb/core/protos
    ydb/library/yql/providers/common/http_gateway
    ydb/library/yql/providers/common/ut_helpers/transport
    ydb/library/yql/providers/common/ut_helpers/transport/yql
)

YQL_LAST_ABI_VERSION()

END()
