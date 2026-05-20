# Локальные и внешние топики в потоковых запросах

[Потоковые запросы](../../concepts/streaming-query.md) читают события из [топиков](../../concepts/datamodel/topic.md) и могут записывать результаты обратно в топики. Источником и приёмником сообщений может быть как топик **в той же базе данных**, в которой выполняется запрос, так и топик **в другой базе** {{ ydb-short-name }}.

Все сценарии использования [потоковых запросов](../../concepts/streaming-query.md) работают одинаково для локальных{#local-topics} и внешних{#external-topics} топиков. Один и тот же запрос может одновременно читать локальный топик, писать во внешний и наоборот.

## Локальные топики {#local-topics}

**Локальные топики**: топики, созданные в **той же базе** {{ ydb-short-name }}, что и [потоковый запрос](../../concepts/streaming-query.md).

В тексте запроса к ним обращаются **по короткому имени** — так же, как к таблице в текущей базе:

```yql
SELECT * FROM input_topic WITH (FORMAT = json_each_row, SCHEMA = (...));
```

```yql
INSERT INTO output_topic SELECT ...;
```

## Внешние топики {#external-topics}

**Внешние топики** — топики, расположенные **в другой базе** {{ ydb-short-name }}.

Доступ к ним из потокового запроса выполняется только через заранее созданный [внешний источник данных](../../concepts/datamodel/external_data_source.md) с `SOURCE_TYPE = "Ydb"`. Создание объекта — команда [CREATE EXTERNAL DATA SOURCE](../../yql/reference/syntax/create-external-data-source.md#ydb); при необходимости аутентификации используются [секреты](../../yql/reference/syntax/create-secret.md).

### Создание внешнего источника {#create-ydb-source}

```yql
CREATE EXTERNAL DATA SOURCE ext_source WITH (
    SOURCE_TYPE = "Ydb",
    LOCATION = "cluster.example.com:2135",
    DATABASE_NAME = "/production/events",
    USE_TLS = "TRUE",
    AUTH_METHOD = "NONE"
);
```

`LOCATION` и `DATABASE_NAME` должны указывать на кластер и базу, где созданы топики. В локальной разработке часто используют `localhost:2136` и `/local` — см. [быстрый старт](../../recipes/streaming_queries/topics.md).

### Совместное чтение (SHARED_READING) {#shared-reading}

Если несколько потоковых запросов читают один и тот же внешний топик в форматах `json_each_row` или `raw`, добавьте `SHARED_READING = "TRUE"` при создании источника — это снижает нагрузку на кластер-источник.

### Обращение к топикам {#external-topic-syntax}

После создания источника, например с именем `ext_source`, обращение к топику `input_topic` во внешней базе записывается так:

```yql
SELECT * FROM ext_source.input_topic WITH (FORMAT = json_each_row, SCHEMA = (...));
```

Имя `ext_source` в документации **условное** — в вашей базе источник может называться иначе; важно, чтобы оно совпадало в `CREATE EXTERNAL DATA SOURCE` и в префиксе перед именем топика.

## См. также

- [Типичные шаблоны потоковых запросов](patterns.md) — готовые фрагменты YQL
- [CREATE STREAMING QUERY](../../yql/reference/syntax/create-streaming-query.md) — создание запроса
- [CREATE EXTERNAL DATA SOURCE](../../yql/reference/syntax/create-external-data-source.md) — объявление источника для внешней базы
- [Внешний источник данных](../../concepts/datamodel/external_data_source.md) — концепция
- [Топик](../../concepts/datamodel/topic.md) — модель данных
- [Обогащение данных](enrichment.md) — примеры с чтением из топика и `JOIN`
- [Отладочное чтение из топика](../../recipes/streaming_queries/debug-read.md)
