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

### Requirement: Inventory pagination is revision-bound and bounded
Inventory pages SHALL accept sizes from 1 through 1000 and reject unsupported sizes. A continuation SHALL require the first page's revision; an intervening inventory mutation MUST return SNAPSHOT_EXPIRED and require a restart. Nonfinal pages MUST be full, with exclusive continuation cursors. During settlement, outcome recordings of pre-settlement work are the only permitted mutations; restarts converge because that set is finite.

#### Scenario: Unsupported page size
- **WHEN** a caller requests a page size of zero or greater than 1000
- **THEN** the request is rejected and no continuation token is issued.

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
Listing SHALL neither stop new setup, resolve unknown outcomes, revoke bindings nor acknowledge cleanup. A deletion coordinator MUST establish its admission and outcome boundaries before treating an export as a complete cleanup set. Secret cleanup requests MUST preserve the secret referrer type.

#### Scenario: Concurrent setup during export
- **WHEN** an administrator exports inventory while setup is still admitted
- **THEN** listing does not claim that the result alone is a complete or already-revoked cleanup set.

### Requirement: Deletion handoff exports the durable outbox
On database deletion, the complete durable set of existing delegation bindings for the incarnation SHALL be exportable to the deleting control plane, beyond the tenant's schema lifetime. Unresolved or uncertain setups SHALL be included as a distinct labelled class with their complete saved bindings. Exported records MUST suffice for the control plane to revoke every listed binding.

#### Scenario: Control plane takes over cleanup
- **WHEN** a database is being deleted and its admission and outstanding lifecycle and claim work are settled
- **THEN** the deleting control plane enumerates every existing delegation binding of that incarnation, including unresolved setups labelled with their state.

### Requirement: Uncertain exports carry their resolution constraint
Exported unresolved or uncertain setups SHALL include the saved IAM operation identifier when one exists. The label SHALL carry that revocation of this class is unsafe until an authoritative setup outcome is available, consistent with Unknown setup is not a failed setup.

#### Scenario: Uncertain setup is handed off
- **WHEN** an export includes an uncertain setup that has a saved IAM operation identifier
- **THEN** the identifier travels with the record and the control plane is told revocation is unsafe until the setup outcome is authoritative.

### Requirement: Deletion export requires a settled incarnation
A durable settling state SHALL be available for an incarnation: while it holds, admission is stopped and workers MUST NOT start new lifecycle transitions or claims for it, and in-flight claims are allowed to lapse; recording authoritative outcomes of work begun before settlement MAY continue and MUST NOT be treated as new work. Outstanding or uncertain work SHALL be included in the export with its state, not awaited. Only an export begun after settlement constitutes a deletion export.

#### Scenario: Export before settlement
- **WHEN** an export starts while admission or lifecycle work for the incarnation is still active
- **THEN** it makes no completeness claim and the coordinator MUST restart it after settlement.

### Requirement: Settling is authorized and reversible before handoff
Entering or lifting the settling state SHALL enforce the same platform-level administrative authorization as the handoff transition and record the requesting identity. While no handoff for the incarnation has committed, an authorized deletion coordinator SHALL be able to lift the settling state, resuming ordinary lifecycle and claim work; after a committed handoff the state is irreversible.

#### Scenario: Settling lifted before handoff
- **WHEN** a deletion is aborted after settlement and the authorized coordinator lifts the settling state before any handoff commits
- **THEN** admission, lifecycle transitions and claim work resume, and any export already made makes no completeness claim.

#### Scenario: Settling requested without administrative authorization
- **WHEN** a caller without platform-level administrative authorization requests entering or lifting the settling state
- **THEN** the request is rejected and the incarnation's lifecycle and cleanup work continue unchanged.

### Requirement: Handoff transfer is a distinct durable transition
Export SHALL NOT release cleanup responsibility; exported records remain durable cleanup work until revocation completes authoritatively. Responsibility SHALL transfer only through a distinct durable handoff transition after a completed export; until it commits, the tablet retains the obligation. Committing the handoff SHALL fence outstanding claims and exclude transferred records from subsequent claim selection.

#### Scenario: Export completes without handoff
- **WHEN** an export completes but the handoff transition has not committed
- **THEN** the tablet retains cleanup responsibility for every exported binding.

#### Scenario: Restarted export re-delivers records
- **WHEN** an export attempt restarts after an incomplete earlier attempt
- **THEN** records may be delivered again and the consuming control plane deduplicates them by referrer.

### Requirement: Handoff requires durable receipt and equal authorization
The export and the handoff transition SHALL each enforce the same platform-level administrative authorization as the administrative inventory operation on every request. The control plane SHALL initiate the handoff only after durably recording the complete export; the handoff request itself SHALL serve as the durable-receipt confirmation.

#### Scenario: Handoff without administrative authorization
- **WHEN** a caller without platform-level administrative authorization requests the handoff transition
- **THEN** it is rejected and cleanup responsibility stays with the tablet.

#### Scenario: Control plane loses an unpersisted export
- **WHEN** the control plane crashes before durably recording the export and never requests the handoff
- **THEN** the tablet retains cleanup responsibility and a later export re-delivers the records.

### Requirement: Handoff is bound to a live settlement and revision
A handoff SHALL commit only while the settling state holds, bound to its export's inventory revision; a mutation since that revision, or a lifted settling state, rejects an uncommitted handoff and requires a fresh export. An exact replay of an already committed handoff SHALL return its recorded outcome even after that revision is superseded.

#### Scenario: Handoff based on a stale export
- **WHEN** the settling state was lifted or inventory mutated after an export completed
- **THEN** a handoff referencing that export is rejected and a fresh export is required.

#### Scenario: Handoff reply is lost after commit
- **WHEN** a handoff commits and its response is lost, and the control plane sends the identical request again
- **THEN** it observes the recorded transfer instead of a staleness rejection.
