LIBRARY()

SRCS(
    kqp_iam_delegation_records.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/protos
    ydb/core/security/iam_delegation
    ydb/core/tx/scheme_cache
    ydb/library/actors/async
    ydb/library/actors/core
    ydb/library/query_actor
    ydb/library/services
    ydb/library/table_creator
    ydb/public/sdk/cpp/src/client/params
    ydb/public/sdk/cpp/src/client/result
)

YQL_LAST_ABI_VERSION()

END()
