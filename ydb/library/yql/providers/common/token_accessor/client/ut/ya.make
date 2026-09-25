GTEST()

# Token accessor client tests (FQ transport test plan, target D) against H5 TFakeTokenAccessor.
# One test per process: a contract test that is red today leaves gRPC completion threads blocked
# for up to RequestTimeout + 10 s, which must not eat into the next test's time budget.

SIZE(SMALL)

FORK_SUBTESTS()

SPLIT_FACTOR(5)

SRCS(
    token_accessor_client_ut.cpp
)

PEERDIR(
    library/cpp/threading/future
    ydb/library/yql/providers/common/token_accessor/client
    ydb/library/yql/providers/common/token_accessor/client/ut_helpers
    ydb/library/yql/providers/common/ut_helpers/transport
)

END()
