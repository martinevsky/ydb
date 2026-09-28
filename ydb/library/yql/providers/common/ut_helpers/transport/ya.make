LIBRARY()

# Framework-neutral transport test harness (see README.md). Depends only on util, library/cpp and
# OpenSSL: no ydb/core, no unittest/gtest.

SRCS(
    counters_inspector.cpp
    loopback_http_server.cpp
    refusing_port.cpp
    test_pki.cpp
    wait.cpp
)

IF (OS_LINUX)
    SRCS(
        blackhole.cpp
    )
ENDIF()

PEERDIR(
    contrib/libs/openssl
    library/cpp/monlib/dynamic_counters
)

END()

RECURSE(
    credentials
    yql
)
