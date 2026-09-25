LIBRARY()

# HTTP gateway config of a KQP node (seam S5 of the FQ transport test plan). Kept apart from
# ydb/core/kqp/federated_query so that the unit tests of these functions do not link the whole node.

SRCS(
    kqp_http_gateway_config.cpp
)

PEERDIR(
    ydb/core/protos
    yql/essentials/providers/common/proto
)

END()
