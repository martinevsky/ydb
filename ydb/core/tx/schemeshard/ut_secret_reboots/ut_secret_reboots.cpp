#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

namespace {
    using namespace NSchemeShardUT_Private;
    using NIamDelegation::TEvIamDelegation;

    // The IAM delegation service of the node, stood in for by an edge actor that receives the revoke requests
    // of the schemeshard's revokers.
    TActorId RegisterFakeIamDelegationService(TTestActorRuntime& runtime) {
        const TActorId edge = runtime.AllocateEdgeActor();
        runtime.RegisterService(NIamDelegation::MakeIamDelegationServiceId(), edge);
        return edge;
    }

    // A revocation still in the outbox is resumed by the schemeshard after a restart: whatever reboot happened
    // during the operation, a fresh schemeshard asks to revoke the delegation.
    void ExpectRevokeAfterRestart(TTestActorRuntime& runtime, const TActorId& iam, const TString& referrerId) {
        const TActorId sender = runtime.AllocateEdgeActor();
        RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);
        const auto revoke = runtime.GrabEdgeEvent<TEvIamDelegation::TEvRevokeDelegation>(iam);
        UNIT_ASSERT(revoke);
        UNIT_ASSERT_VALUES_EQUAL(revoke->Get()->Spec.ReferrerId, referrerId);
    }

    void ExpectEqualSecretDescription(
        const NKikimrScheme::TEvDescribeSchemeResult& describeResult,
        const TString& name,
        const TString& value,
        const ui64 version
    ) {
        UNIT_ASSERT(describeResult.HasPathDescription());
        UNIT_ASSERT(describeResult.GetPathDescription().HasSecretDescription());
        const auto& secretDescription = describeResult.GetPathDescription().GetSecretDescription();
        UNIT_ASSERT_VALUES_EQUAL(secretDescription.GetName(), name);
        UNIT_ASSERT_VALUES_EQUAL(secretDescription.GetValue(), value);
        UNIT_ASSERT_VALUES_EQUAL(secretDescription.GetVersion(), version);
    }

    NKikimrScheme::TEvDescribeSchemeResult DescribePathWithSecretValue(
        TTestActorRuntime& runtime,
        const TString& path
    ) {
        NKikimrSchemeOp::TDescribeOptions opts;
        opts.SetReturnSecretValue(true);
        return DescribePath(runtime, path, opts);
    }
}

