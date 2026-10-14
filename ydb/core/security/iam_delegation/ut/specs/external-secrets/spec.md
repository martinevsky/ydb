# IAM services and delegated tokens

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [token](../../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 57/57 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundary: Real delegation/token/cloud-resolver actors run in TTestActorRuntime; token-manager and cloud RPC events are intercepted at their service boundaries. The fixture controls time, cookies and gRPC outcomes; it does not call a live IAM endpoint.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Shared mint and identity-separated cache

- **GIVEN** two readers of the same persisted service identity.
- **WHEN** one cloud mint completes.
- **THEN** both receive the same token with a five-minute usable margin; a changed service identity starts a distinct mint.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::ConcurrentReadersShareMintAndReceiveUsableBoundary` | [iam_delegated_token_actor_ut.cpp:198](../../iam_delegated_token_actor_ut.cpp#L198) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::PersistedServiceIdentitySeparatesCachedTokens` | [iam_delegated_token_actor_ut.cpp:220](../../iam_delegated_token_actor_ut.cpp#L220) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Strict lifetime boundary

- **GIVEN** responses with zero, four or exactly five minutes of lifetime.
- **WHEN** the producer validates them or reaches a cached token usable deadline.
- **THEN** short tokens fail without a value and equality waits for a fresh mint.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::TokensWithAtMostFiveMinutesRemainingAreRejected` | [iam_delegated_token_actor_ut.cpp:237](../../iam_delegated_token_actor_ut.cpp#L237) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::TokenAtUsableBoundaryWaitsForFreshMint` | [iam_delegated_token_actor_ut.cpp:249](../../iam_delegated_token_actor_ut.cpp#L249) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Refresh failure and revocation

- **GIVEN** an initially usable cached token.
- **WHEN** a malformed refresh or permission-denied/not-found result arrives.
- **THEN** a malformed refresh preserves only a still-usable token; authoritative revocation removes it.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegatedTokenActor::MalformedRefreshKeepsUsableCachedToken` | [iam_delegated_token_actor_ut.cpp:263](../../iam_delegated_token_actor_ut.cpp#L263) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegatedTokenActor::RevocationDiscardsCachedToken` | [iam_delegated_token_actor_ut.cpp:272](../../iam_delegated_token_actor_ut.cpp#L272) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Polling budget and late transport replies

- **GIVEN** a submitted operation with held replies and a fixture clock.
- **WHEN** polling, credential wait or retry reaches its deadline.
- **THEN** no post-deadline poll/retry begins, and undelivered calls and stale cookies cannot complete unrelated requests.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationActor::NoPollStartsAtTheDeadline` | [iam_delegation_actor_ut.cpp:417](../../iam_delegation_actor_ut.cpp#L417) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::PollResponseWaitIsClippedToDeadline` | [iam_delegation_actor_ut.cpp:430](../../iam_delegation_actor_ut.cpp#L430) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::CredentialsCannotOverrunPollDeadline` | [iam_delegation_actor_ut.cpp:443](../../iam_delegation_actor_ut.cpp#L443) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::RetryBackoffCannotStartAPollAfterDeadline` | [iam_delegation_actor_ut.cpp:457](../../iam_delegation_actor_ut.cpp#L457) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::ReplyAtDeadlineCannotStartARetry` | [iam_delegation_actor_ut.cpp:467](../../iam_delegation_actor_ut.cpp#L467) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::ConcurrentRequestsMatchOnlyTheirOwnCookies` | [iam_delegation_actor_ut.cpp:379](../../iam_delegation_actor_ut.cpp#L379) | `ydb/core/security/iam_delegation/ut` |
| `IamDelegationActor::UndeliveredCallsAreRetriedAndLateRepliesAreIgnored` | [iam_delegation_actor_ut.cpp:399](../../iam_delegation_actor_ut.cpp#L399) | `ydb/core/security/iam_delegation/ut` |

#### Scenario: Cancellation during credential subscription

- **GIVEN** the real actor is waiting for system credentials.
- **WHEN** poison is delivered.
- **THEN** the subscription wait is disarmed and late events cannot continue the operation.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationActor::PoisonDuringCredentialsWaitDisarmsTheSubscriptionWait` | [iam_delegation_actor_ut.cpp:492](../../iam_delegation_actor_ut.cpp#L492) | `ydb/core/security/iam_delegation/ut` |
