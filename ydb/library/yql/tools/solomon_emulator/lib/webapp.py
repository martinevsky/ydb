from aiohttp import web
import asyncio
import datetime
import json
import logging
import re
import ssl
import time
from collections import defaultdict
from concurrent import futures

from library.python.monlib.encoder import loads
from .multi_shard import MultiShard
from .shard import Shard
from .tls import generate_self_signed_cert

import grpc
from ydb.library.yql.providers.solomon.solomon_accessor.grpc.data_service_pb2_grpc import \
    DataServiceServicer, add_DataServiceServicer_to_server
from ydb.library.yql.providers.solomon.solomon_accessor.grpc.data_service_pb2 import ReadRequest, ReadResponse, MetricType, Downsampling

routes = web.RouteTableDef()
logger = logging.getLogger(__name__)

CONTENT_TYPE_SPACK = "application/x-solomon-spack"
CONTENT_TYPE_JSON = "application/json"

# Read calls that faults can be injected into: the HTTP listing calls, the HTTP points
# count call and the gRPC data call.
READ_METHODS = ("names", "labels", "sensors", "data", "read")


def _parse_selectors(selectors):
    result = dict()

    match = re.search(r".*{(.*)}.*", selectors)
    if (not match):
        return (result, False)

    group = match[1]
    if (len(group) == 0):
        return (result, True)

    for selector in group.split(","):
        eq_pos = selector.find("=")
        if (eq_pos == -1):
            return (result, False)

        key = selector[:eq_pos].strip()
        value = selector[eq_pos + 1:].strip('=').strip().strip('"')
        result[key] = value

    return (result, True)


def _parse_instant_ms(value):
    # The reader sends TInstant::ToString(), e.g. 2025-03-12T14:40:39.000000Z
    dt = datetime.datetime.strptime(value.rstrip("Z"), "%Y-%m-%dT%H:%M:%S.%f")
    return int(dt.replace(tzinfo=datetime.timezone.utc).timestamp() * 1000)


def _json_error(status, message):
    # Solomon reports errors as JSON with a "message" field, the reader surfaces only that field.
    return web.json_response({"message": message}, status=status)


