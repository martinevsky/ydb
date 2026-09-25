import logging

from ydb.library.yql.tools.solomon_emulator.client.client import set_listing_page_size

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)


class TestListingPages(SolomonReadingTestBase):
    """The api may return fewer metrics per page than asked for and report more pages."""

    @classmethod
    def setup_class(cls):
        super().setup_class("listing_pages")
        cls.create_source("listing_pages", "solomon", "listing_pages")

    def teardown_method(self):
        set_listing_page_size(None)

    def test_every_page_is_read(self):
        set_listing_page_size(self.listing_pages_metrics_size // 3)

        result, error = self.execute_query_once("""
            SELECT test_label FROM listing_pages.listing_pages WITH (
                selectors = @@{cluster="listing_pages", service="my_service", test_type="listing_pages_test"}@@,
                labels = "test_label",
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
            )
        """)
        assert error is None, self.issue_messages(error)

        labels = sorted(int(row["test_label"]) for row in result[0].rows)
        assert labels == list(range(self.listing_pages_metrics_size)), "metrics beyond the first page are lost"
