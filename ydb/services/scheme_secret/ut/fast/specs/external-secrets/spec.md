# Reader timing and refreshing credentials

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [token](../../../../../../core/security/iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md), [consumers](../../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/services/scheme_secret/ut/fast`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 42/42 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundary: TReadFixture uses the real delegation tablet and reader actor with controlled schema/mint events. The C1 backoff case additionally uses the real token producer and default secret resolver, seeded jitter (42), and an intercepted gRPC boundary. TCredentialsFixture controls asynchronous secret-service responses and preserves authenticated user/reference metadata.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Concurrent wait through producer backoff

- **GIVEN** the real producer has reached a 65–75 second next-attempt delay.
- **WHEN** a direct reader and default resolver join it and IAM then recovers.
- **THEN** neither returns or remints after 61 seconds; both receive recovered-token from one new mint.

Contract coverage: iam-delegated-token-acquisition / "A reader permits one complete bounded mint attempt" — scenario "Backoff longer than sixty seconds"; "Concurrent demand shares one producer" — scenario "Several callers arrive during backoff".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::DefaultResolverAndConcurrentReaderWaitThroughRealProducerBackoff` | [iam_delegation_reader_ut.cpp:201](../../iam_delegation_reader_ut.cpp#L201) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Bounded phase timers and cancellation

- **GIVEN** missing/held token, storage or schema reply.
- **WHEN** phase-specific timers fire, cancellation occurs or a late mint arrives.
- **THEN** missing replies time out; stale token timers cannot truncate schema wait and cancelled readers ignore late mint/timer.

Contract coverage: iam-delegated-token-acquisition / "Every wait remains finite and phase-specific" — scenarios "Token service never replies", "Earlier phase timeout arrives late"; "Caller policy and cancellation remain effective" — scenario "One concurrent caller cancels" (cancelled-caller half; the shared-result half is planned below).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::MissingTokenReplyTimesOutAndLateReplyIsIgnored` | [iam_delegation_reader_ut.cpp:280](../../iam_delegation_reader_ut.cpp#L280) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::StorageWaitRemainsBounded` | [iam_delegation_reader_ut.cpp:300](../../iam_delegation_reader_ut.cpp#L300) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::StaleTokenTimerDoesNotShortenBoundedSchemaWait` | [iam_delegation_reader_ut.cpp:312](../../iam_delegation_reader_ut.cpp#L312) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::CancelledReaderIgnoresLateMintAndTimer` | [iam_delegation_reader_ut.cpp:338](../../iam_delegation_reader_ut.cpp#L338) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Expiry and post-mint identity/ACL check

- **GIVEN** a token is held across promotion, schema DROP, reuse or ACL revoke.
- **WHEN** the reader revalidates before replying, including usable-deadline equality.
- **THEN** no obsolete/expired value crosses those boundaries and invalid creating identity never mints.

Contract coverage: external-secret-consumers / "Mint completion is followed by revalidation" — scenarios "DROP while minting", "Replacement promoted during minting", "Token crosses its usable deadline during minting"; iam-delegation-revocation / "DROP ends schema access independently of IAM" (reader half: access ends before the binding is retired).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::HeldTokenCannotCrossPromotion` | [iam_delegation_reader_ut.cpp:435](../../iam_delegation_reader_ut.cpp#L435) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::SchemaDropEndsAccessBeforeLifecycleWorkerRetiresBinding` | [iam_delegation_reader_ut.cpp:452](../../iam_delegation_reader_ut.cpp#L452) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::ReusedSchemaPathCannotReuseOldAuthorization` | [iam_delegation_reader_ut.cpp:463](../../iam_delegation_reader_ut.cpp#L463) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::AclRevocationDeniesAlreadyMintedValue` | [iam_delegation_reader_ut.cpp:469](../../iam_delegation_reader_ut.cpp#L469) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::TokenAtUsableDeadlineIsNotReturned` | [iam_delegation_reader_ut.cpp:475](../../iam_delegation_reader_ut.cpp#L475) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::TokenAtUsableDeadlineDuringSchemaCheckIsNotReturned` | [iam_delegation_reader_ut.cpp:357](../../iam_delegation_reader_ut.cpp#L357) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::CreatingOperationMismatchNeverMints` | [iam_delegation_reader_ut.cpp:427](../../iam_delegation_reader_ut.cpp#L427) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Retry classification and caller budget

- **GIVEN** transient or definitive mint failures.
- **WHEN** default or caller retry policy handles the result.
- **THEN** transient statuses honor policy/budget and final status; definitive failures do not retry.

Contract coverage: iam-delegated-token-acquisition / "Transient failures preserve usable cached tokens" — scenario "Retryable failure with an empty cache"; "Caller policy and cancellation remain effective"; external-secret-schema / "Metadata is independent of token health" — scenario "IAM is unavailable during metadata access" (value-acquisition half: a later acquisition reports the token error).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::SecretServiceRetriesEveryTransientMintStatusWithCallerPolicy` | [iam_delegation_reader_ut.cpp:369](../../iam_delegation_reader_ut.cpp#L369) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::SecretServiceUsesDefaultRetryPolicyForTransientMintFailure` | [iam_delegation_reader_ut.cpp:383](../../iam_delegation_reader_ut.cpp#L383) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::SecretServiceDoesNotRetryDefinitiveMintErrors` | [iam_delegation_reader_ut.cpp:391](../../iam_delegation_reader_ut.cpp#L391) | `ydb/services/scheme_secret/ut/fast` |
| `IamDelegationSecretReader::SecretServiceHonorsCallerRetryBudgetAndFinalStatus` | [iam_delegation_reader_ut.cpp:405](../../iam_delegation_reader_ut.cpp#L405) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Fresh credentials per use

- **GIVEN** a refreshing provider and concurrent acquisition calls.
- **WHEN** one read succeeds and a later use follows a changed value.
- **THEN** concurrent calls share one read, next use reads again, and bearer wrapping remains asynchronous.

Contract coverage: external-secret-consumers / "Credentials are acquired for new external requests".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `RefreshingSecretCredentials::EveryUseReadsCurrentStateAndConcurrentUsesShareOneRead` | [secret_credentials_ut.cpp:77](../../secret_credentials_ut.cpp#L77) | `ydb/services/scheme_secret/ut/fast` |
| `RefreshingSecretCredentials::BearerWrapperPreservesAsynchronousAcquisition` | [secret_credentials_ut.cpp:65](../../secret_credentials_ut.cpp#L65) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Failure and safe structured metadata

- **GIVEN** a previous credential value and authenticated immutable reference.
- **WHEN** a later read is denied/expired or metadata is persisted.
- **THEN** previous value is never returned; metadata retains identity/user SID without the original bearer.

Contract coverage: external-secret-consumers / "Credentials are acquired for new external requests" — scenario "Authorization is lost after an earlier successful request"; iam-delegated-token-acquisition / "Minting uses platform credentials and the saved binding" (no bearer persisted).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `RefreshingSecretCredentials::FailedOrExpiredReadNeverServesPreviousValue` | [secret_credentials_ut.cpp:96](../../secret_credentials_ut.cpp#L96) | `ydb/services/scheme_secret/ut/fast` |
| `RefreshingSecretCredentials::StructuredMetadataContainsIdentityButNoOriginalBearer` | [secret_credentials_ut.cpp:113](../../secret_credentials_ut.cpp#L113) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Shared callers survive one cancellation

- **GIVEN** several concurrent callers sharing one in-flight mint.
- **WHEN** one caller cancels.
- **THEN** the cancelled acquisition coroutine releases its waits, late replies emit no credentials or second result, and the remaining callers still receive the shared result.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: iam-delegated-token-acquisition / "Caller policy and cancellation remain effective" — scenario "One concurrent caller cancels" (shared-result half); external-secret-consumers / "Acquisition state belongs to its coroutine" — scenario "Cancelled acquisition receives a late transport result".

#### Scenario: Per-caller authorization on shared resolution

- **GIVEN** a shared in-flight resolution for one secret identity.
- **WHEN** a caller without value permission attaches to it, or callers with different permissions share a mint.
- **THEN** each caller's authorization is enforced on its own result; sharing never grants access.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-consumers / "Shared resolution enforces per-caller authorization" — scenario "Unauthorized caller attaches to a shared resolution"; iam-delegated-token-acquisition / "Concurrent demand shares one producer" — scenario "Callers with different permissions share a mint".

#### Scenario: Value-read permission separation

- **GIVEN** callers holding only SelectRow or only DescribeSchema on the secret.
- **WHEN** they request the value or the metadata respectively.
- **THEN** the SelectRow-only caller can read the value cold, and the metadata-only caller cannot obtain the value.

Status: planned — no executable case yet; does not contribute to executed counts.

Contract coverage: external-secret-consumers / "Value reads use their own permission" — scenarios "SelectRow-only cold read", "Metadata-only caller".

#### Scenario: Long-lived acquisition rotates platform credentials

- **GIVEN** the real token producer, durable delegation tablet and secret reader with controlled IAM transport.
- **WHEN** a consumer reads for over one hour while platform credentials rotate before the first healthy refresh.
- **THEN** autonomous refreshes use the current platform credential, subsequent reads receive current usable tokens, and no creator bearer is retained or sent.

Status: executable source added; first execution pending. This validates local credential selection, not live IAM authorization semantics.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::LongLivedReaderMintsWithRotatedPlatformCredential` | [iam_delegation_reader_ut.cpp](../../iam_delegation_reader_ut.cpp) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Shared mint retains independent reader ownership and authorization

- **GIVEN** cancelled, allowed, denied and stale-identity readers sharing one real token producer.
- **WHEN** one IAM mint completes and abandoned timers subsequently arrive.
- **THEN** only the allowed reader receives a token, the other readers retain their own authorization and identity results, and cancellation emits no reply or duplicate mint.

Status: executable source added; first execution pending. Controlled schema replies do not establish remote IAM sibling-referrer semantics.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::CancelledReaderLeavesSharedMintAndIndependentAuthorizationIntact` | [iam_delegation_reader_ut.cpp](../../iam_delegation_reader_ut.cpp) | `ydb/services/scheme_secret/ut/fast` |
