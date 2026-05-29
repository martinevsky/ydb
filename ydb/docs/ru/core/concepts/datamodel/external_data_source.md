# Внешние источники данных

Внешний источник (external data source) - это объект в {{ ydb-full-name }}, описывающий параметры подключения к внешнему источнику данных. Например, в случае ClickHouse внешний источник описывает сетевой адрес, логин и пароль для аутентификации в кластере ClickHouse, а в случае S3 ({{ objstorage-name }}) описывает реквизиты доступа и путь к бакету.

В следующем примере приведен пример создания внешнего источника, ведущего на кластер ClickHouse:

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

После создания внешнего источника данных можно выполнять чтение данных из созданного объекта `EXTERNAL DATA SOURCE`. Пример ниже иллюстрирует чтение данных из таблицы `test_table` из базы данных `default` в кластере ClickHouse:

```yql
SELECT * FROM test_data_source.test_table;
```

С помощью внешних источников данных можно выполнять [федеративные запросы](../query_execution/federated_query/index.md) для задач межсистемной аналитики данных.

## IAM-аутентификация для источников данных {{ ydb-short-name }} {#iam-auth}

При использовании {{ ydb-short-name }} в качестве внешнего источника данных в долгосрочных запросах к топикам стандартная аутентификация по токену становится непрактичной: облачные токены имеют ограниченный срок действия, из-за чего запросы приходится перезапускать каждый раз при истечении токена.

Для решения этой проблемы {{ ydb-full-name }} поддерживает IAM-аутентификацию с делегированием сервисному аккаунту для внешних источников данных с `SOURCE_TYPE="Ydb"`. При создании внешнего источника данных указывается начальный IAM-токен в параметре `INITIAL_TOKEN_SECRET_PATH`. Этот токен используется один раз для проверки доступности целевой базы данных и определения её идентификатора ресурса в облаке. Дальнейшая аутентификация выполняется сервисным аккаунтом, указанным в `SERVICE_ACCOUNT_ID`, чьи токены обновляются автоматически. Это позволяет стриминговым запросам работать бесконечно долго без перезапуска.

Для использования данного метода аутентификации необходимо включить флаг `enable_external_data_source_auth_method_iam` в [конфигурации](../../reference/configuration/feature_flags.md).

Чтобы использовать IAM-аутентификацию, сначала сохраните начальный IAM-токен в виде [секрета](secrets.md):

```yql
CREATE SECRET <token_secret_name> WITH (value = "<iam_token>");
```

Где:

- `<token_secret_name>` -- имя, которое вы присваиваете секрету, например `mytoken`.
- `<iam_token>` -- начальный IAM-токен сервисного аккаунта.

Затем создайте внешний источник данных:

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

Где:

- `<source_name>` -- имя, которое вы присваиваете внешнему источнику данных, например `ydb_datasource`.
- `<host>` -- имя хоста целевого экземпляра {{ ydb-short-name }}, например `u-lb.abcde12345.ydb.mdb.yandexcloud.net`.
- `<port>` -- порт gRPCs при `USE_TLS="TRUE"` или порт gRPC в противном случае, например `2135`.
- `<database>` -- полный путь к целевой базе данных, например `/ru-central1/b1g8skpblkos03malf3s/etndqstq7ne4v68n6gpr`.
- `<service_account_id>` -- идентификатор сервисного аккаунта, которому предоставлен доступ к целевой базе данных, например `ajehr5a9p9lc7c7qnhdc`.
- `<token_secret_name>` -- имя секрета, созданного на предыдущем шаге.

В качестве источников данных можно использовать:

- [ClickHouse](../query_execution/federated_query/clickhouse.md)
- [PostgreSQL](../query_execution/federated_query/postgresql.md)
- [{{ ydb-short-name }}](../query_execution/federated_query/ydb.md)
- [Подключения к S3 ({{ objstorage-name }})](../query_execution/federated_query/s3/external_data_source.md)

