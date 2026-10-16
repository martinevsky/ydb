# KQP secret orchestration

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md), [lifecycle](../../../../../security/iam_delegation/tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md).

## Execution and evidence

Latest recorded owning run: **146 GOOD** (full owning suite) at `9a2b73490817`, `final-fixtures-token-lifetime-green-parallel100`; actual combined command exit 0. Historical counts below retain their original revisions.

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/kqp/executer_actor/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Historical source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Historical status: **implemented and verified**, 119/119 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md). These counts apply only to that source revision; added cases below require separate fresh execution.

Fixture boundary: TOrchestratorFixture runs real SchemeShard, SchemeCache, independent delegation tablet, coordinator, worker and reader actors. IAM/cloud responses are intercepted. ACL tests use serialized authenticated identities and assert effective rights before operations; public describe/value probes remain separate from internal lifecycle access.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Authenticated ALTER rights

- **GIVEN** an identity with target AlterSchema but no DescribeSchema/SelectRow.
- **WHEN** ALTER or CREATE OR REPLACE is executed with the required parent rights.
- **THEN** the operation succeeds without granting value/describe access; DescribeSchema-only or hidden-path identities remain unauthorized.

Contract coverage: external-secret-schema / "Operation-specific DDL authorization" — scenarios "Authorized alteration without visibility or value access", "Hidden existing object", "Selective replacement rights".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::AlterNeedsAlterSchemaWithoutDescribeOrValueAccess` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DescribeSchemaAndHiddenPathsDoNotAuthorizeAlter` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::CreateOrReplaceRequiresParentCreateAndTargetAlterWithoutDescribe` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::CreateOrReplaceMissingPathRequiresParentCreatePermission` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Revoked ACL and reused paths

- **GIVEN** authorization or resolution is paused.
- **WHEN** ACL is revoked or the path is dropped and recreated.
- **THEN** lifecycle mutation cannot touch the replacement object or rely on obsolete access.

Contract coverage: external-secret-schema / "Authorization and identity remain current" — scenarios "Permission revoked after lookup", "Path recreated during resolution".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::AlterRechecksRevokedAclBeforeLifecycleMutation` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::AlterCannotTouchRecreatedSecretDuringResolution` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::AlterCannotTouchRecreatedSecretAfterAuthorization` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Coordinator crash and durable recovery

- **GIVEN** a staged CREATE or schema shell.
- **WHEN** the coordinator disappears before submission, after schema creation, or after an accepted IAM operation.
- **THEN** the worker resumes persisted intent/subject/schema identity and polls the saved operation after restart.

Contract coverage: iam-delegation-lifecycle / "Intent precedes remote setup" — scenario "Coordinator fails before schema submission".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::WorkerResumesCrashAfterDurableStageBeforeSchemaSubmission` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerRecoversLostStageReplyWithoutNewUserRequest` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerResumesCrashAfterSchemaBeforeSetup` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerPollsAcceptedSetupAfterClientAndTabletRestart` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Delayed schema copies and consumed identities

- **GIVEN** original CREATE and worker replay are delayed.
- **WHEN** DROP, sibling mutation or parent/database name reuse consumes the saved fence.
- **THEN** all delayed copies use one schema identity or fail without creating a replacement shell.

Contract coverage: iam-delegation-lifecycle / "Missing schema alone does not prove safe abandonment" — scenario "Delayed original CREATE arrives after cleanup"; "Immutable identities and idempotent mutations" — scenario "Database and secret paths are reused".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::DelayedOriginalCreateAndWorkerReplayUseOneSchemaIdentity` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DropBeforeBindingFencesEveryDelayedCreateCopy` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::SiblingMutationConsumesSavedCreateFence` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ParentNameReuseCannotReplayOldCreateIntent` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DatabaseNameReuseCannotReplayOldCreateIntent` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Noops and ordinary SQL integration

- **GIVEN** missing/existing targets and feature configuration.
- **WHEN** noops, EXPLAIN or SQL CREATE/ALTER/DROP run.
- **THEN** noops and EXPLAIN avoid lifecycle/IAM writes; real SQL reaches the tablet and reader.

