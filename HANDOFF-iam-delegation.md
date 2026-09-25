# Handoff: IAM delegation secrets in YDB

This document hands the whole IAM delegation secrets effort to a new Claude instance. It covers what the feature is, where every piece of code and text lives, what the user decided and why, the rules the user set, and what is open. Read it fully before acting. It was written on 2026-09-25.

## 1. The feature in one paragraph

A new kind of schema secret lets YDB act on behalf of a customer's Yandex Cloud service account without storing any credential. `CREATE SECRET ... WITH (SOURCE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "...", RESOURCE = "<cloud id>")` asks IAM ServiceControl to set up a delegation on behalf of the user running the statement, authenticated as YDB's own system service account. `ALTER` replaces the delegation, `DROP` revokes it. Reading such a secret yields a current, short-lived IAM token of the delegated account, which a node-local token service mints through `IamTokenService.CreateForService` and keeps fresh. Every object that accepts a token secret (for example an external data source with `AUTH_METHOD = "TOKEN"`) works with it unchanged. Feature flag: `EnableIamDelegationSecrets` (YDBFEATURES-144).

## 2. Where everything lives

All branches are on the fork `https://github.com/martinevsky/ydb.git` unless stated otherwise. Upstream is `ydb-platform/ydb`.

| Piece | Location | State |
|---|---|---|
| RFC | ydb-platform/ydb-rfc#330, branch `iam-delegation-secrets-rfc`, file `iam_delegation_secrets.md`, head 8fff3a1 | Open. NewBediver requested changes; all three comments are addressed in 8fff3a1 and answered in their threads. Awaiting re-review |
| PR 1, feature flag | ydb-platform/ydb#53659 | Merged. Flag field number 336 |
| PR 2, node-local services | ydb-platform/ydb#53712, branch `iam-delegation-services`, head c5262fdd9f7 | Open, CI was pending at last check. Now **delegation service only** (see 4.2) |
| PR 2 before the split | branch `iam-delegation-services-full`, e0b5fa3d31d | Backup with the token service and the cloud lookup; source for later PRs |
| PR 3, SchemeShard | branch `iam-delegation-schemeshard-wip`, 2ad4df00a61 | Never published as a PR. Staged locally, 44 tests passed when staged. Must be reworked to the RFC's `oneof` before publishing (see 5) |
| Docs (Russian) | ydb-platform/ydb#53519, branch `iam-delegation-secrets-docs`, head 2994cf4199e | Draft. Follows the RFC |
| Whole feature, all code | branch `iam-delegation-feature-wip`, dcb6358378 | Snapshot of the local feature tree on top of an old main (a3d01a30c40). **Never compiled** after the latest changes (see 4.4) |
| This document | branch `iam-delegation-handoff` = the feature snapshot plus this file | |

The local machine this came from has these trees; the cloud instance works from the branches above instead:
- Feature tree `/home/pretender/ydbwork/cloud_tokens/ydb`, branch `cloud_tokens`, all changes uncommitted. The snapshot branch equals it.
- PR worktrees `/home/pretender/ydbwork/iam-delegation-{services,schemeshard,docs,ff}`, RFC at `/home/pretender/ydbwork/cloud_tokens/ydb-rfc`.

## 3. Design as it stands in the RFC

Sections refer to `iam_delegation_secrets.md` at 8fff3a1.

