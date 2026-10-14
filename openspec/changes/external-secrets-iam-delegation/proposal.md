# Proposal

## Why

Long-running queries need current service-account credentials without persisting an IAM bearer or a permanent account key in a secret. The restored implementation spans several upstream drafts; a single specification is needed to preserve its authorization, durable cleanup and recovery guarantees while completing the remaining review fixes.

## What Changes

- Introduce `SOURCE = "YC_IAM_DELEGATION"` secrets through the existing SQL and secret-reference interfaces, with ordinary stored secrets retaining their behavior.
- Keep durable delegation intents, original IAM bindings, setup outcomes, inventory and the revocation queue in an independent tablet that survives tenant deletion. Keep SchemeShard changes limited to schema identity, ACLs, references and fencing.
- Authorize setup on behalf of the initiating cloud subject; persist no authentication bearers or minted tokens.
- Preserve the active delegation during replacement, and atomically promote the replacement while recording cleanup for its predecessor.
- Recover interrupted schema and IAM operations conservatively. An unknown setup without a saved IAM operation ID remains visible and cannot be treated as failed merely because time passed.
- Mint and refresh tokens in a shared node cache; revalidate each secret's current identity and read permissions on acquisition. Allow a complete bounded mint attempt without the reader expiring prematurely.
- Fence leased revocation acknowledgements, expose revision-bound administrative inventory, and preserve cleanup after DROP or database removal.
- Cover crash boundaries, storage reopening, cold-page retries, identity reuse, exact token/lease deadlines and cancellation during asynchronous S3 dispatch with regressions.

This change records both the restored feature and its remaining hardening work. A requirement is a target contract; checked tasks and recorded test runs establish implementation status. It does not claim instant revocation of already issued tokens or automatic resolution of an IAM outcome for which no authoritative recovery contract exists.

## Capabilities

### New Capabilities

- `external-secret-schema`: SQL behavior, DDL authorization, source metadata and compatibility.
- `iam-delegation-lifecycle`: durable intent ownership, identity fencing, replacement and uncertain-outcome recovery.
- `iam-delegation-revocation`: durable cleanup, atomic claims and exact lease/acknowledgement semantics.
- `iam-delegation-inventory`: surviving administrative inventory and bounded, consistent pagination.
- `iam-delegated-token-acquisition`: automatic minting, refresh, caching, bounded waits and token validity.
- `external-secret-consumers`: per-secret authorization, refreshable credentials and cancellation of asynchronous external requests.

### Modified Capabilities

None. This repository had no OpenSpec capability inventory before this change.

## Impact

The feature touches SQL translation and KQP coordination, secret schema/public APIs, the independent delegation tablet and recovery workers, IAM clients and token services, secret resolution, and S3/Solomon/SDK-backed consumers. Deployment requires a service-owned delegation tablet and configured IAM/token-manager services. Administrative cleanup callers must account for database incarnations and the `ydb.secret` referrer type; changes to ydbcp are outside this implementation.

The source inputs are [RFC 330](https://github.com/ydb-platform/ydb-rfc/pull/330), [implementation history in PR 53712](https://github.com/ydb-platform/ydb/pull/53712), and the independent-tablet draft stack through [PR 55694](https://github.com/ydb-platform/ydb/pull/55694). The user's later independent-tablet direction supersedes the older SchemeShard-owned lifecycle design. Other identity providers, Lockbox, and migration of replication/transfer consumers are outside this feature.
