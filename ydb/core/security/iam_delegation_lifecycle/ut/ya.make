UNITTEST_FOR(ydb/core/security/iam_delegation_lifecycle)

FORK_SUBTESTS()
SIZE(MEDIUM)

SRCS(
    client_ut.cpp
    worker_ut.cpp
)

PEERDIR(
    ydb/core/testlib/default
    ydb/core/tx/iam_delegation
)

YQL_LAST_ABI_VERSION()
END()
