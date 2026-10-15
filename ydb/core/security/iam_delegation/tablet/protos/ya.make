PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

PEERDIR(ydb/public/api/protos)

EXCLUDE_TAGS(GO_PROTO JAVA_PROTO)

SRCS(
    iam_delegation.proto
    inventory_page_token.proto
)

GENERATE_ENUM_SERIALIZATION(iam_delegation.pb.h)

END()
