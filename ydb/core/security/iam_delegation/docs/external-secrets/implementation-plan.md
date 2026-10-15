# External secrets specification follow-up implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `executing-plans` to implement this plan task by task. Steps use checkbox syntax for tracking. Independent code reviews use `ydb-code-review`.

**Goal:** Close the confirmed implementation and regression gaps exposed by specification commit `8512b72d0a01cd1627e7fae2b56441446e853911`, while preserving the independent delegation tablet architecture.

**Architecture:** The service-owned tablet retains lifecycle and revocation responsibility after tenant deletion. SchemeShard owns schema, ACLs and immutable references. Coordinators and recovery workers perform IAM calls outside storage transactions; retries reuse standard `TBackoff`.

**Tech stack:** Existing YDB C++ actors, flat-executor transactions, additive protobuf fields, `TBackoff`, deterministic actor-runtime tests, on-disk reopen tests and existing chaos fixtures. C++20 or earlier.

**Spec:** Component contracts indexed by [README.md](README.md), updated under Task 1. Existing [design.md](design.md) and [verification.md](verification.md) remain the architecture and evidence records. This plan proposes changes; its unchecked tasks are not execution evidence.

## Global constraints

- Implement feature-owned asynchronous operations with `NActors::async`, `co_await` and `co_return`. Keep request context, expected identities/revisions, deadlines, awaited replies and temporary backoff in coroutine frames; keep shared caches/producer ownership shared and authoritative lifecycle state durable. Flat-executor transactions stay synchronous.
- Use scoped C++/protobuf enums for operation modes, outcomes, retry reasons, pagination capability and semantic cancellation actions. Adapt legacy wire Booleans only at transport boundaries; ordinary predicates remain Boolean.
- Keep implementation under `ydb/core/security/iam_delegation`, with its existing `tablet` and `lifecycle` subdirectories. Keep each consumer/DDL specification and test scenario beside its owning component. No root-level specification directory.
- Minimize SchemeShard changes; do not move lifecycle state, IAM operations or the revocation queue there.
- A schema reference alone never authorizes setup or token serving. Preserve the full identity, current ACL, operation-revision and database-incarnation checks.
- Preserve durable uncertainty. Timeout, tenant deletion, inventory listing and operator alerts never prove that an unknown setup failed or make it safe to revoke.
- Preserve atomic promotion/retirement, atomic revocation claims, exact committed-ACK replay and generation/attempt/worker/deadline fencing.
- Preserve strict token serving before `expiry - 5 minutes`, finite phase-specific waits, and cancellation of one reader without cancelling unrelated readers.
- Use `./ya make --build relwithdebinfo -tA`; the user explicitly requested 100 build jobs after the OOM recovery, overriding the earlier no-`-j` preference. Keep the host earlyoom service enabled with its existing configuration, as explicitly requested by the user. Use no additional agent memory guard or ya memory-limit override. Keep one link job and retry incomplete builds incrementally; use --keep-going to preserve independent progress after a compiler is terminated. No forced rebuilds or source edits during compilation. Preserve full logs and the actual process exit code.
- Commit and push each test/code checkpoint to the existing fork branch **before starting tests on that checkpoint**. Verify the remote SHA. This includes RED regression runs.
- Run implementation natively. If a separate build monitor is useful, use a cheaper model only for reading build output; keep technical review independent.

## Decisions used by this plan

These are the plan's proposed resolutions of the review's contract questions, not claims that the user previously chose each detail.

