# SchemeShard secret fences

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../../../kqp/executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/tx/schemeshard/ut_secret`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. This component received coverage documentation only during the security relocation; historical source/evidence limits are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md).

Status: **historical executable coverage; no new execution claimed here**. The 166/166 post-relocation result covers only the three security targets, not this executable. Use the linked verification record for the actual historical commands and revisions. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: The SchemeShard test environment applies actual schema transactions, ACL/value changes and tablet reboots. It checks persisted secret reference fields and immutable owner/domain/version fences; IAM is outside this fixture.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Owner and version fences

- **GIVEN** an existing secret and wrong path owner or stale version.
- **WHEN** DROP or ALTER attempts ACL/value mutation.
- **THEN** the precondition rejects it before changing the secret.

Contract coverage: external-secret-schema / "Authorization and identity remain current" (schema-side fences).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TSchemeShardSecretTest::DropSecretRejectsAnotherPathOwner` | [ut_secret.cpp:81](../../ut_secret.cpp#L81) | `ydb/core/tx/schemeshard/ut_secret` |
| `TSchemeShardSecretTest::AlterSecretChecksOwnerAndVersionBeforeChangingAclOrValue` | [ut_secret.cpp:107](../../ut_secret.cpp#L107) | `ydb/core/tx/schemeshard/ut_secret` |

#### Scenario: Persistent complete delegation reference

- **GIVEN** a delegated secret with tablet/create-operation reference.
- **WHEN** SchemeShard reboots or incomplete/domain-mismatched CREATE is attempted.
- **THEN** the full reference survives restart; incomplete and wrong-domain requests fail.

Contract coverage: iam-delegation-lifecycle / "Immutable identities and idempotent mutations" — scenario "Database and secret paths are reused" (schema persistence side); external-secret-schema / "Delegation source syntax" (complete-reference requirement).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TSchemeShardSecretTest::IamDelegationReferenceSurvivesRestart` | [ut_secret.cpp:138](../../ut_secret.cpp#L138) | `ydb/core/tx/schemeshard/ut_secret` |
| `TSchemeShardSecretTest::IamDelegationCreateRequiresCompleteReference` | [ut_secret.cpp:181](../../ut_secret.cpp#L181) | `ydb/core/tx/schemeshard/ut_secret` |
| `TSchemeShardSecretTest::IamDelegationCreateChecksDatabaseDomainIdentity` | [ut_secret.cpp:214](../../ut_secret.cpp#L214) | `ydb/core/tx/schemeshard/ut_secret` |

#### Scenario: Immutable source and ordinary replacement ACL

- **GIVEN** ordinary and delegated secrets.
- **WHEN** ALTER/REPLACE attempts a source/reference change or ordinary replacement updates ACL.
- **THEN** delegation identity/source changes are rejected while ordinary CREATE OR REPLACE retains its ACL semantics.

Contract coverage: external-secret-schema / "Delegation source syntax" — scenario "Source conversion is rejected"; "Stored-secret compatibility".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `TSchemeShardSecretTest::IamDelegationSourceAndReferenceAreImmutable` | [ut_secret.cpp:247](../../ut_secret.cpp#L247) | `ydb/core/tx/schemeshard/ut_secret` |
| `TSchemeShardSecretTest::ReplaceSecretCannotChangeSource` | [ut_secret.cpp:293](../../ut_secret.cpp#L293) | `ydb/core/tx/schemeshard/ut_secret` |
| `TSchemeShardSecretTest::CreateOrReplaceSecretInheritPermissions` | [ut_secret.cpp:1145](../../ut_secret.cpp#L1145) | `ydb/core/tx/schemeshard/ut_secret` |
| `TSchemeShardSecretTest::CreateOrReplaceSecretPicksUpParentAclChanges` | [ut_secret.cpp:1209](../../ut_secret.cpp#L1209) | `ydb/core/tx/schemeshard/ut_secret` |
