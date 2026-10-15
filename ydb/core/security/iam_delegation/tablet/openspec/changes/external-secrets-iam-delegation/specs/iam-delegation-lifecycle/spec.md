# IAM delegation lifecycle

## Purpose

Retain accountable ownership of every possible IAM delegation through creation, replacement, deletion, crashes and tenant teardown.

## ADDED Requirements

### Requirement: Durable ownership survives tenant deletion
Delegation intent, original binding, setup outcome and outstanding cleanup responsibility SHALL remain durable independently of the tenant's schema service. Persisted state MUST NOT contain user authentication bearers, minted IAM tokens or service-account keys.

#### Scenario: Database is removed
- **WHEN** a database and its schema service disappear while delegation work remains
- **THEN** the original delegation identities and cleanup responsibilities remain available to recovery and administrative inventory.

### Requirement: Lifecycle state is owned independently of the schema service
All delegation lifecycle, setup-outcome and cleanup-queue state SHALL be owned by the independent delegation tablet, never by the tenant's schema service. The schema service SHALL retain only ordinary schema responsibilities: object identity, ACLs, immutable references and fences. IAM calls MUST NOT execute inside schema or storage transactions.

#### Scenario: Schema service holds no cleanup queue
- **WHEN** a tenant's schema service is unavailable, replaced or deleted
- **THEN** no delegation intent, setup outcome or cleanup obligation is lost or needs reconstruction from schema state.

### Requirement: Asynchronous coordination keeps operation state in coroutines
Async lifecycle clients, DDL coordinators and recovery workers SHALL use `NActors::async` with `co_await`/`co_return`. Per-operation requests, identities/revisions, deadlines, replies and retry state SHALL stay in the coroutine frame. Shared caches MAY stay in actor members; authoritative state MUST stay in the independent tablet. Storage transactions MUST remain synchronous, with no coroutine suspension or IAM calls.

#### Scenario: Coordinator is cancelled while awaiting a reply
- **WHEN** a coordinator is cancelled while its coroutine awaits storage, schema or IAM
- **THEN** its waits are disarmed and late replies cannot resume a completed operation or emit a second result
- **AND** already committed intent and cleanup responsibility remain recoverable from the tablet.

### Requirement: Modes and outcomes use typed enums
Operation kind, lifecycle outcome, retry reason and pagination mode SHALL use scoped C++ enums or protobuf enums rather than Boolean control arguments or combinations of phase flags. Existing compatible wire fields MAY be adapted at the transport boundary; their meanings MUST be converted to typed decisions before orchestration. Ordinary Boolean predicates are unaffected.

#### Scenario: Pending operation differs from failed operation
- **WHEN** coordination receives a typed pending, failed or unknown IAM outcome
- **THEN** its explicit outcome determines polling, retry and operation-identifier retention without an ambiguous success or retry Boolean.

### Requirement: Immutable identities and idempotent mutations
Each database incarnation and bound secret SHALL use immutable identities rather than path names alone. An operation SHALL retain its original IAM binding and unique referrer. Replaying the same operation MUST preserve its identity; changed replays and stale revisions MUST be rejected without partial mutation.

#### Scenario: Database and secret paths are reused
- **WHEN** a new database or secret reuses an earlier path
- **THEN** delayed work for the earlier incarnation cannot bind, replace, retire or authorize the new object.

#### Scenario: Replay after restart
- **WHEN** a caller retries a committed mutation after losing its reply and restarting storage
- **THEN** the same request observes its durable result without duplicating the delegation
- **AND** a conflicting request using that identity is rejected.

### Requirement: Intent precedes remote setup
CREATE SHALL durably record its complete binding and schema intent before schema creation. Setup dispatch MUST require a verified matching schema object and a durable transition granting dispatch ownership. Concurrent coordinators or recovery workers MUST NOT independently acquire permission to dispatch the same setup.

#### Scenario: Coordinator fails before schema submission
- **WHEN** a CREATE intent commits but its coordinator disappears before submitting the schema operation
- **THEN** recovery can use the saved transaction and the recorded initiating-subject context to complete or safely retire that intent
- **AND** completing setup re-requests delegation on the recorded subject's behalf and is authorized by IAM at dispatch time, never by a stored credential or the platform's own delegation authority.

#### Scenario: Competing workers attempt setup
- **WHEN** multiple workers try to start setup for the same verified intent
- **THEN** at most one obtains dispatch ownership and the others recover the recorded outcome.

### Requirement: Replacement preserves the active binding
ALTER SHALL preserve the current active binding until a replacement setup succeeds. Promotion and recording cleanup for the predecessor MUST commit atomically. A rejected setup MUST NOT replace the current binding.