- **DDL (2.1).** `SOURCE` says where the value comes from; no `SOURCE` means an ordinary stored value. A secret with a source is an *external secret*. `SERVICE_ACCOUNT_ID` is required, `RESOURCE` (the cloud id, bare) is optional. Attribute names are provider-neutral; provider specifics go into values. `ALTER` and `CREATE OR REPLACE` cannot change the source.
- **External secrets (2.2).** One `SOURCE` axis instead of types, justified by composition: a Yandex Lockbox secret can authenticate with a delegation secret (sketch in 7.3). Rules S1 to S4: omitted source means stored value; fixed option set per source with an error listing accepted options; readers never keep the value, expiry is not part of the user interface; `REFRESH_PERIOD` reserved.
- **Stored record (2.3).** `TSecretDescription` holds a `oneof Source { string Value = 2; TIamDelegation IamDelegation = 5; }`, which is wire-compatible with today's `Value`. No `ESecretType` enum.
- **Who may run the statements (6.1).** Only Yandex Cloud subjects; the subject sent to IAM is the user's SID minus `@<AuthConfig.AccessServiceDomain>`, never YDB's system account.
- **Creating (6.2).** G5a: the user's YDB right is checked with the user's token **before any IAM call** (create in the parent directory, alter or drop the secret); SchemeShard rechecks at commit. G5b: compile, prepare and explain never call IAM. `RESOURCE` omitted: derived from the service account with the user's token, else the database's `cloud_id` with a warning (G4).
- **Altering and dropping (6.3).** New delegation before revoking the old one. `DROP` ends access to the secret at commit, new token issuance when IAM accepts the revocation, and already issued tokens only at their expiry.
- **Durable revocation (6.3a, G15a to G15c).** Before any IAM call that sets up or revokes a delegation, YDB durably records the referrer and the secret path; the record goes away when the outcome is final. A background reconciliation revokes recorded referrers no secret names. The RFC deliberately says only "durable", no storage specifics (user's decision).
- **Token freshness (6.4, G17, G20, G20a).** Minimum served lifetime 5 minutes; a cached token below it counts as absent and the read, which returns a future, waits for the next mint as with an empty cache; refresh at half of the lifetime; retries follow the reader's retry policy passed to `DescribeSecret`; no separate "about to expire" failure.
- **Yandex Cloud binding (7.1).** Control plane `iam.private-api.<env>:4283` (SetupDelegation, RevokeDelegation, OperationService, ServiceAccountService), token service `ts.private-api.<env>:4282`, Resource Manager `rm.private-api.<env>:4284` (optional). Referrer: type `ydb.secret`, id `ydb.delegation.<32 hex>`. Other providers (Keycloak, Nebius, OIDC) and Lockbox are sketches, not in scope.

## 4. Code state

### 4.1 Components (in the feature snapshot)

- `ydb/core/security/iam_delegation/`: system token service (SDK metadata provider owned by an actor, answers `TEvGetSystemToken`), delegation service (Setup, Revoke, operation polling, retries), delegated token service (per-key refresh loop, `UsableUntil`, sensors `iam_delegation/component=token_service/{Mints,MintErrors,CachedKeys}`), `iam_actor_base.h` (free `IamCallWithRetry`, `TIamCallCredentials`, `Spawn`, IAM failure hints), `cloud_resolver.{h,cpp}` (a coroutine, not an actor), settings, events.
- `ydb/core/kqp/iam_delegation/kqp_iam_delegation_records.{h,cpp}` (new): durable records in `.metadata/iam_delegation/delegations` of the node's database, keyed by (database, referrer_id), with a 30-minute lease; reconciliation service `IAM_DELEGATION_RECONCILIATION_ACTOR` started by the KQP proxy, every minute.
- `ydb/core/kqp/executer_actor/kqp_iam_delegation_secret_orchestrator.*`: creator, alterer, dropper coroutines. `CheckRight` mirrors tx_proxy's plain `CheckAccess` (no admin bypass for secrets). Records around every IAM call; failed revocations produce the warning "... is not revoked yet ...; the revocation is retried automatically".
- `ydb/services/scheme_secret/`: secret service asks the token service for delegation secrets; `TEvDescribeSecretsResponse::TDescription` carries `ReReadOnUse` and `UsableUntil`; `secret_credentials.cpp` is the refreshing credentials provider for running tasks (past `UsableUntil` it answers with the next read, errors surface as SDK auth errors).
- KQP proxy starts the system token, token, delegation and reconciliation services at bootstrap and when the flag is turned on by config notification; flag off and IamConfig changes need a node restart.
- Config: `TAppConfig.IamConfig = 124` with TokenServiceEndpoint = 1, ServiceControlEndpoint = 2, ServiceId, MicroserviceId (required by IAM), ResourceType, EnableSsl, ResourceManagerEndpoint = 7; identity fields fall back to `replication_config.iam_service_control`. Timeouts are code constants, not config.
- Tests: `ydb/core/security/iam_delegation/ut`, `ydb/core/kqp/executer_actor/ut` (`KqpIamDelegationSecretOrchestrator`), `ydb/core/kqp/ut/scheme` (`KqpScheme::IamDelegation*`), `ydb/core/kqp/ut/federated_query/datastreams` (`KqpIamDelegationSecrets`, `StreamingQuerySecretRefresh`, uses the python IAM emulator `ydb/tests/fq/streaming_common/iam_grpc_emulator`), compatibility tests `ydb/tests/compatibility/secrets` (run with `-DYDB_COMPAT_TARGET_REF=current`).

### 4.2 PR 2 (#53712) after the split

The user chose "delegation service only" and "drop configs and everything not essential". The PR now has the delegation service, the system token service, `TIamConfig` plumbing (AppData), and proxy registration. The token service, the cloud lookup, the live IAM probes, the sample configs and the token mock changes were moved out; the full version is on `iam-delegation-services-full`.

Found on review of the split, **not fixed yet**:
1. The settings were not reduced. `TokenServiceEndpoint`, `ResourceManagerEndpoint`, the token-cache constants and `CanResolveCloud` remain; the proxy calls `ValidateForDelegation`, which still requires `TokenServiceEndpoint`, an endpoint the delegation service never calls. Fix: drop them, drop `TIamConfig` fields 1 and 7 (keep 2 to 6, do not renumber), merge the two validators.
2. The `IamDelegationSettings` test suite was dropped; restore a small one for the reduced settings.
3. The PR description ends mid-sentence ("Тесты в `ydb/core/security/iam_delegation/ut`").

Further strip ideas offered to the user, not decided: remove `TIamCallCredentials` (no user-token caller left in PR 2); fold `TIamActorBase` into the delegation service; move the IAM failure hints to the DDL PR.

Review comments on #53712 by GrigoriyPA (not answered on GitHub; the user answered one himself):
- Done and pushed: enable services on a flag change (config notification), no try/catch around registration, sorted PEERDIRs, actor system pointer in the cloud lookup's client holder.
- Open: move IamConfig from AppData to the KQP proxy constructor (the user asked what the problem is; the orchestrator also reads AppData). Use `GrpcStatusToYdbStatus`: recommended reply is "no", because it maps INVALID_ARGUMENT and FAILED_PRECONDITION to PRECONDITION_FAILED instead of BAD_REQUEST and UNKNOWN or ABORTED to non-retryable statuses, and it lives in the heavier `local_rpc` library.

### 4.3 Planned PR sequence

1. Flag (merged).
2. Delegation service (#53712).
3. Token service plus the lifetime rules of G17 (from `-full` and the snapshot).
4. SchemeShard record (`oneof`) and data source validation (from `iam-delegation-schemeshard-wip`, reworked).
5. Reading delegation secrets: secret service token path, reader retry policy, re-reading credentials provider (type-dependent, no second flag).
6. DDL and orchestration with durable records and reconciliation, last, after the RFC is approved.

The user wants PRs as small as possible and asked that every behavior change be behind the single flag.

### 4.4 Implementation not yet compiled

The last session implemented in the local tree (and the snapshot): the token lifetime rules (`MinServedLifetime` replaces `TokenRefreshMargin`), `UsableUntil` through the secret service and the provider, `CheckRight`, the durable records library and reconciliation service, proxy registration, updated warnings and tests (`TokenBelowMinimumLifetimeCountsAsAbsent`, `RefreshAtHalfOfLifetime`, `IamDelegationRightsCheckedBeforeIam`, `IamDelegationRecordsFollowTheOutcome`, poison test checks the record). A compile-check handoff was issued for another machine; its result is unknown. The PEERDIRs of the new library are guesses. Tests where `bob@builtin` has no grant may now fail at `CheckRight`; granting in the test setup is the allowed fix.

Not written: a reconciliation test (needs time control over the one-minute loop), a provider test for waiting past `UsableUntil`.

The implementation lags the RFC in two places that belong to later PRs: the parser still uses `TYPE` (RFC: `SOURCE`), and the stored record still uses the `ESecretType` enum (RFC: `oneof`).

## 5. Open questions and pending decisions

- **RFC re-review** by NewBediver after 8fff3a1.
- **DESCRIBE of an external secret.** Discussed, not in the RFC, the user changed the subject. Proposal: `Ydb.Secret.DescribeSecret` gains `source` and source parameters (free); plus a per-node state for callers with `SELECT ROW` (status, error, obtained/expires times, masked value in the `MaskTicket` format of `ydb/library/security/util.cpp`); a single-node answer that obtains the value if needed, and an explicit all-nodes mode that only peeks caches via the per-database KQP proxy board, grouped by status and checksum. The user rejected any forced update of delegations for diagnosis.
- **Cloud lookup clients.** The lookup creates two gRPC clients per statement. Proposed owner: the delegation service with a `TEvResolveCloud` request. Previously deferred by the user; re-raised, not decided.
- **Refresh loop simplification.** `FindEntry` calls and `LoopRunning` in the token service's `RefreshLoop` are redundant because only the loop erases entries; proposed to take the entry by reference once. Not done.
- **Durable records storage.** The implementation follows the streaming queries pattern (`.metadata` table plus a background service); the RFC stays storage-neutral.
- **IAM side:** the `internal.service-control.delegator` role on `iam.gizmo:ydb` must be granted via a terraform PR (bootstrap-templates gizmo bindings); open questions to IAM: `ydb.secret` referrer type registration, `with_references`, RevokeDelegation idempotency.
- **Preprod leftovers:** a probe service account from live verification (`bfb63dp6infbu1mpggb9`) should be deleted when no longer needed. Preprod only, never prod.

## 6. Rules the user set (binding)

- **Tests are deterministic.** No sleeps, no assertions on elapsed time; use gates, ordering, counts and sensors; timeouts only as hang guards around waits the test makes inevitable. Existing tests must not depend on wall clock either.
- **Docs follow the RFC.** Every user-visible RFC change is mirrored in the docs PR in the same step and both are pushed together. Internal details (protos, numbers like the 5-minute floor) stay out of the docs.
- **The RFC names no specifics of the implementation's storage**, and no real account, cloud or host ids.
- **No comment-only or whitespace-only changes** to upstream code the change does not otherwise touch.
- **No unnecessary public settings.** Timeouts and intervals are code constants.
- **No test-only crutches in production code.**
- **Typed enums instead of bools; concept checks on template parameters.**
- **No blocking calls on actor threads.** `TActorSystem::Send` is the way back from foreign threads.
- **One feature flag** guards every behavior change.
- **Leave changes local for review** when the user says so; ask before publishing or answering review comments unless told to.
- **Never** print or paste tokens; never use `git stash`, `git reset --hard`, `git checkout -- <path>`, `git clean`, or force-push.
- **Preprod only.** `SetupDelegation` creates real IAM state and must be revoked.
- The babysitter or helper agents do not decide design or public-interface questions.

## 7. Practical notes

- Build: `./ya make --build relwithdebinfo -tA <targets>`. Two concurrent `ya make` runs in one build root corrupt it; check with `ps -u $USER -o pid,cmd | grep '[y]a-bin make'` (a long-running `ya-bin ... --shard report` process is telemetry, not a build). Launch long builds detached (`nohup setsid script.sh &`) and poll the log.
- A full disk makes the linker fail with "Bus error"; `~/.ya/build/symres` is the usual culprit and may be cleaned.
- Pushing: SSH agent sockets come and go; the HTTPS fallback is `git -c credential.helper='!gh auth git-credential' push https://github.com/martinevsky/ydb.git <ref>` (slow, give it minutes).
- Actor coroutines: a `void` member function of an actor with `co_await` is a detached task; `async<T>` is lazy and runs only when awaited; the token service starts its refresh loop through an explicit `Spawn`. `PassAway` waits for coroutine tasks to unwind, hence the `StateDying` handlers.
- The KQP test fixture with the access service cannot run `CREATE STREAMING QUERY`; streaming tests use `TStreamingWithSchemaSecretsTestFixture`.
- A stable binary reads a delegation secret as an empty value (downgrade breaks sources over delegation secrets); the compatibility config drops `iam_config` and the flag while a stable binary runs.

## 8. Suggested first steps for the new instance

1. Fetch the branches, read the RFC at 8fff3a1 and this document.
2. Get the compile-check result of `iam-delegation-feature-wip`, or run it: targets `ydb/core/kqp/iam_delegation`, `ydb/core/security/iam_delegation/ut`, `ydb/services/scheme_secret`, `ydb/core/kqp/executer_actor` (+`/ut`), `ydb/core/kqp/proxy_service`, `ydb/core/kqp/ut/scheme`, `ydb/core/kqp/ut/federated_query/datastreams`.
3. Ask the user before changing PR 2 further; the three findings in 4.2 are ready to fix.
4. Check RFC #330 and PR #53712 for new review comments, and discuss them with the user before answering.