Y_UNIT_TEST_SUITE(TSchemeShardSecretTestReboots) {
    Y_UNIT_TEST(CreateSecret) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                TestMkDir(runtime, ++t.TxId, "/MyRoot", "dir");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestCreateSecret(runtime, ++t.TxId, "/MyRoot/dir",
                R"(
                    Name: "test-secret"
                    Value: "test-value"
                )"
            );
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                const auto describeResult = DescribePathWithSecretValue(runtime, "/MyRoot/dir/test-secret");
                TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSecret});
            }
        });
    }

    Y_UNIT_TEST(AlterSecret) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                TestMkDir(runtime, ++t.TxId, "/MyRoot", "dir");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);

                TestCreateSecret(runtime, ++t.TxId, "/MyRoot/dir",
                    R"(
                        Name: "test-secret"
                        Value: "test-value-0"
                    )"
                );
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestAlterSecret(runtime, ++t.TxId, "/MyRoot/dir",
                R"(
                    Name: "test-secret"
                    Value: "test-value-1"
                )"
            );
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                const auto describeResult = DescribePathWithSecretValue(runtime, "/MyRoot/dir/test-secret");
                TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSecret});
                ExpectEqualSecretDescription(describeResult, "test-secret", "test-value-1", 1);
            }
        });
    }

    Y_UNIT_TEST(DropSecret) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                TestMkDir(runtime, ++t.TxId, "/MyRoot", "dir");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
                TestCreateSecret(runtime, ++t.TxId, "/MyRoot/dir",
                    R"(
                        Name: "test-secret"
                        Value: "test-value"
                    )"
                );
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
                TestLs(runtime, "/MyRoot/dir/test-secret", false, NLs::PathExist);
            }

            TestDropSecret(runtime, ++t.TxId, "/MyRoot/dir", "test-secret");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                TestLs(runtime, "/MyRoot/dir/test-secret", false, NLs::PathNotExist);
            }
        });
    }

    Y_UNIT_TEST(CreateIamDelegationSecret) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(true);
                TestMkDir(runtime, ++t.TxId, "/MyRoot", "dir");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestCreateSecret(runtime, ++t.TxId, "/MyRoot/dir",
                R"(
                    Name: "sa-secret"
                    Type: SECRET_TYPE_IAM_DELEGATION
                    IamDelegation {
                        ServiceAccountId: "aje-sa-1"
                        CloudId: "b1g-cloud-1"
                        ReferrerId: "referrer-1"
                    }
                )"
            );
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                const auto describeResult = DescribePath(runtime, "/MyRoot/dir/sa-secret");
                TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSecret});
                const auto& secret = describeResult.GetPathDescription().GetSecretDescription();
                UNIT_ASSERT_EQUAL(secret.GetType(), NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION);
                UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "aje-sa-1");
                UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetReferrerId(), "referrer-1");
            }
        });
    }

    Y_UNIT_TEST(AlterIamDelegationSecret) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            TActorId iam;
            {
                TInactiveZone inactive(activeZone);
                runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(true);
                iam = RegisterFakeIamDelegationService(runtime);
                TestMkDir(runtime, ++t.TxId, "/MyRoot", "dir");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);

                TestCreateSecret(runtime, ++t.TxId, "/MyRoot/dir",
                    R"(
                        Name: "sa-secret"
                        Type: SECRET_TYPE_IAM_DELEGATION
                        IamDelegation {
                            ServiceAccountId: "aje-sa-1"
                            CloudId: "b1g-cloud-1"
                            ReferrerId: "referrer-1"
                        }
                    )"
                );
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            // the replacement is staged first (the secret keeps its delegation) and promoted afterwards
            TestAlterSecret(runtime, ++t.TxId, "/MyRoot/dir",
                R"(
                    Name: "sa-secret"
                    Type: SECRET_TYPE_IAM_DELEGATION
                    IamDelegation {
                        ServiceAccountId: "aje-sa-2"
                        CloudId: "b1g-cloud-1"
                        ReferrerId: "referrer-2"
                    }
                    IamDelegationAlter: IAM_DELEGATION_ALTER_STAGE
                )"
            );
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                const auto describeResult = DescribePath(runtime, "/MyRoot/dir/sa-secret");
                const auto& secret = describeResult.GetPathDescription().GetSecretDescription();
                UNIT_ASSERT_VALUES_EQUAL(secret.GetVersion(), 1u);
                UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetReferrerId(), "referrer-1");
                UNIT_ASSERT_VALUES_EQUAL(secret.GetPendingIamDelegation().GetReferrerId(), "referrer-2");
            }

            TestAlterSecret(runtime, ++t.TxId, "/MyRoot/dir",
                R"(
                    Name: "sa-secret"
                    Type: SECRET_TYPE_IAM_DELEGATION
                    IamDelegation {
                        ReferrerId: "referrer-2"
                    }
                    IamDelegationAlter: IAM_DELEGATION_ALTER_PROMOTE
                )"
            );
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                const auto describeResult = DescribePathWithSecretValue(runtime, "/MyRoot/dir/sa-secret");
                TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSecret});
                const auto& secret = describeResult.GetPathDescription().GetSecretDescription();
                UNIT_ASSERT_VALUES_EQUAL(secret.GetName(), "sa-secret");
                UNIT_ASSERT_VALUES_EQUAL(secret.GetVersion(), 2u);
                UNIT_ASSERT(secret.GetValue().empty()); // no value even when explicitly requested
                UNIT_ASSERT_EQUAL(secret.GetType(), NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION);
                UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "aje-sa-2");
                UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetCloudId(), "b1g-cloud-1");
                UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetReferrerId(), "referrer-2");
                UNIT_ASSERT(!secret.HasPendingIamDelegation());
                // the promotion put the previous delegation into the outbox
                ExpectRevokeAfterRestart(runtime, iam, "referrer-1");
            }
        });
    }

    Y_UNIT_TEST(DropIamDelegationSecret) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            TActorId iam;
            {
                TInactiveZone inactive(activeZone);
                runtime.GetAppData().FeatureFlags.SetEnableIamDelegationSecrets(true);
                iam = RegisterFakeIamDelegationService(runtime);
                TestMkDir(runtime, ++t.TxId, "/MyRoot", "dir");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
                TestCreateSecret(runtime, ++t.TxId, "/MyRoot/dir",
                    R"(
                        Name: "sa-secret"
                        Type: SECRET_TYPE_IAM_DELEGATION
                        IamDelegation {
                            ServiceAccountId: "aje-sa-1"
                            CloudId: "b1g-cloud-1"
                            ReferrerId: "referrer-1"
                        }
                    )"
                );
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
                TestLs(runtime, "/MyRoot/dir/sa-secret", false, NLs::PathExist);
            }

            TestDropSecret(runtime, ++t.TxId, "/MyRoot/dir", "sa-secret");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                TestLs(runtime, "/MyRoot/dir/sa-secret", false, NLs::PathNotExist);
                ExpectRevokeAfterRestart(runtime, iam, "referrer-1");
            }
        });
    }
}
