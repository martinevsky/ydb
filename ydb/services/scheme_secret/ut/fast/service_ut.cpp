#include <ydb/core/kqp/common/events/script_executions.h>
#include <ydb/services/scheme_secret/secret_credentials.h>
#include <ydb/services/scheme_secret/service.h>

#include <yql/essentials/providers/common/structured_token/yql_structured_token.h>
#include <yql/essentials/providers/common/structured_token/yql_token_builder.h>

#include <ydb/services/scheme_secret/ut/common/helpers.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

#include <library/cpp/testing/unittest/registar.h>

#include <thread>

namespace NKikimr::NSecret {

using NKqp::TKikimrRunner;
using NKqp::TKikimrSettings;
using TDescriptionPromise = NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription>;

Y_UNIT_TEST_SUITE(DescribeSchemaSecretsService) {
    Y_UNIT_TEST(GetNewValue) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TString secretName = "/Root/secret-name";
        TString secretValue = "secret-value";
        CreateSchemaSecret(secretName, secretValue, session);

        for (int i = 0; i < 3; ++i) {
            auto promise = ResolveSecret("/Root/secret-name", kikimr);
            AssertSecretValue(secretValue, promise);
        }
    }

    Y_UNIT_TEST(GetUpdatedValue) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TString secretName = "/Root/secret-name";
        TString secretValue = "secret-value";
        CreateSchemaSecret(secretName, secretValue, session);

        auto promise = ResolveSecret("/Root/secret-name", kikimr);
        AssertSecretValue(secretValue, promise);

