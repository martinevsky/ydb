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
Database registration SHALL retain immutable incarnation and external aliases independently of current schema existence. A known database with empty inventory MUST be distinguishable from an unknown database. Post-deletion or reused-path export MUST identify the intended incarnation explicitly.

#### Scenario: Reused database path
- **WHEN** an administrator requests cleanup information for a deleted database whose path now names another database
- **THEN** selecting the old incarnation returns only the old database's delegation records.

### Requirement: Administrative export is authorized
The public CMS inventory operation SHALL enforce administrative authorization and return non-secret binding and referrer fields sufficient to construct cleanup requests. Continuation tokens MUST be opaque and bound to the selected incarnation, inventory revision and cursor.

#### Scenario: Caller lacks administrative permission
- **WHEN** a caller without administrative authorization requests inventory
- **THEN** no delegation records or continuation token are disclosed.

#### Scenario: Continuation is used for another incarnation
- **WHEN** a continuation token is reused with a different database incarnation
- **THEN** the request is rejected without returning a mixed inventory.

### Requirement: Inventory pagination is revision-bound and bounded
Inventory pages SHALL accept sizes from 1 through 1000 and reject unsupported sizes. A continuation SHALL require the first page's revision; an intervening inventory mutation MUST return SNAPSHOT_EXPIRED and require a restart. Nonfinal pages MUST be full, with exclusive continuation cursors.

#### Scenario: Mutation between pages
- **WHEN** lifecycle or revocation work changes inventory after its first page
- **THEN** continuation using the earlier revision fails instead of combining old and new records.

#### Scenario: Consumer stops on a short page
- **WHEN** a caller enumerates an unchanged inventory and stops at the first short page
- **THEN** it has received every inventory record exactly once.

### Requirement: Database discovery tolerates concurrent registration
Recovery discovery SHALL use bounded pages of registered incarnations, with exclusive cursors and full nonfinal pages. Workers MUST repeat scans from the beginning because registration discovery is not an immutable snapshot.

#### Scenario: Registration appears behind the cursor
- **WHEN** a database is registered lexicographically before a worker's current cursor
- **THEN** a later complete scan discovers it without losing its cleanup work.

### Requirement: Listing is observational
Listing SHALL neither stop new setup, resolve unknown outcomes, revoke bindings nor acknowledge cleanup. A deletion coordinator MUST establish its admission and outcome boundaries before treating an export as a complete cleanup set. Secret cleanup requests MUST preserve the secret referrer type.

#### Scenario: Concurrent setup during export
- **WHEN** an administrator exports inventory while setup is still admitted
- **THEN** listing does not claim that the result alone is a complete or already-revoked cleanup set.
