# External secrets review fixes implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Fix review findings B1, C1 and C2, demonstrate each regression before its fix, and push the verified branch to the user's fork.

**Architecture:** Keep durable delegation state in the existing independent tablet. Correct DDL authorization in the coordinator, align reader lifetime with the token producer's next-attempt contract, and synchronize S3 cancellation with asynchronous download publication.

**Tech Stack:** Existing YDB C++ actors, coroutines, SchemeCache/TxProxy, tablet test runtime and ya tests; C++20 or earlier.

**Spec:** Component-local contracts are indexed in [README.md](README.md). User request to fix all three findings and cover them with regression tests; detailed accepted findings in `/home/pretender/ydbwork/external-secrets-recovery/reviews/2026-10-14-snsiu7ck/final.md` and `judged.json`. The preceding conversation describes the intended ALTER permission fix.

## Global constraints

- Work in the existing isolated `restore/external-secrets-integration` worktree; starting head `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`.
- No additional SchemeShard behavior or persistent format changes.
- Preserve authorization before IAM setup, authenticated schema operations, immutable identity/version fences, and the strictly greater than five minutes token lifetime requirement.
- Preserve ordinary stored secrets, CREATE OR REPLACE and ALTER IF EXISTS behavior.
- Use real components for the failing contracts; control external IAM/network responses at their boundaries. No production test-only controls.
- Build with `--build relwithdebinfo -tA`; no `-j`, forced rebuilds or source edits during compilation. Preserve full logs and pipeline exit status while showing output tails.
- Implementation tasks and builds run sequentially. Build monitors use a cheaper model. No permissions or additional confirmation are needed; fixing, testing, committing and pushing this branch were authorized.

## Review focus

- Selective ACLs, including explicit DescribeSchema denial: Task 1 must allow authorized ALTER without enabling token reads.
- ACL revocation and path reuse between metadata lookup and schema execution: Task 1 preserves rejection and absence of IAM setup.
- Producer backoff and stale reader timers: Task 2 must wait for a next mint without leaving nonresponsive dependencies unbounded.
- Permanent IAM rejection and exact usable-token deadlines: Task 2 preserves failure propagation and expiry checks.
- Cancellation before, during and after download dispatch, including callback reentrancy: Task 3 invokes the appropriate hook exactly once outside the synchronization lock.

### Task 1: Authorize ALTER metadata using its DDL permission

**Files:** `ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.cpp`, `kqp_iam_delegation_secret_orchestrator_ut.cpp`; existing executer UT build target.

**Interfaces:** Consume SchemeCache navigation with the authenticated user token, explicit `entry.Access`, `SyncVersion`, `Self` and `SecretInfo`; produce a coordinator-only authorized existing-secret lookup for ALTER/replacement. No public API change.

- [x] Add a regression using a non-owner allowed AlterSchema and explicitly denied DescribeSchema; execute ALTER and verify the active binding changes. Assert public metadata/value access remains denied without its own rights.
- [x] Run the new regression before changing production code and retain the expected authorization failure.
- [x] Replace the extra DescribeSchema checks with operation-specific authorization; obtain metadata and identity from the authorized lookup. Preserve missing-path/IF EXISTS and replacement behavior. Keep the authenticated schema request and identity fences before lifecycle mutation/setup.
- [x] Cover DescribeSchema-only denial, ACL revocation before schema execution, path replacement during resolution and CREATE OR REPLACE selective authorization; reuse meaningful existing coverage where it already exercises the exact contract. For masked existence, a bounded trusted existence-only probe may distinguish absence; discard its metadata and reject every existing result. Primary lifecycle metadata must come from the authenticated AlterSchema lookup.
- [x] Run the focused coordinator suite and all tests in its executable target; inspect actual test counts and failures. Commit only this task's files after green.
- [x] Independent task review; resolve concrete findings before continuing.

### Task 2: Let delegated reads await a complete next mint attempt

**Files:** `ydb/services/scheme_secret/iam_delegation_reader.cpp`, `ydb/services/scheme_secret/ut/fast/iam_delegation_reader_ut.cpp`; token service/settings and corresponding UT/build declarations only if needed to share the existing producer wait budget and exercise the real producer.

**Interfaces:** Consume `TEvGetToken`/result and its producer's bounded next-attempt lifetime. Preserve resolver default/caller retry policy and response status/issues. No durable schema change.

- [ ] Add a deterministic regression composing the real token producer and reader/resolver with controlled IAM failures and recovery. Reach a supported producer backoff longer than sixty seconds; a new default-policy acquisition must remain pending through the old two-reader failure point and succeed after the next mint. The shared producer must receive concurrent demand without duplicate loops.
- [ ] Run before production changes and retain the expected premature UNAVAILABLE failure.
- [ ] Replace the unconditional reader-wide thirty-second timeout with bounded handling that permits the producer's complete next-attempt budget. Derive any producer bound from one shared definition; keep tablet/schema waits bounded and ignore timers from earlier phases.
- [ ] Cover missing token-service reply, late replies/stale timers, terminal IAM status propagation, and unchanged exact usable-token boundaries. Keep externally imposed cancellation effective.
- [ ] Run affected secret service and token service suites. Commit only task files after green.
- [ ] Independent task review; resolve concrete findings before continuing.

### Task 3: Synchronize S3 cancel-hook publication

**Files:** `ydb/library/yql/providers/s3/actors/yql_s3_read_actor.cpp`, `ydb/library/yql/providers/s3/actors/ut/yql_s3_read_actor_ut.cpp`; a small internal production header and test/build declarations only if required to test the actual cancellation primitive.

**Interfaces:** Consume the HTTP gateway's returned `TCancelHook`. Publish and consume hooks under the same synchronization, with invocation outside the lock. Preserve the pre-dispatch cancellation guard and shared ownership during credentials acquisition.

- [ ] Add a deterministic streaming-read regression with AsyncDecoding enabled. Control the gateway interval after callbacks can be delivered but before Download returns its hook; trigger decoder cancellation and verify the returned hook is invoked. Do not rely on sleep-based race frequency.
- [ ] Run before production changes and retain the expected missed-cancellation failure.
- [ ] Synchronize Cancel and hook publication, including cancellation while Download is in progress. Invoke hooks outside the lock so callbacks cannot deadlock. Preserve cancellation before credentials resolve and normal streaming/retry behavior.
- [ ] Cover cancellation before publication, after publication, repeated cancellation and concurrent publication/cancellation; test the actual production path or primitive, not a copied model.
- [ ] Run the S3 actor suite and relevant credentials tests. Commit only task files after green.
- [ ] Independent task review; resolve concrete findings before continuing.

### Task 4: Integration verification and delivery

- [ ] Inspect the complete fixes diff against the starting head and reconcile every accepted finding with tests and commits.
- [ ] Run the affected coordinator, secret reader/service, token-service and S3 suites together; build the production driver if shared headers or wiring require it. Repeat only deterministic concurrency/timing regressions when their scheduling warrants it.
- [ ] Obtain one independent whole-fixes review; fix supported findings and rerun affected checks.
- [ ] Record actual red/green commands, test counts, limitations and final source hashes in the evidence directory. Update this plan's checkboxes.
- [ ] Push `restore/external-secrets-integration` to `git@github.com:martinevsky/ydb.git` without force and verify the remote SHA. Do not alter upstream draft PRs.
