import logging
from datetime import timezone

import pytest

from ydb.library.yql.tools.solomon_emulator.client.client import get_read_auth_calls, set_read_auth

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)

CLUSTER_TYPES = ["solomon", "monitoring", "monium"]
TOKEN = "test-token"
WRONG_TOKEN = "wrong-token"


def auth_prefix(cluster_type):
    return "OAuth" if cluster_type == "solomon" else "Bearer"


class TestAuthTls(SolomonReadingTestBase):
    """Reads over HTTPS and gRPC over TLS with a token taken from a secret."""

    @classmethod
    def setup_class(cls):
        super().setup_class("auth_tls")

        for name, value in (("solomon_token", TOKEN), ("solomon_wrong_token", WRONG_TOKEN)):
            result, error = cls.execute_query(f'CREATE SECRET `{name}` WITH (value = "{value}")')
            assert error is None, error

    def setup_method(self):
        set_read_auth(None)
        self.first_call = len(get_read_auth_calls())

    def teardown_method(self):
        set_read_auth(None)

    def auth_calls(self):
        return get_read_auth_calls()[self.first_call:]

    def read_query(self, cluster_type, source, program):
        setting = "program" if program else "selectors"
        selectors = self.source_selectors(cluster_type, "auth_tls", {"test_type": "simple"}, program=program)
        return f"""
            SELECT * FROM `{source}`.`{self.source_table(cluster_type, "auth_tls")}` WITH (
                {setting} = @@{selectors}@@,
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
            )
        """

    def check_rows(self, result):
        rows = result[0].rows
        timestamps = [int(row["ts"].replace(tzinfo=timezone.utc).timestamp()) for row in rows]
        values = [row["value"] for row in rows]
        assert timestamps == self.simple_timestamps
        assert values == self.simple_values

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    def test_token_reaches_every_read_call(self, cluster_type):
        expected = f"{auth_prefix(cluster_type)} {TOKEN}"
        set_read_auth(expected)

        source = f"tls_token_{cluster_type}"
        self.create_source(source, cluster_type, "auth_tls", tls=True, auth_method="TOKEN", secret_name="solomon_token")

        # selectors: HTTP listing calls, then gRPC reads
        result, error = self.execute_query_once(self.read_query(cluster_type, source, program=False))
        assert error is None, self.issue_messages(error)
        self.check_rows(result)

        # program: gRPC reads only
        result, error = self.execute_query_once(self.read_query(cluster_type, source, program=True))
        assert error is None, self.issue_messages(error)
        self.check_rows(result)

        calls = self.auth_calls()
        methods = {method for method, _ in calls}
        assert "read" in methods, calls
        assert methods & {"names", "labels", "sensors"}, f"no HTTP listing calls were made: {calls}"
        assert all(value == expected for _, value in calls), calls

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_wrong_token_fails_query(self, cluster_type, program):
        set_read_auth(f"{auth_prefix(cluster_type)} {TOKEN}")

        source = f"tls_wrong_token_{cluster_type}_{int(program)}"
        self.create_source(source, cluster_type, "auth_tls", tls=True, auth_method="TOKEN", secret_name="solomon_wrong_token")

        result, error = self.execute_query_once(self.read_query(cluster_type, source, program))
        assert error is not None, "query with a wrong token succeeded"
        messages = self.issue_messages(error)
        assert "Authentication failed" in messages, messages
        # The token itself must not leak into the error.
        assert WRONG_TOKEN not in messages, messages

    @pytest.mark.parametrize("cluster_type", CLUSTER_TYPES)
    def test_plain_connection_sends_no_token(self, cluster_type):
        source = f"plain_token_{cluster_type}"
        self.create_source(source, cluster_type, "auth_tls", tls=False, auth_method="TOKEN", secret_name="solomon_token")

        result, error = self.execute_query_once(self.read_query(cluster_type, source, program=False))
        assert error is None, self.issue_messages(error)
        self.check_rows(result)

        calls = self.auth_calls()
        assert calls, "no read calls were made"
        assert all(value is None for _, value in calls), f"a token was sent without TLS: {calls}"
