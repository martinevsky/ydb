# IAM delegated token acquisition

## Purpose

Provide current short-lived IAM credentials to authorized consumers while sharing mint work, surviving transient failures and retaining finite acquisition deadlines.

## ADDED Requirements

### Requirement: Minting uses platform credentials and the saved binding
After delegation setup, token minting SHALL use the platform's current managed credentials and the active binding's saved service/account/resource identity. It MUST NOT require the initiating user's original bearer or persist minted tokens in durable delegation storage.

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
The node SHALL refresh active cache entries without reader credentials, normally at half of the token lifetime and at least once per hour for longer-lived tokens. Refresh scheduling MUST respect the usable deadline and avoid a busy loop for near-boundary tokens; scheduling MUST NOT extend their usable lifetime.

#### Scenario: Long-running consumer
- **WHEN** a consumer continues using the same secret beyond the first token's lifetime
- **THEN** later acquisitions receive refreshed credentials without a query restart or another delegation setup.

### Requirement: Concurrent demand shares one producer
Concurrent requests for the same account, cloud and saved service identity SHALL share one refresh producer per node. Token sharing MUST NOT share or bypass schema authorization between secret references. An idle entry MUST NOT be evicted while callers await its next result.

#### Scenario: Several callers arrive during backoff
- **WHEN** callers request the same absent token while its producer waits between failed attempts
- **THEN** they await the next shared mint result without creating duplicate refresh loops or replaying the last failure as a new attempt.

### Requirement: Transient failures preserve usable cached tokens
A transient IAM or platform-credential failure SHALL retain an existing token while it remains usable. An authoritative permission denial or not-found result for the delegated identity MUST invalidate cached use. Retryable failures MUST retain their status and issues when no usable token is available.

#### Scenario: Temporary IAM outage with a usable cache entry
- **WHEN** refresh fails transiently before the cached token's usable deadline
- **THEN** authorized acquisitions can still receive the usable cached token while refresh retries.

#### Scenario: Delegation is authoritatively rejected
- **WHEN** IAM reports a permission denial or missing delegated identity
- **THEN** a previously cached token is no longer served for that key and acquisition reports the terminal failure.

### Requirement: A reader permits one complete bounded mint attempt
On a cache miss, the reader's token-wait budget SHALL cover the producer's supported backoff, credential waits, RPC attempts and retry delays. A shorter schema/tablet phase timer MUST NOT terminate that wait. The default resolver policy MUST be able to receive the next attempt's result without being preempted by repeated thirty-second reader timeouts.

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
