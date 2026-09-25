# -*- coding: utf-8 -*-
import os
import time

from ydb.library.yql.tools.solomon_emulator.client.client import cleanup_emulator, add_solomon_metrics

from ydb.tests.library.harness.kikimr_runner import KiKiMR
from ydb.tests.library.harness.kikimr_config import KikimrConfigGenerator

import ydb
from ydb.issues import GenericError


class SolomonReadingTestBase(object):
    # `query_service_config.solomon` default settings a subclass overrides or adds,
    # e.g. to rerun a whole test class in another mode.
    EXTRA_SETTINGS = {}

    @classmethod
    def setup_class(cls, test_name, extra_settings=None):
        """Seeds the emulator for `test_name` and starts a cluster.

        `extra_settings` overrides or adds default settings on top of EXTRA_SETTINGS.
        """
        cleanup_emulator()

        config = KikimrConfigGenerator(
            extra_feature_flags={"enable_external_data_sources": True}
        )
        config.yaml_config["query_service_config"] = {}
        config.yaml_config["query_service_config"]["available_external_data_sources"] = ["Solomon"]
        config.yaml_config["query_service_config"]["solomon"] = {
            "default_settings": [
                {
                    "name": "_EnableReading",
                    "value": "true"
                },
                {
                    "name": "_EnableRuntimeListing",
                    "value": "true"
                },
                {
                    "name": "_EnableSolomonClientPostApi",
                    "value": "true"
                },
                {
                    "name": "_MaxListingPageSize",
                    "value": 1000
                },
                {
                    "name": "MaxApiInflight",
                    "value": 2500
                }
            ]
        }

        if test_name == "settings_validation":
            add_solomon_metrics("settings_validation", "settings_validation", "my_service", {"metrics": [
                {
                    "labels"        : {"test_type": "setting_validation"},
                    "type"          : "DGAUGE",
                    "timestamps"    : [1000000],
                    "values"        : [0]
                }
            ]})

        elif test_name == "basic_reading":
            cls.basic_reading_timestamps = [0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55]
            cls.basic_reading_values = [0, 1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11]

            add_solomon_metrics("basic_reading", "basic_reading", "my_service", {"metrics": [
                {
                    "labels"        : {"test_type": "basic_reading_test"},
                    "type"          : "DGAUGE",
                    "timestamps"    : cls.basic_reading_timestamps,
                    "values"        : cls.basic_reading_values
                }
            ]})

        elif test_name == "listing_paging":
            cls.listing_paging_metrics_size = 2500

            add_solomon_metrics("listing_paging", "listing_paging", "my_service", {"metrics": [
                *cls._generate_listing_paging_test_metrics(cls.listing_paging_metrics_size)
            ]})

        elif test_name == "listing_batching":
            cls.listing_batching_metrics_sizes = [1100, 600]

            add_solomon_metrics("listing_batching", "listing_batching", "my_service", {"metrics": [
                *cls._generate_listing_batching_test_metrics(*cls.listing_batching_metrics_sizes)
            ]})

        elif test_name == "data_paging":
            cls.data_paging_timeseries_size = 25000
            cls.data_paging_timestamps, cls.data_paging_values = cls._generate_data_paging_timeseries(cls.data_paging_timeseries_size)

            add_solomon_metrics("data_paging", "data_paging", "my_service", {"metrics": [
                {
                    "labels"        : {"test_type": "data_paging_test"},
                    "type"          : "DGAUGE",
                    "timestamps"    : cls.data_paging_timestamps,
                    "values"        : cls.data_paging_values
                }
            ]})

        elif test_name == "backpressure_test":
            cls.backpressure_test_metrics_size = 100

            config.yaml_config["query_service_config"]["solomon"]["default_settings"].extend([
                {
                    "name": "MaxDataInflightBytes",
                    "value": 1
                },
                {
                    "name": "MetricsQueuePrefetchSize",
                    "value": 1
                },
                {
                    "name": "MetricsQueueBatchCountLimit",
                    "value": 1
                },
                {
                    "name": "ComputeActorBatchSize",
                    "value": 1
                }
            ])

            add_solomon_metrics("backpressure_test", "backpressure_test", "my_service", {"metrics": [
                *cls._generate_backpressure_test_metrics(cls.backpressure_test_metrics_size)
            ]})

        elif test_name in ("auth_tls", "transport_errors"):
            # One shard serves all three cluster types: Solomon addresses it by project,
            # Monitoring by folder (project == cluster == folder in the emulator) and
            # Monium by project plus a `cluster` selector.
            cls.simple_timestamps = [0, 15, 30]
            cls.simple_values = [0, 1, 2]
            add_solomon_metrics(test_name, test_name, "my_service", {"metrics": [
                {
                    "labels"        : {"test_type": "simple"},
                    "type"          : "DGAUGE",
                    "timestamps"    : cls.simple_timestamps,
                    "values"        : cls.simple_values
                }
            ]})

        elif test_name == "points_count":
            # Recent points: with downsampling disabled the reader asks the API how many
            # points the last 7 days hold before splitting the range (the /sensors/data call).
            cls.points_count_size = 25000
            end = (int(time.time()) - 3600) // 5 * 5
            start = end - (cls.points_count_size - 1) * 5
            cls.points_count_from = start
            cls.points_count_to = end
            add_solomon_metrics("points_count", "points_count", "my_service", {"metrics": [
                {
                    "labels"        : {"test_type": "points_count_test"},
                    "type"          : "DGAUGE",
                    "timestamps"    : list(range(start, end + 1, 5)),
                    "values"        : list(range(cls.points_count_size))
                },
                {
                    # Exists, but has no points in the recent range.
                    "labels"        : {"test_type": "empty_recent_test"},
                    "type"          : "DGAUGE",
                    "timestamps"    : [0],
                    "values"        : [0]
                }
            ]})

        elif test_name == "data_semantics":
            add_solomon_metrics("data_semantics", "data_semantics", "my_service", {"metrics": [
                {
                    "labels"        : {"test_type": "nan_test"},
                    "type"          : "DGAUGE",
                    "timestamps"    : [0, 15, 30],
                    "values"        : [1, float("nan"), 3]
                },
                *[
                    {
                        "labels"        : {"test_type": "types_test", "kind": kind},
                        "type"          : kind,
                        "timestamps"    : [0],
                        "values"        : [7]
                    }
                    for kind in ("DGAUGE", "IGAUGE", "COUNTER", "RATE")
                ],
                {
                    "labels"        : {"test_type": "labels_test", "host": "h1", "dc": "sas"},
                    "type"          : "DGAUGE",
                    "timestamps"    : [0],
                    "values"        : [1]
                },
                {
                    "labels"        : {"test_type": "labels_test", "host": "h2", "dc": "vla"},
                    "type"          : "DGAUGE",
                    "timestamps"    : [0],
                    "values"        : [2]
                },
                {
                    # Its only point is far outside of the queried range.
                    "labels"        : {"test_type": "out_of_range_test"},
                    "type"          : "DGAUGE",
                    "timestamps"    : [100000],
                    "values"        : [1]
                },
            ]})

        for name, value in dict(cls.EXTRA_SETTINGS, **(extra_settings or {})).items():
            default_settings = config.yaml_config["query_service_config"]["solomon"]["default_settings"]
            default_settings[:] = [s for s in default_settings if s["name"] != name]
            default_settings.append({"name": name, "value": value})

        cls.solomon_http_endpoint = os.environ.get("SOLOMON_HTTP_ENDPOINT")
        cls.solomon_grpc_endpoint = os.environ.get("SOLOMON_GRPC_ENDPOINT")
        cls.solomon_https_endpoint = os.environ.get("SOLOMON_HTTPS_ENDPOINT")
        cls.solomon_grpcs_endpoint = os.environ.get("SOLOMON_GRPCS_ENDPOINT")

        # ydbd inherits the environment: make its gRPC client trust the emulator's
        # self-signed certificate. HTTP does not verify peers at all.
        if ca_file := os.environ.get("SOLOMON_TLS_CA_FILE"):
            os.environ["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = ca_file

        cls.cluster = KiKiMR(config)
        cls.cluster.start()

        cls.endpoint = "%s:%s" % (
            cls.cluster.nodes[1].host, cls.cluster.nodes[1].port
        )
        cls.driver = ydb.Driver(
            ydb.DriverConfig(
                database='/Root',
                endpoint=cls.endpoint
            )
        )
        cls.driver.wait()

    @classmethod
    def teardown_class(cls):
        cls.driver.stop()
        cls.cluster.stop()

    @classmethod
    def execute_query(cls, query):
        with ydb.QuerySessionPool(cls.driver) as session_pool:
            try:
                res = session_pool.execute_with_retries(query)
                return (res, None)
            except GenericError as generic_error:
                return (None, generic_error)

    @classmethod
    def execute_query_once(cls, query):
        """Runs the query without client retries and returns (result, error) for any error status."""
        with ydb.QuerySessionPool(cls.driver) as session_pool:
            try:
                res = session_pool.execute_with_retries(query, retry_settings=ydb.RetrySettings(max_retries=0))
                return (res, None)
            except ydb.Error as error:
                return (None, error)

    @staticmethod
    def issue_messages(error):
        """All messages of an error and of its nested issues, joined into one string."""
        def collect(issue):
            return getattr(issue, "message", "") + "\n" + "".join(collect(i) for i in getattr(issue, "issues", None) or [])
        return str(error) + "\n" + collect(error)

    @classmethod
    def create_source(cls, name, cluster_type, project, tls=False, auth_method="NONE", secret_name=None):
        """Creates an external data source addressing `project` as one of the three cluster types.

        solomon:    no PROJECT/CLUSTER, the table name is the project, `OAuth` tokens
        monitoring: PROJECT (cloud) and CLUSTER (folder), the table name is the service, `Bearer` tokens
        monium:     PROJECT only, the table name is the service, `Bearer` tokens
        """
        properties = {
            "SOURCE_TYPE": "Monium.Metrics",
            "LOCATION": cls.solomon_https_endpoint if tls else cls.solomon_http_endpoint,
            "GRPC_LOCATION": cls.solomon_grpcs_endpoint if tls else cls.solomon_grpc_endpoint,
            "AUTH_METHOD": auth_method,
            "USE_TLS": "true" if tls else "false",
        }
        if cluster_type in ("monitoring", "monium"):
            properties["PROJECT"] = project
        if cluster_type == "monitoring":
            properties["CLUSTER"] = project
        if secret_name is not None:
            properties["TOKEN_SECRET_PATH"] = secret_name

        props = ",\n".join(f'{key} = "{value}"' for key, value in properties.items())
        result, error = cls.execute_query(f"CREATE EXTERNAL DATA SOURCE `{name}` WITH ({props})")
        assert error is None, error

    @staticmethod
    def source_table(cluster_type, project, service="my_service"):
        return project if cluster_type == "solomon" else service

    @staticmethod
    def source_selectors(cluster_type, project, labels, program=False):
        """`{...}` addressing `labels` of `project`/my_service for the cluster type.

        A program is sent as is, so it names the shard itself. Selectors get the shard
        labels injected from the source: all of them for monitoring, the service for monium.
        """
        shard = []
        if program:
            location = "folderId" if cluster_type == "monitoring" else "cluster"
            shard = [f'{location}="{project}"', 'service="my_service"']
        elif cluster_type == "solomon":
            shard = [f'cluster="{project}"', 'service="my_service"']
        elif cluster_type == "monium":
            shard = [f'cluster="{project}"']
        return "{" + ", ".join(shard + [f'{key}="{value}"' for key, value in labels.items()]) + "}"

    @staticmethod
    def _generate_listing_paging_test_metrics(size):
        listing_paging_metrics = [
            {
                "labels"        : {"test_type": "listing_paging_test", "test_label": str(i)},
                "type"          : "DGAUGE",
                "timestamps"    : [0],
                "values"        : [0]
            }
            for i in range(size)
        ]

        listing_paging_metrics.append({
            "labels"        : {"test_type": "listing_paging_test"},
            "type"          : "DGAUGE",
            "timestamps"    : [0],
            "values"        : [0]
        })

        return listing_paging_metrics

    @staticmethod
    def _generate_listing_batching_test_metrics(totalSize, firstLabelSize):
        listing_batching_metrics = [
            {
                "labels"        : {"test_type": "listing_batching_test", "test_label": str(i)},
                "type"          : "DGAUGE",
                "timestamps"    : [0],
                "values"        : [0]
            }
            for i in range(firstLabelSize)
        ]
        for i in range(totalSize - firstLabelSize):
            listing_batching_metrics.append({
                "labels"        : {"test_type": "listing_batching_test", "test_label": "0", "test_label_2": str(i)},
                "type"          : "DGAUGE",
                "timestamps"    : [0],
                "values"        : [0]
            })

        return listing_batching_metrics

    @staticmethod
    def _generate_data_paging_timeseries(size):
        timestamps = [i * 5 for i in range(size)]
        values = [i for i in range(size)]
        return timestamps, values

    @staticmethod
    def _generate_backpressure_test_metrics(size):
        backpressure_test_metrics = [
            {
                "labels"        : {"test_type": "backpressure_test", "test_label": str(i)},
                "type"          : "DGAUGE",
                "timestamps"    : [0],
                "values"        : [0]
            }
            for i in range(size)
        ]

        return backpressure_test_metrics
