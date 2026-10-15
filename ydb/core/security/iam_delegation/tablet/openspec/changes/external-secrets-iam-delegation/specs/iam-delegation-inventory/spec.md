# IAM delegation inventory

## Purpose

Let recovery workers and authorized administrators enumerate all accountable delegation identities, including work whose database or secret no longer exists.

## ADDED Requirements

### Requirement: Inventory is authoritative beyond schema lifetime
Inventory SHALL include active, pending, unknown-setup and revoking delegation identities, including identities whose schema object disappeared. It MUST NOT be reconstructed only from live schema paths or deduplicated by service-account identifier.

#### Scenario: Multiple delegations use one account
- **WHEN** several secrets or replacements refer to the same service account
- **THEN** inventory retains each distinct referrer and its original complete binding.

#### Scenario: Unactivated CREATE shell remains recoverable
- **WHEN** a live secret shell has no active binding and its creating operation is failed or retired
- **THEN** the shell retains a recovery inventory entry until it is dropped or a replacement is promoted.

### Requirement: Database incarnations are explicit
Database registration SHALL retain immutable incarnation and external aliases independently of current schema existence. A known database with empty inventory MUST be distinguishable from an unknown database. A fresh post-deletion or reused-path request MUST identify the intended incarnation explicitly; an authenticated continuation SHALL retain the incarnation selected by its first page.

#### Scenario: Reused database path
- **WHEN** an administrator requests cleanup information for a deleted database whose path now names another database
- **THEN** selecting the old incarnation returns only the old database's delegation records.

### Requirement: Administrative export is authorized
The public CMS inventory operation SHALL enforce platform-level administrative authorization over the requested incarnation on every page request, including continuations, and return non-secret binding and referrer fields sufficient to construct cleanup requests. Continuation tokens MUST be opaque, integrity-protected and bound to the selected incarnation, inventory revision and cursor; a continuation token alone MUST NOT confer authority.

#### Scenario: Caller lacks administrative permission
- **WHEN** a caller without administrative authorization requests inventory
- **THEN** no delegation records or continuation token are disclosed.

#### Scenario: Continuation is used for another incarnation
- **WHEN** a continuation token is reused with a different database incarnation
- **THEN** the request is rejected without returning a mixed inventory.

#### Scenario: Authorization is revoked between pages
- **WHEN** administrative authorization is revoked after the first page
- **THEN** continuation requests are rejected despite a valid continuation token.

#### Scenario: Continuation payload is modified
- **WHEN** a caller changes a continuation's identity, revision, cursor or integrity tag
- **THEN** verification rejects it without returning inventory under the altered fields.

#### Scenario: Database path is reused between pages
- **WHEN** a valid continuation selects an old incarnation and its database path is reused before the next page
- **THEN** the authenticated token selects the original incarnation before fresh path discovery
- **AND** a conflicting explicit incarnation is rejected.

### Requirement: Authenticated pagination has an explicit protocol mode
Authenticated public pagination and trusted internal structured-cursor pagination SHALL use distinct typed protocol modes and an explicit capability enum. The public handler SHALL require authenticated-mode support even for an empty or final page and MUST NOT silently downgrade to an unsigned cursor. Signing state SHALL survive tablet restart and reopening storage. Each public request SHALL recheck administrative authorization before accessing tablet state.

#### Scenario: Tablet lacks authenticated pagination support
- **WHEN** a public handler requests authenticated pagination from a tablet that does not support that mode
- **THEN** the request fails with a compatibility or availability error instead of issuing or accepting an unsigned continuation.

### Requirement: Inventory pagination is revision-bound and bounded
The trusted tablet API SHALL accept page sizes from 1 through 1000 and reject unsupported sizes. The public API SHALL preserve its documented default of 100 for omitted or zero page size and reject values greater than 1000. A continuation SHALL require the first page's revision; an intervening inventory mutation MUST return SNAPSHOT_EXPIRED and require a restart. Nonfinal pages MUST be full, with exclusive continuation cursors. Listing does not freeze concurrent lifecycle or cleanup work.

#### Scenario: Unsupported page size
- **WHEN** a tablet caller requests a page size of zero or greater than 1000, or a public caller requests greater than 1000
- **THEN** the request is rejected and no continuation token is issued.

#### Scenario: Public page size uses the default
- **WHEN** a public caller omits page size or supplies zero
- **THEN** the request uses page size 100.

#### Scenario: Mutation between pages
- **WHEN** lifecycle or revocation work changes inventory after its first page
- **THEN** continuation using the earlier revision fails instead of combining old and new records.

#### Scenario: Consumer stops on a short page
- **WHEN** a caller enumerates an unchanged inventory and stops at the first short page
- **THEN** it has received every inventory record exactly once.

### Requirement: Database discovery tolerates concurrent registration
Recovery discovery SHALL use bounded pages of registered incarnations, with exclusive cursors and full nonfinal pages. Workers MUST repeat scans from the beginning because registration discovery is not an immutable snapshot. Discovery rescans SHALL be paced by a minimum interval between the completion of one full scan and the start of the next.

#### Scenario: Registration appears behind the cursor
- **WHEN** a database is registered lexicographically before a worker's current cursor
- **THEN** a later complete scan discovers it without losing its cleanup work.

#### Scenario: Rescans are paced
- **WHEN** a full discovery scan completes
- **THEN** the next scan does not begin immediately.

### Requirement: Listing is observational
Listing SHALL neither stop new setup, resolve unknown outcomes, revoke bindings nor acknowledge or transfer cleanup responsibility. The independent tablet SHALL remain responsible for cleanup during and after enumeration. Secret cleanup requests MUST preserve the secret referrer type.

#### Scenario: Concurrent setup during export
- **WHEN** an administrator exports inventory while setup is still admitted
- **THEN** listing does not claim that the result alone is a complete or already-revoked cleanup set.

### Requirement: Tablet cleanup ownership survives database deletion
Database deletion SHALL NOT depend on an external settlement, receipt or cleanup-ownership transfer protocol. The service-owned independent tablet SHALL retain the incarnation's original bindings, outcomes and cleanup obligations after tenant schema removal. Recovery SHALL continue confirmed cleanup using the saved bindings, and authorized inventory SHALL remain available for the retained incarnation.

#### Scenario: Tenant disappears with cleanup outstanding
- **WHEN** a database is deleted while confirmed cleanup and uncertain setup remain
- **THEN** recovery can finish confirmed cleanup from the saved bindings
- **AND** uncertain setup remains visible and outside revocation claims until an authoritative outcome is available.

#### Scenario: Inventory response is lost during deletion
- **WHEN** an administrator loses or repeats an inventory response after tenant deletion
- **THEN** cleanup responsibility remains with the tablet and no binding is forgotten or transferred by listing.

### Requirement: Uncertain exports carry their resolution constraint
Exported unresolved or uncertain setups SHALL include the saved IAM operation identifier when one exists. The label SHALL carry that revocation of this class is unsafe until an authoritative setup outcome is available, consistent with Unknown setup is not a failed setup.

#### Scenario: Uncertain setup is listed after deletion
- **WHEN** an export includes an uncertain setup that has a saved IAM operation identifier
- **THEN** the identifier travels with the record and its state explicitly warns that revocation is unsafe until the setup outcome is authoritative.
