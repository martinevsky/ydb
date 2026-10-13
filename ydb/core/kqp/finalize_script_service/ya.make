LIBRARY()

SRCS(
    kqp_check_script_lease_actor.cpp
    kqp_finalize_script_actor.cpp
    kqp_finalize_script_service.cpp
    kqp_script_secrets.cpp
)

PEERDIR(
    ydb/services/scheme_secret
    ydb/core/kqp/counters
    ydb/core/kqp/federated_query/actors
    ydb/core/kqp/proxy_service
    ydb/core/mind
    ydb/core/tx/scheme_cache
    ydb/library/table_creator
    ydb/library/yql/providers/s3/actors_factory
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
