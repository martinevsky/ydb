# Classic event IAM delegation implementation plan

> For agentic workers: use executing-plans inline; preserve the behavior tests and review the complete branch independently.

Goal: provide a complete classic NActors equivalent of PR #53712 at f0689fbf5b9, without coroutine code or coroutine dependencies introduced by this feature.

Architecture: one node-local service owns token subscription, shared standard TBackoff and explicit request records. Request stages are scoped enums. Unique RPC cookies correlate replies; independent generation-fenced request timers drive deadlines and polling, with a separate timer and FIFO notifications for the shared recovery gate. There are no coroutine tasks or worker actors.

Spec: the component and test READMEs and the preserved IAM behavior suites define the observable contract. Preserve public events, configuration, identity, rights hints, redaction, startup behavior and mutation uncertainty. The latest explicit user request overrides the general coroutine preference for this analogue.

Constraints: no tablet or SchemeShard additions; no persisted/wire changes; single token subscription; mutations submitted at most once; only known-operation reads retry; re-read credentials after cooldown; exact absolute polling boundaries; stale replies/outcomes/timers harmless; graceful and forced shutdown release local state. Keep the original remote PR intact. Use a separate local branch and a local Git bundle checkpoint before tests because permission to push was limited to the original PR branch. Build with 100 jobs and earlyoom enabled; do not edit sources during builds.

Review focus: concurrent cookies and late replies; credentials changing during cooldown; deadline/reply ties; many expiring waiters with a surviving request; teardown with active requests and timers.

## Task 1: Shared event-driven backoff

Files: replace core/util/async_backoff.{h,cpp} and async_backoff_ut with shared_backoff.{h,cpp} and shared_backoff_ut; update core/util/ya.make.
Interface: TSharedBackoff::TryAcquire(now, reservation) returns optional generation; OnFailure(generation, now), OnSuccess(generation), GetVersion(), GetNextAllowedAt(). The owner schedules events and retries admission; the helper has no actor/coroutine callbacks.
- [x] Write deterministic tests for healthy concurrency, exact admission boundaries, stale outcomes, a missing probe outcome, early success releasing waiters and standard jitter/cap.
- [x] Run tests against the empty admission stub and observe failure.
- [x] Implement admission and fenced standard backoff; rerun the helper tests.

## Task 2: Classic IAM service

Files: iam_delegation_service.cpp; replace iam_actor_base with iam_errors (retain error classification); update owning build files and README. Preserve the existing IAM test bodies, except comments reflecting the new implementation.
Interface: unchanged CreateIamDelegationService(settings), public request/result messages and token-manager subscription. Internal records contain call kind, phase, caller/cookie, binding/subject, operation ID, deadlines, RPC cookie and backoff generation.
- [x] Add behavior tests for stale RPC replies during a later poll, concurrent deadline expiry with a survivor and forced teardown with pending RPC/backoff work. These should also pass for the reference implementation.
- [x] Implement classic dispatch, credential waiting, one shared backoff admission queue, bounded RPC response waiting, polling and shutdown.
- [x] Run all IAM and helper tests plus driver build. Preserve all existing assertions; diagnose differences against the reference.

## Task 3: Verification and comparison

- [x] Independently review the complete final delta against f0689fbf5b9, especially state ownership and test equivalence; address actionable findings with regressions.
- [x] Run relevant ASAN tests and the existing nine-node row-KV rolling upgrade/downgrade with this branch as current. Clearly distinguish disabled-feature cluster smoke from live IAM.
- [x] Verify no co_await/co_return/NActors::async/TAsyncEvent remains in the analogue's production or test changes; preserve config/event numbering and startup behavior.
- [x] Commit final work, retain local backup and give branch/commit, diff, test results and limitations. No publication of an alternative PR without an explicit request.

## Earlier validation

The initial conversion was validated at `4bd00d04ab0f2f90a96aa9446caafa961bdffaf2`. Subsequent differential testing found event-ordering differences, so those results did not establish complete equivalence. The final corrected revision and evidence are recorded below.

## Task 4: Restore event arbitration parity

The equivalence audit found that batching all overdue requests in one wake changes a queued successful response into a timeout after an unrelated polling wake.

Architecture correction: each request owns a cancellable, generation-fenced event for its current wait. A separate shared timer serves only IAM backoff admission; request timeout events never expire unrelated requests. Preserve the logical timer owner, FIFO notification order and same-turn owner handoff. Only actual health notifications or the owning gate wake resume suspended waiters; newly ready callers try admission directly. Preserve original response-first behavior for shorter credential/RPC waits and strict absolute attempt/poll deadlines. Keep standard shared backoff, typed phases and the public API. Restore original log messages and check the absolute attempt deadline before classifying unknown correlated replies.

- [x] Add deterministic queued-event tests for reply/timeout order, credential readiness behind an unrelated poll wake, retired request timers and wrong-type replies after the attempt deadline. Run unchanged assertions on broken classic and coroutine reference.
- [x] Implement per-request timers and further source-confirmed behavioral corrections. Reproduce the observable timing differences with failing regressions; directly verify allocation-free cleanup for the conditional allocation-failure finding.
- [x] Run complete IAM/helper suites, ASAN, driver/server and rolling validation; independently review the final comparison and distinguish executed evidence from remaining live-IAM/fault-injection limits.
- [x] Update component/test specifications, commit and retain a local bundle. No alternate branch publication.

## Final validation

Corrected implementation: `dc513bacf16d8ecdfc33b93ce4a0fa96667fca8a` on local branch `restore/pr53712-classic-events`. Subsequent documentation commits leave all 28 changed source, test and build-file blobs identical to this tested revision.

- Eight arbitration scenarios use captured scheduler events and explicitly queued replies/token updates. Five fail on the broken classic implementation; all eight pass unchanged on the coroutine reference and corrected classic implementation. Complementary timeout-first, retired-timer and owner-handoff cases also preserve the reference behavior.
- Final regular validation passed 65 IAM tests, seven shared-backoff tests and one nine-node row-KV rolling scenario. Driver, server and CLI builds passed. All nine upgrades and nine downgrades between stable-26-4-1 and the corrected implementation were verified.
- ASAN passed all 72 IAM/helper tests on the same revision. Link-time `--strip-debug` avoids the previously observed 32-bit DWARF relocation overflow while retaining sanitizer instrumentation. The linker emitted a nonblocking warning about the Python archive's `tsan.supp` member.
- Whole-branch review found a conditional allocation failure after sending a result that could duplicate the reply and lose a backoff-owner handoff. The notification queue now links existing request objects without allocation. Targeted independent review verified FIFO order, ownership and teardown against the actual intrusive-list implementation; the stale single-timer documentation was also corrected.
- No coroutine operations or direct coroutine dependency remain in the replacement owning targets. Public events, configuration and wire/persisted representations are unchanged; no tablet or SchemeShard changes were added by the analogue.

These checks establish parity for the reviewed contract and tested event sequences, not an exhaustive proof of all interleavings. Live IAM setup/revoke, cloud rights and production token rotation were not exercised. The rolling scenario keeps IAM delegation disabled, so it validates cluster integration rather than enabled-IAM mixed-version behavior. Recoverable allocation-failure injection was not executed. The rolling test passed with approximately 11.8 GiB usage against its unchanged 8 GiB harness estimate, without OOM.

Logs, immutable reviews, traces, reproducible commands, patches and local Git bundle checkpoints are preserved in `../pr53712-classic-validation/`. The original PR branch is preserved; this analogue has not been published.
