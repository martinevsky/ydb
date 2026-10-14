# Script finalization secret identity

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/kqp/finalize_script_service/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 6/6 GOOD in the feature-filtered cases, with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundary: TFixture serializes/reloads persisted sink metadata, uses the real independent tablet and scheme-secret resolution, and controls describe/mint replies. The consumer boundary is persisted effects, not a live external sink.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution.

#### Scenario: Persisted identity blocks path reuse

- **GIVEN** held finalization with reloaded persisted sink metadata.
- **WHEN** the secret path is recreated.
- **THEN** the old effect cannot consume credentials from the new identity.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::HeldFinalizationCannotUseRecreatedSecretAfterPersistedSinkReload` | [kqp_script_secrets_ut.cpp:123](../../kqp_script_secrets_ut.cpp#L123) | `ydb/core/kqp/finalize_script_service/ut` |

#### Scenario: ALTER refreshes current binding

- **GIVEN** the persisted secret identity remains unchanged.
- **WHEN** ALTER changes its active binding before finalization.
- **THEN** resolution follows the current binding for that identity.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::PersistedIdentityAllowsAlterToRefreshTheCurrentBinding` | [kqp_script_secrets_ut.cpp:143](../../kqp_script_secrets_ut.cpp#L143) | `ydb/core/kqp/finalize_script_service/ut` |

#### Scenario: Legacy effects and unused credentials

- **GIVEN** effects lack a delegated identity pin or use ordinary/plain/no-auth metadata.
- **WHEN** finalization loads them.
- **THEN** legacy delegation fails closed, while plain secrets and effects that need no delegated identity remain supported.

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::LegacyDelegatedEffectsWithoutIdentityFailClosed` | [kqp_script_secrets_ut.cpp:159](../../kqp_script_secrets_ut.cpp#L159) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::LegacyPlainSecretEffectsRemainSupported` | [kqp_script_secrets_ut.cpp:166](../../kqp_script_secrets_ut.cpp#L166) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::UnusedDelegatedSourceDoesNotRequireAnEffectPin` | [kqp_script_secrets_ut.cpp:184](../../kqp_script_secrets_ut.cpp#L184) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::NoAuthAndNoSecretEffectsDoNotRequireStructuredJson` | [kqp_script_secrets_ut.cpp:193](../../kqp_script_secrets_ut.cpp#L193) | `ydb/core/kqp/finalize_script_service/ut` |
