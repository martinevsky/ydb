LIBRARY()

SRCS(
    client.cpp
    schema.cpp
    worker.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/tx/iam_delegation/public
    ydb/core/tx/schemeshard
    ydb/core/tx/tx_proxy
    ydb/library/aclib
    ydb/library/actors/async
    ydb/library/actors/core
)

END()

RECURSE_FOR_TESTS(ut)
