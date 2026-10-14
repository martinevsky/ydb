UNITTEST_FOR(ydb/core/security/iam_delegation/lifecycle)

FORK_SUBTESTS()
SIZE(MEDIUM)

SRCS(
    client_ut.cpp
    worker_ut.cpp
)

PEERDIR(
    ydb/core/testlib/default
    ydb/core/security/iam_delegation/tablet
)

YQL_LAST_ABI_VERSION()
END()
