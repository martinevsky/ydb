# Asynchronous credential ownership regression scenarios

## ADDED Requirements

### Requirement: Consumer cancellation releases pending request data
The credential wait SHALL release consumer-owned request payloads when its actor is destroyed, even while shared credential acquisition remains pending.

#### Scenario: Cancellation before the credential reply
- **WHEN** a consumer is destroyed while its credential future is pending
- **THEN** its request payload is released before the future completes
- **AND** a late credential reply does not resume that consumer

#### Scenario: One consumer leaves a shared acquisition
- **WHEN** two consumers await the same credential future and one is destroyed
- **THEN** the destroyed consumer releases its payload immediately
- **AND** the surviving consumer receives the credential exactly once

Executable coverage: `AsynchronousCredentialOwnership` in `async_credentials_ut.cpp`.

#### Scenario: Direct calls retain the consumer as completion owner
- **WHEN** another actor directly starts the consumer's credential wait
- **THEN** both already-ready and delayed credentials resume the consumer exactly once
- **AND** the calling actor receives no credential completion event

Executable coverage: `DirectCallCompletesOnConsumerInsteadOfCallingActor`.
