# KQP secret orchestration

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md), [lifecycle](../../../../../security/iam_delegation/tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/kqp/executer_actor/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 changes paths only; source hashes and new-path results are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md).

Status: **implemented; new-path/source-revision execution unverified until the verification record records a successful run**. Historical recovery/C1 runs used pre-relocation paths; names alone do not prove a passing run. Consumer and driver runs are deferred to Task4. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: TOrchestratorFixture runs real SchemeShard, SchemeCache, independent delegation tablet, coordinator, worker and reader actors. IAM/cloud responses are intercepted. ACL tests use serialized authenticated identities and assert effective rights before operations; public describe/value probes remain separate from internal lifecycle access.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Authenticated ALTER rights

- **GIVEN** an identity with target AlterSchema but no DescribeSchema/SelectRow.
- **WHEN** ALTER or CREATE OR REPLACE is executed with the required parent rights.
- **THEN** the operation succeeds without granting value/describe access; DescribeSchema-only or hidden-path identities remain unauthorized.

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

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KqpIamDelegationSecretOrchestrator::NoopsNeedNoIamConfigurationOrLifecycleMutation` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1043](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1043) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::ExplainCompilesWithoutLifecycleWritesOrIamConfiguration` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1161](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1161) | `ydb/core/kqp/executer_actor/ut` |
| `KqpIamDelegationSecretOrchestrator::SqlCreateAlterAndDropReachTabletAndReader` | [kqp_iam_delegation_secret_orchestrator_ut.cpp:1182](../../../kqp_iam_delegation_secret_orchestrator_ut.cpp#L1182) | `ydb/core/kqp/executer_actor/ut` |