        for (int i = 0; i < 3; ++i) {
            TString newSecretValue = secretValue + "-" + ToString(i);
            AlterSchemaSecret(secretName, newSecretValue, session);

            auto promise = ResolveSecret("/Root/secret-name", kikimr);
            AssertSecretValue(newSecretValue, promise);
        }
    }

    Y_UNIT_TEST(GetIamDelegationValue) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        RegisterFakeIamDelegatedTokenService(runtime, {{"aje-1", "b1g-1"}});

        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");

        // the value of a delegation secret is the current token of the delegated service account,
        // obtained from the token service for every read; readers see a plain value
        for (int i = 0; i < 3; ++i) {
            AssertSecretValue("delegated-token-b1g-1/aje-1", ResolveSecret("/Root/sa-secret", kikimr));
        }

        // mixed with value secrets, in the order of the request
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSchemaSecret("/Root/plain-secret", "plain-value", session);
        AssertSecretValues({"plain-value", "delegated-token-b1g-1/aje-1", "plain-value", "delegated-token-b1g-1/aje-1"},
            ResolveSecrets({"/Root/plain-secret", "/Root/sa-secret", "/Root/plain-secret", "/Root/sa-secret"}, kikimr));
    }

    Y_UNIT_TEST(GetIamDelegationUpdatedValue) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        RegisterFakeIamDelegatedTokenService(runtime, {{"aje-1", "b1g-1"}, {"aje-2", "b1g-1"}, {"aje-3", "b1g-1"}, {"aje-4", "b1g-1"}});

        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");
        AssertSecretValue("delegated-token-b1g-1/aje-1", ResolveSecret("/Root/sa-secret", kikimr));

        // after ALTER a read returns a token of the new service account
        for (int i = 0; i < 3; ++i) {
            const TString serviceAccountId = TStringBuilder() << "aje-" << (i + 2);
            AlterIamDelegationSecretDirect(runtime, "/Root/sa-secret", serviceAccountId, "b1g-1", TStringBuilder() << "referrer-" << (i + 2));
            AssertSecretValue(TStringBuilder() << "delegated-token-b1g-1/" << serviceAccountId, ResolveSecret("/Root/sa-secret", kikimr));
        }
    }

    Y_UNIT_TEST(GetIamDelegationValueTokenRefused) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        RegisterFakeIamDelegatedTokenService(runtime, {});

        // the token service cannot obtain a token: the read fails with its error and the secret path
        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");
        AssertErrorContains(ResolveSecret("/Root/sa-secret", kikimr), "Cannot obtain the IAM token of secret `/Root/sa-secret`", Ydb::StatusIds::UNAUTHORIZED);
        AssertErrorContains(ResolveSecret("/Root/sa-secret", kikimr), "No delegation for service account aje-1", Ydb::StatusIds::UNAUTHORIZED);
    }

    // A retryable failure of the token service (IAM unreachable past the token service's own retries) is retried
    // by the secret service with the reader's retry policy, so a reader that does not retry itself is not stopped
    // by an outage
    Y_UNIT_TEST(GetIamDelegationValueRetriedWhileUnavailable) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        auto tokens = RegisterFakeIamDelegatedTokenService(runtime, {{"aje-1", "b1g-1"}});
        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");

        // bounded by the number of retries only, never by elapsed time
        const auto quickPolicy = IRetryPolicy<>::GetExponentialBackoffPolicy(
            [](){ return ERetryErrorClass::ShortRetry; },
            /* minDelay */ TDuration::MilliSeconds(10),
            /* minLongRetryDelay */ TDuration::MilliSeconds(10),
            /* maxDelay */ TDuration::MilliSeconds(50),
            /* maxRetries */ 5,
            /* maxTime */ TDuration::Max());

        // three UNAVAILABLE answers, then a token: the read succeeds after four requests
        tokens->UnavailableAnswers = 3;
        AssertSecretValue("delegated-token-b1g-1/aje-1", ResolveSecret("/Root/sa-secret", kikimr, nullptr, {.RetryPolicy = quickPolicy}));
        UNIT_ASSERT_VALUES_EQUAL(tokens->Calls.load(), 4u);

        // the policy is exhausted: the last failure is returned
        tokens->Calls = 0;
        tokens->UnavailableAnswers = 100;
        AssertErrorContains(ResolveSecret("/Root/sa-secret", kikimr, nullptr, {.RetryPolicy = quickPolicy}),
            "Cannot obtain the IAM token of secret `/Root/sa-secret`", Ydb::StatusIds::UNAVAILABLE);
        UNIT_ASSERT_VALUES_EQUAL(tokens->Calls.load(), 6u); // the first request plus maxRetries

        // without a policy a short default applies (MakeShortRetryPolicy: 10 retries within 10 s): one outage,
        // retried after 100 ms, is survived
        tokens->Calls = 0;
        tokens->UnavailableAnswers = 1;
        AssertSecretValue("delegated-token-b1g-1/aje-1", ResolveSecret("/Root/sa-secret", kikimr));
        UNIT_ASSERT_VALUES_EQUAL(tokens->Calls.load(), 2u);
    }

    // A refused token (the delegation is gone) is final: no retries even with a policy
    Y_UNIT_TEST(GetIamDelegationValueRefusedIsNotRetried) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        auto tokens = RegisterFakeIamDelegatedTokenService(runtime, {});
        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");

        AssertErrorContains(ResolveSecret("/Root/sa-secret", kikimr, nullptr, {.RetryPolicy = MakeLongRetryPolicy()}),
            "No delegation for service account aje-1", Ydb::StatusIds::UNAUTHORIZED);
        UNIT_ASSERT_VALUES_EQUAL(tokens->Calls.load(), 1u);
    }

    // Many readers of one delegation secret during a one-shot outage of the token service: every read is retried
    // and succeeds; the token service is asked once per read plus the retry of the one that hit the outage
    Y_UNIT_TEST(GetIamDelegationValueConcurrentReadersDuringOutage) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        auto tokens = RegisterFakeIamDelegatedTokenService(runtime, {{"aje-1", "b1g-1"}});
        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");

        tokens->UnavailableAnswers = 1;
        constexpr ui32 readers = 8;
        TVector<TDescriptionPromise> promises;
        for (ui32 i = 0; i < readers; ++i) {
            promises.push_back(ResolveSecret("/Root/sa-secret", kikimr));
        }
        for (auto& promise : promises) {
            AssertSecretValue("delegated-token-b1g-1/aje-1", promise);
        }
        UNIT_ASSERT_VALUES_EQUAL(tokens->Calls.load(), readers + 1);
    }

    Y_UNIT_TEST(GetIamDelegationValueWithoutTokenService) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);

        // no token service on the node (the feature was never enabled there): a clear error, no hang
        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");
        AssertErrorContains(ResolveSecret("/Root/sa-secret", kikimr), "IAM delegation secrets are not enabled on this node", Ydb::StatusIds::UNAVAILABLE);
    }

    Y_UNIT_TEST(GetUnexistingValue) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto promise = ResolveSecret("/Root/secret-not-exist", kikimr);

        AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-not-exist` not found\n");
    }

    Y_UNIT_TEST(GetDroppedValue) {
        class TTestSecretUpdateListener : public TDescribeSchemaSecretsService::ISecretUpdateListener {
        public:
            NThreading::TPromise<TString> DeletionPromise = NThreading::NewPromise<TString>();

        public:
            void HandleNotifyDelete(const TString& secretName) override {
                Y_ENSURE(!DeletionPromise.HasValue()); // only one call of HandleNotifyDelete is expected
                DeletionPromise.SetValue(secretName);
            }
        };

        TKikimrSettings settings;
        auto secretUpdateListener = MakeHolder<TTestSecretUpdateListener>();
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            secretUpdateListener.Get(),
            /* schemeCacheStatusGetter */ nullptr,
            /* schemeShardStatusGetter */ nullptr);
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TString secretName = "/Root/secret-name";
        TString secretValue = "secret-value";
        CreateSchemaSecret(secretName, secretValue, session);

        auto promise = ResolveSecret("/Root/secret-name", kikimr);
        AssertSecretValue(secretValue, promise);

        DropSchemaSecret(secretName, session);
        UNIT_ASSERT_VALUES_EQUAL("/Root/secret-name", secretUpdateListener->DeletionPromise.GetFuture().GetValueSync());

        promise = ResolveSecret("/Root/secret-name", kikimr);
        AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-name` not found\n");

        secretValue += "-updated";
        CreateSchemaSecret(secretName, secretValue, session);

        promise = ResolveSecret("/Root/secret-name", kikimr);
        AssertSecretValue(secretValue, promise);
    }

    Y_UNIT_TEST(GetInParallel) {
        static const int SECRETS_CNT = 5;
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        // new values
        std::vector<std::pair<TString, TString>> secrets;
        for (int i = 0; i < SECRETS_CNT; ++i) {
            secrets.push_back({"/Root/secret-name-" + ToString(i), "secret-value-" + ToString(i)});
            CreateSchemaSecret(secrets.back().first, secrets.back().second, session);
        }
        std::vector<TDescriptionPromise> promises;
        for (const auto& [secretName, secretValue] : secrets) {
            promises.push_back(ResolveSecret(secretName, kikimr));
        }

        for (int i = 0; i < SECRETS_CNT; ++i) {
            AssertSecretValue(secrets[i].second, promises[i]);
        }

        // altered values
        promises.clear();
        for (int i = 0; i < SECRETS_CNT; ++i) {
            secrets[i].second += "-new";
            AlterSchemaSecret(secrets[i].first, secrets[i].second, session);
        }
        for (const auto& [secretName, secretValue] : secrets) {
            promises.push_back(ResolveSecret(secretName, kikimr));
        }

        for (int i = 0; i < SECRETS_CNT; ++i) {
            AssertSecretValue(secrets[i].second, promises[i]);
        }
    }

    Y_UNIT_TEST(GetSameValueMultipleTimes) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString secretName = "/Root/secret-name";
        const TString secretValue = "secret-value";
        CreateSchemaSecret(secretName, secretValue, session);

        auto promise = ResolveSecrets({secretName, secretName}, kikimr);
        AssertSecretValues({secretValue, secretValue}, promise);
    }

    Y_UNIT_TEST(FailWithoutGrants) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);

        const TString secretName = "/Root/secret-name";
        const TString secretValue = "secret-value";
        auto adminSession = kikimr.GetTableClient(NYdb::NTable::TClientSettings().AuthToken("root@builtin"))
            .CreateSession().GetValueSync().GetSession();

        CreateSchemaSecret(secretName, secretValue, adminSession);

        auto promise = ResolveSecret(secretName, kikimr, GetUserToken("root@builtin"));
        AssertSecretValue(secretValue, promise);

        const auto userToken = GetUserToken("user@builtin");
        { // assert no grants by default
            auto promise = ResolveSecret("/Root/secret-name", kikimr, userToken);
            AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-name` not found\n");
        }

        // provide grants
        const auto grantResult = adminSession.ExecuteSchemeQuery(
            Sprintf("GRANT 'ydb.granular.select_row' ON `%s` TO `%s`;", secretName.data(), "user@builtin")
        ).GetValueSync();
        UNIT_ASSERT_C(grantResult.GetStatus() == NYdb::EStatus::SUCCESS, grantResult.GetIssues().ToString());

        { // assert grants are ok
            auto promise = ResolveSecret("/Root/secret-name", kikimr, userToken);
            AssertSecretValue(secretValue, promise);
        }

        // revoke grants
        const auto revokeResult = adminSession.ExecuteSchemeQuery(
            Sprintf("REVOKE 'ydb.granular.select_row' ON `%s` FROM `%s`;", secretName.data(), "user@builtin")
        ).GetValueSync();
        UNIT_ASSERT_C(revokeResult.GetStatus() == NYdb::EStatus::SUCCESS, grantResult.GetIssues().ToString());

        { // assert no grants after revoking
            auto promise = ResolveSecret("/Root/secret-name", kikimr, userToken);
            AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-name` not found\n");
        }
    }

    Y_UNIT_TEST(GroupGrants) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);

        const TString secretName = "/Root/secret-name";
        const TString secretValue = "secret-value";
        auto adminSession = kikimr.GetTableClient(NYdb::NTable::TClientSettings().AuthToken("root@builtin"))
            .CreateSession().GetValueSync().GetSession();

        CreateSchemaSecret(secretName, secretValue, adminSession);

        auto promise = ResolveSecret(secretName, kikimr, GetUserToken("root@builtin"));
        AssertSecretValue(secretValue, promise);

        const auto userToken = GetUserToken("user@builtin", {"group"});
        { // assert no grants by default
            auto promise = ResolveSecret("/Root/secret-name", kikimr, userToken);
            AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-name` not found\n");
        }

        const auto createGroupResult = adminSession.ExecuteSchemeQuery(
            Sprintf("CREATE GROUP `group` WITH USER `user@builtin`;")
        ).GetValueSync();
        UNIT_ASSERT_C(createGroupResult.GetStatus() == NYdb::EStatus::SUCCESS, createGroupResult.GetIssues().ToString());

        const auto grantResult = adminSession.ExecuteSchemeQuery(
            Sprintf("GRANT 'ydb.granular.select_row' ON `%s` TO `%s`;", secretName.data(), "group")
        ).GetValueSync();
        UNIT_ASSERT_C(grantResult.GetStatus() == NYdb::EStatus::SUCCESS, grantResult.GetIssues().ToString());

        { // assert group grants are ok
            auto promise = ResolveSecret("/Root/secret-name", kikimr, userToken);
            AssertSecretValue(secretValue, promise);
        }

        // revoke grants
        const auto revokeResult = adminSession.ExecuteSchemeQuery(
            Sprintf("REVOKE 'ydb.granular.select_row' ON `%s` FROM `%s`;", secretName.data(), "group")
        ).GetValueSync();
        UNIT_ASSERT_C(revokeResult.GetStatus() == NYdb::EStatus::SUCCESS, grantResult.GetIssues().ToString());

        { // assert no grants after revoking
            auto promise = ResolveSecret("/Root/secret-name", kikimr, userToken);
            AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-name` not found\n");
        }
    }

    Y_UNIT_TEST(BatchRequest) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);

        const TString secretName1 = "/Root/secret-name-1";
        const TString secretValue1 = "secret-value-1";
        const TString secretName2 = "/Root/secret-name-2";
        const TString secretValue2 = "secret-value-2";
        const TString secretName3 = "/Root/secret-name-3";
        const TString secretValue3 = "secret-value-3";

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSchemaSecret(secretName1, secretValue1, session);
        CreateSchemaSecret(secretName2, secretValue2, session);
        CreateSchemaSecret(secretName3, secretValue3, session);

        { // nothing from cache
            auto promise = ResolveSecrets({secretName1, secretName2}, kikimr);
            AssertSecretValues({secretValue1, secretValue2}, promise);
        }

        { // something from cache
            auto promise = ResolveSecrets({secretName2, secretName3}, kikimr);
            AssertSecretValues({secretValue2, secretValue3}, promise);
        }

        { // all from cache
            auto promise = ResolveSecrets({secretName1, secretName2, secretName3}, kikimr);
            AssertSecretValues({secretValue1, secretValue2, secretValue3}, promise);
        }
    }

    Y_UNIT_TEST(BigBatchRequest) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);

        TVector<TString> names;
        TVector<TString> values;
        for (int i = 0; i < 10; ++i) {
            names.push_back("/Root/secret-name-" + ToString(i));
            values.push_back("secret-value-" + ToString(i));
        }

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        for (size_t i = 0; i < names.size(); ++i) {
            CreateSchemaSecret(names[i], values[i], session);
        }

        { // nothing from cache
            const auto SecretsToResolveCnt = names.size() / 2;
            auto promise = ResolveSecrets({names.begin(), names.begin() + SecretsToResolveCnt}, kikimr);
            AssertSecretValues({values.begin(), values.begin() + SecretsToResolveCnt}, promise);
        }

        { // something from cache
            auto promise = ResolveSecrets(names, kikimr);
            AssertSecretValues(values, promise);
        }
    }

    Y_UNIT_TEST(EmptyBatch) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);

        auto promise = ResolveSecrets(TVector<TString>{}, kikimr);
        AssertBadRequest(promise, "<main>: Error: empty secret names list\n");
    }

    Y_UNIT_TEST(MixedGrantsInBatch) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);

        auto adminSession = kikimr.GetTableClient(NYdb::NTable::TClientSettings().AuthToken("root@builtin"))
            .CreateSession().GetValueSync().GetSession();

        TVector<TString> names;
        TVector<TString> values;
        for (int i = 0; i < 2; ++i) {
            names.push_back("/Root/secret-name-" + ToString(i));
            values.push_back("secret-value-" + ToString(i));
            CreateSchemaSecret(names.back(), values.back(), adminSession);
        }

        auto grantResult = adminSession.ExecuteSchemeQuery(
            Sprintf("GRANT 'ydb.granular.select_row' ON `%s` TO `%s`;", names[0].data(), "user@builtin")
        ).GetValueSync();
        UNIT_ASSERT_C(grantResult.GetStatus() == NYdb::EStatus::SUCCESS, grantResult.GetIssues().ToString());

        auto userToken = GetUserToken("user@builtin");
        { // user has grants for names[0], has no grants for names[1]
            auto promise = ResolveSecrets({names[0], names[1]}, kikimr, userToken);
            AssertBadRequest(promise, "<main>: Error: secret `/Root/secret-name-1` not found\n");
        }

        grantResult = adminSession.ExecuteSchemeQuery(
            Sprintf("GRANT 'ydb.granular.select_row' ON `%s` TO `%s`;", names[1].data(), "user@builtin")
        ).GetValueSync();
        UNIT_ASSERT_C(grantResult.GetStatus() == NYdb::EStatus::SUCCESS, grantResult.GetIssues().ToString());

        { // user has grants for all names[0]
            auto promise = ResolveSecrets({names[0], names[1]}, kikimr, userToken);
            AssertSecretValues(values, promise);
        }
    }

    Y_UNIT_TEST(SchemeCacheRetryErrors) {
        TKikimrSettings settings;
        auto schemeCacheStatusGetter = MakeHolder<TTestSchemeCacheStatusGetter>(
            TTestSchemeCacheStatusGetter::EFailProbability::OneTenth);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            schemeCacheStatusGetter.Get(),
            /* schemeShardStatusGetter */ nullptr);
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        static const auto SECRETS_CNT = 20;
        std::vector<std::pair<TString, TString>> secrets;
        for (int i = 0; i < SECRETS_CNT; ++i) {
            secrets.push_back({"/Root/secret-name-" + ToString(i), "secret-value-" + ToString(i)});
            CreateSchemaSecret(secrets.back().first, secrets.back().second, session);
        }
        std::vector<TDescriptionPromise> promises;
        for (const auto& [secretName, secretValue] : secrets) {
            promises.push_back(ResolveSecret(secretName, kikimr));
        }

        for (int i = 0; i < SECRETS_CNT; ++i) {
            AssertSecretValue(secrets[i].second, promises[i]);
        }
    }

    Y_UNIT_TEST(SchemeCacheMultipleNotRetryableErrors) {
        TKikimrRunner kikimr;
        kikimr.GetTestServer().GetRuntime()->GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString secretName1 = "/Root/s1";
        const TString secretName2 = "/Root/s2";
        const TString secretName3 = "/Root/s3";
        CreateSchemaSecret(secretName2, /* secretValue */ "", session);
        auto promise = ResolveSecrets({secretName1, secretName2, secretName3}, kikimr);

        AssertBadRequest(promise, "<main>: Error: secrets `/Root/s1`, `/Root/s3` not found\n");
    }

    Y_UNIT_TEST(SchemeShardRetrySingleSecret) {
        TKikimrSettings settings;
        auto schemeShardStatusGetter = MakeHolder<TTestSchemeShardStatusGetter>(
            /* statusOverwriteRemainingCount */ 2,
            NKikimrScheme::EStatus::StatusNotAvailable);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            /* schemeCacheStatusGetter */ nullptr,
            schemeShardStatusGetter.Get());
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString secretName = "/Root/secret-name";
        const TString secretValue = "secret-value";
        CreateSchemaSecret(secretName, secretValue, session);

        TDescribeSecretSettings describeSettings;
        describeSettings.RetryPolicy = MakeShortRetryPolicy();
        auto promise = ResolveSecret(secretName, kikimr, /* userToken */ nullptr, std::move(describeSettings));
        AssertSecretValue(secretValue, promise);
    }

    Y_UNIT_TEST(SchemeCacheAndSchemeShardRetryErrors) {
        TKikimrSettings settings;
        auto schemeCacheStatusGetter = MakeHolder<TTestSchemeCacheStatusGetter>(
            TTestSchemeCacheStatusGetter::EFailProbability::OneTenth);
        auto schemeShardStatusGetter = MakeHolder<TTestSchemeShardStatusGetter>(
            /* statusOverwriteRemainingCount */ 1,
            NKikimrScheme::EStatus::StatusNotAvailable);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            schemeCacheStatusGetter.Get(),
            schemeShardStatusGetter.Get());
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TVector<TString> secretNames = {"/Root/secret-name-0", "/Root/secret-name-1"};
        const TVector<TString> secretValues = {"secret-value-0", "secret-value-1"};
        for (size_t i = 0; i < secretNames.size(); ++i) {
            CreateSchemaSecret(secretNames[i], secretValues[i], session);
        }

        TDescribeSecretSettings describeSettings;
        describeSettings.RetryPolicy = MakeShortRetryPolicy();
        auto promise = ResolveSecrets(secretNames, kikimr, /* userToken */ nullptr, describeSettings);
        AssertSecretValues(secretValues, promise);
    }

    Y_UNIT_TEST(SchemeShardRetryManySecrets) {
        TKikimrSettings settings;
        static const auto SECRETS_CNT = 20;
        static const auto FAILS_BUDGET_PER_SECRET = 2;
        auto schemeShardStatusGetter = MakeHolder<TTestSchemeShardStatusGetter>(
            /* statusOverwriteRemainingCount */ SECRETS_CNT * FAILS_BUDGET_PER_SECRET,
            NKikimrScheme::EStatus::StatusNotAvailable);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            /* schemeCacheStatusGetter */ nullptr,
            schemeShardStatusGetter.Get());
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TVector<TString> secretNames;
        TVector<TString> secretValues;
        secretNames.reserve(SECRETS_CNT);
        secretValues.reserve(SECRETS_CNT);
        for (int i = 0; i < SECRETS_CNT; ++i) {
            secretNames.push_back("/Root/secret-name-" + ToString(i));
            secretValues.push_back("secret-value-" + ToString(i));
            CreateSchemaSecret(secretNames.back(), secretValues.back(), session);
        }

        TDescribeSecretSettings describeSettings;
        describeSettings.RetryPolicy = MakeShortRetryPolicy();
        auto promise = ResolveSecrets(secretNames, kikimr, /* userToken */ nullptr, describeSettings);
        AssertSecretValues(secretValues, promise);
    }

    Y_UNIT_TEST(SchemeShardRetryManySecretsPartialFailures) {
        TKikimrSettings settings;
        static const auto SECRETS_CNT = 20;
        auto schemeShardStatusGetter = MakeHolder<TTestSchemeShardStatusGetter>(
            /* statusOverwriteRemainingCount */ SECRETS_CNT / 2,
            NKikimrScheme::EStatus::StatusNotAvailable);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            /* schemeCacheStatusGetter */ nullptr,
            schemeShardStatusGetter.Get());
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TVector<TString> secretNames;
        TVector<TString> secretValues;
        secretNames.reserve(SECRETS_CNT);
        secretValues.reserve(SECRETS_CNT);
        for (int i = 0; i < SECRETS_CNT; ++i) {
            secretNames.push_back("/Root/secret-name-" + ToString(i));
            secretValues.push_back("secret-value-" + ToString(i));
            CreateSchemaSecret(secretNames.back(), secretValues.back(), session);
        }

        TDescribeSecretSettings describeSettings;
        describeSettings.RetryPolicy = MakeShortRetryPolicy();
        auto promise = ResolveSecrets(secretNames, kikimr, /* userToken */ nullptr, describeSettings);
        AssertSecretValues(secretValues, promise);
    }

    Y_UNIT_TEST(SchemeShardNonRetryableErrorWithLongRetryPolicy) {
        // We expect that RetryPolicy will not be applied,
        // since the SchemeShard error is not retryable
        TKikimrSettings settings;
        auto schemeShardStatusGetter = MakeHolder<TTestSchemeShardStatusGetter>(
            /* statusOverwriteRemainingCount */ 100,
            NKikimrScheme::EStatus::StatusPathDoesNotExist);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            /* schemeCacheStatusGetter */ nullptr,
            schemeShardStatusGetter.Get());
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString secretName = "/Root/secret-name";
        CreateSchemaSecret(secretName, "secret-value", session);

        TDescribeSecretSettings describeSettings;
        describeSettings.RetryPolicy = MakeLongRetryPolicy();
        auto promise = ResolveSecret(secretName, kikimr, /* userToken */ nullptr, describeSettings);
        AssertBadRequest(promise, "<main>: Error: Secret `/Root/secret-name` not found\n");
    }

    Y_UNIT_TEST(SchemeShardNotAvailableWithoutRetryPolicy) {
        TKikimrSettings settings;
        auto schemeShardStatusGetter = MakeHolder<TTestSchemeShardStatusGetter>(
            /* statusOverwriteRemainingCount */ 1,
            NKikimrScheme::EStatus::StatusNotAvailable);
        auto factory = std::make_shared<TTestDescribeSchemaSecretsServiceFactory>(
            /* secretUpdateListener */ nullptr,
            /* schemeCacheStatusGetter */ nullptr,
            schemeShardStatusGetter.Get());
        settings.SetDescribeSchemaSecretsServiceFactory(factory);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString secretName = "/Root/secret-name";
        CreateSchemaSecret(secretName, "secret-value", session);

        auto promise = ResolveSecret(secretName, kikimr);
        AssertBadRequest(
            promise,
            "<main>: Error: Schemeshard is not available for secret `/Root/secret-name`\n",
            Ydb::StatusIds::UNAVAILABLE);
    }

}

