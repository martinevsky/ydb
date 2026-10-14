# Tasks

Restored baseline: `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`. Cross-mailbox hook synchronization is implemented in `c098e4ceb695` with fixture correction `2e7842af118`; full S3 suites pass 54/54. Independent task review approved specification compliance and quality with no correctness findings.

## 1. Cancellation during asynchronous dispatch

- [x] 1.1 Preserve current credential acquisition and cancellation before credentials resolve; verify the baseline S3 actor/credential tests.
- [x] 1.2 Reproduce decoder cancellation before hook publication using real asynchronous decoding and independently scheduled mailboxes; verify the pre-fix hook is missed without timing sleeps or a copied cancellation model.
- [x] 1.3 Synchronize hook publication and consumption, invoking callbacks outside the lock; verify before/after publication, repeated/concurrent cancellation and reentrant callbacks consume the hook once without deadlock.
- [x] 1.4 Preserve pre-dispatch cancellation and transport retries; verify the S3 actor and credential suites pass with finite waits and reliable cleanup in the streaming regression.
- [x] 1.5 Update the shared design and record evidence; verify an independent task review has no unresolved correctness issue.

## 2. Specification placement

- [x] 2.1 Validate this local OpenSpec change and its links, and review the relocation; verify strict validation and preservation of every requirement/scenario from the reviewed specification.
