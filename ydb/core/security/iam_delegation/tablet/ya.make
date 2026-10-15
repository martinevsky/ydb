LIBRARY()

PEERDIR(
    contrib/libs/openssl
    library/cpp/string_utils/base64
    ydb/core/security/iam_delegation/policy
    ydb/core/base
    ydb/core/engine/minikql
    ydb/core/tablet_flat
    ydb/core/security/iam_delegation/tablet/protos
    ydb/core/security/iam_delegation/tablet/public
)

SRCS(
    page_token.cpp
    tablet.cpp
    tx_request.cpp
)

YQL_LAST_ABI_VERSION()

END()

RECURSE(
    protos
    public
)

RECURSE_FOR_TESTS(
    ut
)
