# Secret description and inventory RPC

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../../kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md), [inventory](../../../../security/iam_delegation/tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/grpc_services/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 20/20 GOOD in the feature-filtered cases (`-F 'DescribeSecretIamDelegation::*' -F 'ListDatabaseIamDelegations::*'`), with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../security/iam_delegation/docs/external-secrets/verification.md). These results describe that historical revision; new cases below require fresh execution.

Fixture boundary: Direct RPC actor fixtures intercept schema and typed tablet replies and observe handler protocol decisions. The two real-inventory cases bridge actual RPC commands and responses through a separate real tablet runtime; they do not reimplement pagination or health projection. Physical storage reopening is covered by the owning tablet suite.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Authorization before metadata

- **GIVEN** plain secret or unauthorized RPC caller.
- **WHEN** DescribeSecret or administrative inventory runs.
- **THEN** plain/denied describes and unauthorized inventory requests never read the tablet.

Contract coverage: external-secret-schema / "Metadata is independent of token health"; "Stored-secret compatibility" — scenario "Existing stored value" (PlainSecretDoesNotReadTablet); iam-delegation-inventory / "Administrative export is authorized" — scenario "Caller lacks administrative permission".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `DescribeSecretIamDelegation::PlainSecretDoesNotReadTablet` | [rpc_describe_secret_ut.cpp:174](../../../rpc_describe_secret_ut.cpp#L174) | `ydb/core/grpc_services/ut` |
| `DescribeSecretIamDelegation::AccessDeniedDoesNotReadTablet` | [rpc_describe_secret_ut.cpp:184](../../../rpc_describe_secret_ut.cpp#L184) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::UnauthorizedNeverReadsTablet` | [rpc_list_database_iam_delegations_ut.cpp:133](../../../rpc_list_database_iam_delegations_ut.cpp#L133) | `ydb/core/grpc_services/ut` |

#### Scenario: Coherent description revision

- **GIVEN** active and pending bindings or uncertain CREATE.
- **WHEN** the handler sees a changed revision or repeated concurrent changes.
- **THEN** description comes from one revision; stale binding data is discarded and retries are bounded.

Contract coverage: external-secret-schema / "Metadata is independent of token health" (description without minting, coherent revision).

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

Contract coverage: iam-delegation-lifecycle / "Immutable identities and idempotent mutations" — scenario "Database and secret paths are reused" (description side).

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

Contract coverage: iam-delegation-inventory / "Inventory is authoritative beyond schema lifetime"; "Database incarnations are explicit" — scenario "Reused database path".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ListDatabaseIamDelegations::ExplicitIncarnationExportsRetainedFullBinding` | [rpc_list_database_iam_delegations_ut.cpp:140](../../../rpc_list_database_iam_delegations_ut.cpp#L140) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::PathReuseRequiresExplicitIncarnation` | [rpc_list_database_iam_delegations_ut.cpp:163](../../../rpc_list_database_iam_delegations_ut.cpp#L163) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::RegisteredEmptyInventoryIsDistinctFromUnknownDatabase` | [rpc_list_database_iam_delegations_ut.cpp:261](../../../rpc_list_database_iam_delegations_ut.cpp#L261) | `ydb/core/grpc_services/ut` |

#### Scenario: Continuation and bounded validation

- **GIVEN** an inventory revision/cursor token.
- **WHEN** page parameters change, snapshot expires or registry scan is required.
- **THEN** expiry requires restart, invalid public parameters never read the tablet, and registry work is bounded. Authenticated-token binding is verified by the tablet in the new cases below. The historical unsigned-token cases were replaced by opaque forwarding and authoritative rejection cases.

Contract coverage: iam-delegation-inventory / "Inventory pagination is revision-bound and bounded" — scenarios "Mutation between pages", "Unsupported page size"; "Administrative export is authorized" — scenario "Continuation is used for another incarnation"; "Database discovery tolerates concurrent registration" (bounded registry scan).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ListDatabaseIamDelegations::ExpiredSnapshotRequiresCallerToRestart` | [rpc_list_database_iam_delegations_ut.cpp:231](../../../rpc_list_database_iam_delegations_ut.cpp#L231) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::InvalidPageSizeAndTokenDoNotReadTablet` | [rpc_list_database_iam_delegations_ut.cpp:242](../../../rpc_list_database_iam_delegations_ut.cpp#L242) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::RegistryScanHasBoundedWork` | [rpc_list_database_iam_delegations_ut.cpp:277](../../../rpc_list_database_iam_delegations_ut.cpp#L277) | `ydb/core/grpc_services/ut` |

#### Scenario: Metadata describes during an IAM outage

- **GIVEN** an active delegated secret while IAM is unavailable.
- **WHEN** DescribeSecret runs.
- **THEN** the description succeeds from durable schema and tablet state without contacting IAM.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-schema / "Metadata is independent of token health" — scenario "IAM is unavailable during metadata access" (describe half).

#### Scenario: Authorization revoked between pages

- **GIVEN** an administrator whose authorization is revoked after the first inventory page.
- **WHEN** a continuation request arrives with an otherwise valid token.
- **THEN** the continuation is rejected and no further records are disclosed.

Status: executable cases mapped below; RED execution pending. No passing verification is claimed.

Contract coverage: iam-delegation-inventory / "Administrative export is authorized" — scenario "Authorization is revoked between pages".

#### Scenario: Exactly-once enumeration

- **GIVEN** an unchanged inventory enumerated over several pages.
- **WHEN** the caller stops at the first short page.
- **THEN** the caller has received every record exactly once.

Status: executable cases mapped below; RED execution pending. No passing verification is claimed.

Contract coverage: iam-delegation-inventory / "Inventory pagination is revision-bound and bounded" — scenario "Consumer stops on a short page".

#### Scenario: No dedup by account

- **GIVEN** several delegations of one incarnation referring to the same service account.
- **WHEN** the inventory is exported.
- **THEN** each distinct referrer and its complete binding appears separately.

Status: executable cases mapped below; RED execution pending. No passing verification is claimed.

Contract coverage: iam-delegation-inventory / "Inventory is authoritative beyond schema lifetime" — scenario "Multiple delegations use one account".

#### Scenario: Inventory after tenant deletion preserves ownership

- **GIVEN** a deleted incarnation whose inventory includes an uncertain setup with a saved operation identifier.
- **WHEN** an authorized caller lists it while cleanup continues, loses the response or repeats enumeration.
- **THEN** the saved binding, operation identifier and unsafe-revocation warning remain visible and cleanup responsibility stays with the tablet.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-inventory / "Tablet cleanup ownership survives database deletion" — scenarios "Tenant disappears with cleanup outstanding", "Inventory response is lost during deletion"; "Uncertain exports carry their resolution constraint" — scenario "Uncertain setup is listed after deletion"; "Listing is observational" — scenario "Concurrent setup during export".

#### Scenario: Authenticated continuations survive identity reuse and storage reopening

- **GIVEN** an authenticated continuation with its original incarnation and a durable signing key.
- **WHEN** payload/tag fields are altered, the path is reused, storage reopens, or a mixed-version tablet lacks authenticated mode.
- **THEN** valid continuations retain the signed identity, tampering is rejected, keys survive reopening, and unsupported mode fails closed; omitted/zero public page size still means 100.

Status: executable cases mapped below; RED execution pending. No passing verification is claimed.

Contract coverage: iam-delegation-inventory / "Administrative export is authorized" — scenarios "Continuation payload is modified", "Database path is reused between pages"; "Authenticated pagination has an explicit protocol mode" — scenario "Tablet lacks authenticated pagination support"; "Inventory pagination is revision-bound and bounded" — scenario "Public page size uses the default".

#### Scenario: Authenticated protocol and observable cleanup health

- **GIVEN** real saved delegation state, optional historical health fields, and mixed frontend/tablet capabilities.
- **WHEN** an administrator enumerates across pages or evaluates configured age/failure thresholds.
- **THEN** the RPC forwards authenticated tokens without rediscovery, refuses unsupported capability and unsigned v1 tokens, exports each same-account referrer separately, preserves unknown age, and exposes progress/recovery without changing cleanup ownership.

Status: executable regression source added; RED execution pending. No passing verification is claimed. The source mappings below also cover the authorization, enumeration and authenticated-continuation scenarios above.

Contract coverage: iam-delegation-inventory / "Administrative export is authorized"; "Authenticated pagination has an explicit protocol mode"; "Inventory pagination is revision-bound and bounded"; iam-delegation-revocation / "Outstanding cleanup is operator-visible".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ListDatabaseIamDelegations::AuthenticatedContinuationIsOpaqueAndSkipsRegistryDiscovery` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::ContinuationRequiresFreshAdministrativeAuthorization` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::OldTabletCannotSilentlyDowngradeEmptyOrFinalPage` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::UnsignedLegacyContinuationRequiresEnumerationRestart` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::TabletRejectsTamperedContinuationWithoutRpcFallback` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::AuthenticatedPageSizesRetainPublicZeroDefault` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::InventoryExportsDurableCleanupHistoryWithoutInventingLegacyAge` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::RealAuthenticatedPagesPreserveEverySameAccountReferrer` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::RealInventorySupportsExternalAgeAndFailureThresholds` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
| `ListDatabaseIamDelegations::ContinuationForwardsRequestedIdentityAndTabletRejection` | [rpc_list_database_iam_delegations_ut.cpp](../../../rpc_list_database_iam_delegations_ut.cpp) | `ydb/core/grpc_services/ut` |
