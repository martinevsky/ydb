UNITTEST_FOR(ydb/core/grpc_services)

FORK_SUBTESTS()

IF (SANITIZER_TYPE)
    SIZE(MEDIUM)
ENDIF()

SRCS(
    rpc_calls_ut.cpp
    rpc_describe_secret_ut.cpp
    rpc_list_database_iam_delegations_ut.cpp
    rpc_load_rows_ut.cpp
    operation_helpers_ut.cpp
)

PEERDIR(
    library/cpp/getopt
    library/cpp/regex/pcre
    library/cpp/svnversion
    ydb/core/client/scheme_cache_lib
    ydb/core/formats/arrow
    ydb/core/scheme
    ydb/core/testlib/default
)

END()
