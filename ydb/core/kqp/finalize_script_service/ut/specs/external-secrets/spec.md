# Script finalization secret identity

## Purpose

Specify test-owned coverage and its reproducible fixture boundaries. The linked feature contracts remain authoritative; these scenarios describe assertions and evidence, not additional runtime requirements.

Authoritative contracts: [consumers](../../../../../../services/scheme_secret/openspec/changes/external-secrets-iam-delegation/specs/external-secret-consumers/spec.md).

## Execution and evidence

Latest recorded owning run: **11 GOOD** (full owning suite) at `1373e723886e`, `task8-10-acquisition-recovery-parallel100`; actual combined command exit 1. Historical counts below retain their original revisions.

Run from the repository root: `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/kqp/finalize_script_service/ut`. A focused reproduction appends `-F 'Suite::Test'` using an exact mapping below. Test source location and build target are separate: `UNITTEST_FOR` can resolve a source from its parent library.

Source revision: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Status: **implemented and verified**, 6/6 GOOD in the feature-filtered cases (`-F 'ScriptExternalEffectSecrets::*'`), with exact command exit 0. Counts, trace hashes, source correspondence and fixture limits are recorded in the [durable verification record](../../../../../security/iam_delegation/docs/external-secrets/verification.md). Historical results remain distinguished from this final run.

Fixture boundary: TFixture serializes/reloads persisted sink metadata, uses the real independent tablet and scheme-secret resolution, and controls describe/mint replies. The consumer boundary is persisted effects, not a live external sink.

## Requirements

### Requirement: Traceable scenario coverage

The owning test target SHALL keep each implemented scenario traceable to its exact executable case, source assertion and fixture boundary, and SHALL distinguish planned or unverified coverage from recorded execution. Each scenario SHALL name the contract requirement and scenario it covers. Planned scenarios have no executable case and do not contribute to executed counts.

#### Scenario: Persisted identity blocks path reuse

- **GIVEN** held finalization with reloaded persisted sink metadata.
- **WHEN** the secret path is recreated.
- **THEN** the old effect cannot consume credentials from the new identity.

Contract coverage: external-secret-consumers / "Each secret reference retains its own identity" — scenario "One of several paths is recreated".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::HeldFinalizationCannotUseRecreatedSecretAfterPersistedSinkReload` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |

#### Scenario: ALTER refreshes current binding

- **GIVEN** the persisted secret identity remains unchanged.
- **WHEN** ALTER changes its active binding before finalization.
- **THEN** resolution follows the current binding for that identity.

Contract coverage: external-secret-consumers / "Credentials are acquired for new external requests" — scenario "Account changes without restarting a query".

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::PersistedIdentityAllowsAlterToRefreshTheCurrentBinding` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |

#### Scenario: Legacy effects and unused credentials

- **GIVEN** effects lack a delegated identity pin or use ordinary/plain/no-auth metadata.
- **WHEN** finalization loads them.
- **THEN** legacy delegation fails closed, while plain secrets and effects that need no delegated identity remain supported.

Contract coverage: external-secret-schema / "Stored-secret compatibility" — scenario "Existing stored value" (LegacyPlainSecretEffectsRemainSupported); external-secret-consumers / "Each secret reference retains its own identity" (fail-closed legacy handling; no dedicated contract scenario).

Executable mapping (status: implemented; execution per verification record):

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::LegacyDelegatedEffectsWithoutIdentityFailClosed` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::LegacyPlainSecretEffectsRemainSupported` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::UnusedDelegatedSourceDoesNotRequireAnEffectPin` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::NoAuthAndNoSecretEffectsDoNotRequireStructuredJson` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |

#### Scenario: Finalizer deadline and independent persisted identities

- **GIVEN** persisted sink pins, real secret acquisition and the actual finalizer with controlled metadata boundaries.
- **WHEN** one path is recreated, a configured deadline expires, or the real producer enters a backoff longer than sixty seconds.
- **THEN** each pin remains independent, the caller budget reaches acquisition, and completion cancels owned waits without accepting late results.

Status: mapped executable cases passed at `1373e723886e`. See the verification record for exact commands, suite coverage and combined-run exits.

Contract coverage: external-secret-consumers.

| Executable case | Test source | Command target |
| --- | --- | --- |
| `ScriptExternalEffectSecrets::ConfiguredFinalizerBudgetReachesSecretAcquisition` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::ConfiguredFinalizerWaitsThroughRealProducerBackoff` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::ExplicitShortDeadlineCancelsResolutionAndIgnoresLateToken` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::FinalizerTimeoutCancelsPendingSecretAcquisition` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |
| `ScriptExternalEffectSecrets::TwoPersistedPinsKeepIndependentIdentityAfterOnePathIsRecreated` | [kqp_script_secrets_ut.cpp](../../kqp_script_secrets_ut.cpp) | `ydb/core/kqp/finalize_script_service/ut` |

Fixture boundary: the parent deadline cases inject completed metadata and stop at preparation/final-metadata boundaries; they do not execute a live S3 commit or metadata database transaction. The producer-backoff case runs the real producer with controlled IAM replies and its actual standard backoff.
