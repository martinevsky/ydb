UNITTEST_FOR(ydb/core/security/iam_delegation/tablet)

FORK_SUBTESTS()

SIZE(MEDIUM)

PEERDIR(
    yql/essentials/ast
    ydb/core/testlib/default
)

SRCS(
    cold_scan_ut.cpp
    durability_ut.cpp
    iam_delegation_ut.cpp
    lifecycle_recovery_ut.cpp
    recovery_commands_ut.cpp
    recovery_chaos_ut.cpp
    revocation_boundaries_ut.cpp
    storage_reopen_ut.cpp
)

YQL_LAST_ABI_VERSION()

END()
