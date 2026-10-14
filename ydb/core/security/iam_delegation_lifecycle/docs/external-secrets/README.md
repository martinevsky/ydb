# External secrets / IAM delegation specifications

The contracts live in the source components that own them. Each component has an independent OpenSpec project; there is no repository-wide OpenSpec root. The shared [proposal](proposal.md), [design](design.md) and [integration checklist](tasks.md) live here beside the lifecycle workers. Requirements describe acceptance targets; checked tasks and recorded tests describe implementation status.

| Owning component | Contract |
| --- | --- |
| `ydb/core/tx/iam_delegation` | [iam-delegation-lifecycle](../../../../tx/iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md) |
| `ydb/core/tx/iam_delegation` | [iam-delegation-revocation](../../../../tx/iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md) |
| `ydb/core/tx/iam_delegation` | [iam-delegation-inventory](../../../../tx/iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md) |
| `ydb/core/kqp/executer_actor` | [external-secret-schema](../../../../kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md) |
| `ydb/core/security/iam_delegation` | [iam-delegated-token-acquisition](../../../iam_delegation/openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md) |
| `ydb/services/scheme_secret` | [external-secret-consumers](../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md) |
| `ydb/library/yql/providers/s3/actors` | [external-secret-s3-cancellation](../../../../../library/yql/providers/s3/actors/openspec/changes/external-secrets-iam-delegation/specs/external-secret-s3-cancellation/spec.md) |

Run these commands from each owning component listed above (once for the tablet's three contracts):

```bash
openspec status --change external-secrets-iam-delegation
openspec validate external-secrets-iam-delegation --strict --json --no-interactive
```

Each local change has its own proposal, design links and task list. Update only the owning contract when behavior changes, and use the shared checklist for cross-component verification and delivery. Archive completed local changes through OpenSpec so their contracts become that component's `openspec/specs/` inventory. Do not create a root-level spec copy or a symlink index.
