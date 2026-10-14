# Delegated token acquisition design

## Context

Own shared platform-authenticated minting, refresh, strict token usability and the producer budget that secret readers must honor. The [shared architecture](../../../../iam_delegation_lifecycle/docs/external-secrets/design.md) records component boundaries, decisions, recovery limits, rollout and validation.

## Goals / Non-Goals

Implement the local capability contracts without expanding this component's authority. Durable formats and the independent-tablet ownership model remain unchanged by the review fixes. Other providers and undocumented IAM replay guarantees are outside scope.

## Decisions

Use the existing component and its real actor/tablet test harness. Keep interface obligations in one capability contract and link to them across component boundaries. Use the shared design for the rationale; do not maintain a second architecture description here.

## Risks / Trade-offs

Cross-component changes must preserve the shared authorization, identity, timing and cancellation contracts. A specification is an acceptance target; only recorded verification completes an implementation task.

## Validation

Local checks and their evidence are listed in `tasks.md`. Combined validation and fork delivery are tracked in the [integration checklist](../../../../iam_delegation_lifecycle/docs/external-secrets/tasks.md).
