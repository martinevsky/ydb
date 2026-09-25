LIBRARY()

# H10 TFakeCredentialsProvider: separate from the core harness because NYdb::ICredentialsProvider
# comes with the YDB SDK credentials target (and its gRPC API protos).

SRCS(
    fake_credentials.cpp
)

PEERDIR(
    ydb/public/sdk/cpp/src/client/types/credentials
)

END()
