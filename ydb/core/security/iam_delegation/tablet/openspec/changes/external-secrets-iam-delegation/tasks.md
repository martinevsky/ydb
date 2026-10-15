# Tasks

Restored baseline: `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`. Existing test results below describe that revision, not later untested code.

## 1. Durable state and recovery

- [x] 1.1 Persist incarnations, intents, original bindings and outcomes independently of tenant schema; verify baseline tablet/lifecycle tests and the component README cover ownership and provisioning.
- [x] 1.2 Grant setup dispatch durably and atomically promote with predecessor cleanup; verify baseline commit/reply-loss and replay tests preserve all responsibility.
- [x] 1.3 Retain unknown outcomes and fence identity reuse and late replies; verify baseline pending-guard, unbound-CREATE and retired-outcome cases.
- [x] 1.4 Recover from cold pages, tablet restarts and reopening on-disk storage; verify baseline model-based chaos cases and the three successful recovery-suite repetitions.

## 2. Revocation and inventory

- [x] 2.1 Lease bounded claim batches atomically; verify baseline tests cover cold-page retries, batch commit/reply loss and invalid limits.
- [x] 2.2 Fence acknowledgements and exact completed replays; verify the microsecond-before, exact-deadline, takeover, generation and changed-replay cases.
- [x] 2.3 Export authorized incarnation-scoped inventory; verify known-empty, removed/reused database, stale continuation and full-page cases in the baseline tablet/public-API suites.
- [x] 2.4 Document observational inventory and uncertain setup; verify the specs and README do not promise admission fencing or undocumented IAM replay.

## 3. Specification placement

- [x] 3.1 Validate this local OpenSpec change and its links, and review the relocation; verify strict validation and preservation of every requirement/scenario from the reviewed specification.

## 4. Specification follow-up (not yet implemented)

- [ ] 4.1 Persist bounded terminal setup diagnostics, revocation failure history and original cleanup age; verify replay, commit/reply loss and disk reopening.
- [ ] 4.2 Use typed retry/pagination enums and support authenticated continuations with a durable signing key; verify tampering, identity reuse and mixed-version failure.
- [ ] 4.3 Keep tablet cleanup ownership through actual tenant deletion; replace external settlement/handoff scenarios with retained-ownership and uncertain-state tests.
- [ ] 4.4 Cover exact ACK replay after its original lease deadline and cold multi-record transitions; credit already implemented claim/pagination cases.
