#include "kqp_iam_delegation_records.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/path.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/library/actors/async/sleep.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/query_actor/query_actor.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/library/table_creator/table_creator.h>

#include <atomic>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::IAM_DELEGATION

namespace NKikimr::NKqp {

using namespace NActors;
using namespace NIamDelegation;
using TRecord = TEvIamDelegation::TDelegationRecord;

namespace {

const TVector<TString> TablePathComponents = {".metadata", "iam_delegation", "delegations"};
constexpr TDuration QueryTimeout = TDuration::Seconds(30);

// The table is created once per node: a flag set after the first successful creation (or when it existed).
std::atomic<bool> TableReady = false;

TString TablePath() {
    return JoinPath(TablePathComponents);
}

NKikimrSchemeOp::TColumnDescription Column(const TString& name, const char* type) {
    NKikimrSchemeOp::TColumnDescription column;
    column.SetName(name);
    column.SetType(type);
    return column;
}

// Creates the table (or finds it) and answers the waiting coroutine with the cookie it was given.
class TTableCreatorWaiter : public TActorBootstrapped<TTableCreatorWaiter> {
public:
    TTableCreatorWaiter(const TActorId& replyTo, ui64 cookie)
        : ReplyTo(replyTo)
        , Cookie(cookie)
    {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        Register(CreateTableCreator(
            TablePathComponents,
            {
                Column("database", "Utf8"),
                Column("referrer_id", "Utf8"),
                Column("secret_path", "Utf8"),
                Column("service_account_id", "Utf8"),
                Column("cloud_id", "Utf8"),
                Column("lease_deadline", "Timestamp"),
            },
            {"database", "referrer_id"},
            NKikimrServices::IAM_DELEGATION,
            Nothing(),
            /* database */ {},
            /* isSystemUser */ true));
    }

    STRICT_STFUNC(StateWork,
        hFunc(TEvTableCreator::TEvCreateTableResponse, Handle);
    )

private:
    void Handle(TEvTableCreator::TEvCreateTableResponse::TPtr& ev) {
        auto result = MakeHolder<TEvIamDelegation::TEvDelegationRecordsResult>();
        if (!ev->Get()->Success) {
            result->Status = Ydb::StatusIds::UNAVAILABLE;
            result->Issues = ev->Get()->Issues;
        }
        Send(ReplyTo, result.Release(), 0, Cookie);
        PassAway();
    }

    const TActorId ReplyTo;
    const ui64 Cookie;
};

enum class EQuery {
    Write,
    Remove,
    ListExpired,
};

// One query over the table, answered with TEvDelegationRecordsResult carrying the cookie it was given.
class TDelegationRecordsQuery : public NKikimr::TQueryBase {
public:
    TDelegationRecordsQuery(EQuery query, TRecord record, TInstant now, ui32 limit, const TActorId& replyTo, ui64 cookie)
        : NKikimr::TQueryBase(NKikimrServices::IAM_DELEGATION, /* sessionId */ {}, /* database */ {}, /* isSystemUser */ true)
        , Query(query)
        , Record(std::move(record))
        , Now(now)
        , Limit(limit)
        , ReplyTo(replyTo)
        , Cookie(cookie)
    {
        SetOperationInfo("IamDelegationRecords", Record.Spec.ReferrerId);
    }

private:
    void OnRunQuery() override {
        NYdb::TParamsBuilder params;
        TStringBuilder sql;
        switch (Query) {
            case EQuery::Write:
                sql << R"(
                    DECLARE $database AS Text;
                    DECLARE $referrer_id AS Text;
                    DECLARE $secret_path AS Text;
                    DECLARE $service_account_id AS Text;
                    DECLARE $cloud_id AS Text;
                    DECLARE $lease_deadline AS Timestamp;

                    UPSERT INTO `)" << TablePath() << R"(`
                        (database, referrer_id, secret_path, service_account_id, cloud_id, lease_deadline)
                    VALUES
                        ($database, $referrer_id, $secret_path, $service_account_id, $cloud_id, $lease_deadline);
                )";
                params
                    .AddParam("$database").Utf8(Record.Database).Build()
                    .AddParam("$referrer_id").Utf8(Record.Spec.ReferrerId).Build()
                    .AddParam("$secret_path").Utf8(Record.SecretPath).Build()
                    .AddParam("$service_account_id").Utf8(Record.Spec.ServiceAccountId).Build()
                    .AddParam("$cloud_id").Utf8(Record.Spec.CloudId).Build()
                    .AddParam("$lease_deadline").Timestamp(Record.LeaseDeadline).Build();
                break;
            case EQuery::Remove:
                sql << R"(
                    DECLARE $database AS Text;
                    DECLARE $referrer_id AS Text;

                    DELETE FROM `)" << TablePath() << R"(`
                    WHERE database = $database AND referrer_id = $referrer_id;
                )";
                params
                    .AddParam("$database").Utf8(Record.Database).Build()
                    .AddParam("$referrer_id").Utf8(Record.Spec.ReferrerId).Build();
                break;
            case EQuery::ListExpired:
                sql << R"(
                    DECLARE $now AS Timestamp;
                    DECLARE $limit AS Uint64;

