LIBRARY()

SRCS(
    setup_failure.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/security/iam_delegation/policy
    ydb/core/security/iam_delegation/tablet/protos
    ydb/library/actors/core
)

END()
