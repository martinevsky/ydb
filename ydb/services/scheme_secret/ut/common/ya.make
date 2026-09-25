LIBRARY()

SRCS(
    helpers.cpp
)

PEERDIR(
    ydb/public/sdk/cpp/src/client/table
    ydb/library/aclib
    ydb/core/kqp/common/events
    ydb/core/kqp/ut/common
    ydb/core/security/iam_delegation
    ydb/services/scheme_secret
    ydb/core/tx/tx_proxy
)

YQL_LAST_ABI_VERSION()

END()
