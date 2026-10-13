LIBRARY()

SRCS(
    dq_fake_ca.cpp
)

PEERDIR(
    ydb/library/yql/providers/common/token_accessor/client
    library/cpp/retry
    library/cpp/testing/unittest
    ydb/library/actors/testlib
    ydb/library/services
    ydb/library/yql/dq/actors/compute
    ydb/library/yql/dq/actors/protos
    yql/essentials/minikql
    yql/essentials/minikql/computation
    yql/essentials/minikql/invoke_builtins
)

YQL_LAST_ABI_VERSION()

END()
