# IAM services and delegated tokens

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [lifecycle](../../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md), [token](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md).

## Execution and evidence

Latest recorded owning run: **73 GOOD** (full owning suite) at `9a2b73490817`, `final-fixtures-token-lifetime-green-parallel100`; actual combined command exit 0. Historical counts below retain their original revisions.

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Historical source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Historical status: **implemented and verified**, 57/57 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../docs/external-secrets/verification.md). These counts apply only to that source revision; added cases below require separate fresh execution.

Fixture boundary: Real delegation/token/cloud-resolver actors run in TTestActorRuntime; token-manager and cloud RPC events are intercepted at their service boundaries. The fixture controls time, cookies and gRPC outcomes; it does not call a live IAM endpoint.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Shared mint and identity-separated cache

- **GIVEN** two readers of the same persisted service identity.
- **WHEN** one cloud mint completes.
- **THEN** both receive the same token with a five-minute usable margin; a changed service identity starts a distinct mint.

Contract coverage: iam-delegated-token-acquisition / "Concurrent demand shares one producer" (shared-mint happy path; no dedicated contract scenario); "Served tokens have strictly more than five minutes remaining" (usable-margin assertion; boundary scenarios are covered below).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::ConcurrentReadersShareMintAndReceiveUsableBoundary` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::PersistedServiceIdentitySeparatesCachedTokens` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Strict lifetime boundary

- **GIVEN** responses with zero, four or exactly five minutes of lifetime.
- **WHEN** the producer validates them or reaches a cached token usable deadline.
- **THEN** short tokens fail without a value and equality waits for a fresh mint.

Contract coverage: iam-delegated-token-acquisition / "Served tokens have strictly more than five minutes remaining" — scenario "Exactly five minutes remain". The positive boundary ("Just before the usable deadline") is mapped below.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::TokensWithAtMostFiveMinutesRemainingAreRejected` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::TokenAtUsableBoundaryWaitsForFreshMint` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Refresh failure and account-level denial

- **GIVEN** an initially usable cached token.
- **WHEN** a malformed refresh or a permission-denied/not-found result for the account arrives.
- **THEN** a malformed refresh preserves only a still-usable token; an authoritative account-level denial removes it.

Contract coverage: iam-delegated-token-acquisition / "Transient failures preserve usable cached tokens" — scenarios "Temporary IAM outage with a usable cache entry", "Delegation is authoritatively rejected". The mapped case `RevocationDiscardsCachedToken` exercises the account-level denial; its name predates the contract's account-versus-referrer terminology fix.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::MalformedRefreshKeepsUsableCachedToken` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::RevocationDiscardsCachedToken` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Polling budget and late transport replies

- **GIVEN** a submitted operation with held replies and a fixture clock.
- **WHEN** polling, credential wait or retry reaches its deadline.
- **THEN** no post-deadline poll/retry begins, and undelivered calls and stale cookies cannot complete unrelated requests.

Contract coverage: test-owned transport discipline supporting iam-delegation-lifecycle / "Unknown setup is not a failed setup" (saved-operation polling); no one-to-one contract scenario.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationActor::NoPollStartsAtTheDeadline` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::PollResponseWaitIsClippedToDeadline` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::CredentialsCannotOverrunPollDeadline` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::RetryBackoffCannotStartAPollAfterDeadline` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::ReplyAtDeadlineCannotStartARetry` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::ConcurrentRequestsMatchOnlyTheirOwnCookies` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::UndeliveredCallsAreRetriedAndLateRepliesAreIgnored` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Cancellation during credential subscription

- **GIVEN** the real actor is waiting for system credentials.
- **WHEN** poison is delivered.
- **THEN** the subscription wait is disarmed and late events cannot continue the operation.

Contract coverage: iam-delegated-token-acquisition / "Caller policy and cancellation remain effective" (producer-side cancellation half).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationActor::PoisonDuringCredentialsWaitDisarmsTheSubscriptionWait` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Minting never uses the original bearer

- **GIVEN** a delegation whose creator's bearer has expired.
- **WHEN** a consumer acquires a token.
- **THEN** minting proceeds on platform credentials and the saved binding.

Status: mapped executable cases passed at `1373e723886e`, `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationSecretReader::LongLivedReaderMintsWithRotatedPlatformCredential` ([source](../../../../../../services/scheme_secret/ut/fast/iam_delegation_reader_ut.cpp)); `IamDelegatedTokenActor::RotatedSystemTokenAuthorizesBackgroundRefresh` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Minting uses platform credentials and the saved binding" — scenario "Original user session has expired".

#### Scenario: Positive usable boundary

