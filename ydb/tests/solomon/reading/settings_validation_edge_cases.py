import logging

import pytest

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)

CLUSTER_TYPES = ["solomon", "monitoring", "monium"]


class TestSettingsValidationEdgeCases(SolomonReadingTestBase):
    """Values on the edge of what the read settings accept, for every cluster type."""

    @classmethod
    def setup_class(cls):
        super().setup_class("settings_validation")
        for cluster_type in CLUSTER_TYPES:
            cls.create_source(f"edge_{cluster_type}", cluster_type, "settings_validation")

    def query(self, cluster_type, settings, program=False):
        setting = "program" if program else "selectors"
        selectors = self.source_selectors(cluster_type, "settings_validation", {"test_type": "setting_validation"}, program=program)
        table = self.source_table(cluster_type, "settings_validation")
        return f"""
            SELECT * FROM `edge_{cluster_type}`.`{table}` WITH (
                {setting} = @@{selectors}@@
                {settings}
            )
        """

    def check_error(self, query, expected):
        result, error = self.execute_query_once(query)
        assert error is not None, "query succeeded, expected: {}".format(expected)
        messages = self.issue_messages(error)
        assert expected in messages, messages

    def check_ok(self, query):
        result, error = self.execute_query_once(query)
        assert error is None, self.issue_messages(error)
        return result[0].rows

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    @pytest.mark.parametrize("value", ["0", "-1", "1.5", "4294967296"])
    def test_grid_interval_must_be_positive_integer(self, cluster_type, program, value):
        self.check_error(
            self.query(cluster_type, f', `downsampling.grid_interval` = "{value}"', program),
            "downsampling.grid_interval must be positive number")

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    @pytest.mark.parametrize("setting", [
        '`downsampling.aggregation` = "MAX"',
        '`downsampling.fill` = "NULL"',
        '`downsampling.grid_interval` = "30"',
    ], ids=["aggregation", "fill", "grid_interval"])
    def test_single_downsampling_setting_with_disabled_downsampling(self, cluster_type, program, setting):
        self.check_error(
            self.query(cluster_type, f', `downsampling.disabled` = "true", {setting}', program),
            "downsampling.disabled must be false if downsampling.aggregation, downsampling.fill or downsampling.grid_interval is specified")

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_from_later_than_to(self, cluster_type, program):
        self.check_error(
            self.query(cluster_type, ', from = "2025-01-02T00:00:00Z", to = "2025-01-01T00:00:00Z"', program),
            "`from` must not be later than `to`")

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_future_range_is_empty(self, cluster_type, program):
        # Both ends are clamped to now: an empty range, not an error.
        rows = self.check_ok(self.query(cluster_type, ', from = "9998-01-01T00:00:00Z", to = "9999-01-01T00:00:00Z"', program))
        assert len(rows) == 0

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("value", [
        "1970-01-12T13:46:40Z",
        "1970-01-12T13:46:40.000Z",
        "1970-01-12T16:46:40+03:00",
    ], ids=["utc", "fraction", "offset"])
    def test_iso8601_variants(self, cluster_type, value):
        # The seeded point is at 1'000'000 s. Every variant names that moment, so a
        # one second range starting there finds the point.
        rows = self.check_ok(self.query(
            cluster_type,
            f', `downsampling.disabled` = "true", from = "{value}", to = "1970-01-12T13:46:41Z"'))
        assert len(rows) == 1, rows

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("aggregation", ["AVG", "COUNT", "DEFAULT_AGGREGATION", "LAST", "MAX", "MIN", "SUM"])
    def test_every_aggregation_is_accepted(self, cluster_type, aggregation):
        self.check_ok(self.query(cluster_type, f', `downsampling.aggregation` = "{aggregation}"'))

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("fill", ["NONE", "NULL", "PREVIOUS"])
    def test_every_fill_is_accepted(self, cluster_type, fill):
        self.check_ok(self.query(cluster_type, f', `downsampling.fill` = "{fill}"'))

    def test_cluster_without_project_is_rejected(self):
        # CLUSTER only means something together with PROJECT (a cloud folder); alone it
        # would be dropped silently and the source would address another installation.
        result, error = self.execute_query_once(f"""
            CREATE EXTERNAL DATA SOURCE cluster_only WITH (
                SOURCE_TYPE = "Monium.Metrics",
                LOCATION = "{self.solomon_http_endpoint}",
                GRPC_LOCATION = "{self.solomon_grpc_endpoint}",
                CLUSTER = "settings_validation",
                AUTH_METHOD = "NONE",
                USE_TLS = "false"
            )""")
        assert error is not None, "CREATE with CLUSTER and without PROJECT succeeded"
        messages = self.issue_messages(error)
        assert "PROJECT" in messages.upper(), messages
