LIBRARY()

# Transport harness helpers that depend on YQL libraries: the curl HTTP gateway (TGatewayScope, H11)
# and the YQL logger (TLogCapture, H7). Kept apart so the core harness stays util/library-only.

SRCS(
    gateway_scope.cpp
    log_capture.cpp
)

PEERDIR(
    library/cpp/logger
    ydb/library/yql/providers/common/http_gateway
    ydb/library/yql/providers/common/ut_helpers/transport
    yql/essentials/providers/common/proto
    yql/essentials/utils/log
)

YQL_LAST_ABI_VERSION()

END()
