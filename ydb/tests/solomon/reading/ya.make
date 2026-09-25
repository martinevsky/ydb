PY3TEST()
ENV(YDB_DRIVER_BINARY="ydb/apps/ydbd/ydbd")

FORK_TEST_FILES()

TEST_SRCS(
    auth_tls.py
    backpressure_test.py
    base.py
    basic_reading.py
    data_paging.py
    data_semantics.py
    get_api.py
    listing_batching.py
    listing_pages.py
    listing_paging.py
    monium.py
    points_count.py
    queue_settings.py
    settings_validation.py
    settings_validation_edge_cases.py
    transport_errors.py
)

SIZE(MEDIUM)
IF (SANITIZER_TYPE)
    REQUIREMENTS(cpu:2)
ENDIF()

INCLUDE(${ARCADIA_ROOT}/ydb/library/yql/tools/solomon_emulator/recipe/recipe.inc)

DEPENDS(
    ydb/apps/ydbd
)

PEERDIR(
    ydb/library/yql/tools/solomon_emulator/client
    ydb/tests/library
    ydb/tests/library/test_meta
)

END()
