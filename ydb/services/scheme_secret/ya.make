YQL_LIBRARY()

SRCS(
    service.cpp
    iam_delegation_reader.cpp
    secret_credentials.cpp
)

PEERDIR(
    library/cpp/json
    library/cpp/retry
    library/cpp/threading/future
    ydb/core/base
    ydb/core/kqp/common/events
    ydb/core/protos
    ydb/core/security/iam_delegation
    ydb/core/security/iam_delegation/lifecycle
    ydb/core/security/iam_delegation/tablet/public
    ydb/core/tx/scheme_board
    ydb/core/tx/scheme_cache
    ydb/core/tx/schemeshard
    ydb/core/tx/tx_proxy
    ydb/library/aclib
    ydb/library/actors/async
    ydb/library/actors/core
    ydb/library/yql/providers/common/token_accessor/client
    ydb/public/sdk/cpp/src/client/types/credentials
    yql/essentials/providers/common/structured_token
    ydb/services/metadata/secret
    ydb/services/metadata
)

END()

RECURSE_FOR_TESTS(
    ut
)
