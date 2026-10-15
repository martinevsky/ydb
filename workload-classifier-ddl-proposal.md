# Proposal: Predicate-based workload classifiers (reject full-scan DELETEs)

## Background

YDB today has resource pool classifiers with a flat key=value settings form:

```sql
CREATE RESOURCE POOL CLASSIFIER MyClassifier WITH (
    RANK = 20,
    RESOURCE_POOL = 'wgUserQueries',
    MEMBER_NAME = 'yandex_query@abc'
);
```

Matching is limited to session attributes (`MEMBER_NAME`, `DATABASE`). There is
no way to classify — or reject — a query by properties of the query itself
(statement type, plan shape, touched tables). The goal here is a DDL that can
express rules like "reject any DELETE that performs a full scan of
`/db/big_table`".

## Design sketch (from the request)

```sql
CREATE CLASSIFIER c_reject_fts WITH (order = 10) AS
DELETE FROM workload
WHERE
    full_scan = 'TRUE' AND path LIKE "/db/big_table";
```

The useful ideas in this sketch, kept in the proposal below:

- a **rank/order** so multiple rules form an ordered list (first match wins);
- a **WHERE predicate** over query/plan attributes instead of only key=value
  settings;
- an **action** applied to matching queries.

Changed relative to the sketch:

- `ORDER` is a reserved word and YDB classifiers already use `RANK` — reuse it
  (lower rank = evaluated first, same as existing classifiers).
- `DELETE FROM workload` reads as DML over a table; the action is modeled
  explicitly as `REJECT` (or `ROUTE TO pool`) instead. This keeps the door open
  to more actions later without overloading DML keywords.
- String `'TRUE'` becomes a real boolean; `path` becomes `TABLE_PATH`, and the
  match attributes become a documented, extensible set.

## Proposed syntax

```sql
CREATE RESOURCE POOL CLASSIFIER <name> WITH (
    RANK = <int>,                -- required; lower = evaluated first
    ACTION = 'REJECT'            -- 'REJECT' | 'ROUTE'
    RESOURCE_POOL = '<pool>',    -- required iff ACTION = 'ROUTE'
    REJECT_MESSAGE = '<text>',   -- optional, surfaced in the error
    MATCH = $$ <predicate> $$    -- required for predicate classifiers
);
```

Example — the motivating rule:

```sql
CREATE RESOURCE POOL CLASSIFIER c_reject_fts WITH (
    RANK = 10,
    ACTION = 'REJECT',
    REJECT_MESSAGE = 'Full-scan DELETE on big_table is not allowed; use a key range',
    MATCH = $$
        statement_type = 'DELETE'
        AND full_scan = TRUE
        AND table_path LIKE '/db/big_table%'
    $$
);
```

Routing variant (throttle instead of reject):

```sql
CREATE RESOURCE POOL CLASSIFIER c_throttle_fts WITH (
    RANK = 20,
    ACTION = 'ROUTE',
    RESOURCE_POOL = 'wg_low_priority',
    MATCH = $$
        full_scan = TRUE
        AND estimated_read_bytes > 1099511627776   -- 1 TiB
    $$
);
```

## MATCH predicate

A restricted boolean expression over **match attributes**. Supported operators:
`=`, `!=`, `<`, `<=`, `>`, `>=`, `AND`, `OR`, `NOT`, parentheses, `LIKE`
(SQL patterns, `%`/`_`), `IN (...)`. No functions, no subqueries, no
references to user tables — the predicate must be evaluable at query-admission
time, before execution starts.

Initial attribute set:

| Attribute              | Type    | Meaning                                              |
|------------------------|---------|------------------------------------------------------|
| `statement_type`       | string  | `'SELECT'`, `'DELETE'`, `'UPDATE'`, `'UPSERT'`, ...  |
| `full_scan`            | bool    | plan reads a table without a key-range/index prefix  |
| `table_path`           | string  | path of a table touched by the plan (multi-valued: matches if ANY touched table matches) |
| `estimated_read_bytes` | int     | optimizer estimate, 0 if unknown                     |
| `estimated_rows`       | int     | optimizer estimate                                   |
| `member_name`          | string  | authenticated user (existing classifier semantic)    |
| `database`             | string  | database path (existing classifier semantic)         |

Multi-valued `table_path` semantics: `table_path LIKE '/db/big_table%'` is true
if **any** table in the plan matches. `NOT (table_path LIKE ...)` therefore
means "no touched table matches", which is the safe reading for guard rules.

## Semantics

- Classifiers are evaluated in ascending `RANK`; the first classifier whose
  `MATCH` predicate (and legacy `MEMBER_NAME`/`DATABASE` settings, if present)
  matches the query applies its action:
  - `REJECT` → query fails at admission with `PRECONDITION_FAILED` (or a new
    dedicated status) and `REJECT_MESSAGE`.
  - `ROUTE` → query runs in `RESOURCE_POOL` (current behavior).
- A classifier with no `MATCH` behaves exactly like today's classifiers —
  full backward compatibility.
- `full_scan` and the estimates are computed from the compiled plan in KQP;
  for queries where the plan cannot be inspected (e.g. some DDL), these
  attributes are `NULL` and comparisons against them are false (three-valued
  logic, NULL-safe).

## Companion DDL

Fits the existing classifier family:

```sql
ALTER RESOURCE POOL CLASSIFIER c_reject_fts
    SET (RANK = 15, MATCH = $$ statement_type IN ('DELETE','UPDATE') AND full_scan = TRUE $$);

ALTER RESOURCE POOL CLASSIFIER c_reject_fts RESET (MATCH);

DROP RESOURCE POOL CLASSIFIER c_reject_fts;
```

Introspection:

```sql
SELECT * FROM `.metadata/workload_manager/resource_pool_classifiers`;
-- plus MATCH text and parsed-attributes columns
```

`MATCH` is stored as text in the same scheme object as other classifier
settings and validated/parsed at CREATE/ALTER time (fail fast on unknown
attributes or operators).

## Grammar impact

`MATCH = $$ ... $$` keeps the existing `WITH (key = value, ...)` setting list:
the predicate is just a string-valued setting at the grammar level, parsed by
the classifier subsystem — no new top-level statement, minimal changes to
`sql_ddl_resource_pool.cpp` / `ddl_resource_pool.cpp`. If the predicate DSL
grows, it can later be promoted to a real grammar production:

```sql
CREATE RESOURCE POOL CLASSIFIER c_reject_fts WITH (RANK = 10, ACTION = 'REJECT')
WHERE statement_type = 'DELETE' AND full_scan = TRUE AND table_path LIKE '/db/big_table%';
```

## Open questions

1. Exact definition of `full_scan` for writes: DELETE/UPDATE compile to
   read+write; is "full scan" about the read side lacking a key prefix?
2. Should `REJECT` apply at prepare/compile time only, or also re-checked at
   execution (plans can change with parameter values)?
3. Multi-valued `table_path` vs. repeating attributes (`ANY_TABLE(...)`) — is
   the implicit-ANY reading sufficient?
4. Do we need `ACTION = 'TAG'` (attach a label, keep running) for observability
   before enforcing rejection?
