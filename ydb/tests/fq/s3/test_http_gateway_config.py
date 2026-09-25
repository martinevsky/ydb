#!/usr/bin/env python
# -*- coding: utf-8 -*-

# FQ transport test plan, T-CFG-7 (F-A-2): a node running both FQ and KQP gets two HttpGateway
# configs (FederatedQueryConfig.Gateways.HttpGateway and QueryServiceConfig.HttpGateway), but the curl
# gateway is a process-wide singleton: the component that makes it first wins and the other config is
# ignored. The decision-gated contract (DG-T-CFG-7) is skipped unless YDB_TRANSPORT_RUN_KNOWN_BUGS=1.

import os

import pytest

from ydb.tests.fq.s3.conftest import get_kikimr_extensions
from ydb.tests.tools.fq_runner.kikimr_utils import ExtensionPoint
from ydb.tests.tools.fq_runner.kikimr_utils import start_kikimr
from ydb.tests.tools.fq_runner.kikimr_utils import YQV1_VERSION_NAME

FQ_MAX_IN_FLIGHT = 7
KQP_MAX_IN_FLIGHT = 3
IGNORED_CONFIG_WARNING = "HTTP gateway is already created, the new configuration is ignored"


def skip_known_bug(bug_id):
    if os.getenv("YDB_TRANSPORT_RUN_KNOWN_BUGS") != "1":
        pytest.skip("known bug {}".format(bug_id))


class HttpGatewayConfigExtension(ExtensionPoint):
    def is_applicable(self, request):
        return True

    def apply_to_kikimr(self, request, kikimr):
        kikimr.compute_plane.fq_config['gateways']['http_gateway'] = {'max_in_flight_count': FQ_MAX_IN_FLIGHT}
        kikimr.compute_plane.qs_config['http_gateway'] = {'max_in_flight_count': KQP_MAX_IN_FLIGHT}


@pytest.fixture(scope="module")
def kikimr_http_gateway(kikimr_params, s3, kikimr_settings, mvp_external_ydb_endpoint):
    extensions = get_kikimr_extensions(s3, YQV1_VERSION_NAME, kikimr_settings, mvp_external_ydb_endpoint)
    extensions.append(HttpGatewayConfigExtension())
    with start_kikimr(kikimr_params, extensions) as kikimr:
        yield kikimr


def max_in_flight_values(kikimr, counters):
    """MaxInFlight gauges of the http_gateway subcomponent in a counters group of node 1."""
    metrics = kikimr.compute_plane.get_sensors(1, counters)
    values = []
    for sensor in metrics.data:
        labels = sensor["labels"]
        if labels.get("subcomponent") == "http_gateway" and labels.get("sensor") == "MaxInFlight":
            values.append(sensor["value"])
    return values


def node_logs(kikimr):
    text = ""
    for node in kikimr.compute_plane.kikimr_cluster.nodes.values():
        for path in (node.stderr_file_name, node.ydbd_log_file_path):
            if path and os.path.isfile(path):
                with open(path, errors="replace") as f:
                    text += f.read()
    return text


class TestHttpGatewayConfig:
    # Pin of today's behaviour: one gateway per process. Its gauges live in the counters of the component
    # that made it ("yq" for FQ, "utils" for KQP); the other group has none. Written, not run (ydbd).
    def test_single_gateway_per_node(self, kikimr_http_gateway):
        fq_values = max_in_flight_values(kikimr_http_gateway, "yq")
        kqp_values = max_in_flight_values(kikimr_http_gateway, "utils")
        assert len(fq_values) + len(kqp_values) <= 1, "fq: {}, kqp: {}".format(fq_values, kqp_values)
        for value in fq_values + kqp_values:
            assert value in (FQ_MAX_IN_FLIGHT, KQP_MAX_IN_FLIGHT), "MaxInFlight {}".format(value)

    # DG-T-CFG-7 (F-A-2): the ignored config is reported (the WARN of T-CFG-4) and both components'
    # http_gateway sensors are populated with the effective configuration.
    def test_ignored_config_is_reported(self, kikimr_http_gateway):
        skip_known_bug("DG-T-CFG-7")
        assert IGNORED_CONFIG_WARNING in node_logs(kikimr_http_gateway)
        fq_values = max_in_flight_values(kikimr_http_gateway, "yq")
        kqp_values = max_in_flight_values(kikimr_http_gateway, "utils")
        assert len(fq_values) == 1 and len(kqp_values) == 1, "fq: {}, kqp: {}".format(fq_values, kqp_values)
        assert fq_values == kqp_values
