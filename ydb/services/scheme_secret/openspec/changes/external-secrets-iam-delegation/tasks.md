# Tasks

Restored baseline: `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`. Reader timing is verified in `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`: focused reader 17/17 and complete secret service 44/44 passed, with independent task review approved. The producer/reader budget is specified by the token-service capability linked from the shared overview.

## 1. Resolution and credentials

- [x] 1.1 Validate each cached reference against its own immutable identity and SelectRow authorization; verify baseline reader tests cover multiple entries, cold reads, ACL changes, path reuse and replacement during minting.
- [x] 1.2 Acquire current credentials for new external requests without stale fallback; verify baseline reader, credential-provider and Solomon tests.
- [x] 1.3 Reproduce premature timeouts with real producer backoff and a missing token reply; verify RED5 reports 11 GOOD / 2 expected FAIL with no crash.
- [x] 1.4 Use the shared mint budget and phase-specific timers; verify long backoff succeeds while schema/tablet waits stay bounded and stale timers cannot expire a later phase.
- [x] 1.5 Cover missing and late replies, cancellation, terminal statuses and final token expiry; verify focused regressions and the full fast-reader suite pass.
- [x] 1.6 Update the shared design and record RED/GREEN evidence; verify an independent task review has no unresolved correctness issue.

## 2. Specification placement

- [x] 2.1 Validate this local OpenSpec change and its links, and review the relocation; verify strict validation and preservation of every requirement/scenario from the reviewed specification.