| Question | Planned behavior |
| --- | --- |
| Cleanup after database deletion | The independent tablet keeps responsibility until authoritative cleanup. Administrative inventory is observational. Remove mandatory external settlement/receipt/handoff requirements. |
| No-op ALTER with active A and pending B | Equality to active attributes is a no-op: authenticate the schema operation, preserve B and every lifecycle revision, dispatch no setup. A real binding change follows the existing replacement guard. This intentionally changes the current cancel-pending behavior. |
| Public page size zero | Keep the existing documented default of 100 for omitted/zero public `page_size`. The trusted tablet API continues to require 1..1000. No public presence/version migration. |
| Near-boundary token | Keep the one-second minimum pacing interval and strict serving cutoff. Explicitly allow a safe period without a usable token when less than one pacing interval remains; do not extend validity or spin. |
| Initial token demand | Keep immediate first mint for a cold caller. Use standard backoff for failures and standard jitter for subsequent healthy refreshes. Narrow the new initial-mint-dispersion promise so it does not introduce unnecessary first-request latency. |
| Operator thresholds | Expose suitable durable age/failure data and use externally configured thresholds. Do not add a second alert engine or assume all existing warnings are absent. |

## Review focus

- A disabled admission flag after Stage must not strand a committed CREATE or admit a fresh grant: Task 2 tests both sides of this boundary.
- Active A/pending B with a no-op ALTER must preserve the pending operation through every setup state: Task 3 covers the four relevant states.
- A worker-owned terminal IAM refusal must survive commit/reply loss and reopening, including old records without diagnostic fields: Task 4.
- New retry state must survive worker/tablet takeover without an old ACK applying twice or a pending remote operation being resubmitted: Tasks 5 and 9.
- Tenant/name reuse and token expiry during asynchronous work must never produce credentials or cleanup for a different identity: Tasks 8–10.

---

## Task 1: Reconcile the component contracts and evidence

**Files:**

- Modify the inventory, lifecycle and revocation specs under `ydb/core/security/iam_delegation/tablet/openspec/changes/external-secrets-iam-delegation/specs/`.
- Modify `ydb/core/security/iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md`.
- Modify `ydb/core/kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md`.
- Reconcile the corresponding component-local `proposal.md`, `design.md` and `tasks.md`, plus this directory's shared design/tasks/verification records.
- Correct the owning `ut/specs/external-secrets/spec.md` mappings in SchemeShard, KQP provider/executer, Solomon actors, security tablet/lifecycle/service and the affected consumers.

**Interfaces:** Produces the contract decisions above; changes no runtime interface.

- [x] Replace mandatory ownership transfer with retained-tablet cleanup after tenant deletion. Keep full saved binding/operation/state export, current administrative authorization and uncertainty warnings.
- [x] Document public zero/default pagination separately from tablet bounds, the minimum-pacing exception for short tokens, and immediate first mint versus dispersed background refresh.
- [x] Keep the no-op promise explicit, including preservation of pending work, and link its new scenarios to Task 3.
- [x] Credit existing invalid-claim, atomic cold-claim and full cold-pagination executable cases instead of marking them absent. Keep distinct missing transition/after-deadline tests planned.
- [x] Correct the SchemeShard 166-test provenance claim; check the copied Provider/Solomon claims against their actual commands. Fix the stored-value scenario's citation to a delegation-only test. Reconcile stale claims about which targets ran at `66189ce6` with the archived manifests.
- [x] Align the planned recovery-authorization test with IAM making the decision: assert saved subject forwarding and a known refusal's durable result, without assuming an unavailable local preflight or automatic resolution of an unknown operation.
- [x] Validate local OpenSpec changes with the repository's installed validator and reconcile requirement/scenario totals from the files; do not retain 59/101 after deleting or rewriting requirements unless those counts still match.
- [x] Commit and push the specification checkpoint. Documentation-only edits require validation, not a product-test rerun by themselves.

## Task 2: Make durable admission independent of later flag changes

**Files:**

- Modify `ydb/core/kqp/executer_actor/kqp_scheme_executer.cpp` only if needed to preserve the existing fresh-request guard.
- Modify `ydb/core/security/iam_delegation/tablet/tx_request.cpp` and relevant tablet test fixtures.
- Modify `ydb/core/tx/schemeshard/schemeshard__operation_create_secret.cpp` narrowly.
- Test `ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator_ut.cpp`, `ydb/core/security/iam_delegation/tablet/ut/iam_delegation_ut.cpp` and `ydb/core/tx/schemeshard/ut_secret/ut_secret.cpp`.

