# External-secrets verification

Task5 base: `67a530cc28eb2269e75fe0b83f2bacf73a1cbe2a`; C1 source commit: `8795e8bd1b186fc53051e5dd79126d9347a0e1b3`. Task5 relocates three existing libraries and their tests without changing symbols, persistent formats or assertions. Its final commit is recorded by version control history for this file. Historical recovery/C1 results used the former lifecycle and tx/tablet paths and do not validate relocated sources.

Run the following exact commands from the repository root, sequentially:

| Command | Exact exit | Result |
| --- | --- | --- |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/ut` | 0 | 57/57 GOOD |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/lifecycle/ut` | 0 | 22/22 GOOD |
| `./ya make --build relwithdebinfo -tA --ignore-recurses --link-threads=1 ydb/core/security/iam_delegation/tablet/ut` | 0 | 87/87 GOOD |

The external evidence collection is `external-secrets-recovery/review-fixes-2026-10-14/task5`. It contains full `service.log`/`service.exit`, `lifecycle.log`/`lifecycle.exit` and `tablet.log`/`tablet.exit`, dereferenced `service-results`, `lifecycle-results` and `tablet-results`, source SHA-256 manifests, strict OpenSpec validation, case/source/target mappings and normative-contract equivalence. External artifact names are provenance identifiers; they are not branch-local links. Detailed SDD bookkeeping is intentionally ignored and is not required to resolve any document in the pushed branch.

All seven authoritative feature contracts retain 43 requirements and 67 scenarios byte-for-byte. The twelve [test-owned specifications](README.md#test-owned-scenarios) map existing coverage intent separately from run results. They do not assert that unrelated consumer targets passed on this revision.

Pending gates: Task4 relocated service/lifecycle/tablet, KQP executer, scheme_secret fast, S3 actors/credentials, changed gRPC/finalizer UT consumers and production-driver integration; Task3/C2 read-side streaming hook cancellation, repeated/concurrent/reentrant cancellation and cancellation before credentials resolve; and two Task3 direct S3 wrapper regressions for retained provider lifetime and future exception propagation. The wrapper has no cancellation API; late request suppression remains owned by read actors.

Provider, SchemeShard and Solomon received coverage documentation only in Task5. Their current executable mappings describe source coverage; existing run evidence is historical and they are not additional required Task4 reruns.

The three relocated targets completed 166/166 GOOD with exact exits 0. The 47-file product/build input SHA-256 manifest `build-source-hashes.json` has digest `31b47be03279fd44c897f2b7a2b3d81629b7bc3ca1c16d68ade08958a67ca229`; inputs matched before and after all runs. The controller independently compared 42 affected C++/header/proto sources to the base after only include/import path substitutions (`controller-source-equivalence.json`). Detailed result/artifact manifests are `test-results-summary.json` and the three `*-artifact-hashes.json` files in the external collection.
