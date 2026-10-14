# Durable tablet and storage recovery

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [lifecycle](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md), [revocation](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md), [inventory](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/tablet/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 87/87 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundary: TTestContext drives the real tablet executor/storage via typed requests. ECrashPoint injects BeforeCommit, AfterDurableCommitBeforeComplete and ResponseLost. DiskStorage uses an 8 GiB disk with real PDisk/VDisk, preserves its file across wholly new runtimes, and explicitly selects Format/Reopen. Seeded chaos uses an independent inventory model, not only command success.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Commit and reply boundaries

- **GIVEN** a registered database and staged lifecycle state.
- **WHEN** each independently registered crash boundary interrupts a command.
- **THEN** precommit state stays unchanged; committed state survives loss of completion/reply and replay preserves canonical records.

Contract coverage: iam-delegation-lifecycle / "Immutable identities and idempotent mutations" — scenario "Replay after restart"; iam-delegation-lifecycle / "Intent precedes remote setup" — scenario "Competing workers attempt setup" (StartDispatchPermission boundary rows); iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Claim reply is lost after commit" (ClaimLeaseLostResponse); iam-delegation-revocation / "Completed acknowledgement replay is exact" — scenario "Retry acknowledgement response is lost" (FinishRetryLostResponse).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationDurability::RegisterDatabaseBeforeCommit` | [durability_ut.cpp:42](../../durability_ut.cpp#L42) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RegisterDatabaseAfterDurableCommit` | [durability_ut.cpp:42](../../durability_ut.cpp#L42) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RegisterDatabaseLostResponse` | [durability_ut.cpp:42](../../durability_ut.cpp#L42) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::CreateIntentBeforeCommit` | [durability_ut.cpp:62](../../durability_ut.cpp#L62) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::CreateIntentAfterDurableCommit` | [durability_ut.cpp:62](../../durability_ut.cpp#L62) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::CreateIntentLostResponse` | [durability_ut.cpp:62](../../durability_ut.cpp#L62) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::BindIntentBeforeCommit` | [durability_ut.cpp:86](../../durability_ut.cpp#L86) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::BindIntentAfterDurableCommit` | [durability_ut.cpp:86](../../durability_ut.cpp#L86) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::BindIntentLostResponse` | [durability_ut.cpp:86](../../durability_ut.cpp#L86) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::StartDispatchPermissionBeforeCommit` | [durability_ut.cpp:113](../../durability_ut.cpp#L113) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::StartDispatchPermissionAfterDurableCommit` | [durability_ut.cpp:113](../../durability_ut.cpp#L113) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::StartDispatchPermissionLostResponse` | [durability_ut.cpp:113](../../durability_ut.cpp#L113) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupSuccessBeforeCommit` | [durability_ut.cpp:136](../../durability_ut.cpp#L136) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupSuccessAfterDurableCommit` | [durability_ut.cpp:136](../../durability_ut.cpp#L136) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupSuccessLostResponse` | [durability_ut.cpp:136](../../durability_ut.cpp#L136) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::PromotionAndPredecessorOutboxBeforeCommit` | [durability_ut.cpp:160](../../durability_ut.cpp#L160) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::PromotionAndPredecessorOutboxAfterDurableCommit` | [durability_ut.cpp:160](../../durability_ut.cpp#L160) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::PromotionAndPredecessorOutboxLostResponse` | [durability_ut.cpp:160](../../durability_ut.cpp#L160) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::DropWithUncertainReplacementBeforeCommit` | [durability_ut.cpp:197](../../durability_ut.cpp#L197) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::DropWithUncertainReplacementAfterDurableCommit` | [durability_ut.cpp:197](../../durability_ut.cpp#L197) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::DropWithUncertainReplacementLostResponse` | [durability_ut.cpp:197](../../durability_ut.cpp#L197) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::ClaimLeaseBeforeCommit` | [durability_ut.cpp:239](../../durability_ut.cpp#L239) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::ClaimLeaseAfterDurableCommit` | [durability_ut.cpp:239](../../durability_ut.cpp#L239) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::ClaimLeaseLostResponse` | [durability_ut.cpp:239](../../durability_ut.cpp#L239) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishSuccessBeforeCommit` | [durability_ut.cpp:271](../../durability_ut.cpp#L271) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishSuccessAfterDurableCommit` | [durability_ut.cpp:271](../../durability_ut.cpp#L271) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishSuccessLostResponse` | [durability_ut.cpp:271](../../durability_ut.cpp#L271) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishRetryBeforeCommit` | [durability_ut.cpp:306](../../durability_ut.cpp#L306) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishRetryAfterDurableCommit` | [durability_ut.cpp:306](../../durability_ut.cpp#L306) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishRetryLostResponse` | [durability_ut.cpp:306](../../durability_ut.cpp#L306) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Promotion and predecessor revocation are atomic

- **GIVEN** an active binding and successfully prepared replacement.
- **WHEN** promotion crashes before/after commit or loses its reply.
- **THEN** current/pending records and predecessor outbox have matching old or new state; exactly one predecessor binding is claimable.

Contract coverage: iam-delegation-lifecycle / "Replacement preserves the active binding" — scenario "Crash at promotion boundaries".

Executable mapping (status: implemented; execution per verification record): the `PromotionAndPredecessorOutbox{BeforeCommit,AfterDurableCommit,LostResponse}` cases in "Commit and reply boundaries" above provide this scenario's coverage; they are listed there once to keep the executed-count mapping duplicate-free.

#### Scenario: Whole-runtime disk reopening

- **GIVEN** a database, unbound intent, mixed cleanup or retired pending replay receipt.
- **WHEN** all actors are discarded and the same disk is reopened in a new runtime.
- **THEN** serialized identity/inventory/receipt state survives, old claims are fenced, and late setup success cannot overwrite newer work.

Contract coverage: iam-delegation-lifecycle / "Recovery preserves committed state under storage failures"; "Retiring pending work preserves responsibility" — scenario "Late success after pending replacement is retired".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationStorageReopen::DatabaseAndUnboundIntentSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp:73](../../storage_reopen_ut.cpp#L73) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationStorageReopen::MixedLifecycleAndCleanupSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp:99](../../storage_reopen_ut.cpp#L99) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationStorageReopen::RetiredPendingAndItsReplayReceiptSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp:25](../../storage_reopen_ut.cpp#L25) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Exact due and lease equality

- **GIVEN** a durable revocation claim and controlled microsecond clock.
- **WHEN** time reaches one microsecond before or exactly at deadline/due time.
- **THEN** finish succeeds just before expiry; equality fences the old claim and makes replacement or retry claimable exactly once.

Contract coverage: iam-delegation-revocation / "Exact lease boundary fences new acknowledgements" — scenarios "One microsecond before expiry", "At the deadline", "Delayed worker after restart or takeover".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRevocationBoundaries::FinishSucceedsOneMicrosecondBeforeLeaseDeadline` | [revocation_boundaries_ut.cpp:232](../../revocation_boundaries_ut.cpp#L232) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::LeaseExpiresExactlyAtDeadlineAndOldClaimCannotFinishReplacement` | [revocation_boundaries_ut.cpp:244](../../revocation_boundaries_ut.cpp#L244) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::RetryBecomesClaimableExactlyAtStoredDueTime` | [revocation_boundaries_ut.cpp:266](../../revocation_boundaries_ut.cpp#L266) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::EveryClaimTokenFieldIsFencedWithoutMutation` | [revocation_boundaries_ut.cpp:290](../../revocation_boundaries_ut.cpp#L290) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Seeded crash schedules

- **GIVEN** independent model state and seed 1, 330 or 20261013.
- **WHEN** late setup outcomes, replay, reboot, identity reuse and takeover are interleaved.
- **THEN** inventory and durable lifecycle transitions match the independent model for each reproducible seed.

Contract coverage: iam-delegation-lifecycle / "Recovery preserves committed state under storage failures" — scenario "Seeded crash and delivery schedule".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryChaos::Seed1` | [recovery_chaos_ut.cpp:277](../../recovery_chaos_ut.cpp#L277) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRecoveryChaos::Seed330` | [recovery_chaos_ut.cpp:281](../../recovery_chaos_ut.cpp#L281) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRecoveryChaos::Seed20261013` | [recovery_chaos_ut.cpp:285](../../recovery_chaos_ut.cpp#L285) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Settling transitions are durable and fence work

- **GIVEN** a registered incarnation with active lifecycle and claim work.
- **WHEN** the settling state is entered or lifted across crash boundaries and reboot.
- **THEN** the state survives the reboot, claims and lifecycle transitions are fenced while it holds, and an authorized lift before handoff resumes work.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-inventory / "Deletion export requires a settled incarnation" — scenario "Export before settlement"; "Settling is authorized and reversible before handoff" — scenario "Settling lifted before handoff".

#### Scenario: Handoff commit boundaries

- **GIVEN** a settled incarnation with a completed export.
- **WHEN** a handoff commit is interrupted before/after commit or its reply is lost, or a stale export or lifted settlement is presented.
- **THEN** precommit the tablet retains responsibility; postcommit replay returns the recorded outcome; a mutated revision or lifted settlement rejects the uncommitted handoff; and a committed handoff fences outstanding claims and excludes transferred records from claim selection.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-inventory / "Handoff transfer is a distinct durable transition" — scenarios "Export completes without handoff", "Restarted export re-delivers records"; "Handoff is bound to a live settlement and revision" — scenarios "Handoff based on a stale export", "Handoff reply is lost after commit".

#### Scenario: Inventory and cleanup survive tenant deletion

- **GIVEN** an incarnation with delegation bindings and pending cleanup work.
- **WHEN** the tenant database is removed without a coordinated deletion handoff.
- **THEN** the delegation identities and cleanup responsibilities remain durable and available to recovery and administrative inventory.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-lifecycle / "Durable ownership survives tenant deletion" — scenario "Database is removed".

#### Scenario: Claim limit validation

- **GIVEN** a revocation claim request.
- **WHEN** batch size is zero or exceeds the bound, or the lease is zero or exceeds its bound.
- **THEN** the request is rejected without claiming any work.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Invalid claim limits".

#### Scenario: Cold page during claim and transition

- **GIVEN** a tablet whose required inventory pages are not in memory.
- **WHEN** a claim or a multi-record transition must read them.
- **THEN** the batch or transition either loads the pages within its bounds or fails safely without a partial mutation.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Batch cannot read a required page"; iam-delegation-lifecycle / "Recovery preserves committed state under storage failures" — scenario "Cold page during a multi-record transition".