**Interfaces:** Preserve `TTxRequest::Stage(NIceDb::TNiceDb&, const TStageRequest&, ui64)` and the current request schema. Stage commit is the durable admission boundary. Exact saved Stage replay is allowed after disabling admission; a new operation ID is not.

- [ ] Add `AdmissionDisabledAfterStageStillRecoversCreate`, `DisabledAdmissionRejectsNewStageButAllowsExactReplay` and SQL-level disabled CREATE/ALTER/OR REPLACE cases. Hold original schema copies and replies, not only the coordinator actor.
- [ ] Commit/push and run the new regressions; establish behavior failures against the current source.
- [ ] Keep fresh admission gating in KQP and enforce it for a genuinely new tablet Stage after the exact-existing-intent replay branch. Do not gate outcome recording, promotion, DROP or revocation of admitted work.
- [ ] Remove the second current-admission check from creation of an already resolved immutable schema reference. Keep unresolved user IAM parameters rejected, complete-reference/value checks, authentication and all saved parent/database fences. A fabricated/missing lifecycle reference must never dispatch setup or return a value. Do not add SchemeShard lifecycle storage or tablet lookups.
- [ ] Cover a lost Stage reply, flag disable, delayed original CREATE plus worker replay, cold transaction retry and tablet restart. Verify one schema identity/setup owner and no orphaned grant.
- [ ] Commit/push, then run the focused cases and the full affected KQP, tablet and SchemeShard targets. Record the exact schema-shell versus grant-admission boundary in their specs.

## Task 3: Preserve pending work on a no-op ALTER

**Files:** Modify `ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.cpp`; test its existing `_ut.cpp`; update the KQP feature and test-owned scenarios.

**Interfaces:** Preserve `async<TGenericResult> Replace(TString, const TExistingSecret&, NKikimrSchemeOp::TSecretSchemaOp&)`. Its computed `needsSetup == false` branch performs no lifecycle mutation.

- [ ] Add `NoopAlterPreservesPendingNotStarted`, `NoopAlterPreservesPendingSucceeded` and young/expired UNKNOWN variants. Assert unchanged serialized secret/delegation records and revisions, zero added setup/revoke calls, and successful authenticated schema completion.
- [ ] Commit/push and run RED cases.
- [ ] Apply pending replacement guards only for a real binding change. After successful authenticated/fenced schema execution, return success for `!needsSetup` before `RetirePending`.
- [ ] Retain the ten-minute guard and supersession behavior for actual replacements. Test ACL denial and path recreation on the no-op branch as well as meaningful ALTER.
- [ ] Commit/push, run the coordinator suite, and record the intentional behavior change from cancellation to preservation.

## Task 4: Persist terminal setup refusal details

**Files:**

- Modify `ydb/core/security/iam_delegation/tablet/protos/iam_delegation.proto` and `tablet/tx_request.cpp`.
- Modify `ydb/core/security/iam_delegation/lifecycle/worker.cpp` and `ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.cpp`.
- Test the tablet durability/reopen fixtures, lifecycle `ut/worker_ut.cpp`, and the coordinator `_ut.cpp`.

**Interfaces:** Add a bounded `TSetupFailure` containing mapped YDB status and safe issue details; use `TDelegation.SetupFailure = 18` and `TSetSetupResultRequest.SetupFailure = 5`. Existing records without it remain readable. Existing outcome/operation-ID fields retain their meanings.

