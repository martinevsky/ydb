import logging
from datetime import datetime, timezone

from ydb.library.yql.tools.solomon_emulator.client.client import (
    clear_read_faults, fail_read, get_read_auth_calls, get_read_requests)

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)


def iso(seconds):
    return datetime.fromtimestamp(seconds, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class TestPointsCount(SolomonReadingTestBase):
    """Reads of recent data with downsampling disabled.

    For the last 7 days the reader cannot compute the number of points from the grid,
    so it asks the API (`count(...)` over /sensors/data) and splits the range by it.
    The emulator rejects any data call with more than 10000 points.
    """

    @classmethod
    def setup_class(cls):
        super().setup_class("points_count")
        cls.create_source("points_count", "solomon", "points_count")

    def teardown_method(self):
        clear_read_faults()

    def read_query(self, test_type):
        return f"""
            SELECT * FROM points_count.points_count WITH (
                selectors = @@{{cluster="points_count", service="my_service", test_type="{test_type}"}}@@,
                `downsampling.disabled` = "true",
                from = "{iso(self.points_count_from)}",
                to = "{iso(self.points_count_to + 1)}"
            )
        """

    def test_recent_range_is_split_by_points_count(self):
        first_call = len(get_read_auth_calls())
        first_request = len(get_read_requests())

        result, error = self.execute_query_once(self.read_query("points_count_test"))
        assert error is None, self.issue_messages(error)

        rows = result[0].rows
        values = sorted(int(row["value"]) for row in rows)
        assert values == list(range(self.points_count_size)), "points are lost or duplicated"

        methods = [method for method, _ in get_read_auth_calls()[first_call:]]
        assert "data" in methods, f"points count was not requested: {methods}"

        requests = get_read_requests()[first_request:]
        assert len(requests) >= 3, f"25000 points must take at least 3 data calls, made {len(requests)}"
        ranges = sorted((r["from_ms"], r["to_ms"]) for r in requests)
        for (_, prev_to), (next_from, _) in zip(ranges, ranges[1:]):
            assert prev_to <= next_from, f"data calls overlap: {ranges}"

    def test_metric_without_recent_points(self):
        # The API answers the count with "Not able to apply function count on vector
        # with size 0", which the reader treats as no points.
        result, error = self.execute_query_once(self.read_query("empty_recent_test"))
        assert error is None, self.issue_messages(error)
        assert len(result[0].rows) == 0

    def test_points_count_error_fails_query(self):
        fail_read("data", count=1, status=500, message="Injected points count failure")
        result, error = self.execute_query_once(self.read_query("points_count_test"))
        assert error is not None, "query succeeded, expected it to fail"
        messages = self.issue_messages(error)
        assert "Injected points count failure" in messages, messages

    def test_points_count_retriable_error_is_retried(self):
        fail_read("data", count=2, status=503)
        result, error = self.execute_query_once(self.read_query("points_count_test"))
        assert error is None, self.issue_messages(error)
        assert len(result[0].rows) == self.points_count_size
