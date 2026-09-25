LIBRARY()

SRCS(
    secret_credentials.cpp
    service.cpp
)

PEERDIR(
    library/cpp/retry
    library/cpp/threading/future
    ydb/core/base
    ydb/core/kqp/common/events
    ydb/core/protos
    ydb/core/security/iam_delegation
    ydb/core/tx/scheme_board
    ydb/core/tx/scheme_cache
    ydb/core/tx/schemeshard
    ydb/core/tx/tx_proxy
    ydb/library/aclib
    ydb/library/actors/core
    ydb/library/services
    ydb/library/yql/providers/common/token_accessor/client
    ydb/public/sdk/cpp/src/client/types/credentials
    ydb/services/metadata/secret
    ydb/services/metadata
    yql/essentials/providers/common/structured_token
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