- [ ] Add `WorkerOwnedSetupRefusalReturnsOriginalStatus` with a held coordinator Stage reply, worker-owned refusal, persisted failure and coordinator reload. Add a restart before reload and a refusal returned through polling. Include legacy records with no diagnostic fields and oversized/invalid failure payloads.
- [ ] Commit/push and run the behavior regression.
- [ ] Persist the bounded terminal failure in the same transaction as SETUP_FAILED, from both coordinator and worker. Return durable diagnostics when the coordinator did not own the remote result. Keep the current generic fallback for legacy records with no details.
- [ ] Preserve terminal-result replay semantics: identical results replay safely; conflicting terminal outcomes/details must not overwrite an established result. Keep failed replacement cleanup from clearing a newer pending slot.
- [ ] Use existing issue conversion/redaction conventions; cap stored diagnostics and exclude bearer tokens, minted tokens and full raw IAM responses. Add oversized/credential-sentinel tests for the newly persisted surface.
- [ ] Commit/push; run service/lifecycle/tablet/coordinator targets, including reopen and lost-reply cases.

## Task 5: Use standard backoff for durable revocation retries

**Files:** Modify IAM-local `settings.h`, `lifecycle/worker.cpp`, tablet protobuf/`tx_request.cpp`, and lifecycle/tablet test files. No replacement backoff utility.

**Interfaces:**

- Add durable `TDelegation.RevokeConsecutiveFailures = 19` and `CleanupQueuedAtUs = 20`.
- Extend `TFinishRevocationRequest` with `RetryReason = 7`, an explicit retry classification: unspecified legacy behavior, retryable failure, or accepted/pending operation. Preserve legacy request defaults and exact serialized ACK receipts.
- Add IAM-local `TDuration RevocationRetryDelay(ui32 consecutiveFailures)` using standard `TBackoff`. Start with the existing one-second base and a one-minute base cap; standard positive jitter is additional. Keep this policy in shared IAM settings.

- [ ] Add failure growth/cap, pending-operation polling, independent record progress, restart/takeover and duplicate-ACK regressions. Observe persisted due times and real worker traffic rather than copy the helper algorithm into tests.
- [ ] Commit/push and establish RED behavior.
- [ ] Use the explicit outcome matrix: Succeeded acknowledges completion; Accepted records pending progress and resets the streak; Failed and Unknown count a failure. Preserve a known operation ID on Unknown and poll it on the next attempt. Only a definitive Failed result permits clearing that ID; absence of an ID alone never clears a saved one. Keep existing safe revoke retry semantics when no operation ID was obtained.
- [ ] Advance the durable failure streak once per committed failure ACK and preserve it across lease/tablet/worker changes. Legacy unspecified ACKs retain their supplied delay and must not reset new history. Deploy the tablet's additive fields before relying on new-worker durable backoff; test both old-shaped ACKs and new-shaped records. Preserve all existing lease fences.
- [ ] Reconstruct the capped standard backoff from durable state with bounded work, calculate the next due time, and persist it when finishing the lease. Do not sleep through backoff while holding a claim. Keep one-record worker claims.
- [ ] Reuse standard jitter for worker scan/dispatch pacing without reducing the existing one-second minimum rescan interval. Apply delays before claiming, not after consuming a lease.
- [ ] Set cleanup-queued time at the first transition into actionable cleanup; never replace it with the latest retry due time. Define legacy missing timestamps as unknown rather than fabricate precise age.
- [ ] Commit/push, run lifecycle/tablet targets, then repeat the relevant fault schedules. Verify old generations and expired unfinished claims still cannot ACK.

## Task 6: Disperse healthy refreshes and share timeout policy bounds

**Files:** Modify `iam_delegated_token_service.cpp`, `settings.h`, `iam_actor_base.cpp` under `ydb/core/security/iam_delegation`; test `ut/iam_delegated_token_actor_ut.cpp` and the real producer/reader cases under `ydb/services/scheme_secret/ut/fast`.

**Interfaces:** Keep the producer API and `TIamDelegationSettings::MintAttemptTimeout()`. Centralize token/RPC retry bounds in the IAM-local settings used by both the actual `TBackoff` constructors and the deterministic budget calculation. Proposed internal helper: `TDuration NextSuccessfulRefreshDelay(TDuration lifetime, TDuration usableRemaining, const TIamDelegationSettings&)`.

