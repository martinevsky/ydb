# Classic event IAM delegation implementation plan

> For agentic workers: use executing-plans inline; preserve the behavior tests and review the complete branch independently.

Goal: provide a complete classic NActors equivalent of PR #53712 at f0689fbf5b9, without coroutine code or coroutine dependencies introduced by this feature.

Architecture: one node-local service owns token subscription, shared standard TBackoff and explicit request records. Request stages are scoped enums. Unique RPC cookies correlate replies; a cancellable, generation-fenced service timer drives ordered request deadlines, polling and the shared recovery gate. No worker actor or timer is required per suspended coroutine because there are no coroutines.

Spec: the current component README and existing 52 IAM behavior tests define the observable contract. Preserve public events, configuration, identity, rights hints, redaction, startup behavior and mutation uncertainty. The latest explicit user request overrides the general coroutine preference for this analogue.

Constraints: no tablet or SchemeShard additions; no persisted/wire changes; single token subscription; mutations submitted at most once; only known-operation reads retry; re-read credentials after cooldown; exact absolute polling boundaries; stale replies/outcomes/timers harmless; graceful and forced shutdown release local state. Keep the original remote PR intact. Use a separate local branch and a local Git bundle checkpoint before tests because permission to push was limited to the original PR branch. Build with 100 jobs and earlyoom enabled; do not edit sources during builds.

Review focus: concurrent cookies and late replies; credentials changing during cooldown; deadline/reply ties; many expiring waiters with a surviving request; teardown with active requests and timers.

## Task 1: Shared event-driven backoff

Files: replace core/util/async_backoff.{h,cpp} and async_backoff_ut with shared_backoff.{h,cpp} and shared_backoff_ut; update core/util/ya.make.
Interface: TSharedBackoff::TryAcquire(now, reservation) returns optional generation; OnFailure(generation, now), OnSuccess(generation), GetVersion(), GetNextAllowedAt(). The owner schedules events and retries admission; the helper has no actor/coroutine callbacks.
- [ ] Write deterministic tests for healthy concurrency, exact admission boundaries, stale outcomes, a missing probe outcome, early success releasing waiters and standard jitter/cap.
- [ ] Run tests against the empty admission stub and observe failure.
- [ ] Implement admission and fenced standard backoff; rerun the helper tests.

## Task 2: Classic IAM service

Files: iam_delegation_service.cpp; replace iam_actor_base with iam_errors (retain error classification); update owning build files and README. Preserve the existing IAM test bodies, except comments reflecting the new implementation.
Interface: unchanged CreateIamDelegationService(settings), public request/result messages and token-manager subscription. Internal records contain call kind, phase, caller/cookie, binding/subject, operation ID, deadlines, RPC cookie and backoff generation.
- [ ] Add behavior tests for stale RPC replies during a later poll, concurrent deadline expiry with a survivor and forced teardown with pending RPC/backoff work. These should also pass for the reference implementation.
- [ ] Implement classic dispatch, credential waiting, one shared backoff admission queue, bounded RPC response waiting, polling and shutdown.
- [ ] Run all IAM and helper tests plus driver build. Preserve all existing assertions; diagnose differences against the reference.

## Task 3: Verification and comparison

- [ ] Independently review the complete final delta against f0689fbf5b9, especially state ownership and test equivalence; address actionable findings with regressions.
- [ ] Run relevant ASAN tests and the existing nine-node row-KV rolling upgrade/downgrade with this branch as current. Clearly distinguish disabled-feature cluster smoke from live IAM.
- [ ] Verify no co_await/co_return/NActors::async/TAsyncEvent remains in the analogue's production or test changes; preserve config/event numbering and startup behavior.
- [ ] Commit final work, retain local backup and give branch/commit, diff, test results and limitations. No publication of an alternative PR without an explicit request.
