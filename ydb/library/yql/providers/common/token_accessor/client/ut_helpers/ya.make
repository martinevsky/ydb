LIBRARY()

# H5 TFakeTokenAccessor (test_plan.md §3): an in-process gRPC TokenAccessorService for the token
# accessor client tests. Framework-neutral (no unittest/gtest), see
# ydb/library/yql/providers/common/ut_helpers/transport/README.md.

SRCS(
    fake_token_accessor.cpp
)

PEERDIR(
    contrib/libs/grpc
    ydb/library/yql/providers/common/token_accessor/grpc
)

END()