- [ ] Add `SuccessfulRefreshesAreDispersed`, `NearBoundaryRefreshRemainsPaced`, exact usable-deadline cases and a long-lived real producer/reader acquisition case after the original creator bearer expires; assert that minting uses current platform credentials, never that bearer.
- [ ] Commit/push and run the new cases against current timing behavior.
- [ ] Keep the first demanded mint immediate. For healthy background refresh, choose a standard-helper jittered delay in an earlier bounded window ending at the current half-life/hourly/usable target; positive jitter must not push beyond that target. For targets below one second, keep the one-second floor and documented safe availability gap.
- [ ] Continue using the existing error `TBackoff`, resetting after success. Do not back off healthy refreshes exponentially or extend token usability.
- [ ] Derive reader allowance from shared retry configuration and deterministic worst-case jitter/credential/RPC bounds, rather than retaining separately embedded 75/14-second assumptions. Do not sample random backoff to calculate a deadline. Preserve finite storage/schema phases and caller cancellation.
- [ ] Cover many entries/nodes with deterministic random/clock control, transient failure behind a usable cache, authoritative invalidation, idle eviction and waiter arrival on either side of eviction. Assert bounded windows, not exact random samples or strict growth of every jittered retry.
- [ ] Commit/push; run IAM service and fast secret-service targets, including the existing backoff-over-sixty-seconds reader regression.

## Task 7: Authenticate inventory continuations and expose cleanup health

**Files:**

- Modify tablet `schema.h`, `tablet.cpp`, `tablet.h`, protobuf and request transaction code; add tablet-owned `page_token.h/.cpp` and update its `ya.make`.
- Modify `ydb/core/grpc_services/rpc_list_database_iam_delegations.cpp`, `ydb/public/api/protos/ydb_cms.proto`, and their existing tests.
- Update component inventory/lifecycle/revocation specifications and shared operator documentation.

**Interfaces:**

- Versioned authenticated opaque continuation binds tablet, database identity/incarnation, inventory revision and exclusive cursor. Signing and verification stay inside the tablet; the RPC forwards it. Use `TListInventoryRequest.ContinuationToken = 5`, `DatabasePath = 6`, typed `PaginationMode = 7`, `TResponse.NextPageToken = 10` and typed `PaginationCapability = 11` without changing trusted internal structured-cursor clients. Use explicit enum values for legacy/structured/authenticated mode and unspecified/authenticated capability, never Boolean enable/support flags. Existing `DatabaseIncarnation` remains optional in authenticated continuations and is checked against the signed identity if explicitly supplied. The tablet must reject inconsistent explicit identity/cursor fields rather than trust unverified routing data.
- Store a tablet-owned signing key durably in a small new tablet table; do not expose it to gRPC handlers or logs. Use an existing HMAC-SHA256 implementation and constant-time verification.
- Extend public inventory records additively with staged time, cleanup-queued time and consecutive retryable failure data. Missing historical fields mean unknown/not recorded, not a fabricated zero-age claim.

