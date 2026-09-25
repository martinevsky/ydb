PY3TEST()
INCLUDE(${ARCADIA_ROOT}/ydb/tests/harness_dep.inc)

FORK_TEST_FILES()
FORK_TESTS()
FORK_SUBTESTS()
SPLIT_FACTOR(10)

TEST_SRCS(
    test_secrets.py
)

SIZE(LARGE)
REQUIREMENTS(cpu:4)
REQUIREMENTS(ram:16)
INCLUDE(${ARCADIA_ROOT}/ydb/tests/large.inc)

DEPENDS(
    ydb/tests/library/compatibility/binaries
)

INCLUDE(${ARCADIA_ROOT}/ydb/tests/fq/streaming_common/vm_metadata_emulator/recipe/recipe.inc)
INCLUDE(${ARCADIA_ROOT}/ydb/tests/fq/streaming_common/iam_grpc_emulator/recipe/recipe.inc)

PEERDIR(
    ydb/tests/library
    ydb/tests/library/compatibility
    ydb/tests/tools/datastreams_helpers
)

END()
