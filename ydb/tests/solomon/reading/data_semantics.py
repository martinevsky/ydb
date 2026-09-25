import logging
import math

import pytest

from ydb.library.yql.tools.solomon_emulator.client.client import get_read_requests

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)

AGGREGATIONS = {
    "AVG": "GRID_AGGREGATION_AVG",
    "COUNT": "GRID_AGGREGATION_COUNT",
    "LAST": "GRID_AGGREGATION_LAST",
    "MAX": "GRID_AGGREGATION_MAX",
    "MIN": "GRID_AGGREGATION_MIN",
    "SUM": "GRID_AGGREGATION_SUM",
    "DEFAULT_AGGREGATION": "GRID_AGGREGATION_UNSPECIFIED",
}
FILLS = {
    "NONE": "GAP_FILLING_NONE",
    "NULL": "GAP_FILLING_NULL",
    "PREVIOUS": "GAP_FILLING_PREVIOUS",
}


def text(value):
    """Labels and `type` are String columns, which the SDK returns as bytes."""
    return value.decode("utf-8") if isinstance(value, bytes) else value


def labels(row):
    return {text(key): text(value) for key, value in row["labels"].items()}


class TestDataSemantics(SolomonReadingTestBase):
    """What the rows contain, and what reaches the Read call, for various metrics and settings."""

    @classmethod
    def setup_class(cls):
        super().setup_class("data_semantics")
        cls.create_source("data_semantics", "solomon", "data_semantics")

    def query(self, test_type, extra="", program=False):
        setting = "program" if program else "selectors"
        return f"""
            SELECT * FROM data_semantics.data_semantics WITH (
                {setting} = @@{{cluster="data_semantics", service="my_service", test_type="{test_type}"}}@@,
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
                {extra}
            )
        """

    def rows(self, query):
        result, error = self.execute_query_once(query)
        assert error is None, self.issue_messages(error)
        return result[0].rows

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_nan_becomes_null(self, program):
        rows = self.rows(self.query("nan_test", ', `downsampling.disabled` = "true"', program))
        assert [row["value"] for row in rows] == [1, None, 3]

    def test_metric_types(self):
        rows = self.rows(self.query("types_test"))
        types = sorted((labels(row)["kind"], text(row["type"])) for row in rows)
        assert types == [("COUNTER", "COUNTER"), ("DGAUGE", "DGAUGE"), ("IGAUGE", "IGAUGE"), ("RATE", "RATE")]
        assert all(row["value"] == 7 for row in rows)

    def test_labels_become_columns(self):
        rows = self.rows(self.query("labels_test", ', labels = "host, dc"'))
        assert sorted((text(row["host"]), text(row["dc"]), row["value"]) for row in rows) == [("h1", "sas", 1), ("h2", "vla", 2)]
        assert "labels" not in rows[0], "with explicit labels the labels dict must not be returned"

    def test_label_aliases(self):
        # `type` and `value` clash with system columns, so they need an alias.
        rows = self.rows(self.query("labels_test", ', labels = "host as type_host, dc as value_dc"'))
        assert sorted((text(row["type_host"]), text(row["value_dc"])) for row in rows) == [("h1", "sas"), ("h2", "vla")]

    def test_labels_dict_without_labels_setting(self):
        rows = self.rows(self.query("labels_test"))
        hosts = sorted((labels(row)["host"], labels(row)["dc"]) for row in rows)
        assert hosts == [("h1", "sas"), ("h2", "vla")]

    def test_no_matching_metrics(self):
        assert self.rows(self.query("no_such_metric")) == []

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_metric_without_points_in_range(self, program):
        assert self.rows(self.query("out_of_range_test", program=program)) == []

    @pytest.mark.parametrize("aggregation", list(AGGREGATIONS))
    @pytest.mark.parametrize("fill", list(FILLS))
    def test_downsampling_reaches_read_call(self, aggregation, fill):
        first_request = len(get_read_requests())
        self.rows(self.query("nan_test", f"""
            , `downsampling.aggregation` = "{aggregation}"
            , `downsampling.fill` = "{fill}"
            , `downsampling.grid_interval` = "30"
        """, program=True))

        requests = get_read_requests()[first_request:]
        assert requests, "no Read calls were made"
        for request in requests:
            assert request["downsampling"] == {
                "disabled": False,
                "grid_interval": 30000,
                "aggregation": AGGREGATIONS[aggregation],
                "fill": FILLS[fill],
            }

    def test_downsampling_defaults(self):
        first_request = len(get_read_requests())
        self.rows(self.query("nan_test", program=True))
        for request in get_read_requests()[first_request:]:
            assert request["downsampling"] == {
                "disabled": False,
                "grid_interval": 15000,
                "aggregation": "GRID_AGGREGATION_AVG",
                "fill": "GAP_FILLING_PREVIOUS",
            }

    def test_disabled_downsampling_reaches_read_call(self):
        first_request = len(get_read_requests())
        self.rows(self.query("nan_test", ', `downsampling.disabled` = "true"', program=True))
        for request in get_read_requests()[first_request:]:
            assert request["downsampling"]["disabled"] is True

    def test_large_grid_interval_does_not_overflow(self):
        # 5'000'000 s is about 58 days: in milliseconds it does not fit into 32 bits.
        grid_sec = 5_000_000
        assert grid_sec * 1000 > 2 ** 32
        first_request = len(get_read_requests())
        self.rows(self.query("nan_test", f', `downsampling.grid_interval` = "{grid_sec}"', program=True))
        for request in get_read_requests()[first_request:]:
            assert request["downsampling"]["grid_interval"] == grid_sec * 1000

    def test_range_reaches_read_call(self):
        first_request = len(get_read_requests())
        self.rows(self.query("nan_test", ', `downsampling.disabled` = "true"', program=True))
        requests = get_read_requests()[first_request:]
        assert requests
        # program mode reads exactly the requested range
        assert all((r["from_ms"], r["to_ms"]) == (0, 60000) for r in requests), requests

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_with_schema(self, program):
        rows = self.rows(self.query("nan_test", """
            , `downsampling.disabled` = "true"
            , SCHEMA (ts Datetime NOT NULL, value Double)
        """, program))
        assert [row["value"] for row in rows] == [1, None, 3]

    def test_value_column_is_nullable_double(self):
        rows = self.rows(self.query("types_test"))
        assert all(isinstance(row["value"], float) and not math.isnan(row["value"]) for row in rows)
