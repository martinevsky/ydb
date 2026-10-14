# Solomon asynchronous credentials

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/solomon/actors/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 changes paths only; source hashes and new-path results are recorded in the [durable verification record](../../../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md).

Status: **implemented; new-path/source-revision execution unverified until the verification record records a successful run**. Historical recovery/C1 runs used pre-relocation paths; names alone do not prove a passing run. This target received coverage documentation only in Task5; existing execution evidence is historical and no additional Task4 rerun is required. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: TAsyncWriter drives the real Solomon write actor with asynchronous credentials, counted HTTP requests and callback/free-space observation. HTTP and credential responses are controlled; no live Solomon service is used.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Held credentials delay HTTP

- **GIVEN** a buffered Solomon write and held credential future.
- **WHEN** the actor pumps and then credentials are released.
- **THEN** one acquisition occurs with zero HTTP/completion while held; release starts exactly one HTTP request without premature completion.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `SolomonAsynchronousCredentials::HeldCredentialsDelayHttpAndCompletion` | [dq_solomon_async_credentials_ut.cpp:83](../../dq_solomon_async_credentials_ut.cpp#L83) | `ydb/library/yql/providers/solomon/actors/ut` |

#### Scenario: Denied credentials restore backpressure

- **GIVEN** a buffered write consumes free space.
- **WHEN** credential acquisition fails.
- **THEN** zero HTTP requests start, one error is delivered, free space is restored, and completion stays unset.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `SolomonAsynchronousCredentials::DeniedCredentialsRestoreBackpressureAndNeverSendHttp` | [dq_solomon_async_credentials_ut.cpp:97](../../dq_solomon_async_credentials_ut.cpp#L97) | `ydb/library/yql/providers/solomon/actors/ut` |