#### Scenario: Crash at promotion boundaries
- **WHEN** storage or a worker fails before commit, after commit or before the promotion reply
- **THEN** recovery sees either the previous active binding or the new binding together with durable predecessor cleanup
- **AND** it never observes a committed promotion that forgot the predecessor.

### Requirement: Terminal setup failures retain their diagnostics
A known terminal IAM refusal SHALL persist its mapped status and bounded credential-free issues with the setup outcome. A coordinator recovering a result produced by another worker SHALL return those recorded diagnostics and preserve the current active binding. Legacy records without diagnostics MAY return an explicit generic failure; missing diagnostics MUST NOT change a known failure into an unknown setup outcome.

#### Scenario: Recovery records refusal before the coordinator resumes
- **WHEN** a recovery worker wins setup dispatch, commits a terminal IAM refusal and the original coordinator resumes after reply loss or restart
- **THEN** the coordinator reports the recorded IAM status and issues without repeating setup or replacing the active binding.

### Requirement: Unknown setup is not a failed setup
A setup whose outcome is uncertain SHALL remain visible and ineligible for revocation while it could still grant access. A saved IAM operation identifier SHALL be used to recover the outcome. Without an authoritative replay or lookup contract, absence of that identifier MUST NOT authorize blind setup replay or timeout-based revocation.

#### Scenario: First setup reply is lost
- **WHEN** IAM may have accepted setup but its reply is lost before an operation identifier is persisted
- **THEN** the intent remains unresolved in inventory across restarts
- **AND** elapsed time alone neither repeats setup nor marks cleanup complete.

#### Scenario: Known operation survives restart
- **WHEN** the IAM operation identifier is durable but its final outcome is not
- **THEN** recovery polls that operation and records its definitive result before promotion or cleanup.

### Requirement: Retiring pending work preserves responsibility
Binding-changing replacements SHALL be blocked while unresolved pending work is younger than ten minutes, using its durable staging timestamp across restart; retirement is allowed at equality. A no-op ALTER MUST preserve pending work without applying this guard. Detachment SHALL retain cleanup responsibility and the active binding; late outcomes MUST affect only their original operation, never a newer pending replacement.

#### Scenario: Replacement attempted during the pending guard
- **WHEN** a binding-changing ALTER targets a secret whose pending replacement is unresolved and younger than the ten-minute guard
- **THEN** the ALTER is rejected without touching the active binding, the pending slot or cleanup state.

#### Scenario: Late success after pending replacement is retired
- **WHEN** an older uncertain setup succeeds after its pending slot was released and a newer operation was staged
- **THEN** the older binding becomes eligible for cleanup while the newer pending operation and current active binding remain intact.

### Requirement: Missing schema alone does not prove safe abandonment
Recovery SHALL retire missing or replaced bound objects by immutable identity. An unbound CREATE MUST be retired only when persisted schema fences prove a delayed original request cannot still create its object. A missing path alone MUST NOT discard an unfenced legacy intent.

#### Scenario: Delayed original CREATE arrives after cleanup
- **WHEN** recovery retires a CREATE using the saved parent/database fences and the original request later arrives
- **THEN** the request cannot recreate the retired object or attach its delegation to a replacement object.

### Requirement: Recovery preserves committed state under storage failures
Committed lifecycle state SHALL survive tablet restart and reopening the same on-disk storage in a new runtime. Retried transactions caused by unavailable pages MUST NOT expose partial effects. Recovery MUST converge under duplicate messages, delayed outcomes and worker takeover whenever authoritative outcomes become available.

#### Scenario: Cold page during a multi-record transition
- **WHEN** a required storage page is unavailable during mutation
- **THEN** retrying the transaction preserves atomicity and emits no premature success reply or remote side effect.

#### Scenario: Seeded crash and delivery schedule
- **WHEN** deterministic failure schedules combine commit/reply loss, restarts, identity reuse and out-of-order IAM outcomes
- **THEN** recovered active, pending and cleanup identities agree with an independent state model and no unresolved responsibility is lost.

### Requirement: Stuck lifecycle work is operator-visible
Unresolved setups and pending replacements persisting beyond a deployment-set age SHALL be exposed through operator-observable state using their durable staging timestamp. Thresholds MUST be configurable rather than fixed by this contract and MAY be evaluated by external monitoring. Reporting degradation MUST NOT resolve uncertainty, retire work or release cleanup responsibility.

#### Scenario: Unknown setup ages past its threshold
- **WHEN** an intent remains unresolved longer than the configured age threshold
- **THEN** operator-observable state reports it without resolving or retiring the intent.
