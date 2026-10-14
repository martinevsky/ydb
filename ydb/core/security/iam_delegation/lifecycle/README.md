# External secret coordination and recovery

SchemeShard stores the secret's ACL, path identity and immutable delegation
tablet reference. The independent tablet stores CREATE intents, full IAM
bindings, setup outcomes, active and pending operations, and the leased
revocation queue. No IAM operation runs inside a SchemeShard transaction.

## Configuration

Provision one `TTabletTypes::IamDelegation` tablet under a service-owned Hive
owner whose lifetime and storage survive tenant deletion. Configure its ID as
`IamConfig.DelegationTabletId`. Enable `EnableIamDelegationSecrets` to admit new
delegation secrets. Disabling that feature flag must not remove the tablet or
disable cleanup of already admitted work.

The IAM services also require the token manager and these `IamConfig` fields:
`SystemTokenName`, `TokenServiceEndpoint`, `ServiceControlEndpoint`, `ServiceId`,
`MicroserviceId`, and `ResourceType`. The configured system token name refers to
the token manager's credentials; no bearer token belongs in the delegation
tablet. `ResourceManagerEndpoint` enables discovery of a service account's cloud
when `RESOURCE` is omitted. The database's `cloud_id` attribute is the documented
fallback and produces a warning.

KQP registers the transport, token service and recovery worker on each configured
node. Multiple workers are supported: dispatch uses a tablet compare-and-swap,
and revocation acknowledgements carry generation, attempt, worker and deadline
fences. Existing bindings retain their original service identity for minting and
cleanup. Configuration changes do not rewrite that identity or migrate the
tablet. Keep recovery running for an old tablet until its inventory is drained.

## CREATE and ALTER

CREATE records the initiating cloud subject ID, verified user/group SIDs, a real
allocated schema transaction ID and the complete bounded schema intent before
submitting its normal authenticated schema request. Recovery uses that same
transaction ID and rechecks authorization through TxProxy. SchemeShard checks
the immutable database domain key and the saved parent path identity/version.
Binding requires the matching completed schema object and its full path ID.
Only a successful `StartSetup` response grants permission to submit IAM setup.
The coordinator records the IAM operation ID and terminal outcome before atomic
promotion. A worker can resume both a staged CREATE that has not reached
SchemeShard and an undispatched setup for a verified shell after the coordinator
disappears. Successful CREATE or DROP consumes the parent version, preventing a
delayed original request from recreating a dropped object. An unrelated sibling
change can also invalidate that version; the caller can retry after retirement.

ALTER keeps the existing active binding usable while its replacement is pending.
It checks authorization and a full path/version precondition through SchemeShard
before staging the replacement. Promotion and enqueueing the predecessor's
revocation happen in one tablet transaction. The ten-minute pending guard permits
detaching an uncertain replacement; it never proves that the remote setup failed.

DROP remains an ordinary schema operation. A repeated, bounded recovery scan
compares the saved binding with the actual schema object and retires missing or
replaced identities. Recovery reads the parent before the child and retires an
unbound CREATE only when the persisted schema fence proves no delayed copy can
create it. An absent path alone does not cancel a legacy intent without schema
metadata: the original request could still commit. Readers independently check both
tablet state and current schema identity/ACL, so they do not wait for this scan
to notice DROP or path reuse.

## Uncertain IAM outcomes

A saved IAM operation ID is recovered using `OperationService.Get`. Recovery
never repeats setup merely because a timer expired or an actor restarted.
If the first setup reply is lost before its operation ID is saved, the record
remains `SETUP_UNKNOWN` (or `SETUP_STARTED` at the dispatch crash boundary) and
stays visible in administrative inventory. It is not eligible for revocation
while the setup could still complete later.

The available IAM API does not establish a durable setup replay contract.
`x-request-id` is used for tracing; it is not treated as an idempotency key.
Consequently this implementation preserves uncertain cleanup responsibility but
cannot automatically resolve that first-reply loss without an authoritative IAM
lookup or replay contract. Persisting the subject ID permits recovery before
dispatch without retaining the user's original authentication token.

## Credentials

Secret readers authorize the schema path and join it to the tablet's active
binding by immutable identity. After minting they recheck tablet revision and
schema ACL/identity. Tokens are served only strictly before their usable deadline,
which leaves the required five minutes before IAM expiry.

Execution credentials retain the secret reference and verified user/group SIDs.
Each asynchronous acquisition resolves the current secret; concurrent acquisitions
may share that one resolution. S3, Solomon and SDK-backed consumers use the
asynchronous provider. A failed resolution cannot return an earlier token.
Authorization is checked when a consumer starts an external request. Transport
retries of that already authorized request can retain its token; the next new
request resolves the secret again. Issued IAM tokens cannot be recalled by DROP.

## Tests

The storage tests cover transaction replay, commit/reply loss, cold pages,
on-disk reopening, exact lease boundaries and seeded failure schedules. Worker
tests use the real tablet and controlled IAM/schema replies. KQP tests use real
SchemeShard and the independent tablet with fake IAM. They do not require a live
IAM endpoint. Test execution evidence belongs in the recovery report; the
presence of these suites alone is not evidence that a build passed.