                    SELECT database, referrer_id, secret_path, service_account_id, cloud_id, lease_deadline
                    FROM `)" << TablePath() << R"(`
                    WHERE lease_deadline < $now
                    LIMIT $limit;
                )";
                params
                    .AddParam("$now").Timestamp(Now).Build()
                    .AddParam("$limit").Uint64(Limit).Build();
                break;
        }
        RunDataQuery(sql, &params);
    }

    void OnQueryResult() override {
        if (Query == EQuery::ListExpired) {
            if (ResultSets.size() != 1) {
                Finish(Ydb::StatusIds::INTERNAL_ERROR, "unexpected number of result sets");
                return;
            }
            NYdb::TResultSetParser result(ResultSets[0]);
            while (result.TryNextRow()) {
                TRecord record;
                record.Database = result.ColumnParser("database").GetOptionalUtf8().value_or("");
                record.Spec.ReferrerId = result.ColumnParser("referrer_id").GetOptionalUtf8().value_or("");
                record.SecretPath = result.ColumnParser("secret_path").GetOptionalUtf8().value_or("");
                record.Spec.ServiceAccountId = result.ColumnParser("service_account_id").GetOptionalUtf8().value_or("");
                record.Spec.CloudId = result.ColumnParser("cloud_id").GetOptionalUtf8().value_or("");
                record.LeaseDeadline = result.ColumnParser("lease_deadline").GetOptionalTimestamp().value_or(TInstant::Zero());
                Records.push_back(std::move(record));
            }
        }
        Finish();
    }

    void OnFinish(Ydb::StatusIds::StatusCode status, NYql::TIssues&& issues) override {
        auto result = MakeHolder<TEvIamDelegation::TEvDelegationRecordsResult>();
        result->Status = status;
        result->Issues = std::move(issues);
        result->Records = std::move(Records);
        Send(ReplyTo, result.Release(), 0, Cookie);
    }

    const EQuery Query;
    const TRecord Record;
    const TInstant Now;
    const ui32 Limit;
    const TActorId ReplyTo;
    const ui64 Cookie;
    std::vector<TRecord> Records;
};

async<TEvIamDelegation::TEvDelegationRecordsResult::TPtr> WaitForRecordsResult(IActor* actor, ui64 cookie) {
    TActivationContext::AsActorContext().Register(actor);
    co_return co_await ActorWaitForEvent<TEvIamDelegation::TEvDelegationRecordsResult>(cookie);
}

