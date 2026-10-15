UNITTEST()

SRCS(
    async_credentials_ut.cpp
)

PEERDIR(
    ydb/library/actors/async
    ydb/library/actors/core
    ydb/library/actors/testlib
    library/cpp/threading/future
)

END()
