import logging
from datetime import timezone

import pytest

from ydb.library.yql.tools.solomon_emulator.client.client import clear_read_faults, fail_read, get_read_requests

from .base import SolomonReadingTestBase

logger = logging.getLogger(__name__)

# The reader retries these HTTP codes (NConstants::RetriableHttpCodes) and these gRPC codes.
RETRIABLE_HTTP_STATUSES = [429, 502, 503, 504]
RETRIABLE_GRPC_CODES = ["UNAVAILABLE", "RESOURCE_EXHAUSTED", "DEADLINE_EXCEEDED", "INTERNAL", "ABORTED"]
FATAL_GRPC_CODES = ["PERMISSION_DENIED", "INVALID_ARGUMENT", "NOT_FOUND", "UNAUTHENTICATED"]
# HTTP read calls of a `selectors` query: label names and labels at planning time,
# labels and metrics listing in the metrics queue.
HTTP_METHODS = ["names", "labels", "sensors"]
DATA_REQUEST_TIMEOUT_MS = 1000


class TestTransportErrors(SolomonReadingTestBase):
    """Faults of the Solomon API during reads: what is retried and what fails the query."""

    @classmethod
    def setup_class(cls):
        super().setup_class("transport_errors", extra_settings={"DataRequestTimeoutMs": DATA_REQUEST_TIMEOUT_MS})
        cls.create_source("transport_errors", "solomon", "transport_errors")

    def teardown_method(self):
        clear_read_faults()

    def read_query(self, program=False):
        setting = "program" if program else "selectors"
        selectors = self.source_selectors("solomon", "transport_errors", {"test_type": "simple"}, program=program)
        return f"""
            SELECT * FROM transport_errors.transport_errors WITH (
                {setting} = @@{selectors}@@,
                from = "1970-01-01T00:00:00Z",
                to = "1970-01-01T00:01:00Z"
            )
        """

    def check_success(self, query):
        result, error = self.execute_query_once(query)
        assert error is None, self.issue_messages(error)
        rows = result[0].rows
        assert [int(row["ts"].replace(tzinfo=timezone.utc).timestamp()) for row in rows] == self.simple_timestamps
        assert [row["value"] for row in rows] == self.simple_values

    def check_failure(self, query, *expected_messages):
        result, error = self.execute_query_once(query)
        assert error is not None, "query succeeded, expected it to fail"
        messages = self.issue_messages(error)
        for expected in expected_messages:
            assert expected in messages, messages

    @pytest.mark.parametrize("method", HTTP_METHODS)
    @pytest.mark.parametrize("status", RETRIABLE_HTTP_STATUSES)
    def test_http_retriable_status_is_retried(self, method, status):
        fail_read(method, count=2, status=status)
        self.check_success(self.read_query())

    @pytest.mark.parametrize("method", HTTP_METHODS)
    @pytest.mark.parametrize("status", [400, 401, 403, 404, 500])
    def test_http_fatal_status_fails_query(self, method, status):
        fail_read(method, count=1, status=status, message=f"Injected {method} {status}")
        # The status code and the "message" field of the JSON body reach the user.
        self.check_failure(self.read_query(), f"HTTP {status}", f"Injected {method} {status}")

    @pytest.mark.parametrize("method", HTTP_METHODS)
    def test_http_retries_are_bounded(self, method):
        fail_read(method, count=100, status=503, message="Still unavailable")
        self.check_failure(self.read_query(), "HTTP 503", "Still unavailable")

    @pytest.mark.parametrize("method", HTTP_METHODS)
    def test_http_malformed_response_fails_query(self, method):
        fail_read(method, count=1, mode="malformed")
        self.check_failure(self.read_query(), "is not a valid json")

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    @pytest.mark.parametrize("code", RETRIABLE_GRPC_CODES)
    def test_grpc_retriable_code_is_retried(self, code, program):
        fail_read("read", count=2, grpc_code=code)
        self.check_success(self.read_query(program))

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    @pytest.mark.parametrize("code", FATAL_GRPC_CODES)
    def test_grpc_fatal_code_fails_query(self, code, program):
        fail_read("read", count=1, grpc_code=code, message=f"Injected read {code}")
        self.check_failure(self.read_query(program), f"Injected read {code}")

    def test_grpc_retries_are_bounded(self):
        fail_read("read", count=1000, grpc_code="UNAVAILABLE", message="Still unavailable")
        self.check_failure(self.read_query(), "Still unavailable")

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_grpc_hung_call_times_out_and_is_retried(self, program):
        first_request = len(get_read_requests())
        # Hangs well past the deadline, then answers normally. The reader must not wait:
        # it gets DEADLINE_EXCEEDED after DataRequestTimeoutMs and asks again.
        fail_read("read", count=1, delay_ms=DATA_REQUEST_TIMEOUT_MS * 5)
        self.check_success(self.read_query(program))
        assert len(get_read_requests()) - first_request >= 2, "the hung call was not retried"

    @pytest.mark.parametrize("program", [False, True], ids=["selectors", "program"])
    def test_grpc_inconsistent_response_fails_query(self, program):
        fail_read("read", count=1, mode="mismatch")
        self.check_failure(self.read_query(program), "timestamps but")

        # The node survived the broken response and keeps serving reads.
        self.check_success(self.read_query(program))
