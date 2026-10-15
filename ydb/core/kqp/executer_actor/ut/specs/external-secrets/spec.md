# KQP secret orchestration

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md), [lifecycle](../../../../../security/iam_delegation/tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/kqp/executer_actor/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 119/119 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

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
| `KqpIamDelegationSecretOrchestrator::AlterNeedsAlterSchemaWithoutDescribeOrValueAccess` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:424](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L424) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DescribeSchemaAndHiddenPathsDoNotAuthorizeAlter` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:453](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L453) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::CreateOrReplaceRequiresParentCreateAndTargetAlterWithoutDescribe` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:548](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L548) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::CreateOrReplaceMissingPathRequiresParentCreatePermission` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:600](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L600) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Revoked ACL and reused paths

- **GIVEN** authorization or resolution is paused.
- **WHEN** ACL is revoked or the path is dropped and recreated.
- **THEN** lifecycle mutation cannot touch the replacement object or rely on obsolete access.

Contract coverage: external-secret-schema / "Authorization and identity remain current" — scenarios "Permission revoked after lookup", "Path recreated during resolution".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::AlterRechecksRevokedAclBeforeLifecycleMutation` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:481](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L481) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::AlterCannotTouchRecreatedSecretDuringResolution` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:513](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L513) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::AlterCannotTouchRecreatedSecretAfterAuthorization` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1010](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1010) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Coordinator crash and durable recovery

- **GIVEN** a staged CREATE or schema shell.
- **WHEN** the coordinator disappears before submission, after schema creation, or after an accepted IAM operation.
- **THEN** the worker resumes persisted intent/subject/schema identity and polls the saved operation after restart.

Contract coverage: iam-delegation-lifecycle / "Intent precedes remote setup" — scenario "Coordinator fails before schema submission".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::WorkerResumesCrashAfterDurableStageBeforeSchemaSubmission` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:679](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L679) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerRecoversLostStageReplyWithoutNewUserRequest` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:710](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L710) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerResumesCrashAfterSchemaBeforeSetup` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:920](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L920) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::WorkerPollsAcceptedSetupAfterClientAndTabletRestart` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:949](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L949) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Delayed schema copies and consumed identities

- **GIVEN** original CREATE and worker replay are delayed.
- **WHEN** DROP, sibling mutation or parent/database name reuse consumes the saved fence.
- **THEN** all delayed copies use one schema identity or fail without creating a replacement shell.

Contract coverage: iam-delegation-lifecycle / "Missing schema alone does not prove safe abandonment" — scenario "Delayed original CREATE arrives after cleanup"; "Immutable identities and idempotent mutations" — scenario "Database and secret paths are reused".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::DelayedOriginalCreateAndWorkerReplayUseOneSchemaIdentity` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:735](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L735) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DropBeforeBindingFencesEveryDelayedCreateCopy` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:764](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L764) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::SiblingMutationConsumesSavedCreateFence` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:837](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L837) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ParentNameReuseCannotReplayOldCreateIntent` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:852](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L852) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::DatabaseNameReuseCannotReplayOldCreateIntent` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:881](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L881) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Noops and ordinary SQL integration

- **GIVEN** missing/existing targets and feature configuration.
- **WHEN** noops, EXPLAIN or SQL CREATE/ALTER/DROP run.
- **THEN** noops and EXPLAIN avoid lifecycle/IAM writes; real SQL reaches the tablet and reader.

Contract coverage: external-secret-schema / "Stored-secret compatibility" — scenarios "No-op statement dispatches nothing", "Missing ALTER IF EXISTS" (NoopsNeedNoIamConfigurationOrLifecycleMutation); "Metadata is independent of token health" — scenario "IAM is unavailable during metadata access" (no-dispatch half: EXPLAIN and noops need no IAM configuration); "Delegation source syntax" — scenario "Create a delegation secret" (happy-path half via SqlCreateAlterAndDrop). This delegation fixture does not establish the separate "Existing stored value" scenario.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::NoopsNeedNoIamConfigurationOrLifecycleMutation` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1043](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1043) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ExplainCompilesWithoutLifecycleWritesOrIamConfiguration` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1161](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1161) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::SqlCreateAlterAndDropReachTabletAndReader` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1182](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1182) | `ydb/core/kqp/executer_actor/ut` |

#### Scenario: Setup subject and cloud authorization

- **GIVEN** a CREATE or ALTER with a delegation source.
- **WHEN** the caller is a builtin identity, IAM denies the initiating subject permission to delegate the account, or the account is not delegable in the recorded cloud.
- **THEN** the builtin identity is rejected before any IAM dispatch; the two IAM-denial cases fail with the IAM error after dispatch, and no binding is activated in any case.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-schema / "Delegation uses the initiating cloud subject" — scenarios "Builtin identity attempts setup", "Caller cannot delegate the account"; "Setup fails for an account not delegable in its cloud" — scenario "Account not delegable in the recorded cloud".

#### Scenario: Cloud resolution and retention

- **GIVEN** cloud discovery unavailable, indeterminate, or a previously recorded cloud.
- **WHEN** setup resolves the cloud or ALTER omits it.
- **THEN** the configured database fallback applies, an indeterminate cloud fails the setup, and ALTER retains the recorded cloud.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-schema / "Resource resolution is explicit or authorized" — scenarios "Discovery unavailable with database fallback", "No cloud can be determined", "ALTER retains the recorded cloud".

#### Scenario: DROP commits during an IAM outage

- **GIVEN** an active delegated secret while IAM is unavailable.
- **WHEN** DROP is executed.
- **THEN** the DROP commits, subsequent acquisitions fail, and the confirmed delegation remains durable cleanup work until revocation succeeds.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-revocation / "DROP ends schema access independently of IAM" — scenario "DROP while IAM is unavailable".

#### Scenario: Admission lifecycle

- **GIVEN** an intent admitted while admission was enabled.
- **WHEN** admission is disabled before that intent completes.
- **THEN** the in-flight intent completes or retires unaffected, while new setup is gated.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-schema / "Admission and cleanup have separate lifetimes" — scenarios "Disable feature after creation", "Admission is disabled between Stage and schema submission".

#### Scenario: No-op ALTER preserves every pending setup state

- **GIVEN** active binding A and pending B in not-started, successful-awaiting-promotion, young unknown or expired unknown state.
- **WHEN** an authorized ALTER requests the active account/cloud unchanged.
- **THEN** authenticated schema execution succeeds without touching pending/current records or revisions and without setup/revoke dispatch.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-schema / "Stored-secret compatibility" — scenario "No-op ALTER has a pending replacement".

#### Scenario: Coordinator coroutine recovers a worker-owned refusal

- **GIVEN** a held coordinator reply and a worker that wins setup and persists a terminal IAM refusal.
- **WHEN** the coordinator resumes, including after storage restart, or is cancelled before a late result.
- **THEN** recovery returns the recorded IAM status/issues with the old binding intact; a cancelled coordinator emits no late result.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-schema / "DDL asynchronous state belongs to the coordinator coroutine" — scenario "Late response after coordinator cancellation"; "Setup fails for an account not delegable in its cloud" — scenario "Recovery owns the terminal refusal".
