# Tasks

Restored baseline: `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`. Cross-mailbox hook synchronization remains pending until its implementation, regression evidence and review are complete.

## 1. Cancellation during asynchronous dispatch

- [x] 1.1 Preserve current credential acquisition and cancellation before credentials resolve; verify the baseline S3 actor/credential tests.
- [ ] 1.2 Reproduce decoder cancellation before hook publication using real asynchronous decoding and independently scheduled mailboxes; verify the pre-fix hook is missed without timing sleeps or a copied cancellation model.
- [ ] 1.3 Synchronize hook publication and consumption, invoking callbacks outside the lock; verify before/after publication, repeated/concurrent cancellation and reentrant callbacks consume the hook once without deadlock.
- [ ] 1.4 Preserve pre-dispatch cancellation and transport retries; verify the S3 actor and credential suites pass with finite waits and reliable cleanup in the streaming regression.
- [ ] 1.5 Update the shared design and record evidence; verify an independent task review has no unresolved correctness issue.

## 2. Specification placement

- [x] 2.1 Validate this local OpenSpec change and its links, and review the relocation; verify strict validation and preservation of every requirement/scenario from the reviewed specification.
