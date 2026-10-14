# S3 credential wrapper coverage

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/credentials/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 changes paths only; source hashes and new-path results are recorded in the [durable verification record](../../../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md).

Status: **implemented; new-path/source-revision execution unverified until the verification record records a successful run**. Historical recovery/C1 runs used pre-relocation paths; names alone do not prove a passing run. Consumer and driver runs are deferred to Task4. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: TCredentials constructs TS3Credentials::TAuthInfo directly. This is wrapper-format coverage only: it neither resolves a secret nor verifies refresh/cancellation. End-to-end refreshing-provider behavior is owned by the reader specs.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Bearer credential formatting

- **GIVEN** a token auth record.
- **WHEN** GetToken is called.
- **THEN** the exact supplied token is returned.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TCredentials::CheckToken` | [credentials_ut.cpp:7](../../../credentials_ut.cpp#L7) | `ydb/library/yql/providers/s3/credentials/ut` |

#### Scenario: AWS credential compatibility

- **GIVEN** AWS key, secret and region.
- **WHEN** signature and user/password formatting is requested.
- **THEN** the wrapper returns aws:amz:region:s3 and key:secret.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TCredentials::CheckAws` | [credentials_ut.cpp:12](../../../credentials_ut.cpp#L12) | `ydb/library/yql/providers/s3/credentials/ut` |

#### Scenario: Asynchronous completion retains provider lifetime

- **GIVEN** a TS3Credentials wrapper whose provider acquisition future remains unresolved.
- **WHEN** GetAuthInfoAsync is called, the wrapper is destroyed, and the held provider future completes successfully.
- **THEN** the provider remains alive through completion and the returned TFuture<TAuthInfo> contains the supplied token.

Status: **planned Task3 wrapper regression; no registered executable exists yet**. Proposed case `TCredentials::AsyncCompletionRetainsProviderAfterWrapperDestruction`; source is planned in `ydb/library/yql/providers/s3/credentials/credentials_ut.cpp`, command target is `ydb/library/yql/providers/s3/credentials/ut`, and focused filter is pending final test naming. Existing [held/failed Parquet actor scenarios](../../../../actors/ut/specs/external-secrets/spec.md) provide indirect acquisition coverage without isolating destruction/lifetime.

#### Scenario: Asynchronous provider error propagates

- **GIVEN** GetAuthInfoAsync waiting on a controlled provider future.
- **WHEN** the provider completes that future with an exception.
- **THEN** the returned TFuture<TAuthInfo> carries the exception and supplies no successful token value.

Status: **planned Task3 wrapper regression; no registered executable exists yet**. Proposed case `TCredentials::AsyncProviderExceptionPropagates`; source is planned in `ydb/library/yql/providers/s3/credentials/credentials_ut.cpp`, command target is `ydb/library/yql/providers/s3/credentials/ut`, and focused filter is pending final test naming. This checks the existing Apply/value.GetValue transformation directly. GetAuthInfoAsync exposes no cancel entrypoint: cancellation of an S3 operation and suppression of late HTTP dispatch remain actor-owned C2 obligations in the [S3 actor specification](../../../../actors/ut/specs/external-secrets/spec.md).
