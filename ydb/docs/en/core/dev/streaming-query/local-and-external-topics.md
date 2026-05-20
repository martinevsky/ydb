# Local and external topics in streaming queries

[Streaming queries](../../concepts/streaming-query.md) read events from [topics](../../concepts/datamodel/topic.md) and may write results back to topics. The source and sink can be a topic **in the same database** as the query, or a topic **in another** {{ ydb-short-name }} database.

All [streaming query](../../concepts/streaming-query.md) use cases work identically for local{#local-topics} and external{#external-topics} topics. A single query can read a local topic and write to an external one (or vice versa).

## Local topics {#local-topics}

**Local topics** are topics created in the **same** {{ ydb-short-name }} database as the [streaming query](../../concepts/streaming-query.md).

In query text they are referenced by **short name**, like a table in the current database:

```yql
SELECT * FROM input_topic WITH (FORMAT = json_each_row, SCHEMA = (...));
```

```yql
INSERT INTO output_topic SELECT ...;
```

## External topics {#external-topics}

**External topics** are topics located **in another** {{ ydb-short-name }} database.

Streaming queries access them only via a pre-created [external data source](../../concepts/datamodel/external_data_source.md) with `SOURCE_TYPE = "Ydb"`. Use [CREATE EXTERNAL DATA SOURCE](../../yql/reference/syntax/create-external-data-source.md#ydb); for authentication, use [secrets](../../yql/reference/syntax/create-secret.md).

### Creating the external data source {#create-ydb-source}

```yql
CREATE EXTERNAL DATA SOURCE ext_source WITH (
    SOURCE_TYPE = "Ydb",
    LOCATION = "cluster.example.com:2135",
    DATABASE_NAME = "/production/events",
    USE_TLS = "TRUE",
    AUTH_METHOD = "NONE"
);
```

`LOCATION` and `DATABASE_NAME` must point at the cluster and database where the topics live. For local development, `localhost:2136` and `/local` are typical — see the [quickstart](../../recipes/streaming_queries/topics.md).

### Shared reads (SHARED_READING) {#shared-reading}

If several streaming queries read the same external topic in `json_each_row` or `raw` formats, set `SHARED_READING = "TRUE"` on the source — this reduces load on the source cluster.

### Referencing topics {#external-topic-syntax}

After creating a source named `ext_source`, you reference topic `input_topic` in the external database as:

```yql
SELECT * FROM ext_source.input_topic WITH (FORMAT = json_each_row, SCHEMA = (...));
```

The name `ext_source` here is **a placeholder** — your source may have a different name; the prefix before the topic name must match the `CREATE EXTERNAL DATA SOURCE` object.

## See also

- [Common streaming query patterns](patterns.md) — ready-to-use YQL snippets
- [CREATE STREAMING QUERY](../../yql/reference/syntax/create-streaming-query.md) — creating a query
- [CREATE EXTERNAL DATA SOURCE](../../yql/reference/syntax/create-external-data-source.md) — declaring a source for an external database
- [External data source](../../concepts/datamodel/external_data_source.md) — concept
- [Topic](../../concepts/datamodel/topic.md) — data model
- [Enrichment](enrichment.md) — examples with topic + `JOIN`
- [Debug read from a topic](../../recipes/streaming_queries/debug-read.md)
