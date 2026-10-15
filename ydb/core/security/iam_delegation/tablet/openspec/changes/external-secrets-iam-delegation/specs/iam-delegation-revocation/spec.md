# IAM delegation revocation

## Purpose

Complete cleanup of retired IAM bindings through durable, bounded worker claims without allowing delayed workers to acknowledge newer work.

## ADDED Requirements

### Requirement: DROP ends schema access independently of IAM
DROP SHALL remain a schema operation that does not wait for IAM. Readers MUST reject the deleted or replaced schema identity immediately upon subsequent authorization. Recovery SHALL retain and retry cleanup for confirmed retired bindings. Already issued IAM tokens remain usable externally until their expiry or IAM's own enforcement.

#### Scenario: DROP while IAM is unavailable
- **WHEN** a secret is dropped during an IAM outage
- **THEN** DROP can commit and subsequent secret acquisitions fail
- **AND** its confirmed delegation remains durable cleanup work until revocation succeeds.

### Requirement: Cleanup uses the saved binding
Revocation SHALL use the complete original binding, including its service identity, account, resource, referrer and reference policy. Configuration changes or another delegation for the same account MUST NOT redirect or replace that cleanup obligation.

#### Scenario: Configuration changes before cleanup
- **WHEN** service configuration changes after setup but before revocation
- **THEN** the worker revokes the original binding using its saved identity.

### Requirement: Claim batches are bounded and atomic
A worker claim SHALL accept a limit from 1 through 100 and a lease from 1 through 60 seconds. It SHALL select only eligible confirmed-successful setup records whose due time is at or before the storage clock. A batch's claim transitions and returned records MUST be atomic across page retries and commit/reply loss.

#### Scenario: Batch cannot read a required page
- **WHEN** a multi-record claim encounters an unavailable record or database page
- **THEN** it retries without publishing a partial batch or retaining partial claims.

#### Scenario: Claim reply is lost after commit
- **WHEN** a claim batch commits and its response is lost
- **THEN** all claimed records retain their leases and become reclaimable at their respective deadlines.

#### Scenario: Invalid claim limits
- **WHEN** a worker requests a zero or over-100 batch, or a lease outside 1 through 60 seconds
- **THEN** the request is rejected without mutating cleanup state.

### Requirement: Exact lease boundary fences new acknowledgements
A new acknowledgement — completion or retry-report — SHALL be accepted only for the current claim and strictly before its deadline, unless it exactly replays an acknowledgement already committed for the same claim (see Completed acknowledgement replay is exact). Claim identity MUST bind generation, attempt, worker and deadline. At the deadline a record SHALL be eligible for takeover and its prior unfinished claim MUST be stale.

#### Scenario: One microsecond before expiry
- **WHEN** the current worker acknowledges a live claim one microsecond before its deadline
- **THEN** the acknowledgement can complete the claim.

#### Scenario: At the deadline
- **WHEN** the former worker acknowledges an unfinished claim exactly at its deadline
- **THEN** the acknowledgement is rejected as stale
- **AND** another worker can claim the due record.

#### Scenario: Delayed worker after restart or takeover
- **WHEN** an acknowledgement carries an earlier generation, attempt, worker or deadline
- **THEN** it cannot complete, release or reschedule the current claim.

### Requirement: Completed acknowledgement replay is exact
An exact replay of an already committed acknowledgement SHALL return its recorded outcome while its claim remains current in the same generation, including after its old deadline. A changed replay MUST be rejected. A newer claim or generation MUST fence the old acknowledgement.

#### Scenario: Retry acknowledgement response is lost
- **WHEN** a retry acknowledgement commits but the worker loses the reply and sends the identical acknowledgement again
- **THEN** it observes the recorded retry without postponing the due time again.

### Requirement: Only authoritative revocation completion removes work
Workers SHALL report successful cleanup only after the IAM revoke operation completes or IAM authoritatively confirms absence. Retryable failures MUST preserve durable work with a retry delay that grows with the record's consecutive failures up to a bounded maximum. Claim-driven remote calls SHALL be dispersed so that draining a large backlog does not synchronize them. Merely dispatching a revoke or losing its reply MUST NOT mark the binding revoked.

#### Scenario: IAM revoke operation remains pending
- **WHEN** IAM accepts a revoke but has not completed its operation
- **THEN** cleanup remains outstanding and recovery retains the operation identifier when available.

#### Scenario: Retry delay grows with failures
- **WHEN** a record's revocation repeatedly fails retryably
- **THEN** its retry windows follow standard bounded backoff growth with jitter
- **AND** individual random samples need not increase strictly and the initial base delay may repeat.

#### Scenario: Backlog drains without synchronized calls
- **WHEN** many records become due together
- **THEN** their revoke dispatches are dispersed rather than simultaneous.

### Requirement: Revocation retries use standard backoff and typed reasons
Workers SHALL reuse `NKikimr::TBackoff` from `ydb/core/util/backoff.h` for retries and dispersion, keeping its temporary state in the coroutine frame and per-record failure history/due time in the tablet. Retry reasons SHALL be typed enums distinguishing pending progress from failure. Backoff waits MUST NOT consume a claimed lease; exact ACK replay MUST NOT advance history or due time again.

#### Scenario: Retry backoff survives worker takeover
- **WHEN** a worker finishes a retry ACK and is replaced before its stored due time
- **THEN** the new coroutine reconstructs standard backoff from durable history without claiming early
- **AND** replaying the committed ACK does not advance the history or due time again.

### Requirement: Revoke outcomes preserve remote operation identity
Succeeded SHALL complete cleanup; Accepted SHALL reset the consecutive failure streak while retaining pending work. Failed and Unknown SHALL advance the streak. Unknown MUST retain any saved operation identifier for polling rather than resubmitting; only a definitive failure permits clearing it.

#### Scenario: Unknown poll retains operation and failure history
- **WHEN** polling a saved revoke operation fails uncertainly and another worker later takes over
- **THEN** the failure streak and due time survive, the saved operation is polled rather than resubmitted, and the next retry follows the standard backoff policy.

#### Scenario: Accepted operation makes progress
- **WHEN** IAM confirms that a revoke operation is accepted but still pending
- **THEN** the operation identifier is retained for polling, the failure streak resets and cleanup remains outstanding.

### Requirement: Uncertain setup is excluded from cleanup claims
An unresolved setup that could still grant access SHALL remain outside the revocation claim queue. Retirement or lease expiry MUST NOT convert uncertainty into a definitive setup result.

#### Scenario: Setup completes after retirement
- **WHEN** an uncertain retired setup later succeeds
- **THEN** its saved binding enters cleanup without altering the active or pending replacement
- **AND** it was not claimed for revocation before that success became authoritative.

### Requirement: Outstanding cleanup is operator-visible
Cleanup work outstanding beyond a deployment-set age and records with repeated retryable failures SHALL be exposed through operator-observable state, with thresholds left to configuration, including external monitoring. Age SHALL use the first durable cleanup-queued timestamp rather than a changing retry due time; missing historical timestamps SHALL be reported as unknown. Claims and restarts MUST NOT reset the durable failure history or original cleanup age.

#### Scenario: Revocation repeatedly fails
- **WHEN** a record's revocation keeps failing retryably past the configured age
- **THEN** operator-observable state distinguishes it from healthy backlog without removing its durable work.
