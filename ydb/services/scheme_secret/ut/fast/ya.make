UNITTEST_FOR(ydb/services/scheme_secret)

SIZE(MEDIUM)
IF (SANITIZER_TYPE)
    REQUIREMENTS(cpu:2)
ELSE()
    REQUIREMENTS(cpu:2)
ENDIF()

PEERDIR(
    ydb/services/scheme_secret
    ydb/services/scheme_secret/ut/common
    ydb/core/kqp/ut/common
    ydb/core/security/iam_delegation/tablet
    ydb/core/security/iam_delegation
    ydb/core/testlib/default
    yql/essentials/sql/pg_dummy
)

SRCS(
    service_ut.cpp
    iam_delegation_reader_ut.cpp
    secret_credentials_ut.cpp
)

YQL_LAST_ABI_VERSION()

END()
