# Tasks

Restored baseline: `c68a9f3b2cafbdee3c6a3f870cb002166dda3dd7`. Selective-ALTER fix: `d1f1add797ecee712354057208f225dc68e92c11`.

## 1. Schema and authorization

- [x] 1.1 Restore source syntax, non-secret metadata and stored-secret compatibility; verify baseline schema, KQP provider and SQL suites, including metadata independent of IAM minting and rejected source conversion.
- [x] 1.2 Preserve authenticated execution and immutable fences before lifecycle/IAM effects; verify coordinator denial, replacement and path-reuse regressions.
- [x] 1.3 Use operation-specific metadata permission for ALTER and replacement; verify the two pre-fix selective-ACL failures and 31 focused / 119 full-target passing tests at the fix revision.
- [x] 1.4 Document the existence-only fallback and independent public read permissions; verify the approved task review covers denied callers, revoked ACLs and recreated paths.

## 2. Specification placement

- [x] 2.1 Validate this local OpenSpec change and its links, and review the relocation; verify strict validation and preservation of every requirement/scenario from the reviewed specification.
