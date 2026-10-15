# S3 asynchronous credential and cancellation coverage

## Purpose

Specify test-owned coverage and reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md), [s3](../../../openspec/changes/external-secrets-iam-delegation/specs/external-secret-s3-cancellation/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/actors/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 50/50 GOOD in the full owning executable, with exact command exit 0. Per-case mappings cite the first verification at `2e7842af1181` (the fixture-correction revision); the final integration run at `66189ce6` re-executed the full owning executable at the current paths, 50/50 GOOD with the archived trace hash recorded in the verification record. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../../../../core/security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundaries: TParquetReader uses the real read actor, asynchronous credentials and a controlled HTTP gateway on the simulated runtime's single thread. Its pending-credential test keeps the parent alive and cancels only the decoder, ensuring the late continuation actually executes the pre-dispatch guard. The streaming publication regression uses real TFakeCASetup worker threads and independently scheduled mailboxes: malformed CSV triggers decoder cancellation while Download has delivered callbacks but withholds its hook. An independent error recipient and a parent mailbox round trip establish ordering; finite waits and deferred gate release bound cleanup. Primitive tests invoke the actual TDownloadCancellation used by TRetryStuff, including 64 latch-started concurrent publication/cancellation attempts; they do not prove arbitrary interleavings or replace a race-detector run. No live IAM or TSan result is claimed.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Credential wait before range request

- **GIVEN** a Parquet source with a held credential future.
- **WHEN** the real read actor pumps before and after fresh-token release.
- **THEN** no range starts while held, the actor remains pumpable, three rows complete, and credential calls match range calls.

Contract coverage: external-secret-consumers / "Credentials are acquired for new external requests" (asynchronous acquisition without blocking the actor).

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TS3ParquetFooter::ParquetRangesAwaitCredentialsWithoutBlockingActor` | [yql_s3_read_actor_ut.cpp:443](../../yql_s3_read_actor_ut.cpp#L443) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Authorization failure before range request

- **GIVEN** the same asynchronous source.
- **WHEN** credential acquisition fails.
- **THEN** the read reports an error with zero HTTP range calls.

Contract coverage: external-secret-consumers / "Credentials are acquired for new external requests" — scenario "Authorization is lost after an earlier successful request" (consumer-side failure without a request).

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TS3ParquetFooter::ParquetAuthorizationFailureNeverStartsHttpRequest` | [yql_s3_read_actor_ut.cpp:459](../../yql_s3_read_actor_ut.cpp#L459) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Cancellation before credentials resolve

- **GIVEN** a streaming decoder waiting for asynchronous credentials, with its parent kept alive.
- **WHEN** the decoder handles cancellation before credentials arrive and the late continuation is drained.
- **THEN** no streaming download or cancel hook is dispatched and no error is reported.

Contract coverage: external-secret-s3-cancellation / "Cancellation before dispatch prevents a new download" — scenario "Cancellation precedes credential resolution".

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3ExternalSecretCancellation::CancelBeforeCredentialsResolve` | [yql_s3_read_actor_ut.cpp:405](../../yql_s3_read_actor_ut.cpp#L405) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Streaming decoder cancellation precedes hook publication

- **GIVEN** an AsyncDecoding CSV read whose gateway delivered callbacks but has not returned its hook.
- **WHEN** malformed Int32 data causes cancellation on the decoder mailbox before Download returns.
- **THEN** after explicit gate release and parent mailbox synchronization, the returned hook is invoked exactly once, with no gate timeout.

Contract coverage: external-secret-s3-cancellation / "Cancellation and download publication are synchronized" — scenario "Decoder cancels before the gateway returns".

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3ExternalSecretCancellation::StreamingCancelBeforeHookPublication` | [yql_s3_read_actor_ut.cpp:366](../../yql_s3_read_actor_ut.cpp#L366) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Repeated or concurrent cancellation and publication

- **GIVEN** the actual production cancellation primitive and one dispatched hook.
- **WHEN** cancellation runs before/after publication, repeatedly, or concurrently with publication.
- **THEN** the hook is consumed exactly once and the state remains cancelled.

Contract coverage: external-secret-s3-cancellation / "Cancellation and download publication are synchronized" — scenario "Repeated or concurrent cancellation".

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3DownloadCancellation::CancelBeforePublication` | [download_cancellation_ut.cpp:12](../../download_cancellation_ut.cpp#L12) | `ydb/library/yql/providers/s3/actors/ut` |
| `S3DownloadCancellation::CancelAfterPublication` | [download_cancellation_ut.cpp:23](../../download_cancellation_ut.cpp#L23) | `ydb/library/yql/providers/s3/actors/ut` |
| `S3DownloadCancellation::ConcurrentPublicationAndCancellation` | [download_cancellation_ut.cpp:34](../../download_cancellation_ut.cpp#L34) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Reentrant cancel callback

- **GIVEN** a hook that synchronously calls IsCancelled and reenters Cancel.
- **WHEN** cancellation happens before or after hook publication.
- **THEN** the hook observes cancelled state and finishes with exactly one invocation outside the synchronization lock.

Contract coverage: external-secret-s3-cancellation / "Cancellation and download publication are synchronized" — scenario "Hook calls back into cancellation".

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3DownloadCancellation::ReentrantHookBeforePublication` | [download_cancellation_ut.cpp:68](../../download_cancellation_ut.cpp#L68) | `ydb/library/yql/providers/s3/actors/ut` |
| `S3DownloadCancellation::ReentrantHookAfterPublication` | [download_cancellation_ut.cpp:56](../../download_cancellation_ut.cpp#L56) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Retry replaces a completed attempt hook

- **GIVEN** hooks from a completed attempt and the current retry.
- **WHEN** the current retry publishes its hook and the request is cancelled.
- **THEN** only the current hook is invoked; the completed attempt hook is not called.

Contract coverage: external-secret-s3-cancellation / "Cancellation and download publication are synchronized" (hook-once semantics across retries).

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3DownloadCancellation::RetryReplacesCompletedAttemptHook` | [download_cancellation_ut.cpp:80](../../download_cancellation_ut.cpp#L80) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Empty hook remains cancellable

- **GIVEN** an empty gateway hook.
- **WHEN** empty hooks are published before and after cancellation.
- **THEN** the primitive safely retains cancelled state.

Contract coverage: external-secret-s3-cancellation / "Cancellation and download publication are synchronized" (empty-hook edge).

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3DownloadCancellation::EmptyHook` | [download_cancellation_ut.cpp:91](../../download_cancellation_ut.cpp#L91) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Ordinary streaming read completes

- **GIVEN** valid CSV data with asynchronous decoding and an immediate gateway hook.
- **WHEN** the real read actor reads to completion.
- **THEN** it reports no error, finishes, delivers one row and starts one streaming request.

Contract coverage: regression guard for external-secret-s3-cancellation; no dedicated contract scenario.

Executable mapping (implemented and verified at `2e7842af1181`):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `S3ExternalSecretCancellation::StreamingReadStillCompletes` | [yql_s3_read_actor_ut.cpp:427](../../yql_s3_read_actor_ut.cpp#L427) | `ydb/library/yql/providers/s3/actors/ut` |

#### Scenario: Suspended credential coroutine is cancelled

- **GIVEN** a feature-owned credential coroutine with a held result and callback-only gateway adapter.
- **WHEN** cancellation releases the wait and credentials or gateway callbacks arrive late.
- **THEN** no new download starts, destroyed operation state is not resumed, and an existing hook remains protected by synchronized exactly-once cancellation.

Status: planned — no executable case yet; does not contribute to executed counts. Existing callback-based cancellation evidence above is preserved; it does not claim the coroutine refactor has run.

Contract coverage: external-secret-s3-cancellation / "Credential waits use coroutine-owned operation state" — scenario "Cancellation while the credential coroutine is suspended".
