import logging
from datetime import timezone

import pytest

from ydb.library.yql.tools.solomon_emulator.client.client import get_read_requests

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)

# Monium: a source with PROJECT and without CLUSTER. The project goes into the Read
# container, the table name is the default `service`, `cluster` is an ordinary selector.

DOWNSAMPLING = {
    "default": "",
    "enabled": """
        , `downsampling.disabled` = "false"
        , `downsampling.aggregation` = "AVG"
        , `downsampling.fill` = "PREVIOUS"
        , `downsampling.grid_interval` = "15"
    """,
    "disabled": ', `downsampling.disabled` = "true"',
}


class TestMoniumBasicReading(SolomonReadingTestBase):
    @classmethod
    def setup_class(cls):
        super().setup_class("basic_reading")
        cls.create_source("monium", "monium", "basic_reading")

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    @pytest.mark.parametrize("downsampling", list(DOWNSAMPLING))
    def test_basic_reading_monium(self, program, downsampling):
        setting = "program" if program else "selectors"
        selectors = self.source_selectors("monium", "basic_reading", {"test_type": "basic_reading_test"}, program=program)
        first_request = len(get_read_requests())

        result, error = self.execute_query_once(f"""
            SELECT * FROM monium.my_service WITH (
                {setting} = @@{selectors}@@,
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
                {DOWNSAMPLING[downsampling]}
            )
        """)
        assert error is None, self.issue_messages(error)

        rows = result[0].rows
        timestamps = [int(row["ts"].replace(tzinfo=timezone.utc).timestamp()) for row in rows]
        values = [int(row["value"]) for row in rows]
        if downsampling == "disabled":
            expected = list(zip(self.basic_reading_timestamps, self.basic_reading_values))
        else:
            expected = [(ts, v) for ts, v in zip(self.basic_reading_timestamps, self.basic_reading_values) if ts % 15 == 0]
        assert list(zip(timestamps, values)) == expected

        requests = get_read_requests()[first_request:]
        assert requests
        assert all(r["project_id"] == "basic_reading" and r["folder_id"] is None for r in requests), requests


class TestMoniumListing(SolomonReadingTestBase):
    @classmethod
    def setup_class(cls):
        super().setup_class("listing_paging")
        cls.create_source("monium", "monium", "listing_paging")

    def test_listing_paging_monium(self):
        # More metrics than one listing page holds: the reader must split the listing.
        result, error = self.execute_query_once("""
            SELECT test_label FROM monium.my_service WITH (
                selectors = @@{cluster="listing_paging", test_type="listing_paging_test", test_label="*"}@@,
                labels = "test_label",
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
            )
        """)
        assert error is None, self.issue_messages(error)

        labels = sorted(int(row["test_label"]) for row in result[0].rows)
        assert labels == list(range(self.listing_paging_metrics_size))
