# CREATE EXTERNAL DATA SOURCE

<!-- markdownlint-disable proper-names -->

The `CREATE EXTERNAL DATA SOURCE` statement creates an [external data source](../../../concepts/datamodel/external_data_source.md).

```yql
CREATE EXTERNAL DATA SOURCE external_data_source WITH (
  SOURCE_TYPE="source_type",
  LOCATION="ip_address_or_fqdn:port",
  USE_TLS="use_tls",
  AUTH_METHOD="auth_method",
  LOGIN="login",
  PASSWORD_SECRET_PATH="password_secret_path"
)
```

Where:

* `external_data_source` — name of the external data source.
* `source_type` — type of the external data source. Possible values: [`ClickHouse`](#clickhouse), [`PostgreSQL`](#postgresql), [`ObjectStorage`](#object_storage).
* `ip_address_or_fqdn:port` — full network address of the external data source, including the port. The network address can be an IP address or FQDN.
* `use_tls` — flag indicating that the connection must use a secure (TLS) connection. Possible values: `TRUE`, `FALSE`.
* `auth_method` — authentication method for the external data source. For external sources of types `ClickHouse` and `PostgreSQL`, only the `BASIC` authentication type is supported. For the `ObjectStorage` external source, only the `NONE` authentication type is currently supported.
* `login` — login used to connect to the external data source.
* `password_secret_path` — [secret](../../../concepts/datamodel/secrets.md) containing the password for connecting to the external data source.

When using secure TLS channels, system certificates located on {{ ydb-full-name }} servers are used.

## Example

The query below creates an external data source named `TestDataSource` to a ClickHouse cluster with IP address `192.168.1.1` and port `8443`, with login `admin` and secret `test_secret`:

```yql
CREATE EXTERNAL DATA SOURCE TestDataSource WITH (
  SOURCE_TYPE="ClickHouse",
  LOCATION="192.168.1.1:8443",
  USE_TLS="TRUE",
  AUTH_METHOD="BASIC",
  LOGIN="admin",
  PASSWORD_SECRET_PATH="test_secret"
)
```

## Connecting to ClickHouse {#clickhouse}

To create a connection to a ClickHouse cluster, create an external data source `EXTERNAL DATA SOURCE` and specify:

- in the `SOURCE_TYPE` field, the value `ClickHouse`;
- in the `LOCATION` field, the full network address of the ClickHouse cluster, including the port. The network address can be an IP address or FQDN. Connection to the ClickHouse cluster is currently always made over the HTTP protocol;
- in the `USE_TLS` field, the flag indicating that the connection to the ClickHouse cluster must use a secure (TLS) connection;
- in the `AUTH_METHOD` field, the value `BASIC`;
- in the `LOGIN` field, the login used to connect to the ClickHouse cluster;
- in the `PASSWORD_SECRET_PATH` field, the [secret](../../../concepts/datamodel/secrets.md) containing the password for connecting to the ClickHouse cluster.

When using secure TLS channels, system certificates located on {{ ydb-full-name }} servers are used.

### Example

The query below creates an external data source named `TestDataSource` to a ClickHouse cluster with IP address `192.168.1.1` and port `8443`, with login `admin` and secret `test_secret`:

```yql
CREATE EXTERNAL DATA SOURCE TestDataSource WITH (
  SOURCE_TYPE="ClickHouse",
  LOCATION="192.168.1.1:8443",
  USE_TLS="TRUE",
  AUTH_METHOD="BASIC",
  LOGIN="admin",
  PASSWORD_SECRET_PATH="test_secret"
)
```

## Connecting to PostgreSQL {#postgresql}

To create a connection to a PostgreSQL cluster, create an `EXTERNAL DATA SOURCE` object and specify in the fields:

- in the `SOURCE_TYPE` field, the value `PostgreSQL`;
- in the `LOCATION` field, the full network address of the PostgreSQL cluster, including the port. The network address can be an IP address or FQDN;
- in the `USE_TLS` field, the flag indicating that the connection to the PostgreSQL cluster must use a secure (TLS) connection;
- in the `AUTH_METHOD` field, the value `BASIC`;
- in the `LOGIN` field, the login used to connect to the PostgreSQL cluster;
- in the `PASSWORD_SECRET_PATH` field, the [secret](../../../concepts/datamodel/secrets.md) containing the password for connecting to the PostgreSQL cluster.

Connection to the PostgreSQL cluster is currently always made over the standard [Frontend/Backend Protocol](https://www.postgresql.org/docs/current/protocol.html) over TCP. When using secure TLS channels, system certificates located on {{ ydb-full-name }} servers are used.

### Example

The query below creates an external data source named `TestDataSource` to a PostgreSQL cluster with IP address `192.168.1.1` and port `5432`, with login `admin` and secret `test_secret`:

```yql
CREATE EXTERNAL DATA SOURCE TestDataSource WITH (
  SOURCE_TYPE="PostgreSQL",
  LOCATION="192.168.1.1:5432",
  USE_TLS="TRUE",
  AUTH_METHOD="BASIC",
  LOGIN="admin",
  PASSWORD_SECRET_PATH="test_secret"
)
```

## Connecting to S3 ({{ objstorage-name }}) {#object_storage}

To create an external data source pointing to a bucket with data in S3 ({{ objstorage-name }}), create an `EXTERNAL DATA SOURCE` object and specify in the fields:

- in the `SOURCE_TYPE` field, the value `ObjectStorage`;
- in the `LOCATION` field, the network path to the bucket;
- in the `AUTH_METHOD` field, the value `NONE` (public bucket), `AWS`, or `SERVICE_ACCOUNT` (private bucket).

For examples with secrets and AWS parameters, see [reading from S3 buckets](../../../concepts/query_execution/federated_query/s3/external_data_source.md).

Connection is possible to any data sources that support the [AWS S3](https://docs.aws.amazon.com/AmazonS3/latest/API/Welcome.html) access protocol. Any URLs to systems supporting this protocol can be specified.

Typical `LOCATION` field values when connecting to different systems for bucket `bucket`:

| System name | URL |
|------|-------|
| {{ objstorage-name }} | `https://storage.yandexcloud.net/bucket/` |
| AWS S3 | `http://s3.amazonaws.com/bucket/` |

### Example

The query below creates an external data source named `TestDataSource` pointing to directory `folder` in bucket `bucket` in {{ objstorage-name }}:

```yql
CREATE EXTERNAL DATA SOURCE TestDataSource WITH (
  SOURCE_TYPE="ObjectStorage",
  LOCATION="http://s3.amazonaws.com/bucket/folder/",
  AUTH_METHOD="NONE"
);
```

## Connecting to {{ ydb-short-name }} (topics in another database) {#ydb}

Use `Ydb` in [streaming queries](../../../concepts/streaming-query.md) to read and write [topics](../../../concepts/datamodel/topic.md) in **another** {{ ydb-short-name }} database. See also [{#T}](../../../dev/streaming-query/local-and-external-topics.md).

Required fields:

| Field | Description |
|-------|-------------|
| `SOURCE_TYPE` | `Ydb` |
| `LOCATION` | Cluster endpoint, e.g. `localhost:2136` |
| `DATABASE_NAME` | Database path with topics, e.g. `/local` |
| `AUTH_METHOD` | `NONE` or `BASIC` |

Optional:

| Field | Description |
|-------|-------------|
| `USE_TLS` | `TRUE` / `FALSE` |
| `LOGIN`, `PASSWORD_SECRET_PATH` | For `AUTH_METHOD="BASIC"` |
| `SHARED_READING` | `TRUE` — shared topic reads across streaming queries for `json_each_row` and `raw` formats |

After creating source `ext_source`, topic `events` in the external database:

```yql
SELECT * FROM ext_source.events WITH (FORMAT = json_each_row, SCHEMA = (...));
```

### Example

```yql
CREATE EXTERNAL DATA SOURCE ydb_events WITH (
  SOURCE_TYPE = "Ydb",
  LOCATION = "localhost:2136",
  DATABASE_NAME = "/local",
  AUTH_METHOD = "NONE"
);
```
