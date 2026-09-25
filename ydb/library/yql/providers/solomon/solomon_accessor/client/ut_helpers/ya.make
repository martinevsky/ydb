LIBRARY()

# H4 TFakeDataService (test_plan.md §3): an in-process gRPC Monitoring DataService for the Solomon
# accessor tests. Framework-neutral (no unittest/gtest), see
# ydb/library/yql/providers/common/ut_helpers/transport/README.md.

SRCS(
    fake_data_service.cpp
)

PEERDIR(
    contrib/libs/grpc
    ydb/library/yql/providers/common/ut_helpers/transport
    ydb/library/yql/providers/solomon/solomon_accessor/grpc
)

END()
