# Durable tablet and storage recovery

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [lifecycle](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md), [revocation](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md), [inventory](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/tablet/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Historical source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Historical status: **implemented and verified**, 87/87 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../docs/external-secrets/verification.md). These counts apply only to that source revision; added cases below require separate fresh execution.

Fresh admission checkpoint: `e6c24582d62` passed all 89 cases (exit 0), including the two disabled-admission regressions. See the follow-up tablet admission validation in the linked verification record. Setup-failure cases added below have not yet run.

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

#### Scenario: Lost inventory response preserves cleanup ownership

- **GIVEN** a retained incarnation with outstanding cleanup after tenant deletion.
- **WHEN** an inventory response is lost or enumeration restarts while recovery continues.
- **THEN** the tablet retains all cleanup obligations and listing neither acknowledges nor transfers them.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-inventory / "Tablet cleanup ownership survives database deletion" — scenario "Inventory response is lost during deletion"; "Listing is observational".

#### Scenario: Inventory and cleanup survive tenant deletion

- **GIVEN** an incarnation with delegation bindings and pending cleanup work.
- **WHEN** the tenant database is removed while the service-owned tablet survives.
- **THEN** confirmed cleanup continues from saved bindings, uncertain setup stays unclaimable and explicit-incarnation inventory remains available across restart and path reuse.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-lifecycle / "Durable ownership survives tenant deletion" — scenario "Database is removed"; iam-delegation-inventory / "Tablet cleanup ownership survives database deletion" — scenario "Tenant disappears with cleanup outstanding".

#### Scenario: Claim limit validation

- **GIVEN** a revocation claim request.
- **WHEN** batch size is zero or exceeds the bound, or the lease is zero or exceeds its bound.
- **THEN** the request is rejected without claiming any work.

Contract coverage: iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Invalid claim limits".

Executable mapping (status: implemented; execution per verification record): `IamDelegationRevocationBoundaries::InvalidClaimAndFinishBoundsLeaveOutboxUnchanged` in [revocation_boundaries_ut.cpp:322](../../revocation_boundaries_ut.cpp#L322).

#### Scenario: Cold page during claim and transition

- **GIVEN** a tablet whose required inventory pages are not in memory.
- **WHEN** a claim or a multi-record transition must read them.
- **THEN** the batch or transition either loads the pages within its bounds or fails safely without a partial mutation.

Executable mapping for atomic claims (status: implemented; execution per verification record): `IamDelegationColdScans::ClaimBatchRetriesColdReadsWithoutDuplicateClaimsOrPartialWrites` in [cold_scan_ut.cpp:174](../../cold_scan_ut.cpp#L174). A cold multi-record promotion/drop transition remains planned and does not contribute to executed counts.

Contract coverage: iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Batch cannot read a required page"; iam-delegation-lifecycle / "Recovery preserves committed state under storage failures" — scenario "Cold page during a multi-record transition".

#### Scenario: Completed ACK replay follows the original lease deadline

- **GIVEN** a committed success or retry ACK in the same tablet generation with no newer claim.
- **WHEN** the identical ACK is replayed after its original lease deadline.
- **THEN** it returns its recorded result without changing due time or failure history; a newer claim or generation still fences it.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegation-revocation / "Completed acknowledgement replay is exact" — scenario "Retry acknowledgement response is lost".

#### Scenario: Retry and refusal records survive commit boundaries

- **GIVEN** a terminal setup diagnostic or a revoke failure history and first cleanup timestamp.
- **WHEN** commit/reply loss, cold reads, tablet restart and fresh-runtime disk reopening occur.
- **THEN** records and replay receipts retain their values atomically; duplicate ACKs do not advance history, and legacy records remain readable.

Status: partially covered by the refusal regressions listed below; follow-up RED evidence is recorded, with GREEN verification pending. Additional backoff or cancellation assertions remain planned and do not contribute to executed counts.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics"; iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; "Outstanding cleanup is operator-visible".

#### Scenario: Terminal refusal validation and replay

- **GIVEN** a started replacement and a separate active binding.
- **WHEN** typed refusal details are committed, replayed after reboot, omitted by a legacy replay, or supplied with invalid/conflicting data.
- **THEN** the status and reasons persist atomically; exact or legacy replay preserves the saved result; invalid requests mutate no canonical or inventory records; conflicting terminal data cannot rewrite history; the active binding is unchanged.

Status: partial RED behavior observed before the broader run was interrupted after symbolization timeouts; complete GREEN verification is pending. These cases do not contribute to the historical verified count above.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationTablet::SetupFailureDetailsSurviveRestartAndFenceConflictingReplay` | [iam_delegation_ut.cpp](../../iam_delegation_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationTablet::InvalidSetupFailureDetailsDoNotMutateIntent` | [iam_delegation_ut.cpp](../../iam_delegation_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationTablet::LegacySetupFailureDoesNotInventDiagnostics` | [iam_delegation_ut.cpp](../../iam_delegation_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Refusal persistence at commit boundaries

- **GIVEN** an active binding and a pending replacement with typed terminal refusal details.
- **WHEN** the result transaction crashes before commit, after durable commit before Complete, or loses its reply.
- **THEN** recovery observes the entire old or committed result and pending-slot update; replay preserves diagnostics without changing the active binding or creating cleanup work.

Status: partial RED behavior observed before the broader run was interrupted after symbolization timeouts; complete GREEN verification is pending. These cases do not contribute to the historical verified count above.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics"; "Recovery preserves committed state under storage failures".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationDurability::SetupFailureDetailsBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupFailureDetailsAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupFailureDetailsLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Refusal persistence after disk reopening

- **GIVEN** typed refusal details committed in the real disk fixture.
- **WHEN** all actors are discarded and the same disk is reopened in two fresh runtimes.
- **THEN** serialized delegation, secret and active records remain unchanged; exact replay succeeds and conflicting details are rejected.

Status: partial RED behavior observed before the broader run was interrupted after symbolization timeouts; complete GREEN verification is pending. These cases do not contribute to the historical verified count above.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics"; "Recovery preserves committed state under storage failures".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationStorageReopen::SetupFailureDetailsSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
