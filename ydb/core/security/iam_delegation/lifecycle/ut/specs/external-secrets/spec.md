# Lifecycle clients and recovery workers

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [lifecycle](../../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md), [revocation](../../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md), [inventory](../../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Latest recorded owning run: **29 GOOD** (full owning suite) at `1373e723886e`, `task8-10-acquisition-recovery-parallel100`; actual combined command exit 1. Historical counts below retain their original revisions.

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/lifecycle/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Historical source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Historical status: **implemented and verified**, 22/22 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../docs/external-secrets/verification.md). These counts apply only to that source revision; added cases below require separate fresh execution.

Fixture boundary: TWorkerFixture uses the real independent tablet and real worker/client actors. IAM submit/poll and schema replies are controlled by event observers. TTestContext persists tablet transactions across reboot; no live IAM or schema transport is required.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Durable transport and uncertain reply

- **GIVEN** a client request with a cookie.
- **WHEN** the durable response succeeds, the tablet reboots, or the committed reply is dropped.
- **THEN** the cookie is forwarded, new reads resolve the rebooted tablet, and a lost reply reports uncertainty without replaying the mutation.

Contract coverage: iam-delegation-lifecycle / "Recovery preserves committed state under storage failures" (client transport half).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationTabletClient::ForwardsCookieAndDurableReply` | [client_ut.cpp](../../client_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationTabletClient::NewRequestResolvesRebootedTablet` | [client_ut.cpp](../../client_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationTabletClient::LostDurableReplyDoesNotReplayMutationAfterDisconnect` | [client_ut.cpp](../../client_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Submission absence versus polling uncertainty

- **GIVEN** a missing local IAM service.
- **WHEN** submit or known-operation poll is attempted.
- **THEN** undispatched submit is Failed while the known operation is Unknown; both carry UNAVAILABLE.

Contract coverage: iam-delegation-lifecycle / "Unknown setup is not a failed setup" (no blind replay of undispatched work).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationTabletClient::MissingLocalIamServiceProvesSubmitWasNotDispatched` | [client_ut.cpp](../../client_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationTabletClient::MissingLocalIamServiceDoesNotProveKnownOperationFailed` | [client_ut.cpp](../../client_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Saved IAM operation recovery

- **GIVEN** a revoke accepted with a persisted operation ID.
- **WHEN** worker and tablet restart, or the saved operation definitively fails.
- **THEN** restart polls the saved ID without duplicate submit; terminal failure clears it before a new submit.

Contract coverage: iam-delegation-lifecycle / "Unknown setup is not a failed setup" — scenario "Known operation survives restart"; iam-delegation-revocation / "Only authoritative revocation completion removes work" — scenario "IAM revoke operation remains pending".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::AcceptedRevokePersistsAndRestartPollsSavedOperation` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::TerminalRevokeFailureClearsOperationBeforeResubmitting` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Unknown setup cleanup responsibility

- **GIVEN** a dropped binding with uncertain setup.
- **WHEN** many days pass without an operation ID, or a known setup is polled.
- **THEN** unknown setup remains WAITING_SETUP with zero submits/polls; a known successful setup is resolved before revoke.

Contract coverage: iam-delegation-revocation / "Uncertain setup is excluded from cleanup claims" — scenario "Setup completes after retirement"; iam-delegation-lifecycle / "Unknown setup is not a failed setup" — scenario "First setup reply is lost"; iam-delegation-lifecycle / "Intent precedes remote setup" — scenario "Competing workers attempt setup" (SetupStartRaceAndUnknownOutcomeNeverResubmit).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::UnknownSetupWithoutOperationIsNeverRevoked` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::KnownSetupAfterDropIsPolledBeforeRevocation` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::SetupStartRaceAndUnknownOutcomeNeverResubmit` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Persisted identity and stale claim fences

- **GIVEN** a full saved binding or an older claim.
- **WHEN** node settings are absent or a late reply follows tablet reboot/worker takeover.
- **THEN** revoke carries every saved field and stale replies cannot complete a newer claim.

Contract coverage: iam-delegation-revocation / "Cleanup uses the saved binding" — scenario "Configuration changes before cleanup"; "Exact lease boundary fences new acknowledgements" — scenario "Delayed worker after restart or takeover".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::RevokeUsesEntireSavedBindingWithoutNodeConfiguration` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::LateRevokeReplyCannotAcknowledgeClaimAfterTabletReboot` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::LateReplyCannotCompleteNewerRevocationClaim` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Schema reuse and ambiguous absence

- **GIVEN** a recovered intent or previously bound schema path.
- **WHEN** schema identity changes or schema reads fail.
- **THEN** replacement identities retire only old work; schema errors or unbound absence never prove deletion.

Contract coverage: iam-delegation-lifecycle / "Missing schema alone does not prove safe abandonment".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::ReusedPathAndChangedCreatingReferenceRetireOldBinding` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::SchemaErrorsAndUnboundAbsenceNeverProveDeletion` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Intent recovery with durable subject

- **GIVEN** a verified intent shell whose schema submission state is uncertain.
- **WHEN** the worker recovers it or restarts mid-setup.
- **THEN** setup resumes with the durable initiating subject and polls the saved operation after restart instead of resubmitting.

Contract coverage: iam-delegation-lifecycle / "Intent precedes remote setup" — scenario "Coordinator fails before schema submission"; iam-delegation-inventory / "Inventory is authoritative beyond schema lifetime" — scenario "Unactivated CREATE shell remains recoverable".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::VerifiedShellResumesSetupWithDurableSubjectAndPollsAfterRestart` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Pending guard blocks replacement

- **GIVEN** a binding with pending lifecycle work inside its ten-minute guard.
- **WHEN** a replacement arrives, including after a restart still inside the window.
- **THEN** the replacement is blocked until the guard elapses.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `KqpIamDelegationSecretOrchestrator::ReplacementGuardUsesOriginalTenMinuteBoundaryAfterRestart` ([source](../../../../../../kqp/executer_actor/kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Retiring pending work preserves responsibility" — scenario "Replacement attempted during the pending guard".

#### Scenario: Stuck lifecycle work is visible

- **GIVEN** a setup that stays in its uncertain state past the configured threshold.
- **WHEN** the threshold elapses.
- **THEN** operator-observable state reports the stuck work without changing its recovery behavior.

Status: mapped executable cases passed at `913c0cb032a1`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `ListDatabaseIamDelegations::RealInventorySupportsExternalAgeAndFailureThresholds` ([source](../../../../../../grpc_services/rpc_list_database_iam_delegations_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Stuck lifecycle work is operator-visible" — scenario "Unknown setup ages past its threshold".

#### Scenario: Outstanding cleanup is visible

- **GIVEN** cleanup work whose IAM revocation repeatedly fails.
- **WHEN** the configured failure threshold elapses.
- **THEN** operator-observable state reports the outstanding cleanup while retries continue.

Status: mapped executable cases passed at `913c0cb032a1`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `ListDatabaseIamDelegations::RealInventorySupportsExternalAgeAndFailureThresholds` ([source](../../../../../../grpc_services/rpc_list_database_iam_delegations_ut.cpp)).

Contract coverage: iam-delegation-revocation / "Outstanding cleanup is operator-visible" — scenario "Revocation repeatedly fails".

#### Scenario: Revoke retries grow and disperse

- **GIVEN** several bindings whose revocation retries fail together.
- **WHEN** subsequent attempts are scheduled.
- **THEN** the worker uses standard `TBackoff`, per-binding retry windows grow to their cap with jitter, and waiting does not consume a claim lease; individual samples need not grow strictly.

Status: mapped executable cases passed at `1373e723886e`, `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationRevocationBoundaries::DurableRevocationFailuresGrowToCapAcrossTakeover` ([source](../../../../tablet/ut/revocation_boundaries_ut.cpp)); `IamDelegationRecoveryWorker::WorkersDisperseScansBeforeClaimingWithoutReducingMinimumPause` ([source](../../worker_ut.cpp)); `IamDelegationRecoveryWorker::FailingRevocationDoesNotHoldAnotherRecordsLease` ([source](../../worker_ut.cpp)).

Contract coverage: iam-delegation-revocation / "Only authoritative revocation completion removes work" — scenarios "Retry delay grows with failures", "Backlog drains without synchronized calls"; "Revocation retries use standard backoff and typed reasons".

#### Scenario: Discovery behind cursor and paced rescans

- **GIVEN** a database registered behind a worker's current discovery cursor.
- **WHEN** scans repeat.
- **THEN** a later complete scan discovers it, and consecutive full scans are paced by the minimum interval.

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationRecoveryWorker::DatabaseInsertedBehindActiveCursorIsFoundOnNextSweep` ([source](../../worker_ut.cpp)); `IamDelegationRecoveryWorker::WorkersDisperseScansBeforeClaimingWithoutReducingMinimumPause` ([source](../../worker_ut.cpp)).

Contract coverage: iam-delegation-inventory / "Database discovery tolerates concurrent registration" — scenarios "Registration appears behind the cursor", "Rescans are paced".

#### Scenario: No cleanup state in the schema service

- **GIVEN** lifecycle and cleanup work recorded for an incarnation.
- **WHEN** the work is created, claimed, retried and acknowledged.
- **THEN** no cleanup queue or lifecycle state is written to or required from the schema service; all state lives in the delegation tablet.

Status: mapped executable cases passed at `1373e723886e`, `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationRecoveryWorker::RevokeUsesEntireSavedBindingWithoutNodeConfiguration` ([source](../../worker_ut.cpp)); `IamDelegationStorageReopen::MixedLifecycleAndCleanupSurviveWholeRuntimeReplacement` ([source](../../../../tablet/ut/storage_reopen_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Lifecycle state is owned independently of the schema service" — scenario "Schema service holds no cleanup queue".

#### Scenario: Recovery dispatch denied by IAM

- **GIVEN** a recovered intent whose initiating subject lost IAM permission before setup dispatch.
- **WHEN** the worker sends setup on the saved initiating subject's behalf and IAM returns a known terminal denial.
- **THEN** the mapped denial is persisted, no binding is activated, and a waiting coordinator can recover the original refusal without resubmitting setup.

Status: mapped executable cases passed at `1373e723886e`, `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationRecoveryWorker::VerifiedShellResumesSetupWithDurableSubjectAndPollsAfterRestart` ([source](../../worker_ut.cpp)); `IamDelegationRecoveryWorker::SubmissionRefusalPersistsSafeDetails` ([source](../../worker_ut.cpp)); `KqpIamDelegationSecretOrchestrator::WorkerOwnedSetupRefusalReturnsOriginalStatusAfterRestart` ([source](../../../../../../kqp/executer_actor/kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Intent precedes remote setup" (dispatch-time authorization clause); "Terminal setup failures retain their diagnostics" — scenario "Recovery records refusal before the coordinator resumes".

#### Scenario: Coroutine cancellation releases the lifecycle wait

- **GIVEN** a lifecycle coroutine suspended on a controlled tablet, schema or IAM reply.
- **WHEN** its owner is cancelled and the reply later arrives.
- **THEN** no completed coroutine resumes or emits a second result, and committed work remains recoverable by another worker.

Status: mapped executable cases passed at `1373e723886e`, `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationTabletClient::CancellingCoroutineClosesItsTabletClientBeforeTimeout` ([source](../../client_ut.cpp)); `IamDelegationRecoveryWorker::LateReplyCannotCompleteNewerRevocationClaim` ([source](../../worker_ut.cpp)); `KqpIamDelegationSecretOrchestrator::WorkerPollsAcceptedSetupAfterClientAndTabletRestart` ([source](../../../../../../kqp/executer_actor/kqp_iam_delegation_secret_orchestrator_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Asynchronous coordination keeps operation state in coroutines" — scenario "Coordinator is cancelled while awaiting a reply".

#### Scenario: Typed pending and uncertain revoke outcomes

- **GIVEN** a saved revoke operation and failure history.
- **WHEN** typed Accepted, Failed and Unknown results arrive across worker takeover.
- **THEN** Accepted resets the streak, Failed/Unknown advance it, Unknown retains the identifier for polling and only definitive failure permits clearing it.

Status: mapped executable cases passed at `1373e723886e`, `969b11b16a9d`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationRecoveryWorker::RevocationOutcomeHistorySurvivesWorkerAndTabletTakeover` ([source](../../worker_ut.cpp)); `IamDelegationRevocationBoundaries::PendingRevocationResetsFailuresAndRetainsOperation` ([source](../../../../tablet/ut/revocation_boundaries_ut.cpp)).

Contract coverage: iam-delegation-lifecycle / "Modes and outcomes use typed enums"; iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; "Revoke outcomes preserve remote operation identity" — scenarios "Unknown poll retains operation and failure history", "Accepted operation makes progress".

#### Scenario: Worker persists only safe refusal details

- **GIVEN** a recovered intent carrying the initiating subject and immutable binding.
- **WHEN** IAM denies submission or polling with repeated typed reasons and a credential-bearing raw issue.
- **THEN** the worker saves the mapped denial and deduplicated reason across reboot; pending clears without activation or revocation; the credential sentinel is absent from durable state. IAM is mocked at the event boundary.

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics"; "Intent precedes remote setup".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::SubmissionRefusalPersistsSafeDetails` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::PolledRefusalPersistsSafeDetails` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Retry progress survives worker takeover

- **GIVEN** a saved revoke operation, repeated transport failures and another independent cleanup record.
- **WHEN** the real worker and tablet restart, then receive accepted, uncertain, definitive-failure and successful replies.
- **THEN** the worker classifies retries explicitly, retains a known operation on uncertainty, resets the streak on progress and never holds another record's lease while waiting on IAM.

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; "Revoke outcomes preserve remote operation identity".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::RevocationOutcomeHistorySurvivesWorkerAndTabletTakeover` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::FailingRevocationDoesNotHoldAnotherRecordsLease` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Worker fleets pace before taking leases

- **GIVEN** thirty-two real recovery workers sharing an empty service tablet.
- **WHEN** their first two scan rounds run with a fixed random seed.
- **THEN** initial claims are dispersed, each scan waits at least one second, each claim requests one record, and no IAM call occurs.

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::WorkersDisperseScansBeforeClaimingWithoutReducingMinimumPause` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Coroutine cancellation closes the tablet client

- **GIVEN** a real tablet client with its response held.
- **WHEN** the awaiting caller is terminated before the transport timeout.
- **THEN** its child is stopped immediately and a late response emits no result.

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: external-secret-consumers / Acquisition state belongs to its coroutine.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationTabletClient::CancellingCoroutineClosesItsTabletClientBeforeTimeout` | [client_ut.cpp](../../client_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Registrations behind a scan cursor are eventually recovered

- **GIVEN** a recovery sweep paused after receiving its exclusive registry cursor.
- **WHEN** a new database is registered before that cursor in sort order.
- **THEN** the next sweep discovers it while later databases and the next record in an existing database retain fair progress and the minimum one-second pause.

Contract coverage: iam-delegation-revocation / "Revocation retries use standard backoff and typed reasons"; iam-delegation-lifecycle / "Recovery preserves committed state under storage failures".

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::DatabaseInsertedBehindActiveCursorIsFoundOnNextSweep` | [worker_ut.cpp](../../worker_ut.cpp) | `ydb/core/security/iam_delegation/lifecycle/ut` |
