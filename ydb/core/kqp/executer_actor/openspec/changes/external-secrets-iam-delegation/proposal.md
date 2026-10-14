# Delegation secret DDL

## Why

Coordinate authenticated secret DDL, source compatibility, setup and immutable schema fences without adding DescribeSchema to ALTER authorization. The contract belongs beside the implementation so changes to this component can be reviewed with their acceptance criteria.

## What Changes

- Specify the restored behavior and remaining hardening work owned by this component.
- Preserve the existing authentication, immutable identity and recovery guarantees.
- Track verified work separately from pending regression fixes in `tasks.md`.

## Capabilities

### New Capabilities

- `external-secret-schema`: Coordinate authenticated secret DDL, source compatibility, setup and immutable schema fences without adding DescribeSchema to ALTER authorization.

### Modified Capabilities

None. This is the initial OpenSpec contract for this component.

## Impact

Coordinate authenticated secret DDL, source compatibility, setup and immutable schema fences without adding DescribeSchema to ALTER authorization. See the [shared feature proposal](../../../../../security/iam_delegation/docs/external-secrets/proposal.md) for RFC/draft provenance and exclusions, and the [shared design](../../../../../security/iam_delegation/docs/external-secrets/design.md) for interactions with other components.
