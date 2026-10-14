# Durable delegation storage

## Why

Own intents, immutable bindings, atomic transitions, leased cleanup and revision-bound inventory independently of tenant schema lifetime. The contract belongs beside the implementation so changes to this component can be reviewed with their acceptance criteria.

## What Changes

- Specify the restored behavior and remaining hardening work owned by this component.
- Preserve the existing authentication, immutable identity and recovery guarantees.
- Track verified work separately from pending regression fixes in `tasks.md`.

## Capabilities

### New Capabilities

- `iam-delegation-lifecycle`: Own intents, immutable bindings, atomic transitions, leased cleanup and revision-bound inventory independently of tenant schema lifetime.
- `iam-delegation-revocation`: Own intents, immutable bindings, atomic transitions, leased cleanup and revision-bound inventory independently of tenant schema lifetime.
- `iam-delegation-inventory`: Own intents, immutable bindings, atomic transitions, leased cleanup and revision-bound inventory independently of tenant schema lifetime.

### Modified Capabilities

None. This is the initial OpenSpec contract for this component.

## Impact

Own intents, immutable bindings, atomic transitions, leased cleanup and revision-bound inventory independently of tenant schema lifetime. See the [shared feature proposal](../../../../docs/external-secrets/proposal.md) for RFC/draft provenance and exclusions, and the [shared design](../../../../docs/external-secrets/design.md) for interactions with other components.