class SolomonEmulator(object):
    def __init__(self, config):
        self._config = config
        self._api_calls = 0
        self._data = MultiShard()
        # Per-shard count of upcoming /api/v2/push requests that must be answered with a
        # retriable (non-terminal) error before the shard starts accepting writes again.
        # Used by tests to exercise the write actor's retry path.
        self._push_failures = {}
        self._reset_read_state()

    def _reset_read_state(self):
        # Authorization value every read call must carry, None to accept anything.
        self.read_auth = None
        # (method, Authorization value) of every read call, in arrival order.
        self.read_auth_log = []
        # Faults to answer the next read calls of each method with, see fail_read.
        self.read_faults = defaultdict(list)
        # Parameters of every gRPC Read call, in arrival order.
        self.read_requests = []

    def _get_shard(self, project, cluster, service):
        return self._data.get_or_create(project, cluster, service)

    def _handle_auth(self, request):
        if self._config.auth:
            auth_header = request.headers.get('AUTHORIZATION')
            if not self._config.auth == auth_header:
                logger.debug(f"Authorization header {auth_header} mismatches expected value {self._config.auth}")
                raise web.HTTPForbidden()

    def check_read_auth(self, method, auth_value):
        """Returns an error message if the read call must be rejected as unauthenticated."""
        self.read_auth_log.append((method, auth_value))
        if self.read_auth is not None and auth_value != self.read_auth:
            logger.debug(f"read {method}: authorization {auth_value} mismatches expected value {self.read_auth}")
            return f"Authentication failed for {method}"
        return None

    def pop_read_fault(self, method):
        faults = self.read_faults.get(method)
        if faults:
            return faults.pop(0)
        return None

    async def _read_http_prologue(self, request, method):
        """Common part of the HTTP read calls: counting, auth and injected faults.

        Returns a response to send instead of handling the call, or None.
        """
        self._api_calls += 1

        if error := self.check_read_auth(method, request.headers.get("Authorization")):
            return _json_error(401, error)

        fault = self.pop_read_fault(method)
        if fault is None:
            return None

        logger.debug(f"injecting fault into {method}: {fault}")
        if fault.get("delay_ms"):
            await asyncio.sleep(fault["delay_ms"] / 1000)
        if fault.get("status"):
            return _json_error(fault["status"], fault.get("message", f"Injected {method} failure"))
        if fault.get("mode") == "malformed":
            return web.Response(text="{this is not json", content_type=CONTENT_TYPE_JSON)
        return None

    @staticmethod
    async def _read_http_params(request):
        """Read call parameters, from the JSON body (POST API) or from the query (GET API)."""
        if request.method == "POST":
            params = await request.json()
            params["_post"] = True
        else:
            params = dict(request.rel_url.query)
            params["_post"] = False
        # pageSize is a query parameter in both APIs.
        if "pageSize" in request.rel_url.query:
            params["_pageSize"] = int(request.rel_url.query["pageSize"])
        return params

    @staticmethod
    def _shard_selectors(params):
        selectors, success = _parse_selectors(params.get("selectors", ""))
        if not success:
            return (None, web.HTTPBadRequest(text="Invalid selectors"))
        if "project" not in selectors or "cluster" not in selectors or "service" not in selectors:
            return (None, web.HTTPBadRequest(text="project, cluster and service labels must be specified"))
        return (selectors, None)

    async def get_ping(self, request):
        return web.Response(status=200)

    async def api_v2_push(self, request):
        self._api_calls += 1

        logger.debug("push: {}".format(await request.read()))
        self._handle_auth(request)

        project = request.rel_url.query['project']
        cluster = request.rel_url.query['cluster']
        service = request.rel_url.query['service']

        key = (project, cluster, service)
        remaining = self._push_failures.get(key, 0)
        if remaining > 0:
            self._push_failures[key] = remaining - 1
            logger.debug(f"injecting transient push failure for {key}, {remaining - 1} left")
            return web.HTTPServiceUnavailable(text="Injected transient failure")

        shard = self._get_shard(project, cluster, service)
        content_type = request.headers['content-type']

        if content_type == CONTENT_TYPE_SPACK:
            metrics_json = json.loads(loads(await request.read()))
            logger.debug(f"spack decoded: {metrics_json}")
        elif content_type == CONTENT_TYPE_JSON:
            metrics_json = await request.json()
            logger.debug(f"json received: {metrics_json}")
        else:
            return web.HTTPBadRequest(text=f"Unknown content type {content_type}")

        return web.json_response({"sensorsProcessed": shard.add_metrics(metrics_json)})

    async def data_write(self, request):
        self._api_calls += 1

        logger.debug("write: {}".format(await request.read()))
        self._handle_auth(request)

        folder_id = request.rel_url.query['folderId']
        service = request.rel_url.query['service']

        shard = self._get_shard(folder_id, folder_id, service)
        content_type = request.headers['content-type']

        if content_type != CONTENT_TYPE_JSON:
            return web.HTTPBadRequest(text=f"Unknown content type {content_type}")

        metrics_json = await request.json()

        return web.json_response({"writtenMetricsCount": shard.add_metrics(metrics_json)})

    async def sensor_names(self, request):
        if response := await self._read_http_prologue(request, "names"):
            return response

        params = await self._read_http_params(request)
        selectors, error = self._shard_selectors(params)
        if error is not None:
            return error

        if "projectId" in params:
            return web.HTTPBadRequest(text="Invalid query params")

        shard = self._get_shard(selectors["project"], selectors["cluster"], selectors["service"])
        result = shard.get_label_names(selectors)

        return web.json_response({"names": result})

    async def sensor_labels(self, request):
        if response := await self._read_http_prologue(request, "labels"):
            return response

        params = await self._read_http_params(request)
        selectors, error = self._shard_selectors(params)
        if error is not None:
            return error

        if "projectId" in params or (params["_post"] and "pageSize" in params):
            return web.HTTPBadRequest(text="Invalid query params")

        shard = self._get_shard(selectors["project"], selectors["cluster"], selectors["service"])
        labels, totalCount = shard.get_labels(selectors)

        return web.json_response({"labels": labels, "totalCount": totalCount})

    async def sensors(self, request):
        if response := await self._read_http_prologue(request, "sensors"):
            return response

        params = await self._read_http_params(request)
        selectors, error = self._shard_selectors(params)
        if error is not None:
            return error

        if "projectId" in params:
            return web.HTTPBadRequest(text="Invalid query params")

        shard = self._get_shard(selectors["project"], selectors["cluster"], selectors["service"])
        metrics, error = shard.get_metrics(selectors, params.get("_pageSize"))

        if error is not None:
            return web.HTTPBadRequest(text=error)

        return web.json_response({"result": metrics, "page": {"pagesCount": 1, "totalCount": len(metrics)}})

    async def sensors_data(self, request):
        """Points count call: the reader sends `count(<selectors>)` to learn how to split reads."""
        if response := await self._read_http_prologue(request, "data"):
            return response

        params = await request.json()
        selectors, success = _parse_selectors(params.get("program", ""))
        if not success or not params.get("program", "").startswith("count("):
            return _json_error(400, "Only count(<selectors>) programs are supported")
        if "project" not in selectors or "cluster" not in selectors or "service" not in selectors:
            return _json_error(400, "project, cluster and service labels must be specified")

        shard = self._get_shard(selectors["project"], selectors["cluster"], selectors["service"])
        count, error = shard.count_points(selectors, _parse_instant_ms(params["from"]), _parse_instant_ms(params["to"]))
        if error is not None:
            return _json_error(400, error)

        return web.json_response({"scalar": count})

    async def metrics_get(self, request):
        cluster = request.rel_url.query.get('cluster', None) or request.rel_url.query['folderId']
        project = request.rel_url.query.get('project', cluster)
        service = request.rel_url.query['service']

        shard = self._get_shard(project, cluster, service)
        if shard is None:
            return web.HTTPNotFound(text=f"Unable to find shard {project}/{cluster}/{service}")
        reply = shard.as_text()
        return web.json_response(text=reply)

    async def metrics_post(self, request):
        project = request.rel_url.query['project']
        cluster = request.rel_url.query['cluster']
        service = request.rel_url.query['service']

        metrics_json = json.loads(await request.read())

        shard = self._get_shard(project, cluster, service)
        shard.add_parsed_metrics(metrics_json)

        return web.Response(status=200)

    async def fail_push(self, request):
        project = request.rel_url.query['project']
        cluster = request.rel_url.query['cluster']
        service = request.rel_url.query['service']
        count = int(request.rel_url.query.get('count', 1))

        self._push_failures[(project, cluster, service)] = count
        return web.Response(status=200)

    async def fail_read(self, request):
        """Queues a fault for the next `count` read calls of `method`.

        A fault may delay the call (delay_ms) and then fail it: with an HTTP status
        (status, message), with a gRPC status code name (grpc_code, message), or with a
        broken payload (mode: "malformed" for HTTP, "mismatch" for gRPC). A fault with
        only a delay lets the call proceed normally afterwards.
        """
        fault = await request.json()
        method = fault.pop("method")
        if method not in READ_METHODS:
            return web.HTTPBadRequest(text=f"Unknown read method {method}, expected one of {READ_METHODS}")
        count = int(fault.pop("count", 1))
        self.read_faults[method].extend(dict(fault) for _ in range(count))
        return web.Response(status=200)

    async def set_read_auth(self, request):
        self.read_auth = (await request.json()).get("expected")
        return web.Response(status=200)

    async def get_read_auth_log(self, request):
        return web.json_response({"calls": self.read_auth_log})

    async def get_read_requests(self, request):
        return web.json_response({"requests": self.read_requests})

    async def get_api_calls(self, request):
        return web.json_response({"api_calls": self._api_calls})

    async def cleanup(self, request):
        cluster = request.rel_url.query.get('cluster', None) or request.rel_url.query.get('folderId', None)
        project = request.rel_url.query.get('project', cluster)
        service = request.rel_url.query.get('service', None)

        if project is None and cluster is None and service is None:
            self._data.clear()
            self._push_failures = {}
            self._reset_read_state()
        else:
            self._data.delete(project, cluster, service)
            self._push_failures.pop((project, cluster, service), None)
        return web.Response(status=200)

    async def cleanup_api_calls(self, request):
        self._api_calls = 0
        return web.Response(status=200)

    def inc_api_calls(self):
        self._api_calls += 1


