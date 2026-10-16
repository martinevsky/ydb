# Asynchronous credential ownership regression scenarios

## Purpose

Specify the shared credential bridge's payload ownership and completion routing. These tests support the [consumer coroutine contract](../../../../../../s3/actors/openspec/changes/external-secrets-iam-delegation/specs/external-secret-s3-cancellation/spec.md); they do not introduce a separate token producer or cancellation API.

## Execution and evidence

Latest recorded owning run: **3 GOOD** (full owning suite) at `8892e3bce7b2`, `final-review-full-green-parallel100`; actual combined command exit 1. Historical counts below retain their original revisions.

Owning target: `ydb/library/yql/providers/common/token_accessor/client/ut`. Run `./ya make --build relwithdebinfo -tA --ignore-recurses --threads=100 ydb/library/yql/providers/common/token_accessor/client/ut`.

All three cases passed at `8892e3bce7b23da9fd0c22e2553bb3f9d4011585` in `final-review-full-green-parallel100`. That combined command exited 1 because other targets failed; this owning suite completed 3/3 GOOD. Source hashes and dereferenced traces are archived in `spec-implementation-2026-10-15`.

Fixture boundary: real actors, real NActors::async waits and a controlled shared credential future. Payload destruction is observed directly. A distinct calling actor invokes the consumer method to exercise completion routing with both ready and delayed futures. No live IAM or arbitrary real-thread shutdown behavior is claimed.

## Requirements

### Requirement: Traceable credential ownership coverage
The owning target SHALL verify immediate release of cancelled consumers' request payloads, continued service for surviving consumers and completion on the correct actor. Each scenario SHALL retain its executable mapping and distinguish its fixture from external IAM behavior.

#### Scenario: Cancellation before the credential reply
- **WHEN** a consumer is destroyed while its credential future is pending
- **THEN** its request payload is released before the future completes
- **AND** a late credential reply does not resume that consumer

Contract coverage: external-secret-s3-cancellation / "Credential waits use coroutine-owned operation state" — scenario "Cancellation while the credential coroutine is suspended".

| Executable case | Test source | Command target |
| --- | --- | --- |
| `AsynchronousCredentialOwnership::CancellationReleasesPayloadBeforeCredentialReply` | [async_credentials_ut.cpp](../../async_credentials_ut.cpp) | `ydb/library/yql/providers/common/token_accessor/client/ut` |

#### Scenario: One consumer leaves a shared acquisition
- **WHEN** two consumers await the same credential future and one is destroyed
- **THEN** the destroyed consumer releases its payload immediately
- **AND** the surviving consumer receives the credential exactly once

Contract coverage: external-secret-s3-cancellation / "Credential waits use coroutine-owned operation state" (shared acquisition preservation at the common bridge boundary).

| Executable case | Test source | Command target |
| --- | --- | --- |
| `AsynchronousCredentialOwnership::CancellingOneConsumerPreservesSharedAcquisition` | [async_credentials_ut.cpp](../../async_credentials_ut.cpp) | `ydb/library/yql/providers/common/token_accessor/client/ut` |

#### Scenario: Direct calls retain the consumer as completion owner
- **WHEN** another actor directly starts the consumer's credential wait
- **THEN** both already-ready and delayed credentials resume the consumer exactly once
- **AND** the calling actor receives no credential completion event

Contract coverage: external-secret-s3-cancellation / "Credential waits use coroutine-owned operation state" (consumer ownership while another actor holds thread-local activation).

| Executable case | Test source | Command target |
| --- | --- | --- |
| `AsynchronousCredentialOwnership::DirectCallCompletesOnConsumerInsteadOfCallingActor` | [async_credentials_ut.cpp](../../async_credentials_ut.cpp) | `ydb/library/yql/providers/common/token_accessor/client/ut` |
