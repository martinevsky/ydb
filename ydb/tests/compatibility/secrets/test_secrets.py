# -*- coding: utf-8 -*-
# Schema secrets across YDB versions: value secrets survive a version change in both directions; a delegation
# secret (TYPE = "IAM_DELEGATION", a feature of the current version only) is created on the current version,
# survives a downgrade to the stable version as a record that the stable binary describes but cannot mint from,
# and serves a token again after the upgrade back.
import logging
import os
import pytest
import time
import yatest

from ydb.tests.library.compatibility.fixtures import RestartToAnotherVersionFixture, current_binary_path
from ydb.tests.library.harness.kikimr_runner import KiKiMR
from ydb.tests.library.harness.kikimr_config import KikimrConfigGenerator
from ydb.tests.library.harness.util import LogLevels
from ydb.tests.library.common.types import Erasure
from ydb.tests.oss.ydb_sdk_import import ydb
from ydb.tests.tools.datastreams_helpers.data_plane import read_stream

logger = logging.getLogger(__name__)

# the IAM emulator (ydb/tests/fq/streaming_common/iam_grpc_emulator) authenticates any token as the cloud user
# "bob" (SID bob@as), sets delegations up on behalf of "bob" only and mints tokens for "delegated-*" accounts
# that have a delegation
CLOUD_USER_TOKEN = "cloud-user-token"
CLOUD_USER_SID = "bob@as"
DELEGATED_SA = "delegated-compat"
CLOUD_ID = "compatcloud"
IAM_EMULATOR_LOG = "iam_grpc_emulator.err.log"
HANG_GUARD_SECONDS = 120


def feature_flags_of(config):
    return config.yaml_config.setdefault("feature_flags", {})