- **GIVEN** a token just inside its usable deadline.
- **WHEN** acquisition completes one microsecond before the deadline.
- **THEN** that token is returned.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegatedTokenActor::NearBoundaryRefreshRemainsPacedAndNeverServesExpiredToken` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Served tokens have strictly more than five minutes remaining" — scenario "Just before the usable deadline".

#### Scenario: Refresh pacing and dispersion

- **GIVEN** entries first minted together and a near-boundary token.
- **WHEN** refreshes are scheduled.
- **THEN** first demand mints immediately, subsequent healthy refreshes disperse within their target using the standard helper, and subsecond usability keeps one-second pacing without serving past the deadline.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegatedTokenActor::SuccessfulRefreshesAreDispersedAcrossEntriesAndNodes` ([source](../../iam_delegated_token_actor_ut.cpp)); `IamDelegatedTokenActor::NearBoundaryRefreshRemainsPacedAndNeverServesExpiredToken` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Refresh is automatic and bounded against busy loops" — scenarios "Long-running consumer", "Near-boundary token"; "Refresh scheduling is dispersed" — scenario "Fleet restart aligns cache population".

#### Scenario: Idle eviction and waiter race

- **GIVEN** an entry with no waiting callers, then one being evicted as a caller arrives.
- **WHEN** the idle bound elapses or eviction races a waiter.
- **THEN** the idle entry is evicted within the bound and the waiter receives a fresh result, never the evicted entry.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegatedTokenActor::IdleEntryStopsBackgroundMintingAndNextDemandStartsImmediately` ([source](../../iam_delegated_token_actor_ut.cpp)); `IamDelegatedTokenActor::DemandBeforeIdleEvictionPreservesCurrentEntry` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Idle entries expire in bounded time" — scenario "Waiter arrives during eviction".

#### Scenario: Sibling delegation survives out-of-band revocation

- **GIVEN** two live delegations for one account.
- **WHEN** one referrer is revoked out-of-band.
- **THEN** minting and serving continue through the remaining delegation.

Status: external IAM behavior requires deployment validation; no local test is credited as proof of remote sibling-referrer semantics. `IamDelegationSecretReader::CancelledReaderLeavesSharedMintAndIndependentAuthorizationIntact` and `SchemaDropEndsAccessBeforeLifecycleWorkerRetiresBinding` cover local account sharing and per-reference authorization with controlled transport.

Contract coverage: iam-delegated-token-acquisition / "Token serving is account-scoped" — scenario "Sibling delegation outlives out-of-band revocation".

#### Scenario: Degraded minting is visible

- **GIVEN** a usable cached token and capture of the real producer's warnings.
- **WHEN** all RPC attempts of a refresh fail retryably.
- **THEN** a warning exposes the failed refresh while the usable token keeps serving, and issues/logs exclude credential material.

Status: the mapped case passed at `9a2b73490817`. It validates observable warning input; it does not configure or validate an external operator threshold policy.

Executable coverage: `IamDelegatedTokenActor::RetryableMintWarningsRemainVisibleWhileCacheServesWithoutCredentials` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Degraded minting is operator-visible" — scenario "Refresh fails behind a usable cache" (observable-warning portion only; external threshold integration requires deployment validation).

#### Scenario: Terminal IAM refusal retains safe diagnostic issues

- **GIVEN** an IAM permission denial with an empty token cache.
- **WHEN** acquisition surfaces its result.
- **THEN** it returns UNAUTHORIZED and the safe RESOURCE hint without token or bearer material in issues/logs.

Status: the mapped case passed at `9a2b73490817`. This covers terminal IAM refusal, not retryable IAM transport failure.

Executable coverage: `IamDelegatedTokenActor::FailedMintPreservesStatusAndSafeHintWithoutRemoteCredentials` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Minting uses platform credentials and the saved binding" (no-credential-material clause); "Transient failures preserve usable cached tokens" (authoritative-denial status).

#### Scenario: Retryable platform-credential failure retains safe issues

- **GIVEN** an empty token cache and an error from the platform credential provider.
- **WHEN** the real token producer handles acquisition.
- **THEN** it returns retryable UNAVAILABLE with nonempty safe issues, excludes the credential sentinel from issues/logs and sends no IAM mint request.

Status: the mapped case passed at `9a2b73490817`. Its controlled failure is at the platform-credential boundary; it does not inject an IAM transport error.

Executable coverage: `IamDelegatedTokenActor::TokenManagerFailureDiagnosticsExcludeCredentialMaterial` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Minting uses platform credentials and the saved binding" (no-credential-material clause); "Transient failures preserve usable cached tokens" — scenario "Retryable failure with an empty cache" (platform-credential failure).

#### Scenario: Producer coroutine owns waits and retry state

- **GIVEN** an owned producer coroutine suspended on IAM or standard backoff with several readers.
- **WHEN** one reader cancels, or the producer owner stops and late replies arrive.
- **THEN** a surviving reader can still complete while the producer lives; stopping the producer releases waits and cannot publish a late result or retain a stale cache-entry reference.

Status: mapped executable cases passed at `1373e723886e`, `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Executable coverage: `IamDelegationSecretReader::CancelledReaderLeavesSharedMintAndIndependentAuthorizationIntact` ([source](../../../../../../services/scheme_secret/ut/fast/iam_delegation_reader_ut.cpp)); `IamDelegatedTokenActor::ProducerShutdownDisarmsMintAndBackoffWaiters` ([source](../../iam_delegated_token_actor_ut.cpp)).

