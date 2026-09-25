GTEST()

# Transport tests for the Solomon/Monium read accessor (FQ transport test plan, target B).
# One test per process (FORK_SUBTESTS + SPLIT_FACTOR = number of tests): the curl gateway is a
# process singleton and its teardown must not race with gRPC (test_plan.md §6.3 D-6, D-7).

SIZE(SMALL)

FORK_SUBTESTS()

SPLIT_FACTOR(23)

SRCS(
    solomon_accessor_client_transport_ut.cpp
)

PEERDIR(
    library/cpp/json
    library/cpp/retry
    ydb/library/yql/providers/common/http_gateway/mock
    ydb/library/yql/providers/common/ut_helpers/transport
    ydb/library/yql/providers/common/ut_helpers/transport/credentials
    ydb/library/yql/providers/common/ut_helpers/transport/yql
    ydb/library/yql/providers/solomon/common
    ydb/library/yql/providers/solomon/solomon_accessor/client
    ydb/library/yql/providers/solomon/solomon_accessor/client/ut_helpers
)

YQL_LAST_ABI_VERSION()

END()