class SecretsTestBase(RestartToAnotherVersionFixture):
    """RestartToAnotherVersionFixture with the cluster config of the secrets tests: the access service and the
    VM metadata emulators (known to every version), and, only while the current version runs, the IAM config
    and the feature flag of delegation secrets (unknown to the stable versions)."""

    def setup_secrets_cluster(self):
        extra_feature_flags = ["enable_schema_secrets", "enable_external_data_sources", "enable_topics_sql_io_operations",
                               "suppress_compatibility_check", "enable_drain_on_shutdown"]
        disabled_feature_flags = ["enable_graceful_shutdown"]
        self.config = KikimrConfigGenerator(
            erasure=Erasure.MIRROR_3_DC,
            binary_paths=[self.all_binary_paths[self.current_binary_paths_index]],
            use_in_memory_pdisks=False,
            extra_feature_flags=extra_feature_flags,
            disabled_feature_flags=disabled_feature_flags,
            table_service_config={"enable_compile_cache_warmup": False},
            additional_log_configs={
                'IAM_DELEGATION': LogLevels.DEBUG,
                'SCHEMA_SECRET_CACHE': LogLevels.DEBUG,
                'KQP_EXECUTER': LogLevels.DEBUG,
            },
        )
        auth_config = self.config.yaml_config.setdefault("auth_config", {})
        auth_config["use_access_service"] = True
        auth_config["access_service_endpoint"] = os.environ["IAM_EMULATOR_ENDPOINT"]
        auth_config["use_access_service_tls"] = False
        auth_config["local_metadata_service"] = {
            "host": os.environ["VM_METADATA_EMULATOR_HOST"],
            "port": int(os.environ["VM_METADATA_EMULATOR_PORT"]),
        }
        self.set_delegation_config(enabled=self.on_current_version())

        self.cluster = KiKiMR(self.config)
        self.cluster.start()
        self.endpoint = "grpc://%s:%s" % ('localhost', self.cluster.nodes[1].port)
        self.http_proxy_endpoint = "http://%s:%s" % ('localhost', self.cluster.nodes[1].http_proxy_port)
        self.database_path = "/Root"
        self.driver = self.create_driver()
        yield
        self.stop_driver()
        self.cluster.stop()

    def on_current_version(self):
        return self.all_binary_paths[self.current_binary_paths_index] == current_binary_path

    def set_delegation_config(self, enabled):
        flags = feature_flags_of(self.config)
        if enabled:
            flags["enable_iam_delegation_secrets"] = True
            iam_endpoint = os.environ["IAM_EMULATOR_ENDPOINT"]
            self.config.yaml_config["iam_config"] = {
                "token_service_endpoint": iam_endpoint,
                "service_control_endpoint": iam_endpoint,
                "resource_manager_endpoint": iam_endpoint,
                "service_id": "ydb",
                "microservice_id": "data-plane",
                "resource_type": "resource-manager.cloud",
                "enable_ssl": False,
            }
        else:
            flags.pop("enable_iam_delegation_secrets", None)
            self.config.yaml_config.pop("iam_config", None)

    def switch_version(self):
        # the config of the next version is written before the restart: the stable binaries know neither the
        # IAM config nor the feature flag
        next_index = (self.current_binary_paths_index + 1) % len(self.all_binary_paths)
        self.set_delegation_config(enabled=self.all_binary_paths[next_index] == current_binary_path)
        self.change_cluster_version()

    def query(self, text, token=None):
        driver = self.driver
        if token is not None:
            driver = ydb.Driver(ydb.DriverConfig(database=self.database_path, endpoint=self.endpoint,
                                                 credentials=ydb.AccessTokenCredentials(token)))
            driver.wait(timeout=60)
        try:
            with ydb.QuerySessionPool(driver) as session_pool:
                return session_pool.execute_with_retries(text, retry_settings=ydb.RetrySettings(max_retries=5))
        finally:
            if token is not None:
                driver.stop()

    def query_fails(self, text, token=None):
        try:
            self.query(text, token)
        except ydb.Error as e:
            return str(e)
        raise AssertionError("the query succeeded: " + text)

    def path_exists(self, path):
        try:
            self.driver.scheme_client.describe_path(path)
            return True
        except ydb.SchemeError:
            return False

    def create_topic(self, name):
        self.query(f"CREATE TOPIC `{name}` (CONSUMER consumer);")

    def create_token_source(self, name, secret_path):
        endpoint = f"localhost:{self.cluster.nodes[1].port}"
        self.query(f"""
            CREATE EXTERNAL DATA SOURCE `{name}` WITH (
                SOURCE_TYPE = "Ydb",
                LOCATION = "{endpoint}",
                DATABASE_NAME = "{self.database_path}",
                AUTH_METHOD = "TOKEN",
                TOKEN_SECRET_PATH = "{secret_path}");
        """)

    def write_through_source(self, source, topic, data):
        # the write goes to the topic of this cluster with the token read from the secret of the source
        self.query(f'INSERT INTO `{source}`.`{topic}` SELECT "{data}";')
        # the write may have been retried by the SDK (no deduplication): the topic is read message by message
        # until the written one comes (hang guard: it was written, so it comes)
        seen = []
        for _ in range(10):
            read_data = read_stream(path=topic, messages_count=1, consumer_name="consumer", database=self.database_path,
                                    endpoint=f"localhost:{self.cluster.nodes[1].port}")
            seen.extend(read_data or [])
            if data in seen:
                return
        raise AssertionError(f"message {data} was not read from {topic}, read {seen}")

    def emulator_mints(self, service_account):
        """Mints of tokens of the service account recorded by the IAM emulator (one log line per CreateForService)."""
        candidates = [yatest.common.output_path(IAM_EMULATOR_LOG)]
        for path in candidates:
            if os.path.exists(path):
                with open(path) as f:
                    return sum(1 for line in f if "CreateForService called" in line and f"target_sa={service_account}" in line)
        return 0

    def wait_mints(self, service_account, at_least):
        # hang guard: the mint is made inevitable by the write that needs the token
        deadline = time.time() + HANG_GUARD_SECONDS
        while self.emulator_mints(service_account) < at_least:
            assert time.time() < deadline, f"no {at_least} mints of {service_account} in the emulator log"
            time.sleep(1)


