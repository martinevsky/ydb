#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import requests
from retry import retry_call

timeout = 15
max_tries = 5
retry_delay = 2


def get_api_url():
    return "http://localhost:{}".format(os.environ['SOLOMON_HTTP_PORT'])


def _do_request_inner(method, url, json):
    resp = requests.request(method=method, url=url, timeout=timeout, json=json)
    resp.raise_for_status()
    return resp


def _do_request(method, url, json=None):
    return retry_call(_do_request_inner, fkwargs={"method": method, "url": url, "json": json}, tries=max_tries, delay=2)


def config_solomon(response_code):  # deprecated
    pass


def cleanup_emulator():
    _do_request("POST", "{url}/cleanup".format(url=get_api_url()))


def cleanup_solomon(project, cluster, service):
    url = "{url}/cleanup?project={project}&cluster={cluster}&service={service}".format(
        url=get_api_url(),
        project=project,
        cluster=cluster,
        service=service)
    _do_request("POST", url)


def cleanup_monitoring(folderId, service):
    cleanup_solomon(folderId, folderId, service)


def add_solomon_metrics(project, cluster, service, metrics):
    url = "{url}/metrics/post?project={project}&cluster={cluster}&service={service}".format(
        url=get_api_url(),
        project=project,
        cluster=cluster,
        service=service)
    _do_request("POST", url, metrics)


def add_monitoring_metrics(folderId, service, metrics):
    return add_solomon_metrics(folderId, folderId, service, metrics)


def fail_solomon_push(project, cluster, service, count=1):
    """Make the emulator answer the next ``count`` pushes to the shard with a retriable error."""
    url = "{url}/fail/push?project={project}&cluster={cluster}&service={service}&count={count}".format(
        url=get_api_url(),
        project=project,
        cluster=cluster,
        service=service,
        count=count)
    _do_request("POST", url)


def get_solomon_metrics(project, cluster, service):
    url = "{url}/metrics/get?project={project}&cluster={cluster}&service={service}".format(
        url=get_api_url(),
        project=project,
        cluster=cluster,
        service=service)
    return sorted(_do_request("GET", url).json(), key=lambda x : x['ts'])


def get_monitoring_metrics(folderId, service):
    return get_solomon_metrics(folderId, folderId, service)


def get_api_calls_count():
    url = "{}/api/calls".format(get_api_url())
    return _do_request("GET", url).json()["api_calls"]


def cleanup_api_calls():
    url = "{}/cleanup/api/calls".format(get_api_url())
    _do_request("POST", url)


def set_read_auth(expected):
    """Make every read call require this exact Authorization value, None accepts anything."""
    _do_request("POST", "{}/config/read_auth".format(get_api_url()), {"expected": expected})


def get_read_auth_calls():
    """(method, Authorization value) of every read call since the last cleanup."""
    return [tuple(c) for c in _do_request("GET", "{}/api/read_auth".format(get_api_url())).json()["calls"]]


def fail_read(method, count=1, **fault):
    """Answer the next ``count`` read calls of ``method`` with a fault.

    ``method`` is one of "names", "labels", "sensors", "data" (HTTP) or "read" (gRPC).
    ``fault`` keys: delay_ms, status and message (HTTP), grpc_code and message (gRPC),
    mode ("malformed" for HTTP, "mismatch" for gRPC).
    """
    _do_request("POST", "{}/fail/read".format(get_api_url()), dict(fault, method=method, count=count))


def get_read_requests():
    """Parameters of every gRPC Read call since the last cleanup."""
    return _do_request("GET", "{}/api/read_requests".format(get_api_url())).json()["requests"]
