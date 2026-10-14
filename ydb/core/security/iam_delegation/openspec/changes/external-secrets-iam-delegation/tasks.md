# Tasks

Restored baseline: `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`. The complete-attempt timing contract is a pending fix until GREEN evidence and review are recorded.

## 1. Minting and caching

- [x] 1.1 Restore platform-authenticated shared minting and proactive refresh; verify baseline IAM tests cover concurrent waiters, transient retention, terminal invalidation and strict expiry-minus-five-minutes boundaries.
- [x] 1.2 Reproduce the integration timeout through the real producer, tablet and default resolver after backoff exceeds sixty seconds; verify the pre-fix run exits 10 with the expected premature reader timeout and bounded-wait failures.
- [ ] 1.3 Expose the producer's existing complete next-attempt budget as one shared definition consumed by the reader; verify the producer behavior remains unchanged and the real-composition regression receives the next result without replacement readers.
- [ ] 1.4 Run the affected IAM suite and review the shared timing contract; verify terminal statuses, token usability and waiter lifetime remain correct and record the tested source revision.
- [ ] 1.5 Reconcile the token spec/shared design with implementation and evidence; verify the task review has no unresolved correctness issue.

## 2. Specification placement

- [x] 2.1 Validate this local OpenSpec change and its links, and review the relocation; verify strict validation and preservation of every requirement/scenario from the reviewed specification.
