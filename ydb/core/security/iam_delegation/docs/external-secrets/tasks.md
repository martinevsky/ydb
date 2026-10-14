# Cross-component verification and delivery

Implementation and local regression tasks belong to their component OpenSpec changes:

- [`ydb/core/security/iam_delegation/tablet` tasks](../../tablet/openspec/changes/external-secrets-iam-delegation/tasks.md)
- [`ydb/core/kqp/executer_actor` tasks](../../../../kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/tasks.md)
- [`ydb/core/security/iam_delegation` tasks](../../openspec/changes/external-secrets-iam-delegation/tasks.md)
- [`ydb/services/scheme_secret` tasks](../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/tasks.md)
- [`ydb/library/yql/providers/s3/actors` tasks](../../../../../library/yql/providers/s3/actors/openspec/changes/external-secrets-iam-delegation/tasks.md)

## 1. Integration

- [x] 1.1 Run the combined checks affected by the final changes and build `ydb/core/driver_lib/run` when shared settings/wiring require it; record commands, exit codes, counts and the tested source revision.
- [x] 1.2 Review the complete fix range and all component contracts together; resolve substantiated findings and verify authorization, timing and cancellation remain compatible.
- [x] 1.3 Reconcile local task lists with final evidence, validate all component OpenSpec changes and preserve dereferenced results; verify pending implementation is never marked complete solely because a test exists.
- [ ] 1.4 Commit the reviewed fixes/specifications and push `restore/external-secrets-integration` to the user's fork; verify the remote branch SHA equals the delivered local commit.

## Workflow follow-up

- Keep a local change active while its implementation or validation tasks remain open. Synchronize and archive completed component changes through OpenSpec when requested.
- Live rollout includes service-owned tablet provisioning and IAM/control-plane validation; deterministic local tests do not establish undocumented remote-service guarantees.
