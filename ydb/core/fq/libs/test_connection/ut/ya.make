UNITTEST_FOR(ydb/core/fq/libs/test_connection)

PEERDIR(
    library/cpp/testing/unittest
    ydb/core/testlib/actors
    ydb/core/testlib/basics/default
    ydb/library/yql/providers/common/token_accessor/client
    yql/essentials/sql/pg_dummy
    yql/essentials/public/udf/service/exception_policy
)

YQL_LAST_ABI_VERSION()

SRCS(
    test_connection_ut.cpp
)

END()
