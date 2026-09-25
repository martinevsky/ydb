UNITTEST_FOR(ydb/library/yql/providers/common/http_gateway)

FORK_SUBTESTS()

SPLIT_FACTOR(20)

SRCS(
    yql_aws_signature_ut.cpp
    yql_dns_gateway_ut.cpp
    yql_http_default_retry_policy_ut.cpp
    yql_http_gateway_budget_ut.cpp
    yql_http_gateway_config_ut.cpp
    yql_http_gateway_sigv4_ut.cpp
    yql_http_gateway_timeout_ut.cpp
    yql_http_gateway_transport_ut.cpp
)

PEERDIR(
    contrib/libs/openssl
    library/cpp/testing/unittest
    ydb/library/yql/providers/common/http_gateway/mock
    ydb/library/yql/providers/common/ut_helpers/transport
    ydb/library/yql/providers/common/ut_helpers/transport/yql
    yql/essentials/utils/log
)

YQL_LAST_ABI_VERSION()

END()
