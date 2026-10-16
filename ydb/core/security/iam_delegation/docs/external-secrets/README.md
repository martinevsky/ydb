# External secrets / IAM delegation specifications

The contracts live in the source components that own them. Each component has an independent OpenSpec project; there is no repository-wide OpenSpec root. The shared [proposal](proposal.md), [design](design.md) and [integration checklist](tasks.md) live here at the common IAM component root. Requirements describe acceptance targets; checked tasks and recorded tests describe implementation status.

| Owning component | Contract |
| --- | --- |
| `ydb/core/security/iam_delegation/tablet` | [iam-delegation-lifecycle](../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-lifecycle/spec.md) |
| `ydb/core/security/iam_delegation/tablet` | [iam-delegation-revocation](../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-revocation/spec.md) |
| `ydb/core/security/iam_delegation/tablet` | [iam-delegation-inventory](../../tablet/openspec/changes/external-secrets-iam-delegation/specs/iam-delegation-inventory/spec.md) |
| `ydb/core/kqp/executer_actor` | [external-secret-schema](../../../../kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md) |
| `ydb/core/security/iam_delegation` | [iam-delegated-token-acquisition](../../openspec/changes/external-secrets-iam-delegation/specs/iam-delegated-token-acquisition/spec.md) |
| `ydb/services/scheme_secret` | [external-secret-consumers](../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md) |
| `ydb/library/yql/providers/s3/actors` | [external-secret-s3-cancellation](../../../../../library/yql/providers/s3/actors/openspec/changes/external-secrets-iam-delegation/specs/external-secret-s3-cancellation/spec.md) |

Run these commands from each owning component listed above (once for the tablet's three contracts):

```bash
openspec status --change external-secrets-iam-delegation
openspec validate external-secrets-iam-delegation --strict --json --no-interactive
```

Each local change has its own proposal, design links and task list. Update only the owning contract when behavior changes, and use the shared checklist for cross-component verification and delivery. Archive completed local changes through OpenSpec so their contracts become that component's `openspec/specs/` inventory. Do not create a root-level spec copy or a symlink index.

The completed restoration and its baseline validation are recorded in the [recovery plan](2026-10-13-external-secrets-recovery.md). Follow-up fixes are tracked in the [review-fixes plan](2026-10-14-external-secrets-review-fixes.md).

## Test-owned scenarios

These specifications describe fixture coverage and exact case/source/command mappings. They link the authoritative contracts above without copying runtime requirements. A case name is coverage intent; execution evidence and source revisions belong to the Task5 report and final integration gate.

- [ydb/core/security/iam_delegation/ut](../../ut/specs/external-secrets/spec.md)
- [ydb/core/security/iam_delegation/lifecycle/ut](../../lifecycle/ut/specs/external-secrets/spec.md)
- [ydb/core/security/iam_delegation/tablet/ut](../../tablet/ut/specs/external-secrets/spec.md)
- [ydb/core/kqp/executer_actor/ut](../../../../kqp/executer_actor/ut/specs/external-secrets/spec.md)
- [ydb/core/kqp/provider/ut](../../../../kqp/provider/ut/specs/external-secrets/spec.md)
- [ydb/core/kqp/finalize_script_service/ut](../../../../kqp/finalize_script_service/ut/specs/external-secrets/spec.md)
- [ydb/core/grpc_services/ut](../../../../grpc_services/ut/specs/external-secrets/spec.md)
- [ydb/core/tx/schemeshard/ut_secret](../../../../tx/schemeshard/ut_secret/specs/external-secrets/spec.md)
- [ydb/services/scheme_secret/ut/fast](../../../../../services/scheme_secret/ut/fast/specs/external-secrets/spec.md)
- [ydb/library/yql/providers/common/token_accessor/client/ut](../../../../../library/yql/providers/common/token_accessor/client/ut/specs/external-secrets/spec.md)
- [ydb/library/yql/providers/s3/actors/ut](../../../../../library/yql/providers/s3/actors/ut/specs/external-secrets/spec.md)
- [ydb/library/yql/providers/s3/credentials/ut](../../../../../library/yql/providers/s3/credentials/ut/specs/external-secrets/spec.md)
- [ydb/library/yql/providers/solomon/actors/ut](../../../../../library/yql/providers/solomon/actors/ut/specs/external-secrets/spec.md)

Historical recovery and C1 runs used `ydb/core/security/iam_delegation_lifecycle` and `ydb/core/tx/iam_delegation`. Their external evidence remains unchanged. Current reproduction targets are `iam_delegation/ut`, `iam_delegation/lifecycle/ut` and `iam_delegation/tablet/ut` under `ydb/core/security`. The three libraries remain separate; consumer and driver verification follows in Task4.

The durable [verification record](verification.md) records source identity, exact commands/results, review closure and deployment validation limits.

## Inventory rollout and cleanup observation

Upgrade the independent IAM delegation tablet before enabling the new inventory
RPC handlers, then finish the frontend rollout before claiming cluster-wide token
integrity. New handlers require authenticated capability on every successful page,
including empty/final pages, and return UNAVAILABLE against an older tablet.
Existing trusted internal structured cursors remain supported by the new tablet.
Public unsigned `v1` tokens require restarting enumeration; public `v2` tokens are
opaque and bind the tablet, immutable database identity, revision and exclusive
cursor. A mutation between pages requires restarting the scan. Every RPC still
requires current cluster administrator authorization.

The signing key belongs to the service-owned tablet's durable storage and survives
restart and disk reopening. Preserve that storage through tenant deletion; local
tests cannot establish the production placement or live IAM control-plane guarantees.
A cursor does not transfer cleanup responsibility away from the tablet.

External monitoring can aggregate inventory without service-account label
cardinality. For example, alert when a recorded `cleanup_queued_at_us` is older
than an operator-selected age or `revoke_consecutive_failures` exceeds an
operator-selected count. `staged_at_us` measures intent age, while cleanup age
starts at the first actionable retirement. Missing historical fields mean unknown;
`DueAtUs` is scheduling state and must not substitute for either timestamp.
Clear an alert after authoritative completion or accepted progress according to
the selected policy. Uncertain setups stay visible with saved operation IDs, but
must not be revoked until their setup outcome is authoritative. Existing mint
failure warnings/counters remain relevant while a usable cached token is served.
