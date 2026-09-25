#include "common.h"

#include <ydb/core/base/counters.h>
#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/security/iam_delegation/iam_delegation_service.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/core/security/iam_delegation/settings.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/services/scheme_secret/secret_credentials.h>
#include <ydb/services/scheme_secret/ut/common/helpers.h>
#include <ydb/library/testlib/solomon_helpers/solomon_emulator_helpers.h>

#include <fmt/format.h>

#include <util/generic/hash_set.h>
#include <util/system/condvar.h>

namespace NKikimr::NKqp {

using namespace fmt::literals;
using namespace NTestUtils;
using namespace NYdb;
using namespace NYdb::NQuery;

namespace {

// The IAM gRPC emulator (ydb/tests/fq/streaming_common/iam_grpc_emulator) authenticates any token as the cloud
// user "bob" (SID bob@as), accepts SetupDelegation only on behalf of "bob" and issues tokens through
// CreateForService only for service accounts "delegated-*" with an active delegation.
constexpr char CLOUD_USER_TOKEN[] = "cloud-user-token";
constexpr char CLOUD_USER_SID[] = "bob@as";
constexpr char CLOUD_ID[] = "iamdelegationcloud";

// A system token service answering every request with the given token: makes a delegation service present
// a user's bearer to ServiceControl.
class TFixedSystemTokenService : public TActor<TFixedSystemTokenService> {
public:
    explicit TFixedSystemTokenService(TString token)
        : TActor(&TThis::StateWork)
        , Token(std::move(token))
    {}

    STRICT_STFUNC(StateWork,
        hFunc(NIamDelegation::TEvIamDelegation::TEvGetSystemToken, Handle);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )

private:
    void Handle(NIamDelegation::TEvIamDelegation::TEvGetSystemToken::TPtr& ev) {
        Send(ev->Sender, new NIamDelegation::TEvIamDelegation::TEvSystemTokenReady(Token, {}), 0, ev->Cookie);
    }

    const TString Token;
};

class TIamDelegationSecretsFixture : public TStreamingWithSchemaSecretsTestFixture {
public:
    void SetUp(NUnitTest::TTestContext& context) override {
        TStreamingWithSchemaSecretsTestFixture::SetUp(context);

        ++DynamicNodeCount;
        StoragePoolType = StoragePoolTypes.emplace_back("hdd");

        auto& appConfig = SetupAppConfig();
        auto& featureFlags = *appConfig.MutableFeatureFlags();
        featureFlags.SetEnableExternalDataSourceAuthMethodIam(true);
        featureFlags.SetEnableIamDelegationSecrets(true);

        // cloud users are authenticated by the emulator; ServiceControl/Operation/IamToken services are served
        // by the emulator at IamConfig.TokenServiceEndpoint/ServiceControlEndpoint (set by the fixture)
        appConfig.MutableAuthConfig()->SetUseAccessService(true);
    }

    std::vector<TResultSet> ExecAs(const TString& token, const std::string& query, EStatus expectedStatus = EStatus::SUCCESS, const std::string& expectedError = "") {
        auto client = GetKikimrRunner()->GetQueryClient(TClientSettings().AuthToken(token));
        auto result = client.ExecuteQuery(query, TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), expectedStatus, query << "\n" << result.GetIssues().ToString());
        if (!expectedError.empty()) {
            UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(), expectedError, query);
        }
        return result.GetResultSets();
    }

    std::vector<TResultSet> ExecAsCloudUser(const std::string& query, EStatus expectedStatus = EStatus::SUCCESS, const std::string& expectedError = "") {
        return ExecAs(CLOUD_USER_TOKEN, query, expectedStatus, expectedError);
    }

