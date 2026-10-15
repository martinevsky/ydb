# IAM delegated token acquisition

## Purpose

Provide current short-lived IAM credentials to authorized consumers while sharing mint work, surviving transient failures and retaining finite acquisition deadlines.

## ADDED Requirements

### Requirement: Minting uses platform credentials and the saved binding
After delegation setup, token minting SHALL use the platform's current managed credentials and the active binding's saved service/account/resource identity. It MUST NOT require the initiating user's original bearer or persist minted tokens in durable delegation storage. Minted tokens and authentication bearers MUST NOT appear in logs, traces, status issues or other diagnostics; status issues SHALL retain IAM status and diagnostic detail but never credential material.

#### Scenario: Original user session has expired
- **WHEN** an authorized consumer acquires a token after the creator's bearer expires
- **THEN** minting can proceed using platform credentials and the active saved binding.

### Requirement: Served tokens have strictly more than five minutes remaining
A token SHALL be served only while the current time is strictly before its usable deadline, defined as IAM expiry minus five minutes. A token at or beyond that deadline MUST be treated as absent, including after asynchronous validation delays.

#### Scenario: Just before the usable deadline
- **WHEN** acquisition completes one microsecond before a token's usable deadline and authorization remains valid
- **THEN** that token can be returned.

#### Scenario: Exactly five minutes remain
- **WHEN** acquisition reaches the usable deadline exactly
- **THEN** the old token is not returned and acquisition must obtain another usable result or fail.

### Requirement: Refresh is automatic and bounded against busy loops
Active entries SHALL refresh without reader credentials, normally by half-life and at least hourly. Attempts MUST be paced by at least one second. When sufficient usable time remains, dispersion MUST stay within the half-life/hourly/usable target. Otherwise the next attempt MAY follow the usable deadline, but the token MUST be absent at that deadline; pacing MUST NOT extend validity or cause a busy loop.

#### Scenario: Long-running consumer
- **WHEN** a consumer continues using the same secret beyond the first token's lifetime
- **THEN** later acquisitions receive refreshed credentials without a query restart or another delegation setup.

#### Scenario: Near-boundary token
- **WHEN** a minted token's usable remaining time is shorter than the normal half-life interval
- **THEN** the next refresh respects minimum pacing and the strict serving cutoff
- **AND** a positive subsecond usable interval may end before the next attempt, leaving a safe period without a usable token.

### Requirement: Refresh scheduling is dispersed
The first demanded mint SHALL start immediately. Subsequent healthy refreshes SHALL incorporate bounded dispersion across entries and nodes so that simultaneous cache population does not synchronize subsequent remote calls. Retry backoff and scheduling dispersion SHALL reuse standard `NKikimr::TBackoff` rather than a separate retry algorithm; healthy refreshes MUST NOT accumulate exponential failure backoff or push the target beyond its allowed bounds.

#### Scenario: Fleet restart aligns cache population
- **WHEN** many entries are first minted together after a fleet restart
- **THEN** their subsequent refreshes are dispersed rather than aligned to the restart time.

### Requirement: Refresh operation state belongs to its coroutine
Each shared producer SHALL run as an owned `NActors::async` coroutine using `co_await`/`co_return`. The coroutine frame SHALL retain the in-flight mint, deadline, temporary result and standard backoff across suspension. The actor MAY retain shared cache entries and waiter notification state; each coroutine MUST revalidate an entry after suspension instead of holding an invalidated reference. Per-operation phases MUST NOT be encoded by actor-level Boolean flags or callback state machines.

#### Scenario: Producer stops with a mint outstanding
- **WHEN** the producer coroutine is cancelled while waiting for IAM or retry pacing
- **THEN** its waits and subscriptions are released, late replies cannot publish a result, and cancellation of one reader does not stop a producer still needed by other readers.

### Requirement: Concurrent demand shares one producer
Concurrent requests for the same account, cloud and saved service identity SHALL share one refresh producer per node. Token sharing MUST NOT share or bypass schema authorization between secret references or between callers.

#### Scenario: Several callers arrive during backoff
- **WHEN** callers request the same absent token while its producer waits between failed attempts
- **THEN** they await the next shared mint result without creating duplicate refresh loops or replaying the last failure as a new attempt.

#### Scenario: Callers with different permissions share a mint
- **WHEN** several callers with different permissions share one pending mint
- **THEN** credentials are returned to a caller only after that caller's verified identity is validated against the current secret identity and ACL.

