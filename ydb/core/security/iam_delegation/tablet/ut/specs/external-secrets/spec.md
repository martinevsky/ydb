# Durable tablet and storage recovery

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [lifecycle](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md), [revocation](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md), [inventory](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Latest recorded owning run: **123 GOOD** (full owning suite) at `969b11b16a9d`, `task9-10-buildfix-green-parallel100`; actual combined command exit 10. Historical counts below retain their original revisions.

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
| `IamDelegationDurability::RegisterDatabaseBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RegisterDatabaseAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RegisterDatabaseLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::CreateIntentBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::CreateIntentAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::CreateIntentLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::BindIntentBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::BindIntentAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::BindIntentLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::StartDispatchPermissionBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::StartDispatchPermissionAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::StartDispatchPermissionLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupSuccessBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupSuccessAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::SetupSuccessLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::PromotionAndPredecessorOutboxBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::PromotionAndPredecessorOutboxAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::PromotionAndPredecessorOutboxLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::DropWithUncertainReplacementBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::DropWithUncertainReplacementAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::DropWithUncertainReplacementLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::ClaimLeaseBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::ClaimLeaseAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::ClaimLeaseLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishSuccessBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishSuccessAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishSuccessLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishRetryBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishRetryAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::FinishRetryLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

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
| `IamDelegationStorageReopen::DatabaseAndUnboundIntentSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationStorageReopen::MixedLifecycleAndCleanupSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationStorageReopen::RetiredPendingAndItsReplayReceiptSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Exact due and lease equality

- **GIVEN** a durable revocation claim and controlled microsecond clock.
- **WHEN** time reaches one microsecond before or exactly at deadline/due time.
- **THEN** finish succeeds just before expiry; equality fences the old claim and makes replacement or retry claimable exactly once.

Contract coverage: iam-delegation-revocation / "Exact lease boundary fences new acknowledgements" — scenarios "One microsecond before expiry", "At the deadline", "Delayed worker after restart or takeover".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRevocationBoundaries::FinishSucceedsOneMicrosecondBeforeLeaseDeadline` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::LeaseExpiresExactlyAtDeadlineAndOldClaimCannotFinishReplacement` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::RetryBecomesClaimableExactlyAtStoredDueTime` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::EveryClaimTokenFieldIsFencedWithoutMutation` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Seeded crash schedules

- **GIVEN** independent model state and seed 1, 330 or 20261013.
- **WHEN** late setup outcomes, replay, reboot, identity reuse and takeover are interleaved.
- **THEN** inventory and durable lifecycle transitions match the independent model for each reproducible seed.

Contract coverage: iam-delegation-lifecycle / "Recovery preserves committed state under storage failures" — scenario "Seeded crash and delivery schedule".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryChaos::Seed1` | [recovery_chaos_ut.cpp](../../recovery_chaos_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRecoveryChaos::Seed330` | [recovery_chaos_ut.cpp](../../recovery_chaos_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRecoveryChaos::Seed20261013` | [recovery_chaos_ut.cpp](../../recovery_chaos_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Lost inventory response preserves cleanup ownership

- **GIVEN** a retained incarnation with outstanding cleanup after tenant deletion.
- **WHEN** an inventory response is lost or enumeration restarts while recovery continues.
- **THEN** the tablet retains all cleanup obligations and listing neither acknowledges nor transfers them.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationTablet::InventoryPaginationSurvivesRebootAndExpiresOnMutation` ([source](../../iam_delegation_ut.cpp)); `IamDelegationStorageReopen::MixedLifecycleAndCleanupSurviveWholeRuntimeReplacement` ([source](../../storage_reopen_ut.cpp)).

Contract coverage: iam-delegation-inventory / "Tablet cleanup ownership survives database deletion" — scenario "Inventory response is lost during deletion"; "Listing is observational".

#### Scenario: Inventory and cleanup survive tenant deletion

- **GIVEN** an incarnation with delegation bindings and pending cleanup work.
- **WHEN** the tenant database is removed while the service-owned tablet survives.
- **THEN** confirmed cleanup continues from saved bindings, uncertain setup stays unclaimable and explicit-incarnation inventory remains available across restart and path reuse.

Status: mapped executable cases passed at `969b11b16a9d`, `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::PopulatedTenantCleanupSurvivesWholeRuntimeDiskReopen` ([source](../../../../../../kqp/executer_actor/kqp_iam_delegation_secret_orchestrator_ut.cpp)); `IamDelegationStorageReopen::MixedLifecycleAndCleanupSurviveWholeRuntimeReplacement` ([source](../../storage_reopen_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Durable ownership survives tenant deletion" — scenario "Database is removed"; iam-delegation-inventory / "Tablet cleanup ownership survives database deletion" — scenario "Tenant disappears with cleanup outstanding".

#### Scenario: Claim limit validation

- **GIVEN** a revocation claim request.
- **WHEN** batch size is zero or exceeds the bound, or the lease is zero or exceeds its bound.
- **THEN** the request is rejected without claiming any work.

Contract coverage: iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Invalid claim limits".

Executable mapping (status: implemented; execution per verification record): `IamDelegationRevocationBoundaries::InvalidClaimAndFinishBoundsLeaveOutboxUnchanged` in [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp).

#### Scenario: Cold page during claim and transition

- **GIVEN** a tablet whose required inventory pages are not in memory.
- **WHEN** a claim or a multi-record transition must read them.
- **THEN** the batch or transition either loads the pages within its bounds or fails safely without a partial mutation.

Executable mapping for atomic claims (status: implemented; execution per verification record): `IamDelegationColdScans::ClaimBatchRetriesColdReadsWithoutDuplicateClaimsOrPartialWrites` in [cold_scan_ut.cpp](../../cold_scan_ut.cpp). Cold promotion/drop coverage is mapped under "Cold promotion and retirement remain atomic" below; it passed in the tablet123-case run at `969b11b`.

Contract coverage: iam-delegation-revocation / "Claim batches are bounded and atomic" — scenario "Batch cannot read a required page"; iam-delegation-lifecycle / "Recovery preserves committed state under storage failures" — scenario "Cold page during a multi-record transition".

#### Scenario: Completed ACK replay follows the original lease deadline

- **GIVEN** a committed success or retry ACK in the same tablet generation with no newer claim.
- **WHEN** the identical ACK is replayed after its original lease deadline.
- **THEN** it returns its recorded result without changing due time or failure history; a newer claim or generation still fences it.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationRevocationBoundaries::CommittedSuccessAckReplaysAtAndAfterExpiredLease` ([source](../../revocation_boundaries_ut.cpp)); `IamDelegationRevocationBoundaries::CommittedRetryAckReplaysPastLeaseUntilNextClaim` ([source](../../revocation_boundaries_ut.cpp)).

Contract coverage: iam-delegation-revocation / "Completed acknowledgement replay is exact" — scenario "Retry acknowledgement response is lost".

#### Scenario: Retry and refusal records survive commit boundaries

- **GIVEN** a terminal setup diagnostic or a revoke failure history and first cleanup timestamp.
- **WHEN** commit/reply loss, cold reads, tablet restart and fresh-runtime disk reopening occur.
- **THEN** records and replay receipts retain their values atomically; duplicate ACKs do not advance history, and legacy records remain readable.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationStorageReopen::SetupFailureDetailsSurviveWholeRuntimeReplacement` ([source](../../storage_reopen_ut.cpp)); `IamDelegationStorageReopen::RevocationFailureHistorySurvivesWholeRuntimeReplacement` ([source](../../storage_reopen_ut.cpp)); `IamDelegationDurability::SetupFailureDetailsAfterDurableCommit` ([source](../../durability_ut.cpp)); `IamDelegationDurability::RevocationFailureHistoryAndCleanupAgeLostResponse` ([source](../../durability_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics"; iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; "Outstanding cleanup is operator-visible".

#### Scenario: Terminal refusal validation and replay

- **GIVEN** a started replacement and a separate active binding.
- **WHEN** typed refusal details are committed, replayed after reboot, omitted by a legacy replay, or supplied with invalid/conflicting data.
- **THEN** the status and reasons persist atomically; exact or legacy replay preserves the saved result; invalid requests mutate no canonical or inventory records; conflicting terminal data cannot rewrite history; the active binding is unchanged.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

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

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

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

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics"; "Recovery preserves committed state under storage failures".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationStorageReopen::SetupFailureDetailsSurviveWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Durable retry policy and cleanup age

- **GIVEN** an actionable cleanup record or an unresolved setup, including historical ACK shapes.
- **WHEN** typed failure and pending ACKs are committed, duplicated, rejected, taken over, or interrupted at commit boundaries and reopened from physical storage.
- **THEN** the failure streak advances once, grows to the bounded standard backoff cap, resets on accepted progress, and survives restart; unknown operation identity and the first actionable cleanup timestamp remain intact; historical missing timestamps stay unknown, saturated counters do not wrap or delay unboundedly, and malformed classification consumes no claim.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; "Outstanding cleanup is operator-visible".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRevocationBoundaries::DurableRevocationFailuresGrowToCapAcrossTakeover` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::PendingRevocationResetsFailuresAndRetainsOperation` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::LegacyRevocationAckPreservesNewFailureHistory` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::LegacyQueuedRecordKeepsUnknownAgeAfterClaimAndRetry` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::SaturatedFailureHistoryKeepsCappedDelayAcrossRestart` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::InvalidRetryClassificationDoesNotConsumeClaim` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::UnknownSetupGetsCleanupAgeOnlyWhenAuthoritativelyResolved` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RevocationFailureHistoryAndCleanupAgeBeforeCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RevocationFailureHistoryAndCleanupAgeAfterDurableCommit` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationDurability::RevocationFailureHistoryAndCleanupAgeLostResponse` | [durability_ut.cpp](../../durability_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationStorageReopen::RevocationFailureHistorySurvivesWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Authenticated inventory continuations

- **GIVEN** multiple records in the real tablet, including cold persisted pages and an independently formatted tablet with the same public identity.
- **WHEN** continuations are modified, conflicting fields are supplied, database names are reused, or the tablet/runtime is replaced.
- **THEN** valid tokens retain their original identity and revision; every forged, unsupported or mismatched request fails without mutation; signing keys survive physical reopening; bounded pages enumerate each record once without preloading the inventory.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-inventory / "Administrative export is authorized"; "Authenticated pagination has an explicit protocol mode"; "Inventory pagination is revision-bound and bounded".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationInventoryTokens::AuthenticatedInventoryContinuationSurvivesRestartAndNameReuse` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::AuthenticatedInventoryRejectsTamperingAndConflictingFields` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::AuthenticatedInventoryAdvertisesCapabilityOnEmptyAndFinalPages` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::AuthenticatedInventoryRejectsTokenFromIndependentTabletStorage` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::UnknownPaginationModeFailsClosedWithoutChangingInventory` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::AuthenticatedInventoryRejectsContinuationOnDifferentTabletId` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::InjectedUnauthenticatedContinuationCannotFallBackToFirstPage` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationStorageReopen::InventorySigningKeySurvivesWholeRuntimeReplacement` | [storage_reopen_ut.cpp](../../storage_reopen_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationColdScans::AuthenticatedPaginationRetriesColdPagesWithoutPreloadingInventory` | [cold_scan_ut.cpp](../../cold_scan_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::ExplicitEmptyPaginationFieldsCannotMixProtocols` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokens::KnownAndUnknownPaginationModesCannotBypassValidation` | [inventory_token_ut.cpp](../../inventory_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Signing binds tablet identity with a shared key

- **GIVEN** codecs with the same key but different tablet IDs and a saved registry identity with an optional alias and unknown fields.
- **WHEN** a continuation is encoded and verified by each codec.
- **THEN** only its owning tablet accepts it; an empty optional alias remains valid, and unbounded unknown registry fields are excluded from the bounded token.

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationInventoryTokenCodec::SharedKeyStillBindsTheTabletIdentity` | [page_token_ut.cpp](../../page_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationInventoryTokenCodec::OptionalDatabaseAliasAndUnknownRegistryFieldsDoNotChangeIdentity` | [page_token_ut.cpp](../../page_token_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Committed acknowledgements outlive their old lease

- **GIVEN** a committed success or retry acknowledgement in the current tablet generation.
- **WHEN** the exact acknowledgement is replayed at and after its original lease deadline.
- **THEN** its durable receipt remains exact and inventory is unchanged; a newer claim or tablet generation fences that old receipt.

Contract coverage: iam-delegation-revocation / "Completed acknowledgement replay is exact".

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRevocationBoundaries::CommittedSuccessAckReplaysAtAndAfterExpiredLease` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRevocationBoundaries::CommittedRetryAckReplaysPastLeaseUntilNextClaim` | [revocation_boundaries_ut.cpp](../../revocation_boundaries_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Cold promotion and retirement remain atomic

- **GIVEN** twenty active/pending pairs compacted onto cold storage pages.
- **WHEN** promotion and drop retry actual page reads, and stale requests are replayed.
- **THEN** canonical identities, revisions and inventory agree; forty successful retired bindings are claimed once and twenty unresolved setups remain unclaimable.

Contract coverage: iam-delegation-lifecycle / "Recovery preserves committed state under storage failures"; iam-delegation-revocation / "Claim batches are bounded and atomic".

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationColdScans::PromotionAndDropRetryColdReadsWithoutPartialLifecycleTransitions` | [cold_scan_ut.cpp](../../cold_scan_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |

#### Scenario: Seeded recovery retains diagnostics and cleanup history

- **GIVEN** independent expected lifecycle roles, saved bindings, failure classifications and cleanup-time windows.
- **WHEN** delayed setup outcomes, replacement, identity reuse, repeated revoke failures, accepted remote progress and takeover interleave across transaction crash boundaries.
- **THEN** terminal diagnostics, consecutive failure history, remote operation IDs and original cleanup age match the model; unknown setups stay unclaimable and old generations cannot complete later work.

Contract coverage: iam-delegation-lifecycle / "Recovery preserves committed state under storage failures"; iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; "Outstanding cleanup is operator-visible".

Status: mapped executable cases passed at `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits. Cleanup timestamps are bounded at their originating transitions and checked for exact preservation; actor-runtime schedules do not establish live IAM or real-thread race behavior.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryChaos::Seed1` | [recovery_chaos_ut.cpp](../../recovery_chaos_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRecoveryChaos::Seed330` | [recovery_chaos_ut.cpp](../../recovery_chaos_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
| `IamDelegationRecoveryChaos::Seed20261013` | [recovery_chaos_ut.cpp](../../recovery_chaos_ut.cpp) | `ydb/core/security/iam_delegation/tablet/ut` |
