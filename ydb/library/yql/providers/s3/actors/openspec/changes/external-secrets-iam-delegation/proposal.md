# S3 credential and download cancellation

## Why

Preserve cancellation across asynchronous credential resolution, decoder mailboxes and download-hook publication. The contract belongs beside the implementation so changes to this component can be reviewed with their acceptance criteria.

## What Changes

- Specify the restored behavior and remaining hardening work owned by this component.
- Preserve the existing authentication, immutable identity and recovery guarantees.
- Track verified work separately from pending regression fixes in `tasks.md`.

## Capabilities

### New Capabilities

- `external-secret-s3-cancellation`: Preserve cancellation across asynchronous credential resolution, decoder mailboxes and download-hook publication.

### Modified Capabilities

None. This is the initial OpenSpec contract for this component.

## Impact

Preserve cancellation across asynchronous credential resolution, decoder mailboxes and download-hook publication. See the [shared feature proposal](../../../../../../../../core/security/iam_delegation_lifecycle/docs/external-secrets/proposal.md) for RFC/draft provenance and exclusions, and the [shared design](../../../../../../../../core/security/iam_delegation_lifecycle/docs/external-secrets/design.md) for interactions with other components.
