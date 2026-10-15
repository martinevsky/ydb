# Execution ledger — plan: implementation-plan.md

Base: `8512b72d0a01cd1627e7fae2b56441446e853911`.

Ruling: Keep the execution ledger beside this plan instead of creating a root `.superpowers` workspace, following the user’s component-local placement requirement. Task briefs are the corresponding sections of the plan.

Ruling: User-required fork backup precedes every product-test checkpoint, including deliberately failing regression checkpoints. A pushed checkpoint is not a passing-test claim.

Pre-flight interfaces:

- Tasks 2–4 share Stage/replacement/setup outcomes: admission gates only new Stage; no-op ALTER bypasses pending mutation; terminal failure details are additive and replay fenced.
- Tasks 4–5 and 7 share the delegation protobuf: reserve fields 18 for failure details, 19 for retry history, 20 for cleanup age; inventory projects those fields without mutating ownership.
- Tasks 5–6 share settings and TBackoff: durable revocation history is separate from temporary token refresh policy; both use the standard helper.
- Tasks 7–10 share identity/recovery: authenticate continuations before resolving reused paths; retained tablet owns old incarnations through teardown; new outcome/age fields join the recovery model.
- Tasks 2–10 share coroutine rules: temporary request state stays in coroutine frames; transaction Execute/Complete remain synchronous; shared caches and cancellation synchronization retain shared owners.

Task 1: specification reconciliation complete. Seven feature specs (64 requirements, 110 scenarios), five strict native OpenSpec projects and twelve standalone test specs passed, with 225 links and 261 contract-name references checked. Artifacts: `/home/pretender/ydbwork/external-secrets-recovery/reviews/2026-10-15-spec-follow-up-9v72e6iy`. The reviewed exact diff hash was rechecked before this checkpoint; no runtime files changed and no product tests ran.

Task 2: in progress — add executable admission/replay regressions before changing production code.

Task 2 RED: `59d2b64a3ba`, tablet target filtered `*DisabledAdmissionRejectsNewStageButAllowsExactReplay*`, actual exit 10. New Stage incorrectly returned SUCCESS instead of PRECONDITION_FAILED after disablement. Build succeeded; behavior assertion failed. Full log and dereferenced results: `spec-implementation-2026-10-15/task2-tablet-red*` in the external recovery workspace.

Ruling: Prepare Task 3 coordinator regressions together with Task 2 before the coordinator RED build because they share the same expensive target. Production changes still follow observed behavior failures; each task retains separate assertions and evidence.

Task 3: regressions prepared for NOT_STARTED, SUCCEEDED, young UNKNOWN and expired UNKNOWN after tablet restart, checking exact lifecycle records and no extra IAM dispatch.

Tasks 2–3 RED: `facf8f483cf`, coordinator target filters `*Admission*`, `*NoopAlterPreserves*`, `*WorkerRecoversLostStageReply*`, actual exit 10 (6 FAIL, 1 CRASHED). The four no-op cases demonstrate rejection/mutation; the admitted CREATE is rejected by the SchemeShard flag gate; worker recovery reaches the fixture timeout/abort for the same gate. Dereferenced results and full output: `spec-implementation-2026-10-15/task2-3-kqp-red*`.

Task 2: Ruling: The new SQL-disabled regression also exposed an INTERNAL_ERROR from the gateway’s exceptional feature-flag future. Return a typed PRECONDITION_FAILED result with a matching issue code, following the neighboring gateway error convention. This is a narrow additional gateway fix; fresh admission stays disabled.

Tasks 2–3 implementation: gate only genuinely new Stage, preserve exact replay, remove the redundant SchemeShard gate on immutable references, preserve pending work on authenticated no-op ALTER. Extend existing ACL-revocation and path-recreation regressions to both binding-changing and no-op ALTER. SchemeShard retains only ordinary schema checks; no lifecycle lookup/state is added.

Tasks 2–3 validation interruption: environment restarts interrupted the first tablet GREEN attempts before an exit was recorded. A second attempt was intentionally stopped with SIGINT (exit 254) to investigate remote-cache access. None of these runs counts as verification.

Ruling: Use a per-invocation `--bazel-remote-base-uri` override with the address returned by the system resolver on this host. The bundled ya resolver returns IPv4 `158.160.147.211` (Network unreachable), while the system resolver returns DNS64 IPv6 `64:ff9b::9ea0:93d3`; the bundled HTTP client itself gets HTTP 200 from `/status` through that IPv6 address. Re-resolve before subsequent builds, retain logs, and do not alter global network/cache settings. This is a build-environment workaround, not a product change.
