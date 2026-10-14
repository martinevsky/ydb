# External-secrets verification

Task5 base: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`; C1 source commit: `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 relocates three existing libraries and their tests without changing symbols, persistent formats or assertions. Its final commit is recorded by version control history for this file. Historical recovery/C1 results used the former lifecycle and tx/tablet paths and do not validate relocated sources.

Task5 implementation commit: `87f8733958c047bd0429e2b8d627b00404b43c75`. Independent task review approved both specification compliance and code quality with no findings, including verification of archived results, source hashes and scenario mappings. This approval covers the relocation and test specifications; final integration and the C2 task review remain separate gates below.

Run the following exact commands from the repository root, sequentially:

| Command | Exact exit | Result |
| --- | --- | --- |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/ut` | 0 | 57/57 GOOD |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/lifecycle/ut` | 0 | 22/22 GOOD |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/tablet/ut` | 0 | 87/87 GOOD |

The external evidence collection is `external-secrets-recovery/review-fixes-2026-10-14/task5`. It contains full `service.log`/`service.exit`, `lifecycle.log`/`lifecycle.exit` and `tablet.log`/`tablet.exit`, dereferenced `service-results`, `lifecycle-results` and `tablet-results`, source SHA-256 manifests, strict OpenSpec validation, case/source/target mappings and normative-contract equivalence. External artifact names are provenance identifiers; they are not branch-local links. Detailed SDD bookkeeping is intentionally ignored and is not required to resolve any document in the pushed branch.

All seven authoritative feature contracts retain 43 requirements and 67 scenarios byte-for-byte. The twelve [test-owned specifications](README.md#test-owned-scenarios) map existing coverage intent separately from run results. They do not assert that unrelated consumer targets passed on this revision.

Pending gates: Task3/C2 independent task review; Task4 relocated service/lifecycle/tablet, KQP executer, scheme_secret fast, S3 actors/credentials, changed gRPC/finalizer UT consumers and production-driver integration; and independent whole-fixes review. C2 and both direct wrapper regressions are implemented and locally verified below. The wrapper has no cancellation API; late request suppression remains owned by read actors.

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
