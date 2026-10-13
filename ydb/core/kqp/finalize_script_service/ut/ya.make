UNITTEST_FOR(ydb/core/kqp/finalize_script_service)

SIZE(MEDIUM)
REQUIREMENTS(cpu:2)

SRCS(
    kqp_script_secrets_ut.cpp
)

PEERDIR(
    ydb/core/tx/iam_delegation
    ydb/core/testlib/default
    ydb/services/scheme_secret
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
