UNITTEST_FOR(ydb/core/kqp/provider)

SRCS(
    yql_kikimr_gateway_ut.cpp
    yql_kikimr_provider_ut.cpp
    read_attributes_utils_ut.cpp
)

PEERDIR(
    ydb/core/kqp/ut/common
    ydb/core/resource_pools
    ydb/services/scheme_secret/ut/common
    yql/essentials/providers/common/structured_token
    yql/essentials/ast
    yql/essentials/sql/pg_dummy
    yql/essentials/sql/v1/translation
    library/cpp/testing/gmock_in_unittest
)

YQL_LAST_ABI_VERSION()

FORK_SUBTESTS()

IF (SANITIZER_TYPE)
    SIZE(MEDIUM)
    REQUIREMENTS(cpu:2)
ELSE()
    SIZE(MEDIUM)
    REQUIREMENTS(cpu:2)
ENDIF()

END()