// Runs the actor, which answers with the given cookie, and throws on a timeout or a failure.
async<std::vector<TRecord>> RunRecordsActor(std::function<IActor*(const TActorId&, ui64)> makeActor, TStringBuf what) {
    const ui64 cookie = AllocateWaitCookie();
    IActor* actor = makeActor(TActivationContext::AsActorContext().SelfID, cookie);
    auto ev = co_await WithTimeout(QueryTimeout, &WaitForRecordsResult, actor, cookie);
    if (!ev) {
        throw yexception() << what << ": timeout";
    }
    if ((*ev)->Get()->Status != Ydb::StatusIds::SUCCESS) {
        throw yexception() << what << " failed: " << Ydb::StatusIds::StatusCode_Name((*ev)->Get()->Status)
            << " " << (*ev)->Get()->Issues.ToOneLineString();
    }
    co_return std::move((*ev)->Get()->Records);
}

async<std::vector<TRecord>> RunQuery(EQuery query, TRecord record, TInstant now, ui32 limit, TStringBuf what) {
    co_await EnsureDelegationRecordsTable();
    co_return co_await RunRecordsActor([&](const TActorId& replyTo, ui64 cookie) -> IActor* {
        return new TDelegationRecordsQuery(query, record, now, limit, replyTo, cookie);
    }, what);
}

} // namespace

async<void> EnsureDelegationRecordsTable() {
    if (TableReady.load()) {
        co_return;
    }
    co_await RunRecordsActor([](const TActorId& replyTo, ui64 cookie) -> IActor* {
        return new TTableCreatorWaiter(replyTo, cookie);
    }, "creating the table of IAM delegation records");
    TableReady.store(true);
}

async<void> WriteDelegationRecord(TRecord record) {
    co_await RunQuery(EQuery::Write, std::move(record), TInstant::Zero(), 0, "writing an IAM delegation record");
}

async<void> RemoveDelegationRecord(TString database, TString referrerId) {
    TRecord record;
    record.Database = std::move(database);
    record.Spec.ReferrerId = std::move(referrerId);
    co_await RunQuery(EQuery::Remove, std::move(record), TInstant::Zero(), 0, "removing an IAM delegation record");
}

async<std::vector<TRecord>> ListExpiredDelegationRecords(TInstant now, ui32 limit) {
    co_return co_await RunQuery(EQuery::ListExpired, TRecord(), now, limit, "listing IAM delegation records");
}

namespace {

// Revokes the recorded delegations that no secret names, once their lease has passed, and removes the records
// whose outcome is final. Several nodes may reconcile the same record at once: the revocation is idempotent
// and a removal of a missing record is a no-op, so the race is harmless.
class TIamDelegationReconciliationService : public TActorBootstrapped<TIamDelegationReconciliationService>, public IActorExceptionHandler {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::IAM_DELEGATION_RECONCILIATION_ACTOR;
    }

    void Bootstrap() {
        Become(&TThis::StateWork);
        Run();
    }

    STRICT_STFUNC(StateWork,
        // late replies after a timeout
        IgnoreFunc(TEvIamDelegation::TEvDelegationRecordsResult);
        IgnoreFunc(TEvIamDelegation::TEvRevokeDelegationResult);
        IgnoreFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult);
        IgnoreFunc(TEvents::TEvUndelivered);
        cFunc(TEvents::TEvPoison::EventType, BeginShutdown);
    )

    STFUNC(StateDying) {
        Y_UNUSED(ev); // PassAway unregisters the actor only when its coroutine tasks have unwound
    }

    using IActorExceptionHandler::OnUnhandledException;
    bool OnUnhandledException(const std::exception& e) override {
        YDB_LOG_ERROR("Unhandled exception in the IAM delegation reconciliation", {"exception", e.what()});
        return true;
    }