class TestValueSecretsRestartToAnotherVersion(SecretsTestBase):
    @pytest.fixture(autouse=True, scope="function")
    def setup(self):
        yield from self.setup_secrets_cluster()

    def test_value_secret_survives_a_version_change(self):
        # a value secret created on the first version is described, read (a topic write with its value as the
        # token, which the access service emulator authenticates as the cloud user) and altered on the second one
        self.query(f"GRANT ALL ON `/Root` TO `{CLOUD_USER_SID}`;")
        self.query('CREATE SECRET `/Root/plain_secret` WITH (VALUE = "plain-token");')
        assert self.path_exists("/Root/plain_secret")
        self.create_topic("plain_topic")
        self.create_token_source("plain_source", "/Root/plain_secret")
        self.write_through_source("plain_source", "plain_topic", "before")

        self.switch_version()

        assert self.path_exists("/Root/plain_secret")
        self.write_through_source("plain_source", "plain_topic", "after")
        self.query('ALTER SECRET `/Root/plain_secret` WITH (VALUE = "plain-token-2");')
        self.write_through_source("plain_source", "plain_topic", "altered")
        self.query('DROP SECRET `/Root/plain_secret`;')
        assert not self.path_exists("/Root/plain_secret")


class TestDelegationSecretsRestartToAnotherVersion(SecretsTestBase):
    @pytest.fixture(autouse=True, scope="function")
    def setup(self):
        yield from self.setup_secrets_cluster()

    def test_delegation_secret_survives_a_downgrade(self):
        # only the current version knows delegation secrets: the scenario starts on it and comes back to it
        if not self.on_current_version() or self.all_binary_paths[1] == current_binary_path:
            pytest.skip("the scenario is a downgrade from the current version and the upgrade back")

        # the delegation is set up on behalf of the cloud user, the token is minted through it
        self.query(f"GRANT ALL ON `/Root` TO `{CLOUD_USER_SID}`;")
        self.query(f'CREATE SECRET `/Root/sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "{DELEGATED_SA}", RESOURCE = "{CLOUD_ID}");',
                   token=CLOUD_USER_TOKEN)
        assert self.path_exists("/Root/sa_secret")
        self.create_topic("delegated_topic")
        self.create_token_source("delegated_source", "/Root/sa_secret")
        self.write_through_source("delegated_source", "delegated_topic", "current-before")
        self.wait_mints(DELEGATED_SA, at_least=1)
        mints_before_downgrade = self.emulator_mints(DELEGATED_SA)

        # the stable version: the cluster starts with the record and describes it as a path, but it knows no
        # delegation secrets: a read of the secret yields an empty value, so the source over it cannot be used
        # ("Token auth requires non-empty value ..."), and nothing is minted meanwhile
        self.switch_version()
        assert self.path_exists("/Root/sa_secret"), "the delegation secret record was lost on the downgrade"
        error = self.query_fails('INSERT INTO `delegated_source`.`delegated_topic` SELECT "stable";')
        assert "requires non-empty value for the secret referenced by TOKEN_SECRET_NAME" in error, error
        assert self.emulator_mints(DELEGATED_SA) == mints_before_downgrade

        # the current version again: the preserved record serves a token again (a fresh node mints anew)
        self.switch_version()
        assert self.path_exists("/Root/sa_secret")
        self.write_through_source("delegated_source", "delegated_topic", "current-after")
        self.wait_mints(DELEGATED_SA, at_least=mints_before_downgrade + 1)
        self.query('DROP SECRET `/Root/sa_secret`;', token=CLOUD_USER_TOKEN)
        assert not self.path_exists("/Root/sa_secret")