### Requirement: Idle entries expire in bounded time
An idle entry MUST NOT be evicted while callers await its next result. An entry with no waiting callers SHALL be evicted within a bounded idle period — distinct from the lifecycle pending guard — and eviction MUST NOT race a concurrently arriving waiter.

#### Scenario: Waiter arrives during eviction
- **WHEN** an entry is being evicted for idleness as a new caller arrives for the same key
- **THEN** the caller awaits a fresh result and is not attached to the evicted entry.

### Requirement: Transient failures preserve usable cached tokens
A transient IAM or platform-credential failure SHALL retain an existing token while it remains usable. An authoritative permission denial or not-found result for the delegated account identity MUST invalidate cached use. Retryable failures MUST retain their status and issues when no usable token is available.

#### Scenario: Temporary IAM outage with a usable cache entry
- **WHEN** refresh fails transiently before the cached token's usable deadline
- **THEN** authorized acquisitions can still receive the usable cached token while refresh retries.

#### Scenario: Delegation is authoritatively rejected
- **WHEN** IAM reports a permission denial or missing delegated account identity
- **THEN** a previously cached token is no longer served for that key and acquisition reports the terminal failure.

#### Scenario: Retryable failure with an empty cache
- **WHEN** minting fails with a retryable status and no usable token exists
- **THEN** the acquisition surfaces the retained retryable status and issues so the caller's retry policy can classify it.

### Requirement: Token serving is account-scoped
Minting and caching are scoped to the account identity rather than an individual referrer; referrer-level revocation is not visible to minting while the account's grant survives. While any live delegation for the same account exists, the system SHALL continue minting and serving tokens for references to that account through the remaining delegation. Dropping a secret SHALL still end its access through schema authorization.

#### Scenario: Sibling delegation outlives out-of-band revocation
- **WHEN** one referrer is revoked out-of-band while another live delegation exists for the same account
- **THEN** minting continues through the remaining delegation
- **AND** only schema authorization ends a dropped reference's access.

### Requirement: A reader permits one complete bounded mint attempt
On a cache miss, the reader's token-wait budget SHALL cover the producer's supported backoff, credential waits, RPC attempts and retry delays, and SHALL be derived from the producer's configured bounds rather than maintained as an independent constant. A shorter schema/tablet phase timer MUST NOT terminate that wait. The default resolver policy MUST be able to receive the next attempt's result without being preempted by repeated thirty-second reader timeouts.

#### Scenario: Backoff longer than sixty seconds
- **WHEN** real transient IAM failures place the producer in a supported backoff longer than sixty seconds and concurrent default-policy acquisitions arrive
- **THEN** the acquisitions remain pending through that backoff and receive the next mint's result
- **AND** they do not spawn replacement readers merely because thirty seconds elapsed.

### Requirement: Every wait remains finite and phase-specific
Non-token storage and schema phases SHALL retain finite deadlines. A missing token-service reply MUST end within a finite token-phase budget. Timers and replies from completed phases MUST NOT expire, revive or complete a later phase incorrectly.

#### Scenario: Token service never replies
- **WHEN** an acquisition receives no token-service result
- **THEN** it terminates with an availability error after the token-phase budget
- **AND** a later response cannot emit a second successful result.

#### Scenario: Earlier phase timeout arrives late
- **WHEN** a schema or tablet timer fires after the reader entered the longer token phase
- **THEN** it is ignored and the current phase retains its own deadline.

### Requirement: Caller policy and cancellation remain effective
The resolver SHALL preserve its caller-supplied retry policy and terminal IAM error propagation. Explicit cancellation or an externally imposed caller deadline MUST remain effective without requiring the shared producer to stop serving unrelated callers.

#### Scenario: One concurrent caller cancels
- **WHEN** one caller cancels while several callers share a pending mint
- **THEN** the cancelled acquisition does not later return credentials
- **AND** the other callers can still receive the shared result.

### Requirement: Degraded minting is operator-visible
Consecutive refresh or mint failures behind a still-usable cached token SHALL be exposed through operator-observable state, with thresholds left to configuration.

#### Scenario: Refresh fails behind a usable cache
- **WHEN** refresh keeps failing while a cached token remains usable
- **THEN** operator-observable state reports the degradation after the configured threshold without waiting for the cached token to become unusable.
