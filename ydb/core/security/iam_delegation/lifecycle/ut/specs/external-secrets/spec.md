# Lifecycle clients and recovery workers

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [lifecycle](../../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md), [revocation](../../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md), [inventory](../../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/lifecycle/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 changes paths only; source hashes and new-path results are recorded in the [durable verification record](../../../../docs/external-secrets/verification.md).

Status: **verified on the Task5 relocated source revision: 22/22 GOOD, exact exit 0**. Historical recovery/C1 runs used pre-relocation paths; names alone do not prove a passing run. Consumer and driver runs are deferred to Task4. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: TWorkerFixture uses the real independent tablet and real worker/client actors. IAM submit/poll and schema replies are controlled by event observers. TTestContext persists tablet transactions across reboot; no live IAM or schema transport is required.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Durable transport and uncertain reply

- **GIVEN** a client request with a cookie.
- **WHEN** the durable response succeeds, the tablet reboots, or the committed reply is dropped.
- **THEN** the cookie is forwarded, new reads resolve the rebooted tablet, and a lost reply reports uncertainty without replaying the mutation.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationTabletClient::ForwardsCookieAndDurableReply` | [client_ut.cpp:47](../../client_ut.cpp#L47) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationTabletClient::NewRequestResolvesRebootedTablet` | [client_ut.cpp:61](../../client_ut.cpp#L61) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationTabletClient::LostDurableReplyDoesNotReplayMutationAfterDisconnect` | [client_ut.cpp:75](../../client_ut.cpp#L75) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Submission absence versus polling uncertainty

- **GIVEN** a missing local IAM service.
- **WHEN** submit or known-operation poll is attempted.
- **THEN** undispatched submit is Failed while the known operation is Unknown; both carry UNAVAILABLE.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationTabletClient::MissingLocalIamServiceProvesSubmitWasNotDispatched` | [client_ut.cpp:29](../../client_ut.cpp#L29) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationTabletClient::MissingLocalIamServiceDoesNotProveKnownOperationFailed` | [client_ut.cpp:38](../../client_ut.cpp#L38) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Saved IAM operation recovery

- **GIVEN** a revoke accepted with a persisted operation ID.
- **WHEN** worker and tablet restart, or the saved operation definitively fails.
- **THEN** restart polls the saved ID without duplicate submit; terminal failure clears it before a new submit.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::AcceptedRevokePersistsAndRestartPollsSavedOperation` | [worker_ut.cpp:186](../../worker_ut.cpp#L186) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::TerminalRevokeFailureClearsOperationBeforeResubmitting` | [worker_ut.cpp:238](../../worker_ut.cpp#L238) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Unknown setup cleanup responsibility

- **GIVEN** a dropped binding with uncertain setup.
- **WHEN** many days pass without an operation ID, or a known setup is polled.
- **THEN** unknown setup remains WAITING_SETUP with zero submits/polls; a known successful setup is resolved before revoke.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::UnknownSetupWithoutOperationIsNeverRevoked` | [worker_ut.cpp:256](../../worker_ut.cpp#L256) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::KnownSetupAfterDropIsPolledBeforeRevocation` | [worker_ut.cpp:269](../../worker_ut.cpp#L269) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Persisted identity and stale claim fences

- **GIVEN** a full saved binding or an older claim.
- **WHEN** node settings are absent or a late reply follows tablet reboot/worker takeover.
- **THEN** revoke carries every saved field and stale replies cannot complete a newer claim.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::RevokeUsesEntireSavedBindingWithoutNodeConfiguration` | [worker_ut.cpp:208](../../worker_ut.cpp#L208) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::LateRevokeReplyCannotAcknowledgeClaimAfterTabletReboot` | [worker_ut.cpp:285](../../worker_ut.cpp#L285) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::LateReplyCannotCompleteNewerRevocationClaim` | [worker_ut.cpp:375](../../worker_ut.cpp#L375) | `ydb/core/security/iam_delegation/lifecycle/ut` |

#### Scenario: Schema reuse and ambiguous absence

- **GIVEN** a recovered intent or previously bound schema path.
- **WHEN** schema identity changes or schema reads fail.
- **THEN** replacement identities retire only old work; schema errors or unbound absence never prove deletion.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecoveryWorker::ReusedPathAndChangedCreatingReferenceRetireOldBinding` | [worker_ut.cpp:472](../../worker_ut.cpp#L472) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::SchemaErrorsAndUnboundAbsenceNeverProveDeletion` | [worker_ut.cpp:496](../../worker_ut.cpp#L496) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::VerifiedShellResumesSetupWithDurableSubjectAndPollsAfterRestart` | [worker_ut.cpp:314](../../worker_ut.cpp#L314) | `ydb/core/security/iam_delegation/lifecycle/ut` |
| `IamDelegationRecoveryWorker::SetupStartRaceAndUnknownOutcomeNeverResubmit` | [worker_ut.cpp:349](../../worker_ut.cpp#L349) | `ydb/core/security/iam_delegation/lifecycle/ut` |