Contract coverage: iam-delegated-token-acquisition / "Refresh operation state belongs to its coroutine" — scenario "Producer stops with a mint outstanding"; "Caller policy and cancellation remain effective" — scenario "One concurrent caller cancels".

#### Scenario: IAM refusal reasons exclude remote free text

- **GIVEN** submission or poll errors containing supported precondition types, duplicates, unknown types and credential sentinels in messages/descriptions.
- **WHEN** the real IAM adapter processes the mocked service response.
- **THEN** the diagnostic vector contains each of the five allowlisted reason groups once with the mapped denial status and no additional setup/revoke; the typed representation has no remote free-text field.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-lifecycle / "Terminal setup failures retain their diagnostics".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecovery::SubmissionRefusalExtractsOnlyBoundedSafeReasons` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationRecovery::PolledRefusalExtractsOnlyBoundedSafeReasons` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Contradictory operation result preserves uncertainty

- **GIVEN** a saved IAM operation identifier.
- **WHEN** polling returns a done operation containing an error object with code OK.
- **THEN** the adapter returns Unknown/INTERNAL_ERROR with the saved identifier; it sends no setup/revoke and does not treat the contradictory response as definitive failure.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegation-lifecycle / "Unknown setup is not a failed setup".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationRecovery::TerminalOperationWithSuccessErrorCodeRemainsUnknown` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Refresh dispersion and exact serving cutoffs

- **GIVEN** 64 independently keyed entries across two controlled runtimes, short-lived tokens, prior failures and idle entries.
- **WHEN** cold demand, successful refresh, a new failure after success, exact expiry and demand after eviction occur.
- **THEN** first mint remains immediate, healthy refreshes spread within the bounded earlier window, subsecond usability still expires at the exact boundary despite one-second pacing, recovery resets failure backoff, and idle entries stop minting until new demand.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegated-token-acquisition / "Refresh scheduling is dispersed"; "Refresh is automatic and bounded against busy loops"; "Idle entries expire in bounded time".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::SuccessfulRefreshesAreDispersedAcrossEntriesAndNodes` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::NearBoundaryRefreshRemainsPacedAndNeverServesExpiredToken` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::SuccessfulMintResetsFailureBackoffBeforeNextFailure` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::IdleEntryStopsBackgroundMintingAndNextDemandStartsImmediately` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Credential material is excluded from diagnostic channels

- **GIVEN** real IAM actors with controlled transport and token-manager diagnostics containing credential sentinels.
- **WHEN** minting, operation submission or polling succeeds, retries or fails.
- **THEN** status and safe hints remain observable while issues and logs exclude credential material; uncertain operation outcomes remain uncertain.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegated-token-acquisition; iam-delegation-revocation.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::FailedMintPreservesStatusAndSafeHintWithoutRemoteCredentials` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::RetryableMintWarningsRemainVisibleWhileCacheServesWithoutCredentials` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::SuccessfulMintDiagnosticsExcludePlatformAndMintedCredentials` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::TokenManagerFailureDiagnosticsExcludeCredentialMaterial` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationRecovery::CompletedOperationIssuesExcludeRawMessageAndDescriptions` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationRecovery::PollTransportIssuesStayRedactedWithoutResolvingUncertainty` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationRecovery::SubmissionIssuesRetainSafeHintWithoutRawTransportDiagnostics` | [iam_delegation_actor_ut.cpp](../../iam_delegation_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Demand around idle eviction and producer shutdown

- **GIVEN** the real token producer and its actual scheduled refresh, or readers suspended during minting/backoff.
- **WHEN** demand arrives immediately before the idle check, after eviction, or the producer receives Poison before a late mint reply.
- **THEN** recent demand preserves the current entry, demand after eviction starts a fresh mint, and shutdown cancels readers and refresh work without publishing late results.

Status: mapped executable cases passed at `9a2b73490817`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: iam-delegated-token-acquisition / "Idle entries expire in bounded time" — scenario "Waiter arrives during eviction"; "Refresh operation state belongs to its coroutine" — scenario "Producer stops with a mint outstanding".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::DemandBeforeIdleEvictionPreservesCurrentEntry` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::IdleEntryStopsBackgroundMintingAndNextDemandStartsImmediately` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::ProducerShutdownDisarmsMintAndBackoffWaiters` | [iam_delegated_token_actor_ut.cpp](../../iam_delegated_token_actor_ut.cpp) | `ydb/core/security/iam_delegation/ut` |
