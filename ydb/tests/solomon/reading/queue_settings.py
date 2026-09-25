import concurrent.futures
import logging

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)

QUERY_DEADLINE_SEC = 60


class TestPrefetchBelowBatchLimit(SolomonReadingTestBase):
    """Metrics queue with MetricsQueuePrefetchSize below MetricsQueueBatchCountLimit.

    Both are user settings; a smaller prefetch than batch must not stall the queue.
    Its own file, so a hang times out only this class.
    """

    EXTRA_SETTINGS = {
        "MetricsQueuePrefetchSize": 2,
        "MetricsQueueBatchCountLimit": 10,
    }

    @classmethod
    def setup_class(cls):
        super().setup_class("listing_pages")
        cls.create_source("queue_settings", "solomon", "listing_pages")

    def test_selectors_read_finishes(self):
        query = """
            SELECT test_label FROM queue_settings.listing_pages WITH (
                selectors = @@{cluster="listing_pages", service="my_service", test_type="listing_pages_test"}@@,
                labels = "test_label",
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
            )
        """
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(self.execute_query_once, query)
            try:
                result, error = future.result(timeout=QUERY_DEADLINE_SEC)
            except concurrent.futures.TimeoutError:
                raise AssertionError(f"the read did not finish in {QUERY_DEADLINE_SEC} s")

        assert error is None, self.issue_messages(error)
        labels = sorted(int(row["test_label"]) for row in result[0].rows)
        assert labels == list(range(self.listing_pages_metrics_size))


class TestPrefetchOneDefaultBatch(TestPrefetchBelowBatchLimit):
    EXTRA_SETTINGS = {
        "MetricsQueuePrefetchSize": 1,
        "MetricsQueueBatchCountLimit": 500,
    }
