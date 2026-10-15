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
        AND EXISTS TABLE WHERE path LIKE '/db/big_table%' AND full_scan
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

There are two attribute scopes: **query-level** and **per-table**.

Query-level attributes:

| Attribute              | Type    | Meaning                                              |
|------------------------|---------|------------------------------------------------------|
| `statement_type`       | string  | `'SELECT'`, `'DELETE'`, `'UPDATE'`, `'UPSERT'`, ...  |
| `estimated_read_bytes` | int     | optimizer estimate for the whole query, 0 if unknown |
| `estimated_rows`       | int     | optimizer estimate                                   |
| `member_name`          | string  | authenticated user (existing classifier semantic)    |
| `database`             | string  | database path (existing classifier semantic)         |

Per-table attributes (only valid inside a table quantifier):

| Attribute    | Type   | Meaning                                                  |
|--------------|--------|----------------------------------------------------------|
| `path`       | string | full path of the table                                   |
| `full_scan`  | bool   | this table is read without a key-range/index prefix      |
| `read_bytes` | int    | optimizer estimate for this table                        |
| `access`     | string | `'READ'`, `'WRITE'`, `'READ_WRITE'`                      |

## Table quantifiers

One request can touch several tables, and full scans may apply to only some of
them — so per-table attributes must be correlated **per table**, not flattened
to the query level. Two quantifiers bind them:

```sql
EXISTS TABLE WHERE <per-table predicate>   -- at least one touched table satisfies it
ALL TABLES WHERE <per-table predicate>     -- every touched table satisfies it
```

Why the flat form is wrong. Consider a DELETE whose plan reads `big_table` by
primary key and full-scans `other_table`:

```sql
-- flat (query-level) predicate: over-rejects
full_scan = TRUE AND table_path LIKE '/db/big_table%'
--   full_scan is TRUE (because of other_table) and big_table IS touched,
--   so the rule fires — even though big_table itself was read by key.

-- correlated per-table predicate: correct
EXISTS TABLE WHERE path LIKE '/db/big_table%' AND full_scan
--   no single table satisfies both conjuncts → no rejection.
```

Truth table for `EXISTS TABLE WHERE path LIKE '/db/big%' AND full_scan`:

| Plan touches                                  | Result |
|-----------------------------------------------|--------|
| big_table full scan only                      | match  |
| big_table full scan + other table key lookup  | match  |
| big_table key lookup + other table full scan  | no match |
| big_table key lookup only                     | no match |

Useful patterns:

```sql
-- reject if ANY table in the query is full-scanned, regardless of which
EXISTS TABLE WHERE full_scan

-- reject full scans of anything under /db/ except an allow-listed dir
EXISTS TABLE WHERE full_scan AND path LIKE '/db/%' AND path NOT LIKE '/db/scratch/%'

-- allow the query only if every touched table is read by key
ALL TABLES WHERE NOT full_scan

-- reject writes that full-scan the written table itself
EXISTS TABLE WHERE full_scan AND access IN ('WRITE', 'READ_WRITE')
```

For convenience the bare query-level `full_scan` remains as a shorthand for
`EXISTS TABLE WHERE full_scan`, but combining it with a path match is a
compile-time warning precisely because of the correlation pitfall above.

## Evaluation semantics (the correlation guarantee)

The evaluator never sees a flat bag of query properties. It works in two
steps:

1. **Record construction.** From the compiled plan, KQP builds one record per
   touched table: `{path, full_scan, read_bytes, access}`. `full_scan` for a
   table is derived from that table's own read nodes in the plan
   (`TableRange` without a key prefix / `TableLookup` etc.), so the flag is a
   property of the table's access, not of the query.
2. **Quantifier evaluation.** `EXISTS TABLE WHERE P` literally means
   *∃ record r: P(r)* — every attribute inside `P` is read from the **same**
   record. `path LIKE '/db/big%'` and `full_scan` in one predicate can only
   both be true if one table's record carries both. There is no code path that
   mixes `path` from one record with `full_scan` from another; the flat form
   that would allow this is the deprecated shorthand above and triggers the
   warning.

Edge case — a table read more than once in one plan (self-join, or key lookup
plus a secondary-index scan): the correlation unit is the **table**, not the
individual read. The record aggregates all reads of that table, with
`full_scan = TRUE` if *any* of its reads lacks a key prefix (conservative:
one full scan of the table is enough to match). Read-level granularity, if
ever needed, would be a separate `EXISTS READ WHERE ...` quantifier — out of
scope here.

Determinism: the record set is a pure function of the compiled plan, so the
same query text + parameters + scheme version always classifies the same way.

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
   (The `access` per-table attribute sidesteps part of this by letting rules
   target the written table explicitly.)
2. Should `REJECT` apply at prepare/compile time only, or also re-checked at
   execution (plans can change with parameter values)?
3. Quantifier nesting: is a single `EXISTS TABLE` / `ALL TABLES` level enough,
   or do real rules need correlation across two tables (e.g. "full scan of A
   joined with any read of B")? A bound table variable
   (`EXISTS TABLE t WHERE t.path = ... AND EXISTS TABLE u WHERE u...`) would
   cover that at the cost of a more complex evaluator.
4. Do we need `ACTION = 'TAG'` (attach a label, keep running) for observability
   before enforcing rejection?
