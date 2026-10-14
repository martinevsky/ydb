# External secret consumers

## Purpose

Use secret-backed credentials safely in long-running external operations, with current per-object authorization and reliable asynchronous cancellation.

## ADDED Requirements

### Requirement: Each secret reference retains its own identity
A multi-secret resolution SHALL validate each entry against its own immutable schema identity, version and delegation reference. A cache hit for one path MUST NOT authorize another path, even if both secrets share an account or token producer.

#### Scenario: One of several paths is recreated
- **WHEN** a consumer resolves several cached secret references and one path now names a different object
- **THEN** that entry is rejected or re-resolved against its new identity without borrowing another entry's authorization.

### Requirement: Value reads use their own permission
Delegated value acquisition SHALL enforce SelectRow on the actual current secret and MUST NOT add DescribeSchema as a prerequisite. Public metadata permissions MUST NOT grant value access or authorize token use.

#### Scenario: SelectRow-only cold read
- **WHEN** a caller has SelectRow but lacks DescribeSchema and no token is cached
- **THEN** an otherwise valid acquisition can mint and return a token after identity and read authorization checks.

#### Scenario: Metadata-only caller
- **WHEN** a caller can describe the secret but lacks SelectRow
- **THEN** it cannot receive a token through secret-value acquisition.

### Requirement: Mint completion is followed by revalidation
Before returning a minted token, acquisition SHALL revalidate the active delegation revision and the current schema identity and ACL. A drop, replacement or permission change during minting MUST prevent returning a token under the obsolete authorization. The final return MUST also recheck token usability.

#### Scenario: DROP while minting
- **WHEN** IAM responds after the secret was dropped or its path reused
- **THEN** acquisition does not return the token under the earlier secret identity.

#### Scenario: Replacement promoted during minting
- **WHEN** a replacement changes the active binding while a token for its predecessor is being minted
- **THEN** the acquisition re-resolves or fails without returning the predecessor as the current secret value.

### Requirement: Credentials are acquired for new external requests
Credential providers SHALL retain a secret reference and verified caller identity, and resolve current credentials for each new external request. Concurrent acquisitions can share one in-flight resolution. A failed resolution MUST NOT fall back to an earlier token. Retries of an already authorized transport request can retain that request's token.

#### Scenario: Account changes without restarting a query
- **WHEN** ALTER promotes another account while a long-running query remains active
- **THEN** its next new external request acquires credentials for the new binding.

#### Scenario: Authorization is lost after an earlier successful request
- **WHEN** a subsequent acquisition fails because the secret was dropped or read access was revoked
- **THEN** the new external request fails instead of using a previously cached provider result.

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
