# Delegation secret DDL design

## Context

Coordinate authenticated secret DDL, source compatibility, setup and immutable schema fences without adding DescribeSchema to ALTER authorization. The [shared architecture](../../../../../security/iam_delegation/docs/external-secrets/design.md) records component boundaries, decisions, recovery limits, rollout and validation.

## Goals / Non-Goals

Implement the local capability contracts without expanding this component's authority. The independent-tablet ownership model remains unchanged. The follow-up adds compatible durable fields and keeps temporary asynchronous state in coroutine frames. Component tasks and the shared verification record distinguish tested behavior from remaining validation. Other providers and undocumented IAM replay guarantees are outside scope.

## Decisions

Use the existing component and its real actor/tablet test harness. Keep interface obligations in one capability contract and link to them across component boundaries. Use the shared design for the rationale; do not maintain a second architecture description here.

## Risks / Trade-offs

Cross-component changes must preserve the shared authorization, identity, timing and cancellation contracts. A specification is an acceptance target; only recorded verification completes an implementation task.

## Validation

Local checks and their evidence are listed in `tasks.md`. Combined validation and fork delivery are tracked in the [integration checklist](../../../../../security/iam_delegation/docs/external-secrets/tasks.md).

## Test-owned scenario specifications

- [ydb/core/kqp/executer_actor/ut](../../../ut/specs/external-secrets/spec.md)
- [ydb/core/kqp/provider/ut](../../../../provider/ut/specs/external-secrets/spec.md)
- [ydb/core/kqp/finalize_script_service/ut](../../../../finalize_script_service/ut/specs/external-secrets/spec.md)
- [ydb/core/grpc_services/ut](../../../../../grpc_services/ut/specs/external-secrets/spec.md)
- [ydb/core/tx/schemeshard/ut_secret](../../../../../tx/schemeshard/ut_secret/specs/external-secrets/spec.md)

These coverage documents map real fixtures and executable cases; pending scenarios and unverified revisions are explicitly labelled. Feature requirements remain in the capability contracts.