class DataService(DataServiceServicer):
    def __init__(self, emulator):
        self._emulator = emulator

    def _record_request(self, request: ReadRequest):
        downsampling = request.downsampling
        container = request.container
        self._emulator.read_requests.append({
            "project_id": container.project_id if container.HasField("project_id") else None,
            "folder_id": container.folder_id if container.HasField("folder_id") else None,
            "program": str(request.queries[0].value) if request.queries else "",
            "from_ms": request.from_time.seconds * 1000 + request.from_time.nanos // 1000000,
            "to_ms": request.to_time.seconds * 1000 + request.to_time.nanos // 1000000,
            "downsampling": {
                "disabled": downsampling.HasField("disabled"),
                "grid_interval": downsampling.grid_interval,
                "aggregation": Downsampling.GridAggregation.Name(downsampling.grid_aggregation),
                "fill": Downsampling.GapFilling.Name(downsampling.gap_filling),
            },
        })

    def Read(self, request: ReadRequest, context) -> ReadResponse:
        logger.debug('ReadRequest: %s', request)

        self._emulator.inc_api_calls()

        if request.container.HasField("project_id") and request.container.project_id in Shard.DEPRECATED_TESTS_PROJECTS:
            return self.DeprecatedTestsLogic(request, context)

        self._record_request(request)

        metadata = dict(context.invocation_metadata())
        if error := self._emulator.check_read_auth("read", metadata.get("authorization")):
            context.abort(grpc.StatusCode.UNAUTHENTICATED, error)

        fault = self._emulator.pop_read_fault("read") or {}
        if fault:
            logger.debug(f"injecting fault into read: {fault}")
        if fault.get("delay_ms"):
            time.sleep(fault["delay_ms"] / 1000)
        if fault.get("grpc_code"):
            context.abort(getattr(grpc.StatusCode, fault["grpc_code"]), fault.get("message", "Injected read failure"))

        selectors, success = _parse_selectors(str(request.queries[0].value))

        if not success:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("Coulnd't parse selectors")
            return ReadResponse()

        if request.container.HasField("project_id"):
            selectors["project"] = request.container.project_id
        else:
            del selectors["folderId"]
            selectors["project"] = request.container.folder_id
            selectors["cluster"] = request.container.folder_id

        if "project" not in selectors or "cluster" not in selectors or "service" not in selectors:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("Selectors should contain ['project', 'cluster', 'service'] labels")
            return ReadResponse()

        project = selectors["project"]
        cluster = selectors["cluster"]
        service = selectors["service"]

        shard = self._emulator._get_shard(project, cluster, service)
        result, error = shard.get_data(selectors, request.from_time, request.to_time, request.downsampling)

        if len(error):
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details(error)
            return ReadResponse()

        timestamps = result["timestamps"]
        if fault.get("mode") == "mismatch":
            # More timestamps than values: a broken response the reader must reject.
            timestamps = list(timestamps) + [max(timestamps, default=0) + 1000]

        return self._build_read_response(result["labels"], result["type"], timestamps, result["values"])

    def DeprecatedTestsLogic(self, request: ReadRequest, context):
        project = request.container.project_id
        if project == "invalid":
            logger.debug("invalid project_id, sending error")
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details(f"Project {project} does not exist")
            return ReadResponse()

        if project == "my_project" or project == "hist":
            labels = self._dict_to_labels(request)
            labels["project"] = project
            return self._build_read_response(labels, "RATE", [10000, 20000, 30000], [100, 200, 300])

    @staticmethod
    def _map_metric_type(kind):
        if (kind == "DGAUGE"):
            return MetricType.DGAUGE
        elif (kind == "IGAUGE"):
            return MetricType.IGAUGE
        elif (kind == "COUNTER"):
            return MetricType.COUNTER
        else:
            return MetricType.RATE

    @staticmethod
    def _dict_to_labels(request):
        result = dict()

        result["from"] = str(request.from_time)
        result["to"] = str(request.to_time)
        result["program"] = f"program length {len(str(request.queries[0].value))}"
        if request.downsampling.HasField("disabled"):
            result["downsampling.disabled"] = f"bool {True}"
        else:
            result["downsampling.aggregation"] = request.downsampling.grid_aggregation
            result["downsampling.fill"] = request.downsampling.gap_filling
            result["downsampling.gridMillis"] = f"int {request.downsampling.grid_interval}"
            result["downsampling.disabled"] = f"bool {False}"

        return result

    @staticmethod
    def _build_read_response(labels, type, timestamps, values):
        response = ReadResponse()

        response_query = response.response_per_query.add()
        response_query.query_name = "query"

        timeseries = response_query.timeseries_vector.values.add()
        for key, value in labels.items():
            timeseries.labels[key] = str(value)
        timeseries.type = DataService._map_metric_type(type)

        timeseries.timestamp_values.values.extend(timestamps)
        timeseries.double_values.values.extend(values)

        return response


