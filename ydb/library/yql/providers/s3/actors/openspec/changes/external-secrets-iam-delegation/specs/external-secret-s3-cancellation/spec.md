# S3 secret credential and download cancellation

## Purpose

Keep asynchronous S3 reads cancellable while credentials resolve and while decoder callbacks race with publication of the download cancellation hook.

## ADDED Requirements

### Requirement: Cancellation before dispatch prevents a new download
If an S3 read is cancelled while asynchronous credentials are unresolved, it SHALL NOT start a download when those credentials later arrive. Shared asynchronous state MUST remain valid until outstanding continuations finish.

#### Scenario: Cancellation precedes credential resolution
- **WHEN** a reader cancels before its credential future completes
- **THEN** completion does not dispatch an HTTP download or access destroyed reader state.

### Requirement: Cancellation and download publication are synchronized
If cancellation occurs after download dispatch begins but before its cancellation hook is published, the returned hook SHALL still be invoked exactly once. Cancellation after publication and repeated cancellation MUST have the same effect. Hook invocation MUST permit reentrant callbacks without deadlock.

#### Scenario: Decoder cancels before the gateway returns
- **WHEN** asynchronous decoding receives stream callbacks, cancels on its own mailbox, and only afterward the gateway returns the download hook
- **THEN** that hook is invoked and no network stream is left running solely because publication followed cancellation.

#### Scenario: Repeated or concurrent cancellation
- **WHEN** cancellation and hook publication overlap, or cancellation is requested repeatedly
- **THEN** the hook for that dispatched download is consumed once without a data race or duplicate invocation.

#### Scenario: Hook calls back into cancellation
- **WHEN** the cancellation hook synchronously reenters the cancellation path
- **THEN** it completes without deadlocking or invoking itself again.
