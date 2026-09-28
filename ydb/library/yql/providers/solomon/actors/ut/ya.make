UNITTEST_FOR(ydb/library/yql/providers/solomon/actors)

IF (SANITIZER_TYPE)
    SIZE(MEDIUM)
ELSE()
    SIZE(SMALL)
ENDIF()

# One process per test: keeps every chunk well under the SMALL 60 s limit (the write tests alone took
# about 51 s in one chunk) and isolates the transport tests' servers and actor runtimes.
FORK_SUBTESTS()

INCLUDE(${ARCADIA_ROOT}/ydb/library/yql/tools/solomon_emulator/recipe/recipe.inc)

SRCS(
    dq_solomon_metrics_queue_ut.cpp
    dq_solomon_write_actor_transport_ut.cpp
    dq_solomon_write_actor_ut.cpp
    ut_helpers.cpp
)

PEERDIR(
    library/cpp/cgiparam
    library/cpp/http/simple
    library/cpp/logger
    library/cpp/retry
    ydb/library/testlib/solomon_helpers
    ydb/library/yql/providers/common/token_accessor/client/ut_helpers
    ydb/library/yql/providers/common/ut_helpers
    ydb/library/yql/providers/common/ut_helpers/transport
    yql/essentials/minikql/computation/llvm16
    yql/essentials/minikql/comp_nodes/llvm16
    yql/essentials/providers/common/comp_nodes
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/sql
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