    // Same as ExecAsCloudUser, but returns the whole result (issues of a successful query, e.g. warnings)
    TExecuteQueryResult ExecAsCloudUserWithResult(const std::string& query, EStatus expectedStatus = EStatus::SUCCESS) {
        auto client = GetKikimrRunner()->GetQueryClient(TClientSettings().AuthToken(CLOUD_USER_TOKEN));
        auto result = client.ExecuteQuery(query, TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), expectedStatus, query << "\n" << result.GetIssues().ToString());
        return result;
    }

    // Asserts that the query fails with the given error regardless of the exact status
    void ExecAsCloudUserExpectFailure(const std::string& query, const std::string& expectedError) {
        auto client = GetKikimrRunner()->GetQueryClient(TClientSettings().AuthToken(CLOUD_USER_TOKEN));
        auto result = client.ExecuteQuery(query, TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(!result.IsSuccess(), query);
        UNIT_ASSERT_STRING_CONTAINS_C(result.GetIssues().ToString(), expectedError, query);
    }

    TString CreateDelegationSecretQuery(const TString& name, const TString& serviceAccountId, const TString& prefix = "CREATE SECRET") {
        return fmt::format(R"({prefix} `{name}` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "{sa}", RESOURCE = "{cloud}");)",
            "prefix"_a = prefix, "name"_a = name, "sa"_a = serviceAccountId, "cloud"_a = CLOUD_ID);
    }

    void AssertTokenStatus(const TString& serviceAccountId, Ydb::StatusIds::StatusCode expected) {
        const auto token = GetDelegatedToken(serviceAccountId, CLOUD_ID);
        UNIT_ASSERT(token);
        UNIT_ASSERT_VALUES_EQUAL_C(token->Get()->Status, expected, serviceAccountId << ": " << token->Get()->Issues.ToOneLineString());
        if (expected == Ydb::StatusIds::SUCCESS) {
            UNIT_ASSERT(!token->Get()->Token.empty());
        }
    }

    NKikimrSchemeOp::TSecretDescription DescribeSecret(const TString& path) {
        const auto navigate = Navigate(GetRuntime(), GetRuntime().AllocateEdgeActor(), path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
        const auto& entry = navigate->ResultSet.at(0);
        UNIT_ASSERT_VALUES_EQUAL(entry.Status, NSchemeCache::TSchemeCacheNavigate::EStatus::Ok);
        UNIT_ASSERT_EQUAL(entry.Kind, NSchemeCache::TSchemeCacheNavigate::EKind::KindSecret);
        UNIT_ASSERT(entry.SecretInfo);
        return entry.SecretInfo->Description;
    }

    bool SecretExists(const TString& path) {
        const auto navigate = Navigate(GetRuntime(), GetRuntime().AllocateEdgeActor(), path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
        return navigate->ResultSet.at(0).Status == NSchemeCache::TSchemeCacheNavigate::EStatus::Ok;
    }

    // Asks the node-local token service for a token of the delegated service account.
    NIamDelegation::TEvIamDelegation::TEvGetTokenResult::TPtr GetDelegatedToken(const TString& serviceAccountId, const TString& cloudId) {
        auto& runtime = GetRuntime();
        const TActorId sender = runtime.AllocateEdgeActor();
        runtime.Send(new IEventHandle(NIamDelegation::MakeIamDelegatedTokenServiceId(), sender,
            new NIamDelegation::TEvIamDelegation::TEvGetToken({serviceAccountId, cloudId})));
        return runtime.GrabEdgeEvent<NIamDelegation::TEvIamDelegation::TEvGetTokenResult>(sender, TDuration::Seconds(120)); // hang guard
    }

    void GrantCloudUser() {
        ExecQuery(fmt::format("GRANT ALL ON `/Root` TO `{user}`", "user"_a = CLOUD_USER_SID));
    }

    // The revocations IAM has accepted, recorded by TRevocationWatcher.
    struct TAcceptedRevocations : TThrRefBase {
        TMutex Mutex;
        TCondVar Changed;
        THashSet<TString> Referrers;
    };

    // Stands in front of the node's IAM delegation service: every request is forwarded to the real service, and
    // the answers to revoke requests are relayed to their senders after the accepted ones have been recorded.
    // (The runtime runs real threads here, so its event observers do not see the traffic.)
    class TRevocationWatcher : public TActor<TRevocationWatcher> {
        using TEvIamDelegation = NIamDelegation::TEvIamDelegation;

    public:
        TRevocationWatcher(const TActorId& realService, TIntrusivePtr<TAcceptedRevocations> accepted)
            : TActor(&TRevocationWatcher::StateWork)
            , RealService(realService)
            , Accepted(std::move(accepted))
        {}

        STFUNC(StateWork) {
            switch (ev->GetTypeRewrite()) {
                case TEvIamDelegation::EvRevokeDelegation: {
                    const ui64 cookie = ++Seq;
                    Pending[cookie] = {ev->Sender, ev->Cookie, ev->Get<TEvIamDelegation::TEvRevokeDelegation>()->Spec.ReferrerId};
                    Send(RealService, ev->ReleaseBase().Release(), 0, cookie);
                    break;
                }
                case TEvIamDelegation::EvRevokeDelegationResult: {
                    const auto it = Pending.find(ev->Cookie);
                    if (it == Pending.end()) {
                        break;
                    }
                    const auto& result = ev->Get<TEvIamDelegation::TEvRevokeDelegationResult>()->Result;
                    if (result.IsSuccess() || result.Status == Ydb::StatusIds::NOT_FOUND) {
                        with_lock (Accepted->Mutex) {
                            Accepted->Referrers.insert(it->second.ReferrerId);
                            Accepted->Changed.BroadCast();
                        }
                    }
                    Send(it->second.Sender, ev->ReleaseBase().Release(), 0, it->second.Cookie);
                    Pending.erase(it);
                    break;
                }
                default:
                    TActivationContext::Send(ev->Forward(RealService)); // setups and the rest: the sender talks to the real service
            }
        }

    private:
        struct TPendingRevoke {
            TActorId Sender;
            ui64 Cookie;
            TString ReferrerId;
        };

        const TActorId RealService;
        const TIntrusivePtr<TAcceptedRevocations> Accepted;
        ui64 Seq = 0;
        THashMap<ui64, TPendingRevoke> Pending;
    };

    // Runs the action (a DROP or an ALTER that stops the secret from naming the delegation) and waits until the
    // schemeshard's revoker got IAM's acceptance of the revocation: the revocation is asynchronous, so a token
    // check after the statement needs this gate (with a hang guard; the action makes the revocation inevitable).
    void RunAndWaitRevoked(const TString& referrerId, const std::function<void()>& action) {
        auto& runtime = GetRuntime();
        if (!Accepted) {
            // the schemeshard of /Root runs on the static node, whose KQP proxy registered the real service
            Accepted = MakeIntrusive<TAcceptedRevocations>();
            const TActorId realService = runtime.GetLocalServiceId(NIamDelegation::MakeIamDelegationServiceId());
            UNIT_ASSERT_C(realService, "the IAM delegation service is not registered on the static node");
            const TActorId watcher = runtime.Register(new TRevocationWatcher(realService, Accepted));
            runtime.RegisterService(NIamDelegation::MakeIamDelegationServiceId(), watcher);
        }
        action();
        const TInstant deadline = TInstant::Now() + TDuration::Seconds(120); // hang guard
        with_lock (Accepted->Mutex) {
            while (!Accepted->Referrers.contains(referrerId)) {
                UNIT_ASSERT_C(Accepted->Changed.WaitD(Accepted->Mutex, deadline), "the revocation of " << referrerId << " was not accepted");
            }
        }
    }

    TIntrusivePtr<TAcceptedRevocations> Accepted;

    TString ReferrerOf(const TString& path) {
        return DescribeSecret(path).GetIamDelegation().GetReferrerId();
    }

    TString StoragePoolType;
};

// Delegation secrets without IAM: the records are written directly and the tokens come from the fake delegated
// token service of the tests, so that a streaming query can be run (the access service of the IAM fixture
// cannot be combined with streaming queries).
class TStreamingWithDelegationSecretsTestFixture : public TStreamingWithSchemaSecretsTestFixture {
public:
    void SetUp(NUnitTest::TTestContext& context) override {
        TStreamingWithSchemaSecretsTestFixture::SetUp(context);
        SetupAppConfig().MutableFeatureFlags()->SetEnableIamDelegationSecrets(true);
    }
};

} // namespace

Y_UNIT_TEST_SUITE(KqpIamDelegationSecrets) {
    Y_UNIT_TEST_F(SecretLifecycle, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // only Yandex Cloud subjects may set up delegations: the builtin user is rejected before any IAM call
        ExecQuery(R"(
            CREATE SECRET `sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-sa1", RESOURCE = "iamdelegationcloud");
        )", EStatus::BAD_REQUEST, "is not a cloud subject");
        UNIT_ASSERT(!SecretExists("/Root/sa_secret"));

        // the service account cannot be looked up and the database has no cloud_id attribute: CLOUD_ID is required
        ExecAsCloudUser(R"(
            CREATE SECRET `sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-nofolder");
        )", EStatus::BAD_REQUEST, "has no cloud_id attribute");
        UNIT_ASSERT(!SecretExists("/Root/sa_secret"));

        // the delegation is rejected by IAM: no secret is created
        ExecAsCloudUser(fmt::format(R"(
            CREATE SECRET `sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-bad", RESOURCE = "{cloud}");
        )", "cloud"_a = CLOUD_ID), EStatus::UNAUTHORIZED, "SetupDelegation for service account delegated-bad failed");
        UNIT_ASSERT(!SecretExists("/Root/sa_secret"));

        // tokens cannot be obtained before the delegation is set up
        {
            const auto token = GetDelegatedToken("delegated-sa1", CLOUD_ID);
            UNIT_ASSERT(token);
            UNIT_ASSERT_VALUES_EQUAL_C(token->Get()->Status, Ydb::StatusIds::UNAUTHORIZED, token->Get()->Issues.ToOneLineString());
        }

        // successful CREATE: SetupDelegation on behalf of the cloud user, then the schemeshard operation
        ExecAsCloudUser(fmt::format(R"(
            CREATE SECRET `sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-sa1", RESOURCE = "{cloud}");
        )", "cloud"_a = CLOUD_ID));

        TString firstReferrer;
        {
            const auto secret = DescribeSecret("/Root/sa_secret");
            UNIT_ASSERT_EQUAL(secret.GetType(), NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION);
            UNIT_ASSERT(!secret.HasValue());
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "delegated-sa1");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetCloudId(), CLOUD_ID);
            firstReferrer = secret.GetIamDelegation().GetReferrerId();
            UNIT_ASSERT_C(firstReferrer.StartsWith("ydb.delegation."), firstReferrer);
            UNIT_ASSERT_C(firstReferrer.size() <= 50, firstReferrer);
        }

        // the token service now mints tokens for the delegated service account
        {
            const auto token = GetDelegatedToken("delegated-sa1", CLOUD_ID);
            UNIT_ASSERT(token);
            UNIT_ASSERT_C(token->Get()->IsSuccess(), token->Get()->Issues.ToOneLineString());
            UNIT_ASSERT(!token->Get()->Token.empty());
            UNIT_ASSERT(token->Get()->ExpiresAt > TInstant::Now());
        }

        // IF NOT EXISTS over an existing secret: nothing happens
        ExecAsCloudUser(fmt::format(R"(
            CREATE SECRET IF NOT EXISTS `sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-sa9", RESOURCE = "{cloud}");
        )", "cloud"_a = CLOUD_ID));
        UNIT_ASSERT_VALUES_EQUAL(DescribeSecret("/Root/sa_secret").GetIamDelegation().GetServiceAccountId(), "delegated-sa1");

        // the type cannot be changed
        ExecAsCloudUser(R"(
            ALTER SECRET `sa_secret` WITH (VALUE = "plain");
        )", EStatus::BAD_REQUEST, "Cannot change secret type");
        ExecAsCloudUser(R"(
            CREATE OR REPLACE SECRET `sa_secret` WITH (VALUE = "plain");
        )", EStatus::BAD_REQUEST, "Cannot change secret type");

        // ALTER: a new delegation is set up (fresh referrer) and the old one is revoked
        ExecAsCloudUser(R"(
            ALTER SECRET `sa_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-sa2");
        )");
        {
            const auto secret = DescribeSecret("/Root/sa_secret");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "delegated-sa2");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetCloudId(), CLOUD_ID);
            UNIT_ASSERT(!secret.GetIamDelegation().GetReferrerId().empty());
            UNIT_ASSERT_VALUES_UNEQUAL(secret.GetIamDelegation().GetReferrerId(), firstReferrer);
            UNIT_ASSERT(!secret.HasPendingIamDelegation());
            UNIT_ASSERT_VALUES_EQUAL(secret.GetVersion(), 2u); // staged, then promoted

            const auto token = GetDelegatedToken("delegated-sa2", CLOUD_ID);
            UNIT_ASSERT(token);
            UNIT_ASSERT_C(token->Get()->IsSuccess(), token->Get()->Issues.ToOneLineString());
        }

        // ALTER without changes does not touch IAM
        ExecAsCloudUser(R"(
            ALTER SECRET `sa_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-sa2");
        )");
        UNIT_ASSERT_VALUES_EQUAL(DescribeSecret("/Root/sa_secret").GetVersion(), 3u);

        // ALTER by a builtin user is rejected
        ExecQuery(R"(
            ALTER SECRET `sa_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-sa3");
        )", EStatus::BAD_REQUEST, "is not a cloud subject");

        // DROP revokes the delegation
        ExecAsCloudUser("DROP SECRET `sa_secret`;");
        UNIT_ASSERT(!SecretExists("/Root/sa_secret"));
        ExecAsCloudUser("DROP SECRET IF EXISTS `sa_secret`;");

        // DROP revokes the delegation (through the schemeshard, after the statement): no token can be obtained
        // for a service account whose secret was dropped (a fresh service account, so that no cached token can answer)
        ExecAsCloudUser(CreateDelegationSecretQuery("drop_secret", "delegated-sa4"));
        RunAndWaitRevoked(ReferrerOf("/Root/drop_secret"), [&]() { ExecAsCloudUser("DROP SECRET `drop_secret`;"); });
        AssertTokenStatus("delegated-sa4", Ydb::StatusIds::UNAUTHORIZED);

        // plain secrets are not affected
        ExecAsCloudUser(R"(CREATE SECRET `plain_secret` WITH (VALUE = "plain");)");
        UNIT_ASSERT_EQUAL(DescribeSecret("/Root/plain_secret").GetType(), NKikimrSchemeOp::SECRET_TYPE_VALUE);
        ExecAsCloudUser(R"(ALTER SECRET `plain_secret` WITH (VALUE = "plain2");)");
        ExecAsCloudUser("DROP SECRET `plain_secret`;");
        UNIT_ASSERT(!SecretExists("/Root/plain_secret"));
    }

    Y_UNIT_TEST_F(CloudIdFromServiceAccount, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // RESOURCE omitted: the cloud of the service account is looked up with the user's token (the emulator
        // refuses the lookup with any other token), so the delegation lands in that cloud
        {
            const auto result = ExecAsCloudUserWithResult(R"(
                CREATE SECRET `sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-sa1");
            )");
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
        }
        UNIT_ASSERT_VALUES_EQUAL(DescribeSecret("/Root/sa_secret").GetIamDelegation().GetCloudId(), "cloud-of-delegated-sa1");
        AssertTokenStatus("delegated-sa1", Ydb::StatusIds::UNAUTHORIZED); // no delegation in the cloud CLOUD_ID, the RESOURCE of the other tests
        {
            const auto token = GetDelegatedToken("delegated-sa1", "cloud-of-delegated-sa1");
            UNIT_ASSERT(token);
            UNIT_ASSERT_C(token->Get()->IsSuccess(), token->Get()->Issues.ToOneLineString());
        }
        ExecAsCloudUser("DROP SECRET `sa_secret`;");

        // an explicit RESOURCE wins over the lookup
        ExecAsCloudUser(CreateDelegationSecretQuery("explicit_secret", "delegated-sa1"));
        UNIT_ASSERT_VALUES_EQUAL(DescribeSecret("/Root/explicit_secret").GetIamDelegation().GetCloudId(), CLOUD_ID);
        ExecAsCloudUser("DROP SECRET `explicit_secret`;");

        // the lookup fails and the database has no cloud_id attribute: the error names both reasons
        ExecAsCloudUser(R"(
            CREATE SECRET `nofolder_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-nofolder");
        )", EStatus::BAD_REQUEST, "database /Root has no cloud_id attribute and the cloud of service account delegated-nofolder is unknown: GetServiceAccount failed");
        UNIT_ASSERT(!SecretExists("/Root/nofolder_secret"));

        ExecAsCloudUser(R"(
            CREATE SECRET `nocloud_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-nocloud");
        )", EStatus::BAD_REQUEST, "did not resolve folder folder-of-delegated-nocloud");
        UNIT_ASSERT(!SecretExists("/Root/nocloud_secret"));
    }

    Y_UNIT_TEST_F(CloudIdFromDatabase, TIamDelegationSecretsFixture) {
        // the cloud_id attribute of the database is the fallback for an omitted CLOUD_ID when the cloud
        // of the service account cannot be looked up
        const auto databasePath = GetKikimrRunner()->CreateDatabase("CloudDb", StoragePoolType, {{"cloud_id", CLOUD_ID}});
        NYdb::TDriver driver(NYdb::TDriverConfig()
            .SetDiscoveryMode(NYdb::EDiscoveryMode::Async)
            .SetEndpoint(GetKikimrRunner()->GetEndpoint())
            .SetDatabase(databasePath)
            .SetAuthToken(CLOUD_USER_TOKEN));
        TQueryClient client(driver);

        // the database root is owned by the creator; grant the cloud user
        {
            NYdb::TDriver rootDriver(NYdb::TDriverConfig()
                .SetDiscoveryMode(NYdb::EDiscoveryMode::Async)
                .SetEndpoint(GetKikimrRunner()->GetEndpoint())
                .SetDatabase(databasePath)
                .SetAuthToken(BUILTIN_ACL_ROOT));
            TQueryClient rootClient(rootDriver);
            const auto result = rootClient.ExecuteQuery(fmt::format("GRANT ALL ON `{db}` TO `{user}`", "db"_a = databasePath, "user"_a = CLOUD_USER_SID), TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            rootDriver.Stop(true);
        }

        // the service account cannot be looked up: the cloud of the database is used, with a warning
        {
            const auto result = client.ExecuteQuery(R"(
                CREATE SECRET `db_sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-nofolder-db");
            )", TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "RESOURCE iamdelegationcloud was taken from the database, the cloud of service account delegated-nofolder-db is unknown: GetServiceAccount failed");
        }
        {
            const auto secret = DescribeSecret(databasePath + "/db_sa_secret");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "delegated-nofolder-db");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetCloudId(), CLOUD_ID);
        }
        {
            const auto result = client.ExecuteQuery("DROP SECRET `db_sa_secret`;", TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        // the service account can be looked up: its cloud wins over the cloud of the database
        {
            const auto result = client.ExecuteQuery(R"(
                CREATE SECRET `db_sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-dbsa");
            )", TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
        }
        UNIT_ASSERT_VALUES_EQUAL(DescribeSecret(databasePath + "/db_sa_secret").GetIamDelegation().GetCloudId(), "cloud-of-delegated-dbsa");
        {
            const auto result = client.ExecuteQuery("DROP SECRET `db_sa_secret`;", TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        driver.Stop(true);
    }

    Y_UNIT_TEST_F(ExternalDataSources, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        constexpr char topicName[] = "iamDelegationTopic";
        constexpr char pqSource[] = "iamDelegationPqSource";
        constexpr char solomonSink[] = "iamDelegationSolomonSink";

        // a "cloud" database with a topic, accessed through an external data source with a delegated service account
        const auto databasePath = GetKikimrRunner()->CreateDatabase("EdsCloud", StoragePoolType, {{"cloud_id", CLOUD_ID}});
        const auto location = GetKikimrRunner()->GetEndpoint();
        NYdb::TDriver driver(NYdb::TDriverConfig()
            .SetDiscoveryMode(NYdb::EDiscoveryMode::Async)
            .SetEndpoint(location)
            .SetDatabase(databasePath));
        NYdb::NTopic::TTopicClient topicClient(driver);
        WaitFor(TEST_OPERATION_TIMEOUT, "CreateTopic", [&](TString& error) {
            auto result = topicClient.CreateTopic(topicName).GetValueSync();
            if (result.IsSuccess()) {
                return true;
            }
            error = result.GetIssues().ToString();
            return false;
        });

        ExecAsCloudUser(fmt::format(R"(
            CREATE SECRET `eds_sa_secret` WITH (TYPE = "IAM_DELEGATION", SERVICE_ACCOUNT_ID = "delegated-eds", RESOURCE = "{cloud}");
        )", "cloud"_a = CLOUD_ID));

        // the value of a delegation secret is an IAM token: the data source references it exactly like
        // any token secret, with the regular TOKEN settings and no service account of its own
        ExecAsCloudUserExpectFailure(fmt::format(R"(
            CREATE EXTERNAL DATA SOURCE `{pq_source}` WITH (
                SOURCE_TYPE = "Ydb",
                LOCATION = "{location}",
                DATABASE_NAME = "{database}",
                AUTH_METHOD = "SERVICE_ACCOUNT",
                SERVICE_ACCOUNT_ID = "delegated-eds",
                SERVICE_ACCOUNT_SECRET_PATH = "eds_sa_secret"
            );)",
            "pq_source"_a = pqSource, "location"_a = location, "database"_a = databasePath),
            "has type IAM_DELEGATION");

        ExecAsCloudUser(fmt::format(R"(
            CREATE EXTERNAL DATA SOURCE `{pq_source}` WITH (
                SOURCE_TYPE = "Ydb",
                LOCATION = "{location}",
                DATABASE_NAME = "{database}",
                AUTH_METHOD = "TOKEN",
                TOKEN_SECRET_PATH = "eds_sa_secret"
            );)",
            "pq_source"_a = pqSource, "location"_a = location, "database"_a = databasePath));

        {
            const auto navigate = Navigate(GetRuntime(), GetRuntime().AllocateEdgeActor(), TStringBuilder() << "/Root/" << pqSource, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
            const auto& entry = navigate->ResultSet.at(0);
            UNIT_ASSERT(entry.ExternalDataSourceInfo);
            UNIT_ASSERT_VALUES_EQUAL(entry.ExternalDataSourceInfo->Description.GetAuth().GetToken().GetTokenSecretName(), "/Root/eds_sa_secret");
        }

        // writing to the topic: the secret is read at compile time like any token secret and yields the
        // current token of the delegated service account
        const auto now = TInstant::Now();
        constexpr char testData[] = "delegated-data";
        ExecAsCloudUser(fmt::format(R"(
            INSERT INTO `{pq_source}`.`{topic}` (Data) VALUES ("{data}");
        )", "pq_source"_a = pqSource, "topic"_a = topicName, "data"_a = testData));
        ReadTopicMessages(topicName, TVector<std::string>{testData}, topicClient, now, true);

        // a user without SELECT ROW on the secret cannot use the external data source
        // (the secret does not inherit permissions from /Root)
        ExecQuery("GRANT ALL ON `/Root` TO `other@builtin`");
        ExecAs("other@builtin", fmt::format(R"(
            INSERT INTO `{pq_source}`.`{topic}` (Data) VALUES ("{data}");
        )", "pq_source"_a = pqSource, "topic"_a = topicName, "data"_a = testData), EStatus::GENERIC_ERROR, "secret `/Root/eds_sa_secret` not found");

        // Monium metrics with a delegation secret
        const TSolomonLocation soLocation = {
            .ProjectId = "iamDelegationProject",
            .FolderId = "iamDelegationFolder",
            .Service = "custom",
            .IsCloud = false,
        };
        CleanupSolomon(soLocation);
        ExecAsCloudUser(fmt::format(R"(
            CREATE EXTERNAL DATA SOURCE `{solomon_sink}` WITH (
                SOURCE_TYPE = "Monium.Metrics",
                LOCATION = "localhost:{solomon_port}",
                AUTH_METHOD = "TOKEN",
                TOKEN_SECRET_PATH = "eds_sa_secret",
                USE_TLS = "false"
            );)",
            "solomon_sink"_a = solomonSink, "solomon_port"_a = getenv("SOLOMON_HTTP_PORT")));
        ExecAsCloudUser(fmt::format(R"(
            INSERT INTO `{solomon_sink}`.`{project}/{folder}/{service}`
            SELECT "delegated" AS sensor, 1 AS value, Timestamp("2025-03-12T14:40:39Z") AS ts;
        )", "solomon_sink"_a = solomonSink, "project"_a = soLocation.ProjectId, "folder"_a = soLocation.FolderId, "service"_a = soLocation.Service));
        UNIT_ASSERT_STRING_CONTAINS(GetSolomonMetrics(soLocation), "delegated");

        // after the secret is dropped the external data sources cannot be compiled any more
        // (a new query text avoids the compiled query cache)
        ExecAsCloudUser("DROP SECRET `eds_sa_secret`;");
        ExecAsCloudUser(fmt::format(R"(
            INSERT INTO `{pq_source}`.`{topic}` (Data) VALUES ("after-drop");
        )", "pq_source"_a = pqSource, "topic"_a = topicName), EStatus::GENERIC_ERROR, "secret `/Root/eds_sa_secret` not found");

        driver.Stop(true);
    }
    Y_UNIT_TEST_F(SlowOperationIsPolled, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // the emulator returns a not-yet-done operation for "delegated-slow": it is polled until done (the
        // statement completes only once the operation is done; the polling itself is pinned by
        // IamDelegationService::SetupPollsOperation)
        ExecAsCloudUser(CreateDelegationSecretQuery("slow_secret", "delegated-slow"));

        const auto secret = DescribeSecret("/Root/slow_secret");
        UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "delegated-slow");
        UNIT_ASSERT(!secret.GetIamDelegation().GetReferrerId().empty());
        AssertTokenStatus("delegated-slow", Ydb::StatusIds::SUCCESS);

        ExecAsCloudUser("DROP SECRET `slow_secret`;");
        UNIT_ASSERT(!SecretExists("/Root/slow_secret"));
    }

    Y_UNIT_TEST_F(CreateSchemeFailureSetsNothingUp, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // the schemeshard operation comes first and fails: the name is longer than the schemeshard limit for a
        // path element (KQP does not validate it), so no delegation is set up at all
        const TString longName = TString(300, 'x');
        auto client = GetKikimrRunner()->GetQueryClient(TClientSettings().AuthToken(CLOUD_USER_TOKEN));
        const auto result = client.ExecuteQuery(CreateDelegationSecretQuery(longName, "delegated-rollback"), TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(!result.IsSuccess(), result.GetIssues().ToString());
        UNIT_ASSERT_C(!result.GetIssues().ToString().contains("SetupDelegation"), result.GetIssues().ToString());
        UNIT_ASSERT(!SecretExists("/Root/" + longName));

        // a fresh key: the token service asks IAM, which has no delegation for the service account
        AssertTokenStatus("delegated-rollback", Ydb::StatusIds::UNAUTHORIZED);
    }

    Y_UNIT_TEST_F(AlterFailureKeepsOldDelegation, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        ExecAsCloudUser(CreateDelegationSecretQuery("alt_secret", "delegated-alt1"));
        const auto before = DescribeSecret("/Root/alt_secret");
        UNIT_ASSERT_VALUES_EQUAL(before.GetIamDelegation().GetServiceAccountId(), "delegated-alt1");

        // the new delegation is rejected by IAM: the staged replacement is cancelled, the delegation of the
        // secret is untouched
        ExecAsCloudUser(R"(
            ALTER SECRET `alt_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-bad");
        )", EStatus::UNAUTHORIZED, "SetupDelegation for service account delegated-bad failed");

        const auto after = DescribeSecret("/Root/alt_secret");
        UNIT_ASSERT_VALUES_EQUAL(after.GetIamDelegation().GetServiceAccountId(), "delegated-alt1");
        UNIT_ASSERT_VALUES_EQUAL(after.GetIamDelegation().GetReferrerId(), before.GetIamDelegation().GetReferrerId());
        UNIT_ASSERT(!after.HasPendingIamDelegation());
        UNIT_ASSERT_VALUES_EQUAL(after.GetVersion(), before.GetVersion() + 2); // staged and cancelled
        AssertTokenStatus("delegated-alt1", Ydb::StatusIds::SUCCESS);

        ExecAsCloudUser("DROP SECRET `alt_secret`;");
    }

    Y_UNIT_TEST_F(AlterRevokesOldDelegation, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // fresh keys on both sides: no token is cached for either service account before the ALTER
        ExecAsCloudUser(CreateDelegationSecretQuery("alt2_secret", "delegated-alt2"));
        RunAndWaitRevoked(ReferrerOf("/Root/alt2_secret"), [&]() {
            ExecAsCloudUser(R"(
                ALTER SECRET `alt2_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-alt3");
            )");
        });
        UNIT_ASSERT_VALUES_EQUAL(DescribeSecret("/Root/alt2_secret").GetIamDelegation().GetServiceAccountId(), "delegated-alt3");

        AssertTokenStatus("delegated-alt3", Ydb::StatusIds::SUCCESS);
        AssertTokenStatus("delegated-alt2", Ydb::StatusIds::UNAUTHORIZED);

        ExecAsCloudUser("DROP SECRET `alt2_secret`;");
    }

    Y_UNIT_TEST_F(CreateOrReplaceOverDelegationActsAsAlter, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        ExecAsCloudUser(CreateDelegationSecretQuery("cor_secret", "delegated-cor1"));
        const auto before = DescribeSecret("/Root/cor_secret");

        ExecAsCloudUser(CreateDelegationSecretQuery("cor_secret", "delegated-cor2", "CREATE OR REPLACE SECRET"));
        const auto after = DescribeSecret("/Root/cor_secret");
        UNIT_ASSERT_VALUES_EQUAL(after.GetVersion(), 2u); // staged, then promoted
        UNIT_ASSERT_VALUES_EQUAL(after.GetIamDelegation().GetServiceAccountId(), "delegated-cor2");
        UNIT_ASSERT_VALUES_UNEQUAL(after.GetIamDelegation().GetReferrerId(), before.GetIamDelegation().GetReferrerId());
        AssertTokenStatus("delegated-cor2", Ydb::StatusIds::SUCCESS);

        // a plain secret cannot be replaced with a delegation secret
        ExecAsCloudUser(R"(CREATE SECRET `plain_cor` WITH (VALUE = "plain");)");
        ExecAsCloudUser(CreateDelegationSecretQuery("plain_cor", "delegated-cor3", "CREATE OR REPLACE SECRET"),
            EStatus::BAD_REQUEST, "Cannot replace secret /Root/plain_cor of type VALUE with a secret of type IAM_DELEGATION");
        UNIT_ASSERT_EQUAL(DescribeSecret("/Root/plain_cor").GetType(), NKikimrSchemeOp::SECRET_TYPE_VALUE);
        AssertTokenStatus("delegated-cor3", Ydb::StatusIds::UNAUTHORIZED);

        ExecAsCloudUser("DROP SECRET `cor_secret`;");
        ExecAsCloudUser("DROP SECRET `plain_cor`;");
    }

    Y_UNIT_TEST_F(AlterIfExistsMissingIsNoop, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        ExecAsCloudUser(R"(
            ALTER SECRET IF EXISTS `nope_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-nope");
        )");
        UNIT_ASSERT(!SecretExists("/Root/nope_secret"));
        AssertTokenStatus("delegated-nope", Ydb::StatusIds::UNAUTHORIZED);
    }

    Y_UNIT_TEST_F(DropWithRevokeRejectedSucceeds, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // the emulator refuses to revoke the delegation of "delegated-norevoke": the secret is dropped anyway,
        // without a warning; the schemeshard keeps the revocation and retries it
        ExecAsCloudUser(CreateDelegationSecretQuery("norevoke_secret", "delegated-norevoke"));
        const auto result = ExecAsCloudUserWithResult("DROP SECRET `norevoke_secret`;");
        UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
        UNIT_ASSERT(!SecretExists("/Root/norevoke_secret"));
    }

    Y_UNIT_TEST_F(DelegationServiceDownDropStillSucceeds, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        ExecAsCloudUser(CreateDelegationSecretQuery("down_secret", "delegated-down"));

        // the node-local delegation service dies: delegations cannot be set up any more (a tracked probe
        // comes back undelivered once it is gone)
        auto& runtime = GetRuntime();
        runtime.Send(new IEventHandle(NIamDelegation::MakeIamDelegationServiceId(), runtime.AllocateEdgeActor(), new TEvents::TEvPoison()));
        {
            const TActorId probe = runtime.AllocateEdgeActor();
            runtime.Send(new IEventHandle(NIamDelegation::MakeIamDelegationServiceId(), probe, new TEvents::TEvWakeup(), IEventHandle::FlagTrackDelivery));
            const auto undelivered = runtime.GrabEdgeEvent<TEvents::TEvUndelivered>(probe, TDuration::Seconds(120));
            UNIT_ASSERT(undelivered);
        }
        ExecAsCloudUser(CreateDelegationSecretQuery("down_secret2", "delegated-down2"), EStatus::PRECONDITION_FAILED, "IAM delegation service is not running on this node");
        UNIT_ASSERT(!SecretExists("/Root/down_secret2"));

        // but existing secrets can still be dropped: the revocation waits in the schemeshard for the service
        const auto result = ExecAsCloudUserWithResult("DROP SECRET `down_secret`;");
        UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
        UNIT_ASSERT(!SecretExists("/Root/down_secret"));
    }

    Y_UNIT_TEST_F(FeatureFlagOffAfterCreation, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        ExecAsCloudUser(CreateDelegationSecretQuery("ff_secret", "delegated-ff"));

        auto& runtime = GetRuntime();
        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
            runtime.GetAppData(node).FeatureFlags.SetEnableIamDelegationSecrets(false);
        }

        // no new delegation secrets with the flag off; the existing one is still dropped with a revoke
        ExecAsCloudUserExpectFailure(CreateDelegationSecretQuery("ff_secret2", "delegated-ff2"), "IAM delegation secrets are disabled");
        UNIT_ASSERT(!SecretExists("/Root/ff_secret2"));
        RunAndWaitRevoked(ReferrerOf("/Root/ff_secret"), [&]() { ExecAsCloudUser("DROP SECRET `ff_secret`;"); });
        UNIT_ASSERT(!SecretExists("/Root/ff_secret"));
        AssertTokenStatus("delegated-ff", Ydb::StatusIds::UNAUTHORIZED);

        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
            runtime.GetAppData(node).FeatureFlags.SetEnableIamDelegationSecrets(true);
        }
    }

    Y_UNIT_TEST_F(UserBearerNeverReachesServiceControl, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        // the emulator refuses SetupDelegation and RevokeDelegation with a user's bearer (UNAUTHENTICATED): a
        // delegation service authorized with the user's token cannot set anything up ...
        auto& runtime = GetRuntime();
        {
            const auto settings = NIamDelegation::TIamDelegationSettings::FromConfig(runtime.GetAppData(0).IamConfig);
            const TActorId userTokenService = runtime.Register(new TFixedSystemTokenService(CLOUD_USER_TOKEN));
            const TActorId asUser = runtime.Register(NIamDelegation::CreateIamDelegationService(settings, userTokenService));
            const TActorId sender = runtime.AllocateEdgeActor();
            NIamDelegation::TDelegationSpec spec{.ServiceAccountId = "delegated-bearer", .CloudId = CLOUD_ID, .ReferrerId = "ydb.delegation.0000000000000000000000000000000a"};
            runtime.Send(new IEventHandle(asUser, sender, new NIamDelegation::TEvIamDelegation::TEvSetupDelegation(spec, "bob")));
            const auto result = runtime.GrabEdgeEvent<NIamDelegation::TEvIamDelegation::TEvSetupDelegationResult>(sender, TDuration::Seconds(120)); // hang guard
            UNIT_ASSERT(result);
            UNIT_ASSERT_VALUES_EQUAL_C(result->Get()->Result.Status, Ydb::StatusIds::UNAUTHORIZED, result->Get()->Result.Issues.ToOneLineString());
            UNIT_ASSERT_STRING_CONTAINS(result->Get()->Result.Issues.ToOneLineString(), "not the token of a system service account");
            runtime.Send(new IEventHandle(asUser, sender, new TEvents::TEvPoison()));
            runtime.Send(new IEventHandle(userTokenService, sender, new TEvents::TEvPoison()));
        }
        // ... while the statements, which authenticate to ServiceControl as YDB and name the user only as the
        // subject, work: the user's bearer is never sent there
        ExecAsCloudUser(CreateDelegationSecretQuery("bearer_secret", "delegated-bearer"));
        AssertTokenStatus("delegated-bearer", Ydb::StatusIds::SUCCESS);
        ExecAsCloudUser(R"(ALTER SECRET `bearer_secret` WITH (SERVICE_ACCOUNT_ID = "delegated-bearer2");)");
        AssertTokenStatus("delegated-bearer2", Ydb::StatusIds::SUCCESS);
        {
            // the revoke is accepted, so it was authenticated as YDB as well (the token of delegated-bearer2
            // minted above stays cached on the node until it expires, so the revocation itself is checked with
            // a fresh service account in SecretLifecycle)
            RunAndWaitRevoked(ReferrerOf("/Root/bearer_secret"), [&]() {
                const auto result = ExecAsCloudUserWithResult("DROP SECRET `bearer_secret`;");
                UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            });
        }

        // IAM refuses the delegation because the service account is in another cloud: the error names the cause
        // (a PreconditionFailure violation in the error details) and points at RESOURCE; nothing is created
        ExecAsCloudUser(CreateDelegationSecretQuery("wrongcloud_secret", "delegated-wrongcloud"), EStatus::BAD_REQUEST,
            "BAD_SERVICE_ACCOUNT_CLOUD (emulated BAD_SERVICE_ACCOUNT_CLOUD): the service account does not belong to the cloud of the delegation, set RESOURCE to the cloud of the service account");
        UNIT_ASSERT(!SecretExists("/Root/wrongcloud_secret"));
        AssertTokenStatus("delegated-wrongcloud", Ydb::StatusIds::UNAUTHORIZED);
    }

    Y_UNIT_TEST_F(AlterResourceMovesDelegation, TIamDelegationSecretsFixture) {
        GrantCloudUser();

        ExecAsCloudUser(CreateDelegationSecretQuery("move_secret", "delegated-move"));
        AssertTokenStatus("delegated-move", Ydb::StatusIds::SUCCESS);
        const TString referrer = DescribeSecret("/Root/move_secret").GetIamDelegation().GetReferrerId();

        // ALTER RESOURCE: the delegation is set up in the new cloud and revoked in the old one; the service
        // account stays. Fresh keys on both sides: no cached token can answer for the old cloud.
        ExecAsCloudUser(R"(ALTER SECRET `move_secret` WITH (RESOURCE = "cloud-of-delegated-move");)");
        {
            const auto secret = DescribeSecret("/Root/move_secret");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetServiceAccountId(), "delegated-move");
            UNIT_ASSERT_VALUES_EQUAL(secret.GetIamDelegation().GetCloudId(), "cloud-of-delegated-move");
            UNIT_ASSERT_VALUES_UNEQUAL(secret.GetIamDelegation().GetReferrerId(), referrer);
        }
        {
            const auto token = GetDelegatedToken("delegated-move", "cloud-of-delegated-move");
            UNIT_ASSERT(token);
            UNIT_ASSERT_C(token->Get()->IsSuccess(), token->Get()->Issues.ToOneLineString());
        }
        // the old key had a token cached by the CREATE above: the node keeps serving it until it expires, so the
        // revocation is checked through a secret of a fresh service account in the old cloud
        ExecAsCloudUser(CreateDelegationSecretQuery("move_secret2", "delegated-move2"));
        RunAndWaitRevoked(ReferrerOf("/Root/move_secret2"), [&]() {
            ExecAsCloudUser(R"(ALTER SECRET `move_secret2` WITH (RESOURCE = "cloud-of-delegated-move2");)");
        });
        AssertTokenStatus("delegated-move2", Ydb::StatusIds::UNAUTHORIZED);

        ExecAsCloudUser("DROP SECRET `move_secret`;");
        ExecAsCloudUser("DROP SECRET `move_secret2`;");
        UNIT_ASSERT(!SecretExists("/Root/move_secret"));
    }

    Y_UNIT_TEST_F(DelegationSecretIsATokenSecret, TIamDelegationSecretsFixture) {
        GrantCloudUser();
        ExecAsCloudUser(CreateDelegationSecretQuery("typed_secret", "delegated-typed"));

        // the value of a delegation secret is an IAM token: it can only stand where a token is expected
        const auto s3Query = [](const TString& secret) {
            return fmt::format(R"(
                CREATE EXTERNAL DATA SOURCE `s3Source` WITH (
                    SOURCE_TYPE = "ObjectStorage",
                    LOCATION = "http://localhost:1/bucket/",
                    AUTH_METHOD = "SERVICE_ACCOUNT",
                    SERVICE_ACCOUNT_ID = "delegated-typed",
                    SERVICE_ACCOUNT_SECRET_PATH = "{secret}"
                );)", "secret"_a = secret);
        };
        ExecAsCloudUserExpectFailure(s3Query("typed_secret"), "has type IAM_DELEGATION");

        // a VALUE secret is a plain value wherever it is used, whatever it contains: the declared type of
        // the secret decides, never its content
        ExecAsCloudUser(fmt::format(R"(CREATE SECRET `forged_secret` WITH (VALUE = '{{"iam_sa_id": "delegated-typed", "iam_resource_id": "{cloud}"}}');)", "cloud"_a = CLOUD_ID));
        ExecAsCloudUser(s3Query("forged_secret"));

        ExecAsCloudUser("DROP EXTERNAL DATA SOURCE `s3Source`;");
        ExecAsCloudUser("DROP SECRET `forged_secret`;");
        ExecAsCloudUser("DROP SECRET `typed_secret`;");
    }
}


