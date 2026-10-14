# Secret description and inventory RPC

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../../kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md), [inventory](../../../../security/iam_delegation/tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/grpc_services/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 20/20 GOOD in the feature-filtered cases, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundary: Direct RPC actor fixtures intercept schema and typed tablet replies, authentication and continuation tokens. They check whether a tablet request was made. These tests cover handler protocol decisions; tablet durability belongs to the tablet suite.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Authorization before metadata

- **GIVEN** plain secret or unauthorized RPC caller.
- **WHEN** DescribeSecret or administrative inventory runs.
- **THEN** plain/denied describes and unauthorized/invalid inventory input never read the tablet.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `DescribeSecretIamDelegation::PlainSecretDoesNotReadTablet` | [rpc_describe_secret_ut.cpp:174](../../../rpc_describe_secret_ut.cpp#L174) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::AccessDeniedDoesNotReadTablet` | [rpc_describe_secret_ut.cpp:184](../../../rpc_describe_secret_ut.cpp#L184) | `ydb/core/grpc_services/ut` |

#### Scenario: Coherent description revision

- **GIVEN** active and pending bindings or uncertain CREATE.
- **WHEN** the handler sees a changed revision or repeated concurrent changes.
- **THEN** description comes from one revision; stale binding data is discarded and retries are bounded.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `DescribeSecretIamDelegation::DescribesCurrentAndPendingFromOneRevision` | [rpc_describe_secret_ut.cpp:191](../../../rpc_describe_secret_ut.cpp#L191) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::CreateWithUnknownSetupHasOnlyPendingBinding` | [rpc_describe_secret_ut.cpp:205](../../../rpc_describe_secret_ut.cpp#L205) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::ChangedRevisionRestartsAndDiscardsOldBinding` | [rpc_describe_secret_ut.cpp:216](../../../rpc_describe_secret_ut.cpp#L216) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::ConcurrentChangesHaveBoundedRetries` | [rpc_describe_secret_ut.cpp:231](../../../rpc_describe_secret_ut.cpp#L231) | `ydb/core/grpc_services/ut` |

#### Scenario: Immutable description identity

- **GIVEN** a creating-operation mismatch or dropped object.
- **WHEN** the description is resolved.
- **THEN** no mismatched binding is exposed and tablet unavailability fails the call.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `DescribeSecretIamDelegation::CreateIdentityMismatchDoesNotExposeBinding` | [rpc_describe_secret_ut.cpp:242](../../../rpc_describe_secret_ut.cpp#L242) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::DroppedSecretDoesNotExposeBinding` | [rpc_describe_secret_ut.cpp:252](../../../rpc_describe_secret_ut.cpp#L252) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::TabletUnavailableFailsDescription` | [rpc_describe_secret_ut.cpp:261](../../../rpc_describe_secret_ut.cpp#L261) | `ydb/core/grpc_services/ut` |

#### Scenario: Retained inventory export

- **GIVEN** retained full binding and possibly reused database path.
- **WHEN** an explicit incarnation is requested or a path must be resolved.
- **THEN** all retained fields are exported; path reuse requires explicit identity and known empty differs from unknown.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ListDatabaseIamDelegations::ExplicitIncarnationExportsRetainedFullBinding` | [rpc_list_database_iam_delegations_ut.cpp:140](../../../rpc_list_database_iam_delegations_ut.cpp#L140) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::PathReuseRequiresExplicitIncarnation` | [rpc_list_database_iam_delegations_ut.cpp:163](../../../rpc_list_database_iam_delegations_ut.cpp#L163) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::RegisteredEmptyInventoryIsDistinctFromUnknownDatabase` | [rpc_list_database_iam_delegations_ut.cpp:261](../../../rpc_list_database_iam_delegations_ut.cpp#L261) | `ydb/core/grpc_services/ut` |

#### Scenario: Continuation and bounded validation

- **GIVEN** an inventory revision/cursor token.
- **WHEN** page parameters change, snapshot expires or registry scan is required.
- **THEN** token binding is enforced, expiry requires restart, malformed input never reads the tablet, and registry work is bounded.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ListDatabaseIamDelegations::UnauthorizedNeverReadsTablet` | [rpc_list_database_iam_delegations_ut.cpp:133](../../../rpc_list_database_iam_delegations_ut.cpp#L133) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::ContinuationCarriesRevisionAndCursor` | [rpc_list_database_iam_delegations_ut.cpp:201](../../../rpc_list_database_iam_delegations_ut.cpp#L201) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::ContinuationCannotChangePathOrIncarnation` | [rpc_list_database_iam_delegations_ut.cpp:218](../../../rpc_list_database_iam_delegations_ut.cpp#L218) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::ExpiredSnapshotRequiresCallerToRestart` | [rpc_list_database_iam_delegations_ut.cpp:231](../../../rpc_list_database_iam_delegations_ut.cpp#L231) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::InvalidPageSizeAndTokenDoNotReadTablet` | [rpc_list_database_iam_delegations_ut.cpp:242](../../../rpc_list_database_iam_delegations_ut.cpp#L242) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::RegistryScanHasBoundedWork` | [rpc_list_database_iam_delegations_ut.cpp:277](../../../rpc_list_database_iam_delegations_ut.cpp#L277) | `ydb/core/grpc_services/ut` |
