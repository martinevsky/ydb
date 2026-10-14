# Tasks

This checklist describes the restored feature and the remaining review fixes. Checked restoration tasks refer to the tested baseline `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`; they do not claim that later changes were tested. The selective-ALTER fix is verified at `d1f1add797ecee712354057208f225dc68e92c11`. Pending work remains unchecked until its implementation and regression evidence are available.

## 1. Specify the restored feature

- [x] 1.1 Reconcile RFC 330, PR 53712 and the independent-tablet draft stack with the current source; verify that `proposal.md` identifies the source precedence, scope and explicit exclusions.
- [x] 1.2 Define the six capability contracts and their acceptance scenarios; verify that the specs cover authorization, immutable identity, uncertain setup, exact lease boundaries, token lifetime and cancellation.
- [x] 1.3 Record component ownership, recovery choices, operational limits and the implementation/test mapping in `design.md`; verify that pending fixes are distinguished from existing evidence.
- [x] 1.4 Validate and review the OpenSpec artifacts for coherent requirements and actionable tasks; verify `openspec validate external-secrets-iam-delegation --strict --json --no-interactive` passes and resolve any substantiated contract inconsistencies.

## 2. Preserve the schema surface and authorization

- [x] 2.1 Restore delegation-source SQL, metadata and ordinary stored-secret compatibility; verify the baseline schema, KQP provider and SQL translation suites pass, including unsupported source conversion and metadata without IAM minting.
- [x] 2.2 Keep authenticated schema operations and immutable identity/version fences before lifecycle mutation; verify baseline coordinator tests exercise setup denial, replacement and path reuse.
- [x] 2.3 Fix ALTER and CREATE OR REPLACE metadata access to use the operation's permission; verify the pre-fix selective-ACL failures and the post-fix 31 coordinator / 119 full-target passing cases at `d1f1add797e`, including denied callers, revoked rights and recreated paths.
- [x] 2.4 Document the operation-specific authorization and existence-only fallback in the schema spec and design; verify the fallback cannot supply lifecycle metadata or authorize a mutation in the task review.

## 3. Own durable lifecycle state in an independent tablet

- [x] 3.1 Persist database incarnations, CREATE intents, original bindings and setup outcomes outside SchemeShard; verify baseline tablet and lifecycle tests preserve identities across replay and tenant removal, and the tablet README explains provisioning and ownership.
- [x] 3.2 Grant setup dispatch ownership durably, preserve active bindings during replacement, and atomically promote with predecessor cleanup; verify baseline tests at transaction commit and response-loss boundaries.
- [x] 3.3 Preserve unknown setup outcomes, the pending guard and late-result isolation; verify baseline lifecycle tests retain unresolved responsibility and never promote or clear a newer pending operation from an old reply.
- [x] 3.4 Recover with database/parent/object fences and independent storage state; verify baseline identity-reuse, cold-page retry, fresh-runtime on-disk reopening and independent-model chaos cases, including the three successful recovery-suite repetitions.
- [x] 3.5 Document recovery semantics and the unresolved first-IAM-reply limit; verify the lifecycle spec, design and existing lifecycle README do not infer an IAM replay guarantee from request identifiers.

## 4. Lease revocations and expose administrative inventory

- [x] 4.1 Persist confirmed cleanup work and claim bounded batches atomically; verify baseline tests cover invalid limits, multi-record cold-page retries and claim commit/reply loss.
- [x] 4.2 Fence acknowledgements by generation, attempt, worker and deadline; verify baseline tests cover one microsecond before expiry, exact expiry, takeover, restart and exact committed acknowledgement replay.
- [x] 4.3 Keep unknown setup outside the claim queue and finish cleanup only on authoritative IAM outcomes; verify baseline lifecycle/worker tests and the revocation spec preserve the original binding across configuration changes.
- [x] 4.4 Expose authorized incarnation-scoped inventory with revision-bound pagination; verify baseline tablet and public API tests cover known-empty databases, deleted/reused paths, stale or mismatched continuations and full nonfinal pages.
- [x] 4.5 Document inventory as observational and require repeated database discovery scans; verify the inventory spec and tablet README preserve admission/outcome coordination as a control-plane responsibility.

## 5. Bound token acquisition by the real producer lifecycle

- [x] 5.1 Restore platform-authenticated minting, shared refresh and strict expiry-minus-five-minutes usability; verify baseline IAM tests cover boundary expiry, transient cache retention, terminal invalidation and concurrent waiters.
- [x] 5.2 Preserve per-secret read authorization and revalidation after asynchronous minting; verify baseline reader tests cover SelectRow-only reads, ACL changes, identity reuse and replacement during minting.
- [x] 5.3 Reproduce the reader timeout with the real producer, tablet and default resolver after actual transient failures cause backoff longer than sixty seconds; verify pre-fix evidence shows a premature reader result and that the harness reaches the intended mint cycle.
- [ ] 5.4 Share the producer's complete next-attempt budget and use phase-specific reader timers; verify the regression receives the next mint without thirty-second replacement readers while ordinary schema/tablet waits remain bounded.
- [ ] 5.5 Cover a missing token-service reply, stale phase timers, late replies, cancellation and terminal status propagation; verify focused reader regressions and the affected IAM and fast-reader suites pass on the final token fix.
- [ ] 5.6 Update the token spec/design with the implemented timing contract and record RED/GREEN evidence; verify an independent task review finds no unresolved correctness issue.

## 6. Resolve current consumer credentials and synchronize S3 cancellation

- [x] 6.1 Resolve each cached secret entry against its own path identity and acquire current credentials for new requests; verify baseline reader, S3 credential and Solomon tests cover separate identities, refresh and failure without stale fallback.
- [ ] 6.2 Reproduce decoder cancellation before download-hook publication using real asynchronous decoding and independently scheduled mailboxes; verify the pre-fix stream's hook is missed without relying on timing sleeps or a copied cancellation model.
- [ ] 6.3 Synchronize hook publication and consumption while invoking callbacks outside the lock; verify cancel-before-publication, cancel-after-publication, repeated/concurrent cancellation and reentrant callbacks consume the hook once without deadlock.
- [ ] 6.4 Preserve cancellation before credential completion and transport retry behavior; verify the affected S3 actor and credential suites pass and the real streaming regression completes with finite waits and cleanup.
- [ ] 6.5 Update the consumer spec/design with the implemented cancellation contract and evidence; verify an independent task review finds no unresolved correctness issue.

## 7. Verify and deliver the combined result

- [ ] 7.1 Run the integration checks affected by the combined changes and build the production driver library where shared service wiring/settings require it; record commands, exit codes, counts and the tested source revision without attributing baseline runs to new code.
- [ ] 7.2 Review the complete fix range and capability contracts together; resolve substantiated findings and verify the earlier selective-ACL, timing and cancellation contracts remain compatible.
- [ ] 7.3 Reconcile this checklist with final evidence, validate all OpenSpec artifacts again and preserve dereferenced test results; verify no pending implementation is marked complete solely because a test exists.
- [ ] 7.4 Commit the reviewed fixes and specifications and push `restore/external-secrets-integration` to the user's fork; verify the remote branch SHA equals the delivered local commit.

## Workflow follow-up

- Keep this change active while implementation or validation tasks remain open. Once complete, synchronize its capability specs and archive it through the OpenSpec workflow when requested.
- Operational rollout includes service-owned tablet provisioning and live IAM/control-plane validation; local deterministic tests do not establish undocumented remote-service guarantees.
