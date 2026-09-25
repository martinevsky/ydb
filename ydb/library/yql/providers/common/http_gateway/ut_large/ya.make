UNITTEST_FOR(ydb/library/yql/providers/common/http_gateway)

# The real curl gateway against the moto S3 recipe (FQ transport test plan §2.4, T-E2E-2).

SIZE(MEDIUM)

DATA(arcadia/ydb/library/yql/providers/common/http_gateway/ut_large/test.json)

SRCS(
    yql_http_gateway_ut.cpp
)

PEERDIR(
    library/cpp/testing/common
    library/cpp/testing/hook
    library/cpp/testing/unittest
    library/cpp/threading/future
    ydb/library/aws_init
    ydb/library/yql/providers/common/http_gateway
    yql/essentials/sql/pg_dummy
    yql/essentials/utils
)

YQL_LAST_ABI_VERSION()

INCLUDE(${ARCADIA_ROOT}/ydb/tests/tools/s3_recipe/recipe.inc)

END()