def create_web_app(emulator):
    webapp = web.Application()
    webapp.add_routes([
        web.post("/api/v2/projects/{project}/sensors/names", emulator.sensor_names),
        web.get("/api/v2/projects/{project}/sensors/names", emulator.sensor_names),
        web.post("/api/v2/projects/{project}/sensors/labels", emulator.sensor_labels),
        web.get("/api/v2/projects/{project}/sensors/labels", emulator.sensor_labels),
        web.post("/api/v2/projects/{project}/sensors/data", emulator.sensors_data),
        web.post("/api/v2/projects/{project}/sensors", emulator.sensors),
        web.get("/api/v2/projects/{project}/sensors", emulator.sensors),
        web.get("/api/calls", emulator.get_api_calls),
        web.get("/api/read_auth", emulator.get_read_auth_log),
        web.get("/api/read_requests", emulator.get_read_requests),
        web.get("/metrics/get", emulator.metrics_get),
        web.get("/ping", emulator.get_ping),
        web.post("/api/v2/push", emulator.api_v2_push),
        web.post("/monitoring/v2/data/write", emulator.data_write),
        web.post("/metrics/post", emulator.metrics_post),
        web.post("/cleanup", emulator.cleanup),
        web.post("/cleanup/api/calls", emulator.cleanup_api_calls),
        web.post("/config/read_auth", emulator.set_read_auth),
        web.post("/fail/push", emulator.fail_push),
        web.post("/fail/read", emulator.fail_read),
    ])

    return webapp


