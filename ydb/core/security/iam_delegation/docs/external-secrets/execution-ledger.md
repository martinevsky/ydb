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
