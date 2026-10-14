# S3 asynchronous credential and cancellation coverage

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md), [s3](../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-s3-cancellation/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/actors/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 changes paths only; source hashes and new-path results are recorded in the [durable verification record](../../../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md).

Status: **implemented; new-path/source-revision execution unverified until the verification record records a successful run**. Historical recovery/C1 runs used pre-relocation paths; names alone do not prove a passing run. Consumer and driver runs are deferred to Task4. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: TParquetReader uses the real read actor, an asynchronous credential factory and controlled HTTP range gateway. It reads generated Parquet data and counts credentials/ranges. Existing footer tests indirectly exercise TS3Credentials::GetAuthInfoAsync through held and failed acquisition. They do not exercise the read-side TRetryStuff streaming hook publication race or reentrant/repeated cancellation; those are C2 work.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Credential wait before range request

- **GIVEN** a Parquet source with held credential future.
- **WHEN** the read actor pumps before and after fresh-token release.
- **THEN** no range starts while held, the actor remains pumpable, three rows complete, and credential calls match range calls.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TS3ParquetFooter::ParquetRangesAwaitCredentialsWithoutBlockingActor` | [yql_s3_read_actor_ut.cpp:268](../../yql_s3_read_actor_ut.cpp#L268) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Authorization failure before range request

- **GIVEN** the same asynchronous source.
- **WHEN** credential acquisition fails.
- **THEN** the read reports an error with zero HTTP range calls.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TS3ParquetFooter::ParquetAuthorizationFailureNeverStartsHttpRequest` | [yql_s3_read_actor_ut.cpp:284](../../yql_s3_read_actor_ut.cpp#L284) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Cancellation before credentials resolve

- **GIVEN** a read actor waiting for an asynchronous credential future.
- **WHEN** the read is cancelled before credentials arrive.
- **THEN** the late continuation starts no download and does not access destroyed reader state.

Status: **planned C2; no registered executable case exists yet**. Proposed case `S3ExternalSecretCancellation::CancelBeforeCredentialsResolve` belongs to this actor target; exact source and focused command pending Task3.

#### Scenario: Streaming decoder cancellation precedes hook publication

- **GIVEN** an AsyncDecoding streaming read whose gateway has delivered callbacks but has not returned its cancellation hook.
- **WHEN** the decoder cancels before Download returns and TRetryStuff publishes that hook.
- **THEN** the returned hook is consumed exactly once and the stream is cancelled.

Status: **planned C2; no registered executable case exists yet**. Proposed case `S3ExternalSecretCancellation::StreamingCancelBeforeHookPublication` belongs to this actor target; source and focused command pending Task3. The fixture must control the gateway interval deterministically, without sleep-based race frequency.

#### Scenario: Repeated or concurrent cancellation and publication

- **GIVEN** the actual read-side cancellation primitive with hook publication held or completed.
- **WHEN** cancellation runs before/after publication, concurrently with publication, or repeatedly.
- **THEN** the dispatched download hook is consumed once without a data race or duplicate invocation.

Status: **planned C2; no registered executable case exists yet**. Proposed case `S3ExternalSecretCancellation::RepeatedConcurrentCancellation` belongs to this actor target; source and focused command pending Task3.

#### Scenario: Reentrant cancel callback

- **GIVEN** a published download hook that synchronously reenters cancellation.
- **WHEN** cancellation invokes the hook.
- **THEN** invocation occurs outside the synchronization lock and finishes without deadlock or invoking itself again.

Status: **planned C2; no registered executable case exists yet**. Proposed case `S3ExternalSecretCancellation::ReentrantCancelCallback` belongs to this actor target; source and focused command pending Task3.