def create_grpc_server(emulator, port, tls_port=None, cert_path=None, key_path=None):
    # Enough workers for concurrent reads while some of them sleep on an injected delay.
    grpc_server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    add_DataServiceServicer_to_server(
        DataService(emulator), grpc_server
    )
    grpc_server.add_insecure_port(f'[::]:{port}')

    if tls_port:
        with open(key_path, "rb") as f:
            key = f.read()
        with open(cert_path, "rb") as f:
            cert = f.read()
        grpc_server.add_secure_port(f'[::]:{tls_port}', grpc.ssl_server_credentials([(key, cert)]))

    return grpc_server


async def _serve_http(app, http_port, https_port, ssl_context, grpc_port, grpcs_port):
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, port=http_port).start()
    if https_port:
        await web.TCPSite(runner, port=https_port, ssl_context=ssl_context).start()

    # The recipe waits for this line, so it goes out only when every port listens.
    logger.info(f"Started Solomon emulator on http port {http_port}, grpc port {grpc_port}, "
                f"https port {https_port}, grpcs port {grpcs_port}")
    await asyncio.Event().wait()


def run_web_app(config, http_port, grpc_port, https_port=None, grpcs_port=None, tls_dir=None):
    emulator = SolomonEmulator(config)

    app = create_web_app(emulator)

    cert_path, key_path, ssl_context = None, None, None
    if tls_dir:
        cert_path, key_path = generate_self_signed_cert(tls_dir)
        ssl_context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        ssl_context.load_cert_chain(cert_path, key_path)

    server = create_grpc_server(emulator, grpc_port, grpcs_port, cert_path, key_path)
    server.start()

    asyncio.run(_serve_http(app, http_port, https_port, ssl_context, grpc_port, grpcs_port))
