# Secret resolution and consumers

## Why

Validate each secret reference independently and provide current credentials after schema, delegation and lifetime revalidation. The contract belongs beside the implementation so changes to this component can be reviewed with their acceptance criteria.

## What Changes

- Specify the restored behavior and remaining hardening work owned by this component.
- Preserve the existing authentication, immutable identity and recovery guarantees.
- Track verified work separately from pending regression fixes in `tasks.md`.

## Capabilities

### New Capabilities

- `external-secret-consumers`: Validate each secret reference independently and provide current credentials after schema, delegation and lifetime revalidation.

### Modified Capabilities

None. This is the initial OpenSpec contract for this component.

## Impact

Validate each secret reference independently and provide current credentials after schema, delegation and lifetime revalidation. See the [shared feature proposal](../../../../../core/security/iam_delegation_lifecycle/docs/external-secrets/proposal.md) for RFC/draft provenance and exclusions, and the [shared design](../../../../../core/security/iam_delegation_lifecycle/docs/external-secrets/design.md) for interactions with other components.
