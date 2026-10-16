# External-secrets verification

The [current follow-up record](#current-follow-up-validation-record) lists the completed owning-suite runs, driver build and independent reviews. Earlier sections preserve the evidence and limits of their original checkpoints.

Task5 base: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`; C1 source commit: `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 relocates three existing libraries and their tests without changing symbols, persistent formats or assertions. Its final commit is recorded by version control history for this file. Historical recovery/C1 results used the former lifecycle and tx/tablet paths and do not validate relocated sources.

Task5 implementation commit: `87f8733958c047bd0429e2b8d627b00404b43c75`. Independent task review approved both specification compliance and code quality with no findings, including verification of archived results, source hashes and scenario mappings. This approval covers the relocation and test specifications; final integration results are recorded below.

Run the following exact commands from the repository root, sequentially:

| Command | Exact exit | Result |
| --- | --- | --- |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/ut` | 0 | 57/57 GOOD |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/lifecycle/ut` | 0 | 22/22 GOOD |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/tablet/ut` | 0 | 87/87 GOOD |

The external evidence collection is `external-secrets-recovery/review-fixes-2026-10-14/task5`. It contains full `service.log`/`service.exit`, `lifecycle.log`/`lifecycle.exit` and `tablet.log`/`tablet.exit`, dereferenced `service-results`, `lifecycle-results` and `tablet-results`, source SHA-256 manifests, strict OpenSpec validation, case/source/target mappings and normative-contract equivalence. External artifact names are provenance identifiers; they are not branch-local links. Detailed SDD bookkeeping is intentionally ignored and is not required to resolve any document in the pushed branch.

At the recorded checkpoints the seven authoritative feature contracts retained 43 requirements and 67 scenarios byte-for-byte; a subsequent adversarial specification review revised them to 59 requirements and 101 scenarios (see the closing section below). The twelve [test-owned specifications](README.md#test-owned-scenarios) map existing coverage intent separately from run results. They do not assert that unrelated consumer targets passed on this revision.

All Task4 integration targets passed below. C2 and both direct wrapper regressions are implemented and verified. Independent whole-fixes review approved the committed/runtime changes; final delivery is recorded by branch history. The wrapper has no cancellation API; late request suppression remains owned by read actors.

Provider, SchemeShard and Solomon received coverage documentation only in Task5. Their current executable mappings describe source coverage; existing run evidence is historical and they are not additional required Task4 reruns.

The three relocated targets completed 166/166 GOOD with exact exits 0. The 47-file product/build input SHA-256 manifest `build-source-hashes.json` has digest `31b47be03279fd44c897f2b7a2b3d81629b7bc3ca1c16d68ade08958a67ca229`; inputs matched before and after all runs. The controller independently compared 42 affected C++/header/proto sources to the base after only include/import path substitutions (`controller-source-equivalence.json`). Detailed result/artifact manifests are `test-results-summary.json` and the three `*-artifact-hashes.json` files in the external collection.

## S3 cancellation and wrapper regressions (Task3/C2)

Production and regression backup commit: `c098e4ceb6957899cd17eb64b8c2b4d331d81703`. Fixture activity-name correction and tested revision: `2e7842af118177858824d740015af632ce10a2fb`. Both commits were pushed before new tests, as requested. The fixture correction changes only its decoder lookup to the runtime activity name S3_READ.

From the repository root, the exact commands were:

| Command | Exact exit | Result |
| --- | --- | --- |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/actors/ut -F 'S3ExternalSecretCancellation::StreamingCancelBeforeHookPublication'` on pre-fix source | 10 | 1 FAIL: actual cancel-hook calls 0, expected 1 |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/actors/ut ydb/library/yql/providers/s3/credentials/ut -F 'S3DownloadCancellation::*' -F 'S3ExternalSecretCancellation::*' -F 'TCredentials::*'` | 0 | 14/14 GOOD: actors10 + credentials4 |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/library/yql/providers/s3/actors/ut ydb/library/yql/providers/s3/credentials/ut` | 0 | 54/54 GOOD: actors50 + credentials4 |

External artifacts live in `external-secrets-recovery/review-fixes-2026-10-14`: `task3-red-4.log/.exit/-source.sha256/-test-results`, `task3-green-2.log/.exit/-source.json/-test-results`, and `task3-green-full.log/.exit/-source.json/-test-results`. The GREEN source manifests cover 35 S3 product/test/build files, verified unchanged after each run. The full source-manifest SHA-256 is `5e054860ab26d10dfbb12a379f1a51793ebf69a37f7b685786521a223a829acd`. All archived result trees are dereferenced and contain no symlinks.

| Trace | SHA-256 | Result |
| --- | --- | --- |
| `task3-red-4-test-results/unittest/ytest.report.trace` | `ad34cc421fa531fef1510d8d2036ebd1a36885ca4cef395e83c3c6569786dd48` | expected missed-hook failure |
| `task3-green-full-test-results/actors/unittest/ytest.report.trace` | `fe2e8a417fe63f9f99267fb8af43452af90fa4abd25049983a8bab4a3c6e4540` | 50 GOOD |
| `task3-green-full-test-results/credentials/unittest/ytest.report.trace` | `363d89b354e8e90804a3752b816f9e3eae675206f4caa0abfb81122e82681da9` | 4 GOOD |

Earlier RED1–3 failed fixture/build setup, and focused GREEN1 passed 13/14 with an actor-name fixture failure; those are archived diagnostic attempts, not the valid RED or final GREEN. The real streaming regression fixes callback/cancellation ordering without sleeps; primitive coverage tests before/after/repeated/concurrent/reentrant cancellation, completed-attempt hook replacement and empty hooks. The pending-credentials actor test keeps its parent alive to exercise the late continuation guard; a valid streaming control delivers a row. Direct wrapper success/error tests verify provider retention and future propagation after wrapper destruction, covering existing behavior without adding a cancellation API. No live IAM or race-detector run is claimed. Successful output includes actor diagnostics for deliberately invalid inputs; one focused run also disabled an unreachable remote cache and completed successfully.

Task3 independent review approved specification compliance and code quality at `96de6630f11800e7b081706274535be20bf72ff6`, with no correctness findings. The reviewer recomputed archived trace/log hashes, confirmed zero symlinks, matched all 35 current source inputs and verified the cancellation/error/mailbox ordering. The only non-blocking finding concerns expected negative-test diagnostics and unavailable-cache output; no rerun was required. Final combined integration passed below.

## Final integration (Task4)

Tested source commit: `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`. Runtime/product/build inputs are frozen across both stages and still match all 69 entries in `final-integration-source.json` (SHA-256 `17901c36f91a795748e248d62fb22f71ab2eb1832ac06160681b6cc4548adf49`). Later controller delivery changes are status/evidence documentation only. Six feature-spec Markdown files were concurrently edited in the shared worktree during the main run; they do not affect any checked compiled input. The result manifests retain that doc-only Git status, and a `concurrent-spec-edits` snapshot preserves those external edits. They are kept separate from this delivery and its reviewed contracts.

Exact commands from the repository root:

| Command | Exact exit | Result |
| --- | --- | --- |
| `./ya make --build relwithdebinfo -tA --keep-going --ignore-recurses --link-threads=1 --test-threads=4 ydb/core/security/iam_delegation/ut ydb/core/security/iam_delegation/lifecycle/ut ydb/core/security/iam_delegation/tablet/ut ydb/core/kqp/executer_actor/ut ydb/services/scheme_secret/ut/fast ydb/library/yql/providers/s3/actors/ut ydb/library/yql/providers/s3/credentials/ut ydb/core/driver_lib/run` | 0 | 381/381 GOOD in 7 suites; production driver library built |
| `./ya make --build relwithdebinfo -tA --keep-going --ignore-recurses --link-threads=1 --test-threads=4 ydb/core/grpc_services/ut ydb/core/kqp/finalize_script_service/ut -F '*Iam*' -F '*Secret*'` | 0 | 26/26 GOOD in 2 suites; feature-filtered API/finalizer checks |

Total: **407/407 GOOD across 9 owning executables**. Both outer commands were collected by the controller. The second command filters the IAM/secret cases; it is not a claim that every unrelated API test ran. Detailed per-target trace counts and hashes:

| Target | GOOD | Archived trace SHA-256 |
| --- | --- | --- |
| `ydb/core/security/iam_delegation/ut` | 57 | `6554e7f72cb845b6800ed6dd8139c46e3347681d63170121223214b54a56cb1e` |
| `ydb/core/security/iam_delegation/lifecycle/ut` | 22 | `dd6b44611e6f6dcd0a34ce486cd7acfebe673c26778dc4782ae86c5e50812db7` |
| `ydb/core/security/iam_delegation/tablet/ut` | 87 | `a1fadcd73b38262bc7acb99507e2a6dd28a953bd17f6274af2ec835a96ab7cf7` |
| `ydb/core/kqp/executer_actor/ut` | 119 | `852da9a9cb45830fa132a3a7aa914311a74cd245b0411f644ee55cc7ea262d49` |
| `ydb/services/scheme_secret/ut/fast` | 42 | `50afad43b24aeb44384b7625fba9ec31f3b041baa53481948d4b5d41950ab6e5` |
| `ydb/library/yql/providers/s3/actors/ut` | 50 | `3a03507f60774dd01ebc98bf2d52c8f5d5bb065461d91f4dd7036841a0923b0a` |
| `ydb/library/yql/providers/s3/credentials/ut` | 4 | `3ccec46c095481c1ff0e490df6101953b41828ed85fe2e5c79883a6fec99c6c2` |
| `ydb/core/grpc_services/ut` | 20 | `e31f44a54f46791263f9a0dfdae56e98c0091bffdb244f43e62e64f72b021eeb` |
| `ydb/core/kqp/finalize_script_service/ut` | 6 | `009d1f0990a52f09b92ca4ac30d1ec3c6d3d812d9002611960b0a5b726ba94f2` |

Evidence collection: `external-secrets-recovery/review-fixes-2026-10-14/final-integration.log/.exit/-source.json/-results/manifest.json` and corresponding `final-api-integration` files. All copied result trees are dereferenced, with zero symlinks. The final tablet run includes seeded chaos schedules 1, 330 and 20261013, on-disk reopening, commit/reply crash boundaries and atomic revocation/exact lease cases. The DDL, shared-producer backoff/phase-timer and actual S3 publication regressions pass together with final wiring.

Test output retains expected injected failures, denial diagnostics, virtual-disk warnings and an unreachable optional remote-cache message; all commands and finalized traces pass. No live-IAM, rollout or TSan result is claimed. Runtime deployment still requires the documented service-owned delegation tablet and IAM control-plane configuration.

Final whole-fixes review approved the committed/runtime range through `66189ce6fa6a234fd7d20f32bda4fd41692c07ef`, with no blocking correctness findings. The reviewer independently verified all 407 archived GOOD results, nine trace hashes, both full-log hashes, zero symlinks and all 69 source-input matches. The minor stale design-status rows were corrected during this documentation reconciliation. At that checkpoint, six concurrent specification revisions were outside the runtime approval; they were subsequently incorporated into specification commit `8512b72d0a01cd1627e7fae2b56441446e853911`.

Final delivery specification validation uses the staged Git tree, excluding the six concurrent edits: all 12 test specifications and all five native OpenSpec projects pass strict validation, all 275 relative document links resolve in that tree, and its seven reviewed feature contracts remain byte-identical (43 requirements, 67 scenarios) at that checkpoint. Evidence: `final-delivery-spec-validation-inputs.json`, `final-delivery-standalone-validation.json` and five `final-delivery-native-*.json` records.

## Specification revision after the verified delivery

After the verified delivery checkpoint, the seven feature contracts and the twelve test-owned specifications underwent an adversarial multi-round specification review. The contracts grew from 43 requirements and 67 scenarios to 59 requirements and 101 scenarios; the twelve test-owned specifications gained per-scenario contract-coverage citations traceable to exact contract requirements and scenarios, and a planned-scenario backlog that does not contribute to executed counts. Every run result above remains recorded at its original revision and is unchanged; this revision is documentation-only and asserts no new test execution. Post-revision, all five native OpenSpec projects pass strict validation again at the revised state.

Verified delivery checkpoint: `9456bbd241faa0aaafade40592dbd4cca885d404` was pushed without force to `martinevsky/ydb:restore/external-secrets-integration`, and an independent HTTPS remote query matched that exact SHA. Runtime sources match tested `66189ce6fa6`; the subsequent completion marker is documentation only. At that delivery checkpoint six concurrent feature-spec edits were still uncommitted; their later revision is committed in `8512b72d0a01cd1627e7fae2b56441446e853911`.

## Specification-first coroutine and recovery follow-up

The follow-up to `8512b72d` defines coroutine-owned temporary operation state, typed modes/outcomes, standard `TBackoff`, retained tablet cleanup ownership and the reviewed DDL/inventory boundary corrections. Its seven feature contracts contain 64 requirements and 110 scenarios. At that specification-only checkpoint, component tasks and new test scenarios were unchecked/planned. Later implementation evidence is recorded below; the historical executable results above are unchanged.

Documentation verification: all five native OpenSpec projects and all twelve test-owned specifications pass strict validation with zero issues; native commands completed with exit 0. Relative links and contract-name citations resolve. An independent builder check found no remaining material design question. No product code, build, product-test run or deployment verification is included in this specification-only follow-up. The implementation sequence is in [implementation-plan.md](implementation-plan.md).

## Follow-up tablet admission validation

Tested source: `e6c24582d62ecb6ffac1bc11a2dc9e696068fca5`, pushed before execution. Exact command: `./ya make --build relwithdebinfo -tA ydb/core/security/iam_delegation/tablet/ut --threads=40 --link-threads=1 --test-threads=1 --cache-size=110GiB --bazel-remote-base-uri http://[64:ff9b::9ea0:93d3]:8081 --keep-going`. Actual exit: **0**, **89/89 GOOD**. This is a completed incremental retry after the recorded resource failures, with host earlyoom enabled.

Both new admission regressions passed, including cold-page replay after feature disablement. Evidence: `spec-implementation-2026-10-15/task2-tablet-green-adaptive40-retry4.command/.commit/.log/.exit/.results-manifest.json` and dereferenced `.results/`. Trace SHA-256: `2628a09c8a155a6e734757caca2bc8967ce1d230973df9444b2f9506278ad427`. New setup-diagnostic tests introduced after this source snapshot are unexecuted; the 89-test result does not cover them. Coordinator/SchemeShard validation and the remaining tasks were pending at that checkpoint; the current follow-up record below tracks their subsequent execution.

## Follow-up setup refusal regression evidence

Tested source: `a9df8cc1454e0b368152c12fcd36f4a4fa82ac1b`, backed up to the fork before testing. Native focused runs completed with exit 1: service 3 FAIL, lifecycle worker 2 FAIL, coordinator 6 FAIL and 1 GOOD. These are expected behavior regressions for bounded diagnostics, refusal status recovery, raw-text exclusion and contradictory operation outcomes. Exact commands, source hashes, binary identity, traces and exits are under `spec-implementation-2026-10-15/task4-{service,worker,kqp}-direct-red*`.

The broader four-target retry completed compilation but encountered three test-chunk symbolization timeouts and was intentionally interrupted (exit 254). Its partial results are diagnostic evidence, not a complete suite result. At that checkpoint, the subsequent production fix had not yet passed GREEN validation; later results are recorded below and earlier passing counts do not cover it.

## Current follow-up validation record

This record supersedes pending implementation statements above for the specifically listed cases and source revisions. Historical results remain separate. Suite counts are read from archived terminal traces; a nonzero combined command is reported as such even when an independent owning suite completed successfully. Failed or interrupted cases are not credited.

| Owning target | Latest recorded result | Source | Command evidence | Actual combined exit |
| --- | --- | --- | --- | --- |
| `ydb/core/grpc_services/ut` | 32 GOOD | `913c0cb032a1` | `final-review-create-rpc-green-abort-red-parallel100` | 1 |
| `ydb/core/kqp/executer_actor/ut` | 146 GOOD | `9a2b73490817` | `final-fixtures-token-lifetime-green-parallel100` | 0 |
| `ydb/core/kqp/finalize_script_service/ut` | 11 GOOD | `1373e723886e` | `task8-10-acquisition-recovery-parallel100` | 1 |
| `ydb/core/kqp/provider/ut` | 188 GOOD | `9a2b73490817` | `final-schema-provider-green-parallel100` | 0 |
| `ydb/core/security/iam_delegation/lifecycle/ut` | 29 GOOD | `1373e723886e` | `task8-10-acquisition-recovery-parallel100` | 1 |
| `ydb/core/security/iam_delegation/tablet/ut` | 123 GOOD | `969b11b16a9d` | `task9-10-buildfix-green-parallel100` | 10 |
| `ydb/core/security/iam_delegation/ut` | 73 GOOD | `9a2b73490817` | `final-fixtures-token-lifetime-green-parallel100` | 0 |
| `ydb/core/tx/schemeshard/ut_secret` | 34 GOOD | `9a2b73490817` | `final-schema-provider-green-parallel100` | 0 |
| `ydb/library/yql/providers/common/token_accessor/client/ut` | 3 GOOD | `8892e3bce7b2` | `final-review-full-green-parallel100` | 1 |
| `ydb/library/yql/providers/s3/actors/ut` | 54 GOOD | `9a2b73490817` | `final-fixtures-token-lifetime-green-parallel100` | 0 |
| `ydb/library/yql/providers/s3/credentials/ut` | 4 GOOD | `8892e3bce7b2` | `final-review-full-green-parallel100` | 1 |
| `ydb/library/yql/providers/solomon/actors/ut` | 32 GOOD | `a746ead6f12b` | `final-review-consumer-green-executor-red-parallel100` | 254 |
| `ydb/services/scheme_secret/ut/fast` | 48 GOOD | `1373e723886e` | `task8-10-acquisition-recovery-parallel100` | 1 |
| `ydb/services/scheme_secret/ut/slow` | 2 GOOD | `8892e3bce7b2` | `final-review-full-green-parallel100` | 1 |

Evidence collection: `external-secrets-recovery/spec-implementation-2026-10-15`. Every label identifies its exact `.command`, `.commit`, `.exit`, `.result.json`, `.source-manifest.json`, `.results-manifest.json` and dereferenced `.results/` tree. Source hashes were verified at the end of each invocation before any checkout mutation. The a746 interrupted run was archived directly from its fresh build root because its checkout symlink still pointed to an older run.

Exact commands for the table:

- `task7-green-buildfix-parallel100`: `./ya make --build relwithdebinfo -tA ydb/core/security/iam_delegation/tablet/ut --threads=100 --link-threads=2 --test-threads=1 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/grpc_services/ut -F \*Iam\* --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 0.
- `task8-10-acquisition-recovery-parallel100`: `./ya make --build relwithdebinfo -tA ydb/services/scheme_secret/ut/fast --threads=100 --link-threads=2 --test-threads=1 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/security/iam_delegation/lifecycle/ut ydb/core/security/iam_delegation/ut ydb/core/kqp/finalize_script_service/ut ydb/core/security/iam_delegation/tablet/ut ydb/core/kqp/executer_actor/ut --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 1.
- `task9-10-buildfix-green-parallel100`: `./ya make --build relwithdebinfo -tA ydb/core/security/iam_delegation/tablet/ut --threads=100 --link-threads=2 --test-threads=1 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/kqp/executer_actor/ut --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 10.
- `final-review-create-rpc-green-abort-red-parallel100`: `./ya make --build relwithdebinfo -tA ydb/core/kqp/executer_actor/ut --threads=100 --link-threads=2 --test-threads=4 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/grpc_services/ut ydb/library/yql/providers/s3/actors/ut ydb/library/yql/providers/solomon/actors/ut -F KqpIamDelegationSecretOrchestrator::\* -F DescribeSecretIamDelegation::\* -F ListDatabaseIamDelegations::\* -F TArrow\* -F TS3\* -F S3\* -F \*Solomon\* -F TMetricsEncoder\* --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 1.
- `final-review-consumer-green-executor-red-parallel100`: `./ya make --build relwithdebinfo -tA ydb/library/yql/providers/common/token_accessor/client/ut --threads=100 --link-threads=2 --test-threads=4 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/kqp/executer_actor/ut ydb/library/yql/providers/s3/actors/ut ydb/library/yql/providers/solomon/actors/ut ydb/library/yql/providers/s3/credentials/ut -F AsynchronousCredentialOwnership::\* -F KqpIamDelegationSecretOrchestrator::\* -F TArrow\* -F TS3\* -F S3\* -F \*Solomon\* -F TMetricsEncoder\* -F TCredentials::\* --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 254.
- `final-review-full-green-parallel100`: `./ya make --build relwithdebinfo -tA ydb/library/yql/providers/common/token_accessor/client/ut --threads=100 --link-threads=2 --test-threads=4 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/kqp/executer_actor/ut ydb/library/yql/providers/s3/actors/ut ydb/library/yql/providers/s3/credentials/ut ydb/core/kqp/provider/ut ydb/core/tx/schemeshard/ut_secret ydb/services/scheme_secret/ut/slow --ignore-recurses --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 1.
- `final-fixtures-token-lifetime-green-parallel100`: `./ya make --build relwithdebinfo -tA ydb/core/kqp/executer_actor/ut --threads=100 --link-threads=1 --test-threads=4 --cache-size=110GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/library/yql/providers/s3/actors/ut ydb/core/security/iam_delegation/ut --ignore-recurses --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 0.
- `final-schema-provider-green-parallel100`: `./ya make --build relwithdebinfo -tA ydb/core/kqp/provider/ut --threads=100 --link-threads=2 --test-threads=4 --cache-size=50GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 ydb/core/tx/schemeshard/ut_secret ydb/core/driver_lib/run --ignore-recurses --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; exit 0.

The tablet run includes independent-model chaos seeds **1, 330 and 20261013**, exact lease and committed-ACK replay boundaries, cold-page transaction retries, generation/identity reuse and physical storage reopening. The KQP tenant-deletion case deletes/recreates the real tenant identity, retains service-owned lifecycle state, reopens the actual PDisk in a fresh runtime and controls the post-reopen schema/IAM replies. It does not claim a complete cluster reopen or production tablet placement.

Live IAM sibling-referrer behavior, production storage placement and control-plane rollout remain deployment validation. Local tests establish saved-subject forwarding, per-reference authorization, account-key cache sharing and retained uncertain state; they do not infer remote authorization guarantees from a mock. No TSan result is claimed.

### Driver build and independent review closure

Standalone driver command: `./ya make --build relwithdebinfo ydb/core/driver_lib/run --threads=100 --link-threads=2 --test-threads=1 --cache-size=50GiB --bazel-remote-base-uri http://\[64:ff9b::9ea0:93d3\]:8081 --ignore-recurses --keep-going -DLDFLAGS=-Wl\,--compress-debug-sections=zlib`; actual exit **0**, source `9a2b734908179612c35d651e810560572ce085e4`. The materialized `ydb/core/driver_lib/run/librun.a` is 51502696 bytes, SHA-256 `2a0d0c330b8ecbdda021abccb316b97ddac091416b8cb8a83bf6f6c1ac8a7d18`. Evidence: `final-driver-build-parallel100.result.json` and `.driver-build.json`. The earlier mixed test command is credited only for provider/SchemeShard tests.

The complete feature review at `1373e723` read 220 files through independent Strict, Defender and Judge roles. The final production delta review at `8892e3bc` read all 25 complete changed files, with the same independent role sequence. B1/B2 authorization and executor-lifetime findings were fixed; D1/D2 fixture corrections passed at `9a2b7349`. The Judge independently verified the three-test-file final delta, 224 source hashes and 652 archived result files; no new finding remained. Review collections are `reviews/2026-10-16-final-1373` and `reviews/2026-10-16-final-delta-8892`, including `resolution-9a2b.json`.

A separate strict OOP review covered ownership, modularity, testability, coroutine frames and shared-state boundaries. Its A1/A2/F1/F2/R1 findings are resolved with regression evidence. The final report at `reviews/2026-10-16-oop-final-9a2b` independently verifies all 273 current KQP/S3/IAM results; no new finding remains. `architecture.md` SHA-256: `16fad3fd86179a9ce8f01565ec73ab5efd18191b51930450234c49fa6328a118`; `findings.json`: `499092e77558d7054894f23e221d5141d287e673ea278fc03533ff015d84fbae`. These source reviews do not claim live-IAM, sanitizer or production-placement validation.

The latest two test invocations at `9a2b7349` passed **495/495** cases with exit 0 (KQP 146, S3 actors 54, IAM 73, provider 188, SchemeShard 34). Other required owning-suite evidence is retained in the table above at its tested source revision. The independent-tablet suite includes 123 passing cases and the recorded chaos/reopen scenarios. All build sequences started with 100 compiler jobs; later link concurrency and cache size were adjusted from measured disk use. Host earlyoom remained active and enabled.

### Final specification reconciliation

All five native OpenSpec projects and all thirteen test-owned specifications passed strict validation with zero issues. The reconciliation checked 451 relative links, 323 quoted contract citations and 271 executable/source table mappings (including 36 macro-expanded durability cases), with no unresolved item. The seven feature contracts contain 64 requirements and 111 scenarios. Evidence: `final-delivery-native-*.json`, `final-delivery-standalone-validation.json` and `final-delivery-document-integrity.json`.

A further independent Strict/Defender/Judge documentation review read all 28 draft documents and resolved DOC1/DOC2: warning coverage explicitly excludes configured-threshold integration; terminal IAM denial and retryable platform-credential failure have separate mappings. Authoritative feature contracts were preserved. Review collection: `reviews/2026-10-16-final-docs-draft`; final correction acceptance is `resolution.md`, SHA-256 `03fe32822bcdc3da0e5f0a50835832fc0170db5f9fc9333f8642063d75dcfaff`. All role artifacts passed the phase-integrity checker with retained digests. Final delivery changes only documentation; compiled inputs retain tested source `9a2b734908179612c35d651e810560572ce085e4`.
