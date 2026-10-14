# S3 credential wrapper coverage

## Purpose

Specify test-owned coverage and reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/credentials/ut`. A focused reproduction appends `-F 'TCredentials::Test'` using an exact mapping below. UNITTEST_FOR resolves credentials_ut.cpp from the parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 4/4 GOOD in the full owning executable, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundaries: formatting tests construct TAuthInfo directly. Direct async tests construct the real TS3Credentials wrapper with a test-local controlled provider: acquisition counters detect synchronous fallback, a weak provider reference observes lifetime without retaining the provider, and a held promise delivers success or exception after wrapper destruction. These cover existing wrapper behavior, not a pre-fix failure. They do not resolve a secret or test provider refresh; those belong to the reader specs. GetAuthInfoAsync returns a future and exposes no cancellation API; late HTTP-dispatch suppression and download cancellation remain read-actor responsibilities in the [S3 actor specification](../../../../actors/ut/specs/external-secrets/spec.md). Completion does not require immediate provider destruction while future state may retain captures.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Bearer credential formatting

- **GIVEN** a token auth record.
- **WHEN** GetToken is called.
- **THEN** the exact supplied token is returned.

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TCredentials::CheckToken` | [credentials_ut.cpp:57](../../../credentials_ut.cpp#L57) | `ydb/library/yql/providers/s3/credentials/ut` |

#### Scenario: AWS credential compatibility

- **GIVEN** AWS key, secret and region.
- **WHEN** signature and user/password formatting is requested.
- **THEN** the wrapper returns aws:amz:region:s3 and key:secret.

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TCredentials::CheckAws` | [credentials_ut.cpp:62](../../../credentials_ut.cpp#L62) | `ydb/library/yql/providers/s3/credentials/ut` |

#### Scenario: Asynchronous completion retains provider lifetime

- **GIVEN** a wrapper with an unresolved controlled provider future.
- **WHEN** GetAuthInfoAsync returns a pending future, the wrapper is destroyed, and acquisition succeeds.
- **THEN** the provider remains alive through completion, the future returns the supplied token, and no synchronous acquisition occurs.

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TCredentials::AsyncSuccessAfterWrapperDestruction` | [credentials_ut.cpp:68](../../../credentials_ut.cpp#L68) | `ydb/library/yql/providers/s3/credentials/ut` |

#### Scenario: Asynchronous provider error propagates

- **GIVEN** a pending acquisition future retained after wrapper destruction.
- **WHEN** the provider completes its promise with a controlled exception.
- **THEN** the returned future carries that exception, has no successful value and performs no synchronous acquisition.

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TCredentials::AsyncFailureAfterWrapperDestruction` | [credentials_ut.cpp:86](../../../credentials_ut.cpp#L86) | `ydb/library/yql/providers/s3/credentials/ut` |