- [ ] Add valid continuation/reboot/reopen, altered payload/tag, wrong tablet/incarnation, stale revision, malformed/oversized token and administrative-revocation-between-pages cases. Add full exactly-once public enumeration with multiple records for the same account: never deduplicate distinct referrers. Add omitted/zero/1000/1001 public page-size cases and retain the tablet's independent zero rejection.
- [ ] Commit/push before RED runs. For new token fields, keep the scaffold compilable and establish an observable integrity failure rather than count a compilation failure as the regression.
- [ ] Initialize/read the signing key before advertising the tablet active; cold reads and transaction retries must not publish partial startup state. Keep the key stable over restart and on-disk reopen. Bound token bytes and nested payload parsing before allocation; verify the authenticated payload before using its revision/cursor. If the RPC inspects routing fields, treat them as untrusted until tablet verification. A cursor-signing key is internal service state, not a caller bearer or minted token.
- [ ] Keep first-page path/incarnation discovery through the existing RPC registry flow. For a continuation, skip that discovery and forward requested path, optional explicit incarnation and opaque token directly to the tablet; it verifies the signature and derives the saved incarnation before listing. This must continue the original incarnation after path reuse even when the client omitted an explicit incarnation.
- [ ] Move public token generation/verification to the authenticated tablet mode and reject tampering. Require the response capability enum to confirm authenticated support even on an empty/final page: a new RPC talking to an old tablet fails closed with an availability/upgrade error, never falls back to unsigned mode. Trusted internal structured mode remains separate. Upgrade the tablet before new RPC handlers and complete frontend rollout before claiming cluster-wide enforcement; test both mixed-version directions.
- [ ] Reject old unsigned v1 public tokens with a clear restart-enumeration response. Ordinary public request/response schemas remain compatible and listing stays read-only with respect to delegation ownership.
- [ ] Project the existing staged timestamp and Task 5 cleanup fields into administrative inventory. Public enum/field documentation must explicitly state that uncertain setup cannot safely be revoked until an authoritative outcome, even when its saved operation ID is present. Document external age/failure thresholds and the existing mint-failure warning/counters. Add suitable aggregate counters only if the selected operator integration cannot use these existing/new surfaces; avoid per-account metric cardinality.
- [ ] Test a low-threshold observer against aged pending work and repeated cleanup failures, recovery/progress clearing the unhealthy condition, retained uncertain state, and mint warnings while a usable cache still serves.
- [ ] Commit/push, run tablet and feature-filtered gRPC targets, and verify pagination still uses bounded full pages and fresh authorization on every RPC.

## Task 8: Complete shared-reader, identity and consumer scenarios

**Files:** Refactor feature-owned asynchronous acquisition in `ydb/services/scheme_secret/iam_delegation_reader.cpp`, `secret_credentials.cpp` and relevant S3/Solomon credential waits as needed; extend existing tests and their test-owned specifications:

- `ydb/services/scheme_secret/ut/fast/{iam_delegation_reader_ut.cpp,service_ut.cpp,secret_credentials_ut.cpp}`.
- `ydb/core/kqp/finalize_script_service/ut/kqp_script_secrets_ut.cpp`.
- `ydb/library/yql/providers/s3/actors/ut/{yql_s3_read_actor_ut.cpp,download_cancellation_ut.cpp}` and S3 credential UTs.
- `ydb/library/yql/providers/solomon/actors/ut/dq_solomon_async_credentials_ut.cpp`.
- KQP provider and coordinator UTs for SQL/metadata/cloud-resolution coverage.

**Interfaces:** Preserve public consumer APIs with small adapters where callbacks are required. Internally use owned `NActors::async` acquisition coroutines and typed modes/outcomes. Request-local verified identity, revisions, deadlines and intermediate replies belong to coroutine frames; shared cache/provider ownership and cross-thread cancellation synchronization remain in their shared owners.

- [ ] Verify coroutine refactoring releases waits/subscriptions on cancellation and ignores late events without a second result. Preserve post-suspension identity/ACL checks and synchronous tablet transaction boundaries; implementation review must check that phase state is not duplicated in actor Boolean flags.
- [ ] Add two concurrent real readers sharing a producer: cancel one, allow the survivor to succeed, and assert one mint. Repeat with principals having different ACLs and with distinct references to the same account.
- [ ] Test SelectRow-only value access and DescribeSchema-only denied value access, ACL changes while mint is held, and stale schema/tablet timers arriving after token-phase transition.
- [ ] Extend real long-lived S3/Solomon/provider use to a later request after ALTER, ACL loss and token refresh; assert current resolution or failure, never fallback to stale credentials. Keep transport retries of an already authorized request distinct from a new request.
- [ ] Persist at least two secret pins, recreate only one path, and assert each path is checked against its own immutable identity. Credit the existing batch-cache version/reordering test rather than relabel it as a recreation test.
- [ ] Grant ordinary schema rights to a builtin identity and assert subject validation rejects setup before lifecycle writes. Verify caller credential forwarding for discovery, fallback warning severity, public chosen-cloud metadata, and retained cloud without discovery on ALTER. Cover configured discovery failure with no database cloud: actionable error, zero activation and no IAM setup.
- [ ] Test public metadata/EXPLAIN during IAM outage without mint/setup, and DROP committing during a revoke outage while cleanup remains durable for later recovery. Test retryable failure status/issues with an empty token cache and a token crossing its usable deadline while final identity/ACL validation is held.
- [ ] Add supported diagnostic-channel sentinel assertions without inventing a claim that the current implementation leaks credentials. Controlled sibling-referrer cases must preserve the account-scoped cache/API behavior; obtain authoritative IAM semantics or live integration evidence separately, without treating a mock as proof of remote authorization.
- [ ] Commit/push each test batch before its first run; execute focused cases followed by each affected owning target. Preserve the existing cancellation regression and its real cross-mailbox ordering.

