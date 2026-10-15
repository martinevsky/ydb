# Provider parsing and EXPLAIN

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [schema](../../../../executer_actor/openspec/changes/external-secrets-iam-delegation/specs/external-secret-schema/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/kqp/provider/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source baseline: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`, including C1 product commit `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. This component received coverage documentation only during the security relocation; historical source/evidence limits are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md).

Status: **historical executable coverage; no new execution claimed here**. The 166/166 post-relocation result covers only the three security targets, not this executable. Use the linked verification record for the actual historical commands and revisions. Planned scenarios have no executable case and do not contribute to executed counts.

Fixture boundary: SecretSettings constructs real expression nodes and parser issue contexts. KikimrIcGateway uses TKikimrRunner and the public query client with schema/delegation flags but without IAM configuration; its EXPLAIN probes do not dispatch IAM.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Delegation source parsing

- **GIVEN** CREATE/ALTER expression settings.
- **WHEN** source parameters are parsed.
- **THEN** CREATE carries only account/cloud/source fields and ALTER infers delegation from RESOURCE without a stored value.

Contract coverage: external-secret-schema / "Delegation source syntax" — scenario "Create a delegation secret"; the ALTER-side inference has no dedicated contract scenario.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `SecretSettings::CreateDelegationPassesOnlySourceParameters` | [secret_settings_ut.cpp:21](../../../secret_settings_ut.cpp#L21) | `ydb/core/kqp/provider/ut` |
| `SecretSettings::AlterInfersDelegationSourceFromResource` | [secret_settings_ut.cpp:35](../../../secret_settings_ut.cpp#L35) | `ydb/core/kqp/provider/ut` |

#### Scenario: Invalid source options

- **GIVEN** duplicate/unsupported options, legacy source or mixed stored value.
- **WHEN** the parser validates settings.
- **THEN** each input is rejected with an error and issues.

Contract coverage: external-secret-schema / "Delegation source syntax" — scenario "Source conversion is rejected"; "Stored-secret compatibility".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `SecretSettings::RejectDuplicateAndUnsupportedOptions` | [secret_settings_ut.cpp:44](../../../secret_settings_ut.cpp#L44) | `ydb/core/kqp/provider/ut` |
| `SecretSettings::RejectLegacySourceAndMixedValue` | [secret_settings_ut.cpp:57](../../../secret_settings_ut.cpp#L57) | `ydb/core/kqp/provider/ut` |

#### Scenario: EXPLAIN without provisioning

- **GIVEN** a runner with feature flags and no configured IAM services.
- **WHEN** CREATE, IF NOT EXISTS, OR REPLACE and ALTER variants execute in Explain mode.
- **THEN** all explanations succeed and DescribePath confirms the secret was not created.

Contract coverage: external-secret-schema / "Metadata is independent of token health" — scenario "IAM is unavailable during metadata access" (EXPLAIN half: no lifecycle or IAM dispatch during compilation).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `KikimrIcGateway::ExplainDelegationSecretWithoutIamConfiguration` | [yql_kikimr_gateway_ut.cpp:328](../../../yql_kikimr_gateway_ut.cpp#L328) | `ydb/core/kqp/provider/ut` |
