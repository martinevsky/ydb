# External secret schema

## Purpose

Expose delegated service-account credentials through existing secret DDL and references while preserving schema authorization and stored-secret compatibility.

## ADDED Requirements

### Requirement: Delegation source syntax
The system SHALL accept `YC_IAM_DELEGATION` as a secret source, require `SERVICE_ACCOUNT_ID`, and accept optional `RESOURCE` as the cloud identifier. ALTER and CREATE OR REPLACE MUST reject changing an existing secret's source.

#### Scenario: Create a delegation secret
- **WHEN** an authorized cloud subject creates a secret with the delegation source, account and explicit resource
- **THEN** the secret uses that account and cloud binding after setup succeeds.

#### Scenario: Source conversion is rejected
- **WHEN** ALTER or CREATE OR REPLACE attempts to convert a stored-value secret into a delegation secret, or the reverse
- **THEN** the request fails and the existing secret remains unchanged.

### Requirement: Stored-secret compatibility
A secret created without SOURCE SHALL retain its existing stored-value behavior. Existing secret-reference syntax and the normal IF NOT EXISTS, OR REPLACE and IF EXISTS semantics SHALL remain available, subject to authorization and source compatibility. A satisfied IF NOT EXISTS or an ALTER that changes nothing SHALL remain a plain schema operation and MUST NOT dispatch IAM setup.

#### Scenario: Existing stored value
- **WHEN** an authorized consumer reads a secret created without SOURCE
- **THEN** it receives the stored value without delegation setup or token minting.

#### Scenario: Missing ALTER IF EXISTS
- **WHEN** ALTER IF EXISTS targets a genuinely absent secret
- **THEN** it completes without creating a secret or an IAM delegation.

#### Scenario: No-op statement dispatches nothing
- **WHEN** CREATE IF NOT EXISTS finds an existing secret, or ALTER changes no delegation attribute
- **THEN** the statement completes without any IAM setup dispatch or lifecycle mutation.

#### Scenario: No-op ALTER has a pending replacement
- **WHEN** an authorized ALTER equals the active account/cloud binding while a different replacement is pending
- **THEN** it completes after authenticated schema execution without retiring the pending operation, changing lifecycle revisions or dispatching IAM
- **AND** this applies to not-started, successful-awaiting-promotion and young or expired unknown pending setup.

### Requirement: DDL asynchronous state belongs to the coordinator coroutine
Schema/tablet/IAM coordination SHALL use `NActors::async` and `co_await`/`co_return`, keeping request context, fences, deadlines and intermediate replies in its coroutine frame. Create/alter operation modes and setup outcomes SHALL be typed enums rather than Boolean control parameters. Cancellation MUST disarm pending waits without losing already committed lifecycle work or dispatching setup again.

#### Scenario: Late response after coordinator cancellation
- **WHEN** a coordinator is cancelled with a schema or IAM reply outstanding
- **THEN** its coroutine cannot return a second result or resume an obsolete phase
- **AND** recovery uses the durable intent and original identities.

### Requirement: Operation-specific DDL authorization
ALTER of a delegation secret SHALL require AlterSchema and any rights needed for requested attributes, without adding DescribeSchema or SelectRow as prerequisites. CREATE OR REPLACE of an existing secret SHALL enforce both parent creation rights and target alteration rights. Authorization failure MUST precede lifecycle mutation or IAM setup.

#### Scenario: Authorized alteration without visibility or value access
- **WHEN** a non-owner has AlterSchema but is explicitly denied DescribeSchema and SelectRow
- **THEN** an otherwise valid ALTER can replace the active binding
- **AND** public metadata and secret-value requests remain denied.

#### Scenario: Hidden existing object
- **WHEN** a caller lacks alteration rights on an existing secret that is hidden by its ACL
- **THEN** ALTER IF EXISTS and CREATE OR REPLACE reject the request without lifecycle writes or IAM setup.

#### Scenario: Selective replacement rights
- **WHEN** a caller has only parent creation rights or only target alteration rights
- **THEN** CREATE OR REPLACE rejects the request
- **AND** a caller with both rights can replace the binding even when target DescribeSchema is denied.

### Requirement: Authorization and identity remain current
A schema-changing request SHALL be checked with the caller's identity and immutable object/version preconditions at execution. A metadata lookup MUST NOT authorize mutation of a later object that reuses the path.