Contract coverage: external-secret-schema / "Stored-secret compatibility" — scenarios "No-op statement dispatches nothing", "Missing ALTER IF EXISTS" (NoopsNeedNoIamConfigurationOrLifecycleMutation); "Metadata is independent of token health" — scenario "IAM is unavailable during metadata access" (no-dispatch half: EXPLAIN and noops need no IAM configuration); "Delegation source syntax" — scenario "Create a delegation secret" (happy-path half via SqlCreateAlterAndDrop). This delegation fixture does not establish the separate "Existing stored value" scenario.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::NoopsNeedNoIamConfigurationOrLifecycleMutation` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ExplainCompilesWithoutLifecycleWritesOrIamConfiguration` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::SqlCreateAlterAndDropReachTabletAndReader` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Setup subject and cloud authorization

- **GIVEN** a CREATE or ALTER with a delegation source.
- **WHEN** the caller is a builtin identity, IAM denies the initiating subject permission to delegate the account, or the account is not delegable in the recorded cloud.
- **THEN** the builtin identity is rejected before any IAM dispatch; the two IAM-denial cases fail with the IAM error after dispatch, and no binding is activated in any case.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::BuiltinSubjectWithSchemaRightsCannotStageDelegation` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::CoordinatorOwnedPolledRefusalPersistsSafeDetails` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: external-secret-schema / "Delegation uses the initiating cloud subject" — scenarios "Builtin identity attempts setup", "Caller cannot delegate the account"; "Setup fails for an account not delegable in its cloud" — scenario "Account not delegable in the recorded cloud".

#### Scenario: Cloud resolution and retention

- **GIVEN** cloud discovery unavailable, indeterminate, or a previously recorded cloud.
- **WHEN** setup resolves the cloud or ALTER omits it.
- **THEN** the configured database fallback applies, an indeterminate cloud fails the setup, and ALTER retains the recorded cloud.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::CloudLookupForwardsCallerCredentialAndAlterRetainsChosenCloud` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::ConfiguredCloudLookupFailureWithoutDatabaseCloudNeverStages` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::NoBearerUsesDatabaseCloudWithoutLookup` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: external-secret-schema / "Resource resolution is explicit or authorized" — scenarios "Discovery unavailable with database fallback", "No cloud can be determined", "ALTER retains the recorded cloud".

#### Scenario: DROP commits during an IAM outage

- **GIVEN** an active delegated secret while IAM is unavailable.
- **WHEN** DROP is executed.
- **THEN** the DROP commits, subsequent acquisitions fail, and the confirmed delegation remains durable cleanup work until revocation succeeds.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::DropCommitsDuringRevokeOutageAndWorkerLaterDrainsCleanup` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: iam-delegation-revocation / "DROP ends schema access independently of IAM" — scenario "DROP while IAM is unavailable".

#### Scenario: Admission lifecycle

- **GIVEN** an intent admitted while admission was enabled.
- **WHEN** admission is disabled before that intent completes.
- **THEN** the in-flight intent completes or retires unaffected, while new setup is gated.

Status: mapped executable cases passed at `969b11b16a9d`, `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::AdmissionDisabledAfterStageStillRecoversCreate` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::DisabledAdmissionRejectsFreshSqlMutations` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `IamDelegationTablet::DisabledAdmissionRejectsNewStageButAllowsExactReplay` ([source](../../../../../security/iam_delegation/tablet/ut/iam_delegation_ut.cpp)).

Contract coverage: external-secret-schema / "Admission and cleanup have separate lifetimes" — scenarios "Disable feature after creation", "Admission is disabled between Stage and schema submission".

#### Scenario: No-op ALTER preserves every pending setup state

- **GIVEN** active binding A and pending B in not-started, successful-awaiting-promotion, young unknown or expired unknown state.
- **WHEN** an authorized ALTER requests the active account/cloud unchanged.
- **THEN** authenticated schema execution succeeds without touching pending/current records or revisions and without setup/revoke dispatch.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::NoopAlterPreservesPendingNotStarted` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::NoopAlterPreservesPendingSucceeded` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::NoopAlterPreservesYoungUnknownAfterRestart` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::NoopAlterPreservesExpiredUnknownAfterRestart` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: external-secret-schema / "Stored-secret compatibility" — scenario "No-op ALTER has a pending replacement".

#### Scenario: Coordinator coroutine recovers a worker-owned refusal