## Task 9: Add exact boundary, restart and chaos coverage

**Files:** Extend tablet `ut/{revocation_boundaries_ut.cpp,cold_scan_ut.cpp,durability_ut.cpp,storage_reopen_ut.cpp,recovery_chaos_ut.cpp,lifecycle_recovery_ut.cpp}`, lifecycle `ut/worker_ut.cpp` and coordinator `_ut.cpp`.

**Interfaces:** Reuse existing fault hooks and independent state model; extend the model for diagnostics, failure streak and cleanup age. Do not replace it with assertions copied from production transitions.

- [ ] Test committed success and retry ACK replay after the old lease deadline in the same generation with no later claim. Test that a newer claim/generation still fences the replay.
- [ ] Test the original durable pending timestamp after restart at one microsecond before and exactly ten minutes. No-op behavior follows Task 3; a real replacement follows the guard.
- [ ] Add a cold multi-record promotion/drop transition case distinct from the existing atomic cold claim test. Assert no partial canonical/index writes and no duplicated effects after Execute retry.
- [ ] Exercise new diagnostic/backoff state at before-commit, after-commit-before-Complete, dropped response, worker death, tablet restart and fresh-runtime on-disk reopen boundaries.
- [ ] Extend seeded independent-model chaos with delayed setup outcomes, pending replacement, repeated revoke failure, identity reuse and lease takeover. Assert unresolved setup is never revoked, every successful retired binding remains accountable, and no old actor completes new work.
- [ ] Insert a database registration behind an active scan cursor and verify eventual discovery with the existing minimum pause and fairness for other databases.
- [ ] Commit/push before test execution. Repeat the targeted recovery schedules with recorded seeds; broaden repetition only for new failures or unresolved nondeterminism.

## Task 10: Test cleanup through actual tenant teardown

**Files:** Extend the existing real-tablet/SchemeShard integration fixture in `ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator_ut.cpp` and lifecycle integration tests; keep its scenario beside those tests.

**Interfaces:** Uses the already provisioned service-owned tablet and configured tablet ID. Does not introduce per-tenant tablet provisioning or ownership-transfer APIs.

- [ ] Create delegated secrets with active, pending and unknown outcomes; delete the tenant/database while preserving the service-owned delegation tablet, then continue recovery using the saved immutable bindings.
- [ ] Verify known successful retired bindings drain authoritatively, known operation IDs are polled, unknown first replies remain visible and unclaimed, and explicit-incarnation inventory remains available after schema removal.
- [ ] Recreate the same tenant/path names and ensure old cleanup cannot mutate new identities. Restart the service tablet and worker during cleanup and repeat with actual on-disk reopening.
- [ ] Commit/push before running. If the existing fixture cannot perform real tenant deletion, extend it and its owning target; do not rename a secret-DROP/runtime-restart case as tenant-teardown evidence.
- [ ] Document the remaining deployment prerequisite: production storage must actually be owned outside the managed tenant. Local tests do not prove production placement or live IAM/control-plane behavior.