// A long execution must not keep the value of a token secret: it re-reads the secret while it runs, so that
// a rotating secret (an IAM delegation, whose value is a short-lived token) never goes stale. The mechanism
// is the same for every token secret, so it is tested here with a value secret, whose rotation is visible
// without IAM.
Y_UNIT_TEST_SUITE(StreamingQuerySecretRefresh) {
    // Re-reading is decided by the type of the secret, not by a flag: a value secret is resolved once when the
    // task starts, as before the feature, even on a cluster with delegation secrets enabled. No credentials
    // provider over a secret reference is created for it.
    Y_UNIT_TEST_F(ValueSecretIsNotReReadWhileTheQueryRuns, TStreamingWithDelegationSecretsTestFixture) {
        // the streaming queries system view is read below
        ExecQuery("GRANT ALL ON `/Root` TO `" BUILTIN_ACL_ROOT "`");

        constexpr char inputSource[] = "noRefreshInputPqSource";
        constexpr char outputSource[] = "noRefreshOutputPqSource";
        constexpr char inputTopic[] = "noRefreshInputTopic";
        constexpr char outputTopic[] = "noRefreshOutputTopic";
        constexpr char secretPath[] = "/Root/no_refresh_token_secret";
        constexpr char queryName[] = "noRefreshTokenQuery";

        CreatePqSource(inputSource);
        CreateTopic(inputTopic);
        CreateTopic(outputTopic);
        ExecQuery(fmt::format(R"(CREATE SECRET `{secret}` WITH (value = "token-1");)", "secret"_a = secretPath));
        ExecQuery(fmt::format(R"(
            CREATE EXTERNAL DATA SOURCE `{output_source}` WITH (
                SOURCE_TYPE = "Ydb",
                LOCATION = "{location}",
                DATABASE_NAME = "{database}",
                AUTH_METHOD = "TOKEN",
                TOKEN_SECRET_PATH = "{secret}"
            );)",
            "output_source"_a = outputSource, "location"_a = YDB_ENDPOINT, "database"_a = YDB_DATABASE, "secret"_a = secretPath));
        ExecQuery(fmt::format(R"(
            CREATE STREAMING QUERY `{query_name}` AS
            DO BEGIN
                INSERT INTO `{output_source}`.`{output_topic}`
                SELECT key || value FROM `{input_source}`.`{input_topic}` WITH (
                    FORMAT = "json_each_row",
                    SCHEMA (
                        key String NOT NULL,
                        value String NOT NULL
                    )
                )
            END DO;)",
            "query_name"_a = queryName, "output_source"_a = outputSource, "output_topic"_a = outputTopic,
            "input_source"_a = inputSource, "input_topic"_a = inputTopic));
        WaitStreamingQueryStatus(queryName, "RUNNING");

        // the query writes (hang guard: the output is inevitable once the pipeline has started)
        const TInstant readFrom = TInstant::Now() - TDuration::Minutes(5);
        auto readSession = [&]() {
            NYdb::NTopic::TReadSessionSettings settings;
            settings.WithoutConsumer().AppendTopics(
                NYdb::NTopic::TTopicReadSettings(outputTopic).ReadFromTimestamp(readFrom).AppendPartitionIds(0));
            settings.EventHandlers_.StartPartitionSessionHandler(
                [](NYdb::NTopic::TReadSessionEvent::TStartPartitionSessionEvent& event) { event.Confirm(0); });
            return GetTopicClient()->CreateReadSession(settings);
        }();
        WaitFor(TDuration::Minutes(3), "output message", [&](TString& error) {
            WriteTopicMessage(inputTopic, R"({"key": "no-", "value": "refresh"})");
            while (readSession->WaitEvent().Wait(TDuration::Seconds(1))) {
                auto event = readSession->GetEvent(/* block */ true);
                if (const auto data = std::get_if<NYdb::NTopic::TReadSessionEvent::TDataReceivedEvent>(&*event)) {
                    for (const auto& message : data->GetMessages()) {
                        if (message.GetData() == "no-refresh") {
                            return true;
                        }
                    }
                } else if (const auto stop = std::get_if<NYdb::NTopic::TSessionClosedEvent>(&*event)) {
                    error = stop->DebugString();
                    return false;
                }
            }
            error = "not received yet";
            return false;
        });

        // the tasks were started (they wrote) and none of them got a re-reading credentials provider
        ui64 providers = 0;
        auto& runtime = GetRuntime();
        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
            providers += GetServiceCounters(runtime.GetAppData(node).Counters, "schema_secrets")
                ->GetSubgroup("component", "refreshing_credentials")->GetCounter("ProvidersCreated", true)->Val();
        }
        UNIT_ASSERT_VALUES_EQUAL(providers, 0u);

        ExecQuery(fmt::format("DROP STREAMING QUERY `{query_name}`;", "query_name"_a = queryName));
    }

    // A streaming query writing through a delegation secret: every re-read of the secret mints a token (the fake
    // token service answers a distinct token per request), a mint held back by the token service does not stop
    // the query, and the released mint is picked up by the same execution.
    Y_UNIT_TEST_F(DelegationSecretIsReMintedWhileTheQueryRuns, TStreamingWithDelegationSecretsTestFixture) {
        LogSettings.AddLogPriority(NKikimrServices::SCHEMA_SECRET_CACHE, NLog::PRI_DEBUG);
        LogSettings.AddLogPriority(NKikimrServices::IAM_DELEGATION, NLog::PRI_DEBUG);
        ExecQuery("GRANT ALL ON `/Root` TO `" BUILTIN_ACL_ROOT "`");

        constexpr char inputSource[] = "remintInputPqSource";
        constexpr char outputSource[] = "remintOutputPqSource";
        constexpr char inputTopic[] = "remintInputTopic";
        constexpr char outputTopic[] = "remintOutputTopic";
        constexpr char secretPath[] = "/Root/remint_delegation_secret";
        constexpr char queryName[] = "remintTokenQuery";

        CreatePqSource(inputSource);
        CreateTopic(inputTopic);
        CreateTopic(outputTopic);

        // the KQP proxies registered the real token service at bootstrap (they have served queries by now):
        // the fake, registered on every node and sharing one record, replaces it
        auto& runtime = GetRuntime();
        auto tokens = NSecret::RegisterFakeIamDelegatedTokenService(runtime, {{"aje-remint", "b1g-cloud"}}, "minted-");
        tokens->UniqueTokens = true;
        for (ui32 node = 1; node < runtime.GetNodeCount(); ++node) {
            NSecret::RegisterFakeIamDelegatedTokenService(runtime, {}, "", node, tokens);
        }
        NSecret::CreateIamDelegationSecretDirect(runtime, secretPath, "aje-remint", "b1g-cloud", "ydb.delegation.0000000000000000000000000000000f");

        ExecQuery(fmt::format(R"(
            CREATE EXTERNAL DATA SOURCE `{output_source}` WITH (
                SOURCE_TYPE = "Ydb",
                LOCATION = "{location}",
                DATABASE_NAME = "{database}",
                AUTH_METHOD = "TOKEN",
                TOKEN_SECRET_PATH = "{secret}"
            );)",
            "output_source"_a = outputSource, "location"_a = YDB_ENDPOINT, "database"_a = YDB_DATABASE, "secret"_a = secretPath));
        ExecQuery(fmt::format(R"(
            CREATE STREAMING QUERY `{query_name}` AS
            DO BEGIN
                INSERT INTO `{output_source}`.`{output_topic}`
                SELECT key || value FROM `{input_source}`.`{input_topic}` WITH (
                    FORMAT = "json_each_row",
                    SCHEMA (
                        key String NOT NULL,
                        value String NOT NULL
                    )
                )
            END DO;)",
            "query_name"_a = queryName, "output_source"_a = outputSource, "output_topic"_a = outputTopic,
            "input_source"_a = inputSource, "input_topic"_a = inputTopic));
        WaitStreamingQueryStatus(queryName, "RUNNING");

        const TInstant readFrom = TInstant::Now() - TDuration::Minutes(5);
        auto readSession = [&]() {
            NYdb::NTopic::TReadSessionSettings settings;
            settings.WithoutConsumer().AppendTopics(
                NYdb::NTopic::TTopicReadSettings(outputTopic).ReadFromTimestamp(readFrom).AppendPartitionIds(0));
            settings.EventHandlers_.StartPartitionSessionHandler(
                [](NYdb::NTopic::TReadSessionEvent::TStartPartitionSessionEvent& event) { event.Confirm(0); });
            return GetTopicClient()->CreateReadSession(settings);
        }();
        // hang guard: the query is running, so the output of a written message is inevitable
        const auto waitForOutput = [&](const TString& key, const TString& value) {
            const std::string expected = TString(key + value);
            WaitFor(TDuration::Minutes(3), TStringBuilder() << "output message " << expected, [&](TString& error) {
                WriteTopicMessage(inputTopic, TStringBuilder() << R"({"key": ")" << key << R"(", "value": ")" << value << R"("})");
                while (readSession->WaitEvent().Wait(TDuration::Seconds(1))) {
                    auto event = readSession->GetEvent(/* block */ true);
                    if (const auto data = std::get_if<NYdb::NTopic::TReadSessionEvent::TDataReceivedEvent>(&*event)) {
                        for (const auto& message : data->GetMessages()) {
                            if (message.GetData() == expected) {
                                return true;
                            }
                        }
                    } else if (const auto stop = std::get_if<NYdb::NTopic::TSessionClosedEvent>(&*event)) {
                        error = stop->DebugString();
                        return false;
                    }
                }
                error = "not received yet";
                return false;
            });
        };
        waitForOutput("before-", "remint");
        UNIT_ASSERT_C(tokens->Calls.load() >= 1, "the execution did not read the secret at its start");

        const auto executionId = [&]() {
            const auto& result = ExecQuery(fmt::format(
                R"(SELECT LastExecutionId FROM `.sys/streaming_queries` WHERE Path = "/Root/{query_name}";)",
                "query_name"_a = queryName));
            UNIT_ASSERT_VALUES_EQUAL(result.size(), 1);
            std::string id;
            CheckScriptResult(result[0], 1, 1, [&id](TResultSetParser& parser) {
                id = parser.ColumnParser("LastExecutionId").GetOptionalUtf8().value_or("");
            });
            UNIT_ASSERT(!id.empty());
            return id;
        };
        const std::string executionBefore = executionId();
        const auto sensor = [&](const char* name) {
            ui64 total = 0;
            for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
                total += GetServiceCounters(runtime.GetAppData(node).Counters, "schema_secrets")
                    ->GetSubgroup("component", "refreshing_credentials")->GetCounter(name, true)->Val();
            }
            return total;
        };

        // the token service holds every mint: the query keeps writing with the token it has
        tokens->HoldReplies = true;
        waitForOutput("held-", "remint");
        UNIT_ASSERT_VALUES_EQUAL(executionId(), executionBefore);

        // the mints are released: a re-read of the secret completes and hands the query a new token (the fake
        // records every token it minted, all distinct)
        const ui64 reReads = sensor("ReReads");
        tokens->HoldReplies = false;
        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
            NSecret::ReleaseHeldDelegatedTokenReplies(runtime, node);
        }
        // hang guard: the writes keep the topic session asking for the token, which re-reads the secret
        WaitFor(TDuration::Minutes(2), "secret re-read by the running query", [&]() {
            WriteTopicMessage(inputTopic, R"({"key": "tick-", "value": "tock"})");
            Sleep(TDuration::MilliSeconds(300));
            return sensor("ReReads") > reReads;
        });
        const auto minted = tokens->MintedTokens();
        UNIT_ASSERT_C(minted.size() >= 2, minted.size());
        UNIT_ASSERT_VALUES_UNEQUAL(minted.front(), minted.back());
        UNIT_ASSERT_VALUES_EQUAL(THashSet<TString>(minted.begin(), minted.end()).size(), minted.size());
        UNIT_ASSERT_VALUES_EQUAL(sensor("ReReadErrors"), 0u);

        // the same execution goes on with the new token
        UNIT_ASSERT_VALUES_EQUAL(executionId(), executionBefore);
        WaitStreamingQueryStatus(queryName, "RUNNING");
        waitForOutput("after-", "remint");

        ExecQuery(fmt::format("DROP STREAMING QUERY `{query_name}`;", "query_name"_a = queryName));
    }
}

} // namespace NKikimr::NKqp