- **GIVEN** a held coordinator reply and a worker that wins setup and persists a terminal IAM refusal.
- **WHEN** the coordinator resumes, including after storage restart, or is cancelled before a late result.
- **THEN** recovery returns the recorded IAM status/issues with the old binding intact; a cancelled coordinator emits no late result.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::WorkerOwnedSetupRefusalReturnsOriginalStatus` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::WorkerOwnedPolledRefusalReturnsOriginalStatusAfterRestart` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)); `KqpIamDelegationSecretOrchestrator::ExecutorAbortAfterAdmissionPreservesDurableRecovery` ([source](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: external-secret-schema / "DDL asynchronous state belongs to the coordinator coroutine" — scenario "Late response after coordinator cancellation"; "Setup fails for an account not delegable in its cloud" — scenario "Recovery owns the terminal refusal".

#### Scenario: Worker-owned refusal returns its original status

- **GIVEN** an active binding and a held coordinator Stage reply for a replacement.
- **WHEN** the worker owns submission or polling and commits a refusal before the coordinator resumes, with or without tablet reboot.
- **THEN** the statement returns saved UNAUTHORIZED and the refusal issue; the old active record and token remain intact; no revoke or extra setup is dispatched.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: external-secret-schema / "Setup fails for an account not delegable in its cloud" — scenario "Recovery owns the terminal refusal".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::WorkerOwnedSetupRefusalReturnsOriginalStatus` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerOwnedSetupRefusalReturnsOriginalStatusAfterRestart` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerOwnedPolledRefusalReturnsOriginalStatus` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerOwnedPolledRefusalReturnsOriginalStatusAfterRestart` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Coordinator-owned refusal saves safe diagnostics

- **GIVEN** an active binding and a coordinator-owned replacement setup.
- **WHEN** submission or polling returns denial with safe typed reasons and a raw credential sentinel.
- **THEN** the coordinator returns the mapped status without the sentinel, persists only safe diagnostics across reboot, and preserves the active binding and token.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::CoordinatorOwnedSubmissionRefusalPersistsSafeDetails` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::CoordinatorOwnedPolledRefusalPersistsSafeDetails` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Legacy refusal keeps its generic fallback

- **GIVEN** a terminal failed replacement saved by an old-shaped worker request without diagnostic fields.
- **WHEN** the tablet restarts and the original coordinator resumes from its held Stage reply.
- **THEN** the statement returns the generic UNAVAILABLE refusal without resubmitting setup, rewriting the terminal record or replacing the active binding.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics" (legacy compatibility).

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::LegacyWorkerRefusalKeepsGenericFallbackAfterRestart` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Authorization, discovery and later reads

- **GIVEN** real schema operations, the independent tablet and controlled IAM transport.
- **WHEN** permissions, cloud discovery, active bindings or IAM availability change.
- **THEN** metadata remains independent of minting, each value read rechecks authorization and identity, and durable cleanup survives an outage.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: external-secret-schema; external-secret-consumers.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::BuiltinSubjectWithSchemaRightsCannotStageDelegation` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::CloudLookupForwardsCallerCredentialAndAlterRetainsChosenCloud` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ConfiguredCloudLookupFailureWithoutDatabaseCloudNeverStages` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DropCommitsDuringRevokeOutageAndWorkerLaterDrainsCleanup` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::LongLivedProviderReadsAlteredBindingAndRejectsLaterAclLoss` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::MetadataRemainsAvailableDuringIamOutageWithoutMint` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::SelectRowRevocationWhileMintHeldPreventsValueReturn` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ValueAccessNeedsSelectRowWithoutDescribeSchema` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Replacement uses the original persisted guard deadline

- **GIVEN** an unresolved pending replacement and a tablet restart nine minutes after staging.
- **WHEN** a new replacement reaches the guard one microsecond before, then exactly at, the original ten-minute deadline.
- **THEN** the earlier attempt is refused without mutation or IAM dispatch; the boundary attempt succeeds and retains the unresolved old binding for recovery.

Contract coverage: iam-delegation-lifecycle / "Retiring pending work preserves responsibility" — scenario "Replacement attempted during the pending guard".

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::ReplacementGuardUsesOriginalTenMinuteBoundaryAfterRestart` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Tenant deletion survives complete storage reopening

- **GIVEN** a real tenant with active, accepted, unknown and undispatched setup records in the service-owned tablet.
- **WHEN** real SchemeShard force-drops the tenant, the same names are recreated, all actors/storage services are destroyed, and the existing PDisk is reopened in a fresh runtime.
- **THEN** saved bindings and immutable identities survive; accepted setup is polled, known successful cleanup drains after an outage and worker/tablet takeover, unknown setup stays visible and unclaimed, and new identities remain unchanged.

Contract coverage: iam-delegation-lifecycle / "Durable ownership survives tenant deletion"; iam-delegation-revocation / "Cleanup uses the saved binding" and "Uncertain setup is excluded from cleanup claims".

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits. The reopened PDisk is real; post-reopen schema/IAM replies are controlled. This does not reopen a full SchemeShard cluster or prove production placement.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::PopulatedTenantCleanupSurvivesWholeRuntimeDiskReopen` | [kqp_iam_delegation_secret_orchestrator_ut.cpp](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp) | `ydb/core/kqp/executer_actor/ut` |

### Requirement: CREATE and cancellation preserve ordinary schema contracts
Delegated CREATE SHALL use ordinary CREATE authorization and cancellation before durable admission SHALL stop the owned coordinator.

#### Scenario: CREATE without DescribeSchema
- **WHEN** a non-owner has parent CreateTable and the existing secret denies DescribeSchema
- **THEN** stored and delegated CREATE IF NOT EXISTS both succeed without changing the secret or contacting IAM
- **AND** fresh CREATE under a parent granting CreateTable but denying DescribeSchema also succeeds

#### Scenario: Executor cancellation before admission
- **WHEN** the real scheme executor is aborted while the coordinator's first metadata read is held
- **THEN** the coordinator terminates and a late reply cannot admit lifecycle work or start IAM setup

Executable coverage: `CreateIfNotExistsNeedsParentCreateWithoutTargetDescribe`, `CreateNeedsCreatePermissionWithoutParentDescribe`, and `ExecutorAbortBeforeAdmissionCancelsCoordinator`.

#### Scenario: Executor cancellation after admission preserves recovery
- **WHEN** executor cancellation stops a coordinator after IAM accepted setup and its poll reply is held
- **THEN** the late coordinator reply cannot mutate lifecycle state
- **AND** after tablet reboot an independent worker polls the saved operation, activates the same secret and never resubmits setup

Executable coverage: `ExecutorAbortAfterAdmissionPreservesDurableRecovery`.