## Task 11: Final review, evidence and fork delivery

**Files:** Update only affected component test-owned specs/tasks and shared [verification.md](verification.md) after real execution.

- [ ] Reconcile every requirement/scenario with implemented code and exact executable assertions. Keep any unexecuted scenario explicitly planned; record external IAM guarantees separately.
- [ ] Run `ydb-code-review` over the full implementation delta using independent Strict/Defender/Judge roles. Resolve substantiated findings and add regressions where needed; push each fix before rerunning its tests.
- [ ] Verify the final source snapshot with the affected targets listed below. Use sequential groups for large links; do not rerun unaffected broad suites solely to increase counts.
- [ ] Build `ydb/core/driver_lib/run` for changed shared settings, protobufs and wiring.
- [ ] Archive dereferenced result directories, commands, source manifests, real exit codes and chaos seeds. Keep the historical 407 GOOD record separate from the new evidence.
- [ ] Commit the final evidence/spec updates, push to the user's fork and verify remote branch SHA equals delivered HEAD. Do not merge or publish a GitHub review merely as a side effect of this plan.

## Commands and completion evidence

Run from the implementation worktree. For each checkpoint, inspect `git status`, stage only task-owned files, commit and push the existing `restore/external-secrets-integration` branch to its configured fork remote, then compare the remote branch SHA with `git rev-parse HEAD`. Resolve the remote name from existing configuration; do not assume `origin` points to the fork.

Each product test invocation uses this pattern, with a unique log for that task and RED/GREEN phase:

```bash
set -o pipefail
./ya make --build relwithdebinfo -tA ydb/core/security/iam_delegation/tablet/ut 2>&1 | tee /tmp/external-secrets-tablet-green.log | tail -n 80
```

For focused regressions append `-F '*SuiteName::CaseName*'` using the actual declared suite/case names. A RED run must fail for the asserted behavior. A GREEN result requires process exit 0 and a fresh result trace containing the expected case; intermediate OK output and prior symlinked results are insufficient.

| Target | When required |
| --- | --- |
| `ydb/core/security/iam_delegation/tablet/ut` | Admission, diagnostics, retry state, token integrity, storage/recovery changes |
| `ydb/core/security/iam_delegation/lifecycle/ut` | Worker dispatch, retry, discovery, uncertainty and teardown scenarios |
| `ydb/core/security/iam_delegation/ut` | Token/RPC policy, healthy refresh and diagnostics |
| `ydb/core/kqp/executer_actor/ut` | DDL orchestration, SQL admission, no-op and teardown |
| `ydb/core/tx/schemeshard/ut_secret` | Narrow schema-shell admission change and immutable-reference compatibility |
| `ydb/services/scheme_secret/ut/fast` | Real shared producer/reader, ACL, identity, cancellation and cache scenarios |
| `ydb/core/grpc_services/ut`, filter `*Iam*` | Administrative inventory, continuations, authorization and health fields |
| `ydb/core/kqp/finalize_script_service/ut`, filter `*Secret*` | Multiple persisted pins and recreated path |
| `ydb/core/kqp/provider/ut` | New/modified provider/metadata SQL cases |
| `ydb/library/yql/providers/s3/actors/ut` | Later-request re-resolution and preserved cancellation behavior |
| `ydb/library/yql/providers/s3/credentials/ut` | Credential integration changes/cases |
| `ydb/library/yql/providers/solomon/actors/ut` | Later-request refresh/ACL cases |

Build-only final command: `./ya make --build relwithdebinfo ydb/core/driver_lib/run`, with the same full-log/exit handling.

This plan does not include a new external cleanup handoff, unknown-operation replay guarantees, revocation of already issued tokens, terminal-record garbage collection or automatic tablet provisioning. Those are separate design/deployment work. Controlled tests can verify saved subjects and local transitions; live IAM authorization and sibling-referrer semantics still need authoritative external evidence.