private:
    static constexpr TDuration Interval = TDuration::Minutes(1);
    static constexpr ui32 BatchSize = 100;

    void BeginShutdown() {
        Become(&TThis::StateDying);
        PassAway();
    }

    // top-level coroutine: the reconciliation loop
    void Run() {
        for (;;) {
            co_await AsyncSleepFor(Interval);
            try {
                co_await ReconcileOnce();
            } catch (const std::exception& e) {
                YDB_LOG_WARN("IAM delegation reconciliation failed, retried at the next round", {"error", e.what()});
            }
        }
    }

    async<void> ReconcileOnce() {
        const auto records = co_await ListExpiredDelegationRecords(TActivationContext::Now(), BatchSize);
        for (const auto& record : records) {
            try {
                co_await Reconcile(record);
            } catch (const std::exception& e) {
                YDB_LOG_WARN("Cannot reconcile an IAM delegation record, retried at the next round",
                    {"database", record.Database}, {"path", record.SecretPath}, {"spec", record.Spec.ToString()}, {"error", e.what()});
            }
        }
    }

    async<void> Reconcile(TRecord record) {
        if (co_await SecretNamesReferrer(record)) {
            // the schema change committed with this delegation: the record is final
            co_await RemoveDelegationRecord(record.Database, record.Spec.ReferrerId);
            co_return;
        }
        YDB_LOG_NOTICE("Revoking an IAM delegation that no secret names",
            {"database", record.Database}, {"path", record.SecretPath}, {"spec", record.Spec.ToString()});
        auto ev = co_await WithTimeout(RevokeTimeout, &TIamDelegationReconciliationService::RevokeRequest, this, record.Spec);
        if (!ev) {
            throw yexception() << "RevokeDelegation: timeout";
        }
        if ((*ev)->GetTypeRewrite() != TEvIamDelegation::TEvRevokeDelegationResult::EventType) {
            throw yexception() << "RevokeDelegation: the IAM delegation service is not running on this node";
        }
        const auto& result = (*ev)->Get<TEvIamDelegation::TEvRevokeDelegationResult>()->Result;
        if (!result.IsSuccess() && result.Status != Ydb::StatusIds::NOT_FOUND) {
            throw yexception() << "RevokeDelegation failed: " << result.Issues.ToOneLineString();
        }
        co_await RemoveDelegationRecord(record.Database, record.Spec.ReferrerId);
    }

    static constexpr TDuration RevokeTimeout = TDuration::Minutes(5);

    // Waits for a reply of any type: the typed result, or TEvUndelivered when the service is not running.
    async<IEventHandle::TPtr> RevokeRequest(TDelegationSpec spec) {
        co_return co_await ActorRequest<IEventHandle>(MakeIamDelegationServiceId(),
            new TEvIamDelegation::TEvRevokeDelegation(spec), IEventHandle::FlagTrackDelivery);
    }

    async<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr> NavigateRequest(TString database, TString path) {
        auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
        request->DatabaseName = database;
        auto& entry = request->ResultSet.emplace_back();
        entry.Path = SplitPath(path);
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByPath;
        entry.RedirectRequired = false;
        entry.SyncVersion = true;
        co_return co_await ActorRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(
            MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
    }

    // Whether the secret at the recorded path exists and names the recorded referrer. A missing path counts as
    // "not named"; any other lookup failure throws, so the record is left for the next round.
    async<bool> SecretNamesReferrer(TRecord record) {
        using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
        auto ev = co_await WithTimeout(QueryTimeout, &TIamDelegationReconciliationService::NavigateRequest, this, record.Database, record.SecretPath);
        if (!ev) {
            throw yexception() << "timeout while resolving " << record.SecretPath;
        }
        const auto& resultSet = (*ev)->Get()->Request->ResultSet;
        Y_ENSURE(resultSet.size() == 1);
        const auto& entry = resultSet.front();
        switch (entry.Status) {
            case EStatus::Ok:
                co_return entry.SecretInfo
                    && entry.SecretInfo->Description.HasIamDelegation()
                    && entry.SecretInfo->Description.GetIamDelegation().GetReferrerId() == record.Spec.ReferrerId;
            case EStatus::RootUnknown:
            case EStatus::PathErrorUnknown:
                co_return false;
            default:
                throw yexception() << "cannot resolve " << record.SecretPath << ": " << entry.Status;
        }
    }
};

} // namespace

IActor* CreateIamDelegationReconciliationService() {
    return new TIamDelegationReconciliationService();
}

} // namespace NKikimr::NKqp
