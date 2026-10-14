# External secrets recovery implementation plan

> **For agentic workers:** Use executing-plans to implement the independent components below; preserve the shared interface contracts and run builds sequentially.

**Goal:** Restore RFC #330 external secrets using the independent IAM delegation tablet, including durable recovery and deterministic chaos tests.

**Architecture:** SchemeShard owns only an immutable external-secret reference, path identity and ACL. The restored upstream tablet owns intents, current/pending bindings, inventory and the leased revocation queue. Node services submit and resume IAM operations using the persisted binding; readers resolve the current tablet binding after schema authorization.

**Tech stack:** YDB actors, C++20, protobuf, NiceDb, ya make.

**Spec:** RFC #330 and user direction of 2026-10-13: minimize SchemeShard changes and use the upstream independent tablet. Recovery evidence is archived under `/home/pretender/ydbwork/external-secrets-recovery`.

## Constraints

- Preserve upstream drafts #55679–#55694 and independent reader fix #55687.
- Preserve PR #53712's token-manager integration and public events in core/base.
- SOURCE is `YC_IAM_DELEGATION`; ordinary stored secrets remain compatible.
- Never persist a user bearer token or assume x-request-id gives IAM idempotency.
- Unknown setup is not eligible for revocation until a terminal outcome is proven.
- Serve delegated tokens only while more than five minutes remain.
- Keep builds isolated from edits; deterministic event barriers and virtual time, no correctness sleeps.

## Review focus

- Lost first IAM reply: retain UNKNOWN when no operation ID can be recovered.
- Late setup completion after replacement: retire only that operation, preserve new pending/current.
- Path/database reuse: compare immutable identities before binding, promotion or reading.
- Lease expiry and reordered replies: stale workers cannot acknowledge newer claims.
- Token expiry or final read failure: execution must not retain usable-looking stale credentials.

## Tasks

- [x] Inventory all 47 fork branches and all 67 authored YDB PRs, including upstream drafts; inspect RFC and #53712 histories.
- [x] Restore the ten-PR tablet stack and per-entry reader identity fix in an isolated Git checkout.
- [x] Run the restored tablet and reader suites against the integrated implementation: tablet 87/87 and readers 36/36 passed.
- [x] Extend tablet recovery: bounded `ListDatabases`, CAS `RetirePending`, late retired setup outcomes. Add transaction-boundary, on-disk reopen and seeded schedule tests.
- [x] Restore services and token/cloud clients. Add submit/poll operation API with typed outcomes, saved full binding, strict token lifetime and deterministic actor tests.
- [x] Restore missing provider wiring. Persist `TIamDelegationSecretReference {TabletId, DatabaseIncarnation, CreateOperationId}` as the schema source; reject source changes. Test flag, validation, persistence and ordinary-secret compatibility.
- [x] Add tablet client and recovery worker. Consume the restored leased queue, persist remote operation IDs before resuming, reconcile schema identities, and retry safely after actor/tablet loss. Test real tablet with fake IAM and reordered/duplicate/lost responses.
- [x] Restore KQP CREATE/ALTER orchestration: authenticate before external lookup, allocate real schema transaction ID, stage durable intent, create/verify shell, bind, start setup, persist outcome and atomically promote. DROP uses ordinary schema operation and durable reconciliation.
- [x] Restore reader and execution credentials refresh against the authoritative tablet current binding; verify ACL/path identity before cache use. Add exact five-minute, ALTER, DROP and identity-reuse tests.
- [x] Restore safe public describe and inventory access; retain UNKNOWN inventory after tenant removal.
- [x] Build and run targeted storage, service, SchemeShard, provider, reader and KQP integration suites; replay deterministic chaos seeds and examine failures before claiming success.
- [x] Review the integrated diff, preserve recovery commits/artifacts and report exact test results and any externally blocked contract limitations.

## Validation

The final combined run exited 0 on 2026-10-13: 411 selected tests passed across 13 suites, and the production driver library built. The counts were tablet 87, IAM services 57, readers/credentials 36, SchemeShard secrets 34, KQP provider 21, KQP orchestrator 25, lifecycle/client 22, script finalizer 6, public API 20, SQL translation 53, S3 actors 24, S3 credentials 2 and Solomon actors 24.

A separate repeated recovery run exited 0: 119 cases passed on each of three attempts (357 executions). Each attempt included 72 tablet, 22 lifecycle/client and 25 orchestration cases. Coverage includes atomic cross-database revocation batches, exact lease expiry/due-time boundaries, all claim-token fields, commit/reply crashes, cold-page retries, whole-runtime on-disk reopening, identity reuse, and chaos seeds 1, 330 and 20261013. Tests use real delegation tablets and, for orchestration, real SchemeShard with controlled IAM responses; live IAM was not exercised.

Independent final review is complete. Its accepted transient-retry and error-propagation findings have failing-before/passing-after reader regressions. The final reader suite also covers SELECT_ROW-only cold reads and strict usable-token deadlines. Earlier fixture failures were corrected and verified: serialized admin token creation, local TxProxy routing, queued actor bootstrap/delivery and enum assertion printing.

Source hashes matched the combined-run snapshot through all repeated executions. Only this validation document changed afterward. Logs, per-test traces with timestamps and SHA-256 hashes, research provenance and source snapshots are retained under `/home/pretender/ydbwork/external-secrets-recovery`. The restoration is preserved on `restore/external-secrets-integration`; the original checkout and other user worktrees are retained.

### Reproduce the combined build and tests

```sh
set -o pipefail
./ya make --build relwithdebinfo -tA --keep-going --ignore-recurses \
  --link-threads=1 --test-threads=4 \
  ydb/core/tx/iam_delegation/ut \
  ydb/services/scheme_secret/ut/fast \
  ydb/core/security/iam_delegation/ut \
  ydb/core/security/iam_delegation_lifecycle/ut \
  ydb/core/tx/schemeshard/ut_secret \
  ydb/core/kqp/provider/ut \
  ydb/core/kqp/executer_actor/ut \
  ydb/core/kqp/finalize_script_service/ut \
  yql/essentials/sql/v1/translation/ut \
  ydb/core/grpc_services/ut \
  ydb/library/yql/providers/s3/actors/ut \
  ydb/library/yql/providers/s3/credentials/ut \
  ydb/library/yql/providers/solomon/actors/ut \
  ydb/core/driver_lib/run \
  -F '*Iam*' -F '*Secret*' -F '*Credentials*' -F '*S3*' -F '*Solomon*' \
  2>&1 | tail -100
```

### Repeat recovery and chaos cases

```sh
set -o pipefail
./ya make --build relwithdebinfo -tA --keep-going --ignore-recurses \
  --link-threads=1 --test-threads=4 --test-retries=3 \
  ydb/core/tx/iam_delegation/ut \
  ydb/core/security/iam_delegation_lifecycle/ut \
  ydb/core/kqp/executer_actor/ut \
  -F '*Recovery*' -F '*RevocationBoundaries*' -F '*Durability*' \
  -F '*StorageReopen*' -F '*ColdScans*' -F '*IamDelegationTabletClient*' \
  -F '*KqpIamDelegationSecretOrchestrator*' \
  2>&1 | tail -100
```

## Operational boundary

Provision and configure a service-owned delegation tablet whose lifetime survives tenant deletion; enabling the feature does not provision one. A saved IAM operation ID is polled after restart. A lost first setup reply without a saved ID remains UNKNOWN and visible in inventory: available IAM documentation does not establish an authoritative replay/lookup guarantee. Recovery never repeats that uncertain setup or revokes while it could still complete. The component READMEs describe configuration, cleanup responsibility and credential behavior.
