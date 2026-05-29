# External Data Sources

An external data source is an object in {{ ydb-full-name }} that describes the connection parameters to an external data source. For example, in the case of ClickHouse, the external data source describes the network address, login, and password for authentication in the ClickHouse cluster. In the case of S3 ({{ objstorage-name }}), it describes the access credentials and the path to the bucket.

The following example demonstrates creating an external data source pointing to a ClickHouse cluster:

```yql
CREATE EXTERNAL DATA SOURCE test_data_source WITH (
  SOURCE_TYPE="ClickHouse",
  LOCATION="192.168.1.1:8123",
  DATABASE_NAME="default",
  AUTH_METHOD="BASIC",
  USE_TLS="TRUE",
  LOGIN="login",
  PASSWORD_SECRET_PATH="test_password_path",
  PROTOCOL="NATIVE"
);
```

After creating an external data source, you can read data from the created `EXTERNAL DATA SOURCE` object. The example below illustrates reading data from the `test_table` table in the `default` database in the ClickHouse cluster:

```yql
SELECT * FROM test_data_source.test_table;
```

External data sources allow execution of [federated queries](../query_execution/federated_query/index.md) for cross-system data analytics tasks.

## IAM Authentication for {{ ydb-short-name }} Data Sources {#iam-auth}

When {{ ydb-short-name }} is used as an external data source in long-running topic queries, standard token-based authentication becomes impractical: cloud tokens have a limited validity period, forcing queries to be restarted each time a token expires.

To solve this, {{ ydb-full-name }} supports IAM authentication with service account delegation for `SOURCE_TYPE="Ydb"` external data sources. When the external data source is created, an initial IAM token is provided via `INITIAL_TOKEN_SECRET_PATH`. This token is used once to verify connectivity to the target database and to resolve its resource identifier in the cloud. After that, all ongoing authentication is handled by the service account specified in `SERVICE_ACCOUNT_ID`, whose tokens are refreshed automatically, allowing streaming queries to run indefinitely without restarts.

This authentication method requires the `enable_external_data_source_auth_method_iam` [feature flag](../../reference/configuration/feature_flags.md) to be enabled.

To use IAM authentication, first store the initial IAM token as a [secret](secrets.md):

```yql
CREATE SECRET <token_secret_name> WITH (value = "<iam_token>");
```

Where:

- `<token_secret_name>` is the name you assign to the secret, for example `mytoken`.
- `<iam_token>` is the initial IAM token for the service account.

Then create the external data source:

```yql
CREATE EXTERNAL DATA SOURCE <source_name> WITH (
    SOURCE_TYPE="Ydb",
    LOCATION="<host>:<port>",
    DATABASE_NAME="<database>",
    AUTH_METHOD="IAM",
    SERVICE_ACCOUNT_ID="<service_account_id>",
    INITIAL_TOKEN_SECRET_PATH="<token_secret_name>",
    USE_TLS="TRUE"
);
```

Where:

- `<source_name>` is the name you assign to the external data source, for example `ydb_datasource`.
- `<host>` is the hostname of the target {{ ydb-short-name }} instance, for example `u-lb.abcde12345.ydb.mdb.yandexcloud.net`.
- `<port>` is the gRPCs port when `USE_TLS="TRUE"`, or the gRPC port otherwise, for example `2135`.
- `<database>` is the full path to the target database, for example `/ru-central1/b1g8skpblkos03malf3s/etndqstq7ne4v68n6gpr`.
- `<service_account_id>` is the ID of the service account that has been granted access to the target database, for example `ajehr5a9p9lc7c7qnhdc`.
- `<token_secret_name>` is the name of the secret created in the previous step.

The following data sources can be used:

- [ClickHouse](../query_execution/federated_query/clickhouse.md)
- [PostgreSQL](../query_execution/federated_query/postgresql.md)
- [{{ ydb-short-name }}](../query_execution/federated_query/ydb.md)
- [Connections to S3 ({{ objstorage-name }})](../query_execution/federated_query/s3/external_data_source.md)
