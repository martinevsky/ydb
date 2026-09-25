# The other suites enable the POST listing API, while the reader defaults to the GET
# one. Rerun the listing and reading suites with the default. Modules, not classes, are
# imported, so that pytest does not collect the POST variants a second time here.
from . import basic_reading, data_paging, listing_batching, listing_paging

GET_API = {"_EnableSolomonClientPostApi": "false"}


class TestListingPagingGetApi(listing_paging.TestListingPaging):
    EXTRA_SETTINGS = GET_API


class TestListingBatchingGetApi(listing_batching.TestListingBatching):
    EXTRA_SETTINGS = GET_API


class TestBasicReadingGetApi(basic_reading.TestBasicReading):
    EXTRA_SETTINGS = GET_API


class TestDataPagingGetApi(data_paging.TestDataPaging):
    EXTRA_SETTINGS = GET_API
