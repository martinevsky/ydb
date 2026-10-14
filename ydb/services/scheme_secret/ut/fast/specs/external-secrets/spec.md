# Reader timing and refreshing credentials

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [token](../../../../../../core/security/iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md), [consumers](../../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/services/scheme_secret/ut/fast`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 changes paths only; source hashes and new-path results are recorded in the [durable verification record](../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md).

Status: **implemented; new-path/source-revision execution unverified until the verification record records a successful run**. Historical recovery/C1 runs used pre-relocation paths; names alone do not prove a passing run. Consumer and driver runs are deferred to Task4. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: TReadFixture uses the real delegation tablet and reader actor with controlled schema/mint events. The C1 backoff case additionally uses the real token producer and default secret resolver, seeded jitter (42), and an intercepted gRPC boundary. TCredentialsFixture controls asynchronous secret-service responses and preserves authenticated user/reference metadata.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Concurrent wait through producer backoff

- **GIVEN** the real producer has reached a 65–75 second next-attempt delay.
- **WHEN** a direct reader and default resolver join it and IAM then recovers.
- **THEN** neither returns or remints after 61 seconds; both receive recovered-token from one new mint.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `IamDelegationSecretReader::DefaultResolverAndConcurrentReaderWaitThroughRealProducerBackoff` | [iam_delegation_reader_ut.cpp:201](../../iam_delegation_reader_ut.cpp#L201) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Bounded phase timers and cancellation

- **GIVEN** missing/held token, storage or schema reply.
- **WHEN** phase-specific timers fire, cancellation occurs or a late mint arrives.
- **THEN** missing replies time out; stale token timers cannot truncate schema wait and cancelled readers ignore late mint/timer.

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

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `RefreshingSecretCredentials::EveryUseReadsCurrentStateAndConcurrentUsesShareOneRead` | [secret_credentials_ut.cpp:77](../../secret_credentials_ut.cpp#L77) | `ydb/services/scheme_secret/ut/fast` |
| `RefreshingSecretCredentials::BearerWrapperPreservesAsynchronousAcquisition` | [secret_credentials_ut.cpp:65](../../secret_credentials_ut.cpp#L65) | `ydb/services/scheme_secret/ut/fast` |

#### Scenario: Failure and safe structured metadata

- **GIVEN** a previous credential value and authenticated immutable reference.
- **WHEN** a later read is denied/expired or metadata is persisted.
- **THEN** previous value is never returned; metadata retains identity/user SID without the original bearer.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `RefreshingSecretCredentials::FailedOrExpiredReadNeverServesPreviousValue` | [secret_credentials_ut.cpp:96](../../secret_credentials_ut.cpp#L96) | `ydb/services/scheme_secret/ut/fast` |
| `RefreshingSecretCredentials::StructuredMetadataContainsIdentityButNoOriginalBearer` | [secret_credentials_ut.cpp:113](../../secret_credentials_ut.cpp#L113) | `ydb/services/scheme_secret/ut/fast` |