#### Scenario: Permission revoked after lookup
- **WHEN** AlterSchema is revoked after metadata resolution but before schema execution
- **THEN** ALTER fails without staging a replacement or dispatching IAM setup.

#### Scenario: Path recreated during resolution
- **WHEN** the resolved secret is dropped and another object is created at the same path before execution
- **THEN** the delayed request fails and leaves the new object and the old delegation's cleanup responsibility intact.

### Requirement: Delegation uses the initiating cloud subject
CREATE and ALTER requiring IAM setup SHALL identify an authenticated cloud subject and request delegation on that subject's behalf. The platform credential MUST authenticate the service without substituting its own delegation authority. Local or builtin identities without a valid cloud subject MUST be rejected for setup.

#### Scenario: Caller cannot delegate the account
- **WHEN** IAM denies the initiating subject permission to delegate the account
- **THEN** the operation returns the denial and does not activate the requested binding.

#### Scenario: Builtin identity attempts setup
- **WHEN** a local or builtin database identity issues CREATE or ALTER with the delegation source
- **THEN** the request is rejected before any IAM setup dispatch.

### Requirement: Resource resolution is explicit or authorized
An explicit RESOURCE SHALL select the delegation cloud. When it is omitted, the system SHALL attempt account-cloud discovery with the caller's credentials, then use the database cloud attribute if necessary; any fallback use MUST be returned as a distinct warning issue and the chosen cloud MUST be recorded in the secret's public metadata. ALTER that omits RESOURCE SHALL retain the existing binding's cloud without re-derivation. Failure to determine a cloud MUST return an actionable error.

#### Scenario: Discovery unavailable with database fallback
- **WHEN** RESOURCE is omitted, account lookup fails, and the database has a cloud identifier
- **THEN** setup uses that identifier and reports that the fallback was used.

#### Scenario: No cloud can be determined
- **WHEN** RESOURCE is omitted, account-cloud discovery fails and the database has no cloud identifier
- **THEN** setup fails with an actionable error and no binding is activated.

#### Scenario: ALTER retains the recorded cloud
- **WHEN** ALTER changes the account but omits RESOURCE
- **THEN** the replacement binding keeps the existing binding's recorded cloud without re-derivation.

### Requirement: Setup fails for an account not delegable in its cloud
Setup MUST fail with the IAM error when the account is not delegable in the binding's chosen or recorded cloud, and MUST NOT activate such a binding.

#### Scenario: Account not delegable in the recorded cloud
- **WHEN** ALTER retains the existing binding's cloud and IAM denies delegation because the new account is not delegable there
- **THEN** setup fails and the current active binding is preserved.

#### Scenario: Recovery owns the terminal refusal
- **WHEN** a recovery worker persists the IAM refusal before the original statement resumes
- **THEN** the statement returns that recorded mapped status and credential-free issues rather than substituting a generic availability error.

### Requirement: Metadata is independent of token health
Public secret description SHALL expose the source and non-secret delegation metadata without returning a minted IAM token. Referencing an existing secret during object creation or query planning MUST NOT require successful minting. Compilation and explanation MUST NOT dispatch IAM setup.

#### Scenario: IAM is unavailable during metadata access
- **WHEN** an authorized caller describes a delegation secret or creates an object referencing it while IAM is unavailable
- **THEN** schema validation can succeed independently of minting
- **AND** a later value acquisition reports the relevant token error.

### Requirement: Admission and cleanup have separate lifetimes
New CREATE/replacement Stage SHALL require enabled admission and configured services. Exact Stage replay and completing or safely retiring an admitted intent, including its immutable schema shell, MUST remain possible after admission is disabled. Disabling admission MUST NOT erase durable state, block ordinary DROP or stop cleanup. A schema reference alone MUST NOT authorize setup or token serving.

#### Scenario: Disable feature after creation
- **WHEN** admission is disabled after a delegation secret exists
- **THEN** new admission is rejected while DROP and recovery of existing cleanup work remain possible.

#### Scenario: Admission is disabled between Stage and schema submission
- **WHEN** CREATE Stage commits, admission is disabled and the original or recovered schema submission subsequently runs
- **THEN** the admitted intent can complete or safely retire under its saved parent/database fences
- **AND** fresh CREATE and binding-changing replacement intents remain rejected.
