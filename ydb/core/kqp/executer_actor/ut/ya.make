UNITTEST_FOR(ydb/core/kqp/executer_actor)

SIZE(MEDIUM)
REQUIREMENTS(cpu:4)
FORK_SUBTESTS()
SPLIT_FACTOR(8)

SRCS(
    kqp_executer_stats_ut.cpp
    kqp_executer_ut.cpp
    kqp_iam_delegation_secret_orchestrator_ut.cpp
    kqp_tasks_graph_ut.cpp
    max_tasks_graph_ut.cpp
)

PEERDIR(
    ydb/core/kqp/common
    ydb/core/security/iam_delegation
    ydb/core/tx/tx_proxy
    ydb/library/aclib
    ydb/core/kqp/ut/common
    ydb/library/testlib/service_mocks
    ydb/services/scheme_secret/ut/common
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
