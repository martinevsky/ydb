# Real IAM protocol POC

This separate no-PR branch adds a deliberately simple compile-time identity
override for manually exercising the complete external-data-source delegation
lifecycle against a real IAM private-api endpoint. Never commit or push a
non-empty token.

## Prepare a matching token and subject

Use one `yc` profile for both commands:

```bash
yc iam whoami --format json
yc iam create-token
```

Take the `id` from `whoami`, append `@as`, and paste it into
`IamDelegationTestUserSid` in `iam_delegation_ddl_runner.cpp`. Paste the raw
output of `create-token` into `IamDelegationTestToken`.

The token constant is empty in committed code, which disables the override.
The manual value is compiled into the source and binary. Clear it immediately
after the run and rebuild before sharing the worktree or binary.

The synthetic `NACLib::TUserToken` follows the ordinary DDL identity path. Its
serialized representation, including `OriginalUserToken`, travels through
SchemeCache, TxProxy, and SchemeShard for schema authorization and auditing.
The delegation path requires this token: its serialized form must parse back
to the same AccessService SID and non-empty original bearer before any
ServiceControl operation starts. If the compile-time override is empty, the
incoming request must provide an equivalent authenticated token itself.
`SetupDelegation.on_behalf_of_subject_id` is derived from the matching verified
SID. The original raw user token is also the bearer used to authenticate
`EnsureEnabled`, `SetupDelegation`, `RevokeDelegation`, and
`OperationService.Get`. This lets the manual run exercise the same initiating
user credential end to end, including SchemeShard transport.

## Build and configure YDB

```bash
./ya make --build relwithdebinfo ydb/apps/ydbd
```

Merge the following into the local server configuration, replacing endpoint
and registration values for the target stand:

```yaml
feature_flags:
  enable_external_data_sources: true
  enable_external_data_source_auth_method_iam: true
  enable_external_data_source_iam_delegation: true

replication_config:
  iam_service_control:
    endpoint: ts.private-api.cloud.yandex.net:4282
    service_id: ydb
    microservice_id: data-plane
    resource_type: resource-manager.cloud
    enable_ssl: true
```

The database root must expose the cloud whose resource is delegated:

```bash
ydb -e grpc://localhost:2136 -d /Root \
  database attribute set cloud_id=<SOURCE_CLOUD_ID>
```

Grant the synthetic SID schema rights using an administrator identity:

```bash
ydb -e grpc://localhost:2136 -d /Root \
  scheme permissions grant -p ydb.generic.full /Root <SUBJECT_ID>@as
```

## Exercise the lifecycle

```sql
CREATE EXTERNAL DATA SOURCE `/Root/iam_poc` WITH (
    SOURCE_TYPE = "Ydb",
    LOCATION = "grpcs://target-ydb-endpoint:2135",
    DATABASE_NAME = "/target/database",
    USE_TLS = "true",
    AUTH_METHOD = "IAM",
    SERVICE_ACCOUNT_ID = "<TARGET_SERVICE_ACCOUNT_ID>"
);
```

Run `CREATE OR REPLACE`, `CREATE IF NOT EXISTS`, and then:

```sql
DROP EXTERNAL DATA SOURCE `/Root/iam_poc`;
```

Verify in IAM introspection that the partly readable
`eds:<name-prefix>:<uuid>` referrer appears after CREATE, changes without
leaking the old reference after replacement, does not multiply after an
`IF NOT EXISTS` no-op, and disappears after DROP. IAM operations may initially
return `done=false`; the YDB DDL must remain pending and poll the same operation
id until it reaches a terminal state.

The ordinary sequential `IF NOT EXISTS` no-op is covered by a preflight
describe. This POC intentionally does not change the shared
`TSchemeOpRequestHandler` to distinguish a raced `StatusAlreadyExists` from
SQL-level success. Do not use concurrent `IF NOT EXISTS` calls as a lifecycle
guarantee test; resolving that narrow race is production follow-up work.