Y_UNIT_TEST_SUITE(RefreshingSecretCredentials) {
    // Hang guard: polls the provider until it hands out `expected` (the test makes that inevitable) and returns the last value.
    std::string WaitForValue(const NYdb::TCredentialsProviderPtr& provider, const std::string& expected, TDuration timeout = TDuration::Seconds(120)) {
        const TInstant deadline = TInstant::Now() + timeout;
        std::string value = provider->GetAuthInfo();
        while (value != expected && TInstant::Now() < deadline) {
            Sleep(TDuration::MilliSeconds(100));
            value = provider->GetAuthInfo();
        }
        return value;
    }

    // Hang guard: polls until the condition holds (the test makes it inevitable).
    template <class TCondition>
    void WaitUntil(TCondition condition, TStringBuf what, TDuration timeout = TDuration::Seconds(120)) {
        const TInstant deadline = TInstant::Now() + timeout;
        while (!condition()) {
            UNIT_ASSERT_C(TInstant::Now() < deadline, "timed out waiting for " << what);
            Sleep(TDuration::MilliSeconds(50));
        }
    }

    Y_UNIT_TEST(KeepTokenSecretReference) {
        const TString json = NYql::TStructuredTokenBuilder().SetTokenAuthWithSecret("/Root/token-secret", "").ToJson();
        const TString resolved = NYql::CreateStructuredTokenParser(json).ToBuilder().ReplaceReferences({{"/Root/token-secret", "the-token"}}).ToJson();

        // a schema secret: the reference and the database are kept next to the token
        const auto kept = NYql::ParseStructuredToken(KeepTokenSecretReference(resolved, "/Root/token-secret", "/Root"));
        UNIT_ASSERT_VALUES_EQUAL(kept.GetField("token"), "the-token");
        UNIT_ASSERT_VALUES_EQUAL(kept.GetField(TString(SecretReferenceField)), "/Root/token-secret");
        UNIT_ASSERT_VALUES_EQUAL(kept.GetField(TString(SecretDatabaseField)), "/Root");
        UNIT_ASSERT(!kept.HasField("token_ref"));

        // a secret of the old metadata provider is not re-read: nothing is added
        UNIT_ASSERT_VALUES_EQUAL(KeepTokenSecretReference(resolved, "old-secret-name", "/Root"), resolved);
    }

    Y_UNIT_TEST(FactoryFallsBackOutsideActors) {
        // outside an actor (no actor system at hand) the wrapped factory is used as before
        class TCountingFactory : public NYql::IStructuredTokenCredentialsFactory {
        public:
            std::shared_ptr<NYdb::ICredentialsProviderFactory> Create(const TString& json, bool) override {
                ++Calls;
                return NYdb::CreateOAuthCredentialsProviderFactory(NYql::ParseStructuredToken(json).GetField("token"));
            }
            ui32 Calls = 0;
        };
        auto inner = std::make_shared<TCountingFactory>();
        auto factory = CreateRefreshingSecretCredentialsFactoryOverFactory(inner);
        const TString json = KeepTokenSecretReference(NYql::TStructuredTokenBuilder().SetIAMToken("the-token").ToJson(), "/Root/token-secret", "/Root");
        UNIT_ASSERT_VALUES_EQUAL(factory->Create(json, false)->CreateProvider()->GetAuthInfo(), "the-token");
        UNIT_ASSERT_VALUES_EQUAL(inner->Calls, 1u);
    }

    Y_UNIT_TEST(ValueSecretIsReReadOnUse) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        runtime.GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSchemaSecret("/Root/token-secret", "token-1", session);
        const auto makeProvider = [&](const TString& initial, EBearerPrefix bearer) {
            return CreateRefreshingSecretCredentialsProviderFactory(runtime.GetActorSystem(0), "/Root/token-secret", "/Root", initial, bearer)->CreateProvider();
        };

        // the provider starts with the value resolved by the executer, so the first use costs no wait
        auto provider = makeProvider("token-1", EBearerPrefix::None);
        UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-1");

        // the secret is altered: the next uses take the new value
        AlterSchemaSecret("/Root/token-secret", "token-2", session);
        UNIT_ASSERT_VALUES_EQUAL(WaitForValue(provider, "token-2"), "token-2");

        // reads happen only when the token is asked for, and only one of them is in flight at a time: 100 uses
        // start at most 100 reads (observed through the ReReads sensor of the node)
        const auto sensor = [&runtime](const char* name) {
            return GetServiceCounters(runtime.GetAppData(0).Counters, "schema_secrets")
                ->GetSubgroup("component", "refreshing_credentials")->GetCounter(name, true)->Val();
        };
        const auto reReads = [&sensor]() { return sensor("ReReads"); };
        const i64 before = reReads();
        for (ui32 i = 0; i < 100; ++i) {
            UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-2");
        }
        UNIT_ASSERT_C(reReads() - before <= 100, reReads() - before);

        // with the bearer prefix for HTTP clients
        auto bearer = makeProvider("token-2", EBearerPrefix::Add);
        UNIT_ASSERT_VALUES_EQUAL(bearer->GetAuthInfo(), "Bearer token-2");
        AlterSchemaSecret("/Root/token-secret", "token-3", session);
        UNIT_ASSERT_VALUES_EQUAL(WaitForValue(bearer, "Bearer token-3"), "Bearer token-3");

        // the secret is dropped: reads fail and the task keeps the value it has. At most one read is in flight,
        // so once a re-read has failed no successful one can be pending any more: from then on the count of
        // successful re-reads is frozen while the errors grow with the uses.
        UNIT_ASSERT_VALUES_EQUAL(WaitForValue(provider, "token-3"), "token-3");
        DropSchemaSecret("/Root/token-secret", session);
        const i64 errorsBefore = sensor("ReReadErrors");
        WaitUntil([&]() {
            UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-3");
            return sensor("ReReadErrors") > errorsBefore;
        }, "the first failed re-read");
        const i64 refreshes = reReads();
        const i64 errors = sensor("ReReadErrors");
        WaitUntil([&]() {
            UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-3");
            return sensor("ReReadErrors") > errors;
        }, "another failed re-read");
        UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-3");
        UNIT_ASSERT_VALUES_EQUAL(reReads(), refreshes);
    }

    // A failed re-read keeps the value and is counted
    Y_UNIT_TEST(ReReadErrorIsCounted) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        runtime.GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        const auto sensor = [&runtime](const char* name) {
            return GetServiceCounters(runtime.GetAppData(0).Counters, "schema_secrets")
                ->GetSubgroup("component", "refreshing_credentials")->GetCounter(name, true)->Val();
        };

        // the secret behind the provider does not exist (dropped after the execution started)
        auto provider = CreateRefreshingSecretCredentialsProviderFactory(runtime.GetActorSystem(0), "/Root/missing-secret", "/Root", "token-1", EBearerPrefix::None)->CreateProvider();
        UNIT_ASSERT_VALUES_EQUAL(sensor("ProvidersCreated"), 1);
        const i64 errorsBefore = sensor("ReReadErrors");
        UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-1"); // starts the one re-read, which fails
        WaitUntil([&]() { return sensor("ReReadErrors") > errorsBefore; }, "the failed re-read");
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReadErrors"), errorsBefore + 1);
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReads"), 0);
        UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-1");
    }

    // Stand-in for the describe-secret service of the node: counts the requests and holds every reply until the
    // test releases them (with a TEvWakeup), answering `Value` then.
    struct TFakeDescribeSecrets : TThrRefBase {
        std::atomic<ui32> Requests = 0;
        TString Value;
    };

    class TFakeDescribeSecretsService : public NActors::TActorBootstrapped<TFakeDescribeSecretsService> {
    public:
        explicit TFakeDescribeSecretsService(TIntrusivePtr<TFakeDescribeSecrets> state)
            : State(std::move(state))
        {}

        void Bootstrap() {
            Become(&TThis::StateWork);
        }

        STRICT_STFUNC(StateWork,
            hFunc(TDescribeSchemaSecretsService::TEvResolveSecret, Handle);
            cFunc(NActors::TEvents::TEvWakeup::EventType, Reply);
            cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
        )

    private:
        void Handle(TDescribeSchemaSecretsService::TEvResolveSecret::TPtr& ev) {
            Held.push_back(ev->Get()->Promise);
            ++State->Requests;
        }

        void Reply() {
            auto held = std::move(Held);
            Held.clear();
            for (auto& promise : held) {
                promise.SetValue(NKqp::TEvDescribeSecretsResponse::TDescription(std::vector<TString>{State->Value}));
            }
        }

        const TIntrusivePtr<TFakeDescribeSecrets> State;
        TVector<TDescriptionPromise> Held;
    };

    // GetAuthInfo hands out the value of the last read and never waits for the secret service: while the service
    // holds its reply, thousands of concurrent calls return the current value (a blocking implementation would
    // hang here), and only one re-read is in flight; the held reply, once released, is the next value.
    Y_UNIT_TEST(GetAuthInfoNeverWaitsForTheSecretService) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        runtime.GetAppData(0).FeatureFlags.SetEnableSchemaSecrets(true);
        auto* actorSystem = runtime.GetActorSystem(0);
        const auto sensor = [&runtime](const char* name) {
            return GetServiceCounters(runtime.GetAppData(0).Counters, "schema_secrets")
                ->GetSubgroup("component", "refreshing_credentials")->GetCounter(name, true)->Val();
        };
        auto state = MakeIntrusive<TFakeDescribeSecrets>();
        state->Value = "token-2";
        const TActorId fake = runtime.Register(new TFakeDescribeSecretsService(state));
        runtime.RegisterService(MakeDescribeSchemaSecretServiceId(runtime.GetNodeId(0)), fake);

        auto provider = CreateRefreshingSecretCredentialsProviderFactory(actorSystem, "/Root/token-secret", "/Root", "token-1", EBearerPrefix::None)->CreateProvider();

        // 8 threads x 1000 calls while the service holds the (single) re-read
        const auto hammer = [&provider](const std::string& expected) {
            constexpr ui32 threads = 8;
            constexpr ui32 callsPerThread = 1000;
            std::atomic<ui32> unexpected = 0;
            TVector<std::thread> workers;
            for (ui32 t = 0; t < threads; ++t) {
                workers.emplace_back([&provider, &expected, &unexpected]() {
                    for (ui32 i = 0; i < callsPerThread; ++i) {
                        if (provider->GetAuthInfo() != expected) {
                            ++unexpected;
                        }
                    }
                });
            }
            for (auto& worker : workers) {
                worker.join();
            }
            UNIT_ASSERT_VALUES_EQUAL(unexpected.load(), 0u);
        };
        hammer("token-1");
        WaitUntil([&]() { return state->Requests.load() >= 1; }, "the re-read to reach the fake service");
        UNIT_ASSERT_VALUES_EQUAL(state->Requests.load(), 1u); // one re-read in flight, the other uses did not start another
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReads"), 0);
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReadErrors"), 0);
        UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "token-1");

        // the held answer is released: the next uses take the new value. The use that first sees it also
        // starts the next re-read (held again), so exactly two requests have reached the service.
        runtime.Send(new IEventHandle(fake, runtime.AllocateEdgeActor(), new NActors::TEvents::TEvWakeup()));
        UNIT_ASSERT_VALUES_EQUAL(WaitForValue(provider, "token-2"), "token-2");
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReads"), 1);
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReadErrors"), 0);
        WaitUntil([&]() { return state->Requests.load() >= 2; }, "the next re-read to reach the fake service");
        UNIT_ASSERT_VALUES_EQUAL(state->Requests.load(), 2u);

        // the service holds again: the calls still return the last value at once and start no further re-read
        state->Value = "token-3";
        hammer("token-2");
        UNIT_ASSERT_VALUES_EQUAL(state->Requests.load(), 2u);
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReads"), 1);
        runtime.Send(new IEventHandle(fake, runtime.AllocateEdgeActor(), new NActors::TEvents::TEvWakeup()));
        UNIT_ASSERT_VALUES_EQUAL(WaitForValue(provider, "token-3"), "token-3");
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReads"), 2);
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReadErrors"), 0);
    }

    Y_UNIT_TEST(DelegationSecretIsReReadOnUse) {
        TKikimrRunner kikimr;
        auto& runtime = *kikimr.GetTestServer().GetRuntime();
        auto& featureFlags = runtime.GetAppData(0).FeatureFlags;
        featureFlags.SetEnableSchemaSecrets(true);
        featureFlags.SetEnableIamDelegationSecrets(true);
        RegisterFakeIamDelegatedTokenService(runtime, {{"aje-1", "b1g-1"}, {"aje-2", "b1g-1"}});
        CreateIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-1", "b1g-1", "referrer-1");

        // the task got the token of aje-1 when the execution started; the secret is altered while it runs
        // and the next uses take a token of the new service account
        auto provider = CreateRefreshingSecretCredentialsProviderFactory(runtime.GetActorSystem(0), "/Root/sa-secret", "/Root", "delegated-token-b1g-1/aje-1", EBearerPrefix::None)->CreateProvider();
        UNIT_ASSERT_VALUES_EQUAL(provider->GetAuthInfo(), "delegated-token-b1g-1/aje-1");
        AlterIamDelegationSecretDirect(runtime, "/Root/sa-secret", "aje-2", "b1g-1", "referrer-2");
        UNIT_ASSERT_VALUES_EQUAL(WaitForValue(provider, "delegated-token-b1g-1/aje-2"), "delegated-token-b1g-1/aje-2");
    }
}

} // NKikimr::NSecret
