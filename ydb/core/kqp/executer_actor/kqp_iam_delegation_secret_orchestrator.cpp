#include "kqp_iam_delegation_secret_orchestrator.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/path.h>
#include <ydb/core/kqp/gateway/actors/scheme.h>
#include <ydb/core/protos/auth.pb.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation/cloud_resolver.h>
#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation_lifecycle/operations.h>
#include <ydb/core/security/iam_delegation_lifecycle/schema.h>
#include <ydb/core/security/iam_delegation/iam_actor_base.h>

#include <ydb/core/security/iam_delegation/settings.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/async/sleep.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/ycloud/api/folder_service.h>
#include <ydb/library/ycloud/api/service_account_service.h>
#include <ydb/library/services/services.pb.h>

#include <util/generic/guid.h>
#include <util/string/printf.h>

#include <concepts>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::IAM_DELEGATION

namespace NKikimr::NKqp {

using namespace NActors;
using namespace NIamDelegation;
using TGenericResult = NYql::IKikimrGateway::TGenericResult;

namespace {

// Errors of the orchestration reported to the user as is.
class TOrchestrationError : public yexception {
public:
    explicit TOrchestrationError(Ydb::StatusIds::StatusCode status)
        : Status(status)
    {}

    const Ydb::StatusIds::StatusCode Status;
};

struct TEvPrivate {
    enum EEv {
        EvSchemeOpDone = EventSpaceBegin(TEvents::ES_PRIVATE),
    };

    struct TEvSchemeOpDone : TEventLocal<TEvSchemeOpDone, EvSchemeOpDone> {
        TGenericResult Result;

        explicit TEvSchemeOpDone(TGenericResult result)
            : Result(std::move(result))
        {}
    };
};

namespace P = NKikimrIamDelegation;

struct TExistingSecret {
    bool Exists = false;
    bool IsSecret = false;
    NKikimrSchemeOp::TSecretDescription Description;
    NKikimrSchemeOp::TDirEntry Self;
    bool IsDelegation() const { return Exists && IsSecret && Description.HasIamDelegation(); }
};

TGenericResult MakeError(Ydb::StatusIds::StatusCode status, const TString& message) {
    TGenericResult result;
    result.SetStatus(NYql::YqlStatusFromYdbStatus(status));
    result.AddIssue(NYql::TIssue(message));
    return result;
}

TGenericResult MakeSuccess() {
    TGenericResult result;
    result.SetSuccess();
    return result;
}

// Building blocks shared by the two operations. Every method is a nested coroutine and keeps its data in
// its frame; the actor itself owns only the pending scheme request and the promise.
template <class TDerived>
class TDelegationSecretActorBase : public TActorBootstrapped<TDerived>, public IActorExceptionHandler {
public:
    using TBase = TActorBootstrapped<TDerived>;
    using TThis = TDerived;

    explicit TDelegationSecretActorBase(TIamDelegationSecretOperation op)
        : Op(std::move(op))
        , RequestTemplate(Op.Request->Record) // the follow-up scheme requests carry the same database, token and flags
    {
        static_assert(std::derived_from<TDerived, TDelegationSecretActorBase>, "TDerived must derive from TDelegationSecretActorBase<TDerived>");
        static_assert(requires (TDerived& derived) { { derived.Run() } -> std::same_as<async<TGenericResult>>; },
            "TDerived must provide async<TGenericResult> Run()");
    }

    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::IAM_DELEGATION_SECRET_ORCHESTRATOR;
    }

    // top-level coroutine: runs the operation, completes the promise, dies
    void Bootstrap() {
        this->Become(&TDelegationSecretActorBase::StateWork);
        TGenericResult result;
        try {
            result = co_await static_cast<TDerived*>(this)->Run();
        } catch (const TOrchestrationError& e) {
            result = MakeError(e.Status, e.what());
        } catch (const std::exception& e) {
            result = MakeError(Ydb::StatusIds::INTERNAL_ERROR, TStringBuilder() << "IAM delegation secret operation failed: " << e.what());
        }
        Op.Promise.SetValue(std::move(result));
        this->Become(&TDelegationSecretActorBase::StateDying);
        this->PassAway();
    }

    STRICT_STFUNC(StateWork,
        // replies are consumed by ActorRequest; late ones (after a timeout) end up here
        IgnoreFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult);
        IgnoreFunc(TEvIamDelegation::TEvDelegationOperationResult);
        IgnoreFunc(TEvIamDelegationTablet::TEvResponse);
        IgnoreFunc(TEvTxUserProxy::TEvAllocateTxIdResult);
        IgnoreFunc(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult);
        IgnoreFunc(NCloud::TEvServiceAccountService::TEvGetServiceAccountResponse); // the clients of ResolveCloud
        IgnoreFunc(NCloud::TEvFolderService::TEvResolveFoldersResponse);
        IgnoreFunc(TEvents::TEvUndelivered);
        IgnoreFunc(TEvPrivate::TEvSchemeOpDone);
        cFunc(TEvents::TEvPoison::EventType, BeginShutdown);
    )

    STFUNC(StateDying) {
        Y_UNUSED(ev); // PassAway unregisters the actor only when its coroutine tasks have unwound; events arriving meanwhile are dropped
    }

    // An exception outside the coroutine of Run (Bootstrap catches those): the executer still waits for the promise.
    using IActorExceptionHandler::OnUnhandledException;
    bool OnUnhandledException(const std::exception& e) override {
        YDB_LOG_ERROR("Unhandled exception in the IAM delegation secret orchestrator", {"database", Op.Database}, {"exception", e.what()});
        if (!Op.Promise.HasValue()) {
            Op.Promise.SetValue(MakeError(Ydb::StatusIds::INTERNAL_ERROR, TStringBuilder() << "IAM delegation secret operation failed: " << e.what()));
        }
        this->Become(&TDelegationSecretActorBase::StateDying);
        this->PassAway();
        return true;
    }

protected:
    // The executer waits for the promise: a cancelled operation must complete it rather than leave it hanging.
    // The independent tablet retains every delegation that may have reached IAM.
    void BeginShutdown() {
        if (!Op.Promise.HasValue()) {
            YDB_LOG_WARN("Cancelled while in flight", {"database", Op.Database});
            Op.Promise.SetValue(MakeError(Ydb::StatusIds::CANCELLED, "IAM delegation secret operation was cancelled"));
        }
        this->Become(&TDelegationSecretActorBase::StateDying);
        this->PassAway();
    }

    NKikimrSchemeOp::TModifyScheme& ModifyScheme() {
        AFL_ENSURE(Op.Request)("database", Op.Database)("reason", "the scheme operation was already executed");
        return *Op.Request->Record.MutableTransaction()->MutableModifyScheme();
    }

    TString SecretPath(const TString& name) {
        return CanonizePath(JoinPath({RequestTemplate.GetTransaction().GetModifyScheme().GetWorkingDir(), name}));
    }

    // The statement's own scheme request, executed once.
    THolder<TEvTxUserProxy::TEvProposeTransaction> TakeRequest() {
        AFL_ENSURE(Op.Request)("database", Op.Database)("reason", "the scheme operation was already executed");
        return std::move(Op.Request);
    }

    // A follow-up scheme request of the statement: the same database, token and flags, another operation.
    THolder<TEvTxUserProxy::TEvProposeTransaction> MakeRequest(NKikimrSchemeOp::EOperationType type) {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record = RequestTemplate;
        auto& modifyScheme = *request->Record.MutableTransaction()->MutableModifyScheme();
        modifyScheme.Clear();
        modifyScheme.SetWorkingDir(RequestTemplate.GetTransaction().GetModifyScheme().GetWorkingDir());
        modifyScheme.SetOperationType(type);
        return request;
    }

    THolder<TEvTxUserProxy::TEvProposeTransaction> DropRequest(const TString& name) {
        auto request = MakeRequest(NKikimrSchemeOp::ESchemeOpDropSecret);
        request->Record.MutableTransaction()->MutableModifyScheme()->MutableDrop()->SetName(name);
        return request;
    }

    // Settings of the paths that call ServiceControl (SetupDelegation and RevokeDelegation). Reading a
    // delegation secret does not go through here: it needs the token service only.
    TIamDelegationSettings DelegationSettings() {
        auto settings = TIamDelegationSettings::FromConfig(AppData()->IamConfig, AppData()->ReplicationConfig);
        if (const TString error = settings.ValidateForDelegation()) {
            throw TOrchestrationError(Ydb::StatusIds::PRECONDITION_FAILED) << error;
        }
        return settings;
    }

    TString ResolveSubjectId() {
        if (!Op.UserToken || Op.UserToken->GetUserSID().empty()) {
            throw TOrchestrationError(Ydb::StatusIds::UNAUTHORIZED)
                << "IAM delegation secrets require an authenticated Yandex Cloud user";
        }
        const auto subjectId = ExtractCloudSubjectId(*Op.UserToken, AppData()->AuthConfig.GetAccessServiceDomain());
        if (!subjectId) {
            throw TOrchestrationError(Ydb::StatusIds::BAD_REQUEST)
                << "IAM delegation secrets can be created or altered only by Yandex Cloud subjects; user '"
                << Op.UserToken->GetUserSID() << "' is not a cloud subject";
        }
        return *subjectId;
    }

    async<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr> NavigateRequest(TString path, ui32 access) {
        auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
        request->DatabaseName = Op.Database;
        request->UserToken = Op.UserToken;
        auto& entry = request->ResultSet.emplace_back();
        entry.Path = SplitPath(path);
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        entry.Access = access;
        entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByPath;
        entry.RedirectRequired = false;
        entry.SyncVersion = true;
        co_return co_await ActorRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(
            MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
    }

    async<NSchemeCache::TSchemeCacheNavigate::TEntry> Navigate(TString path, ui32 access = NACLib::DescribeSchema) {
        auto ev = co_await WithTimeout(Op.Timeouts.Navigate, &TDelegationSecretActorBase::NavigateRequest, this, path, access);
        if (!ev) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "timeout while resolving " << path;
        }
        auto& result = *(*ev)->Get()->Request;
        AFL_ENSURE(result.ResultSet.size() == 1)("path", path)("entries", result.ResultSet.size());
        co_return std::move(result.ResultSet.front());
    }

    async<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr> DescribeRequest(TString path) {
        auto request = MakeHolder<TEvTxUserProxy::TEvNavigate>();
        request->Record.SetDatabaseName(Op.Database);
        request->Record.SetUserToken(RequestTemplate.GetUserToken());
        request->Record.MutableDescribePath()->SetPath(path);
        co_return co_await ActorRequest<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>(MakeTxProxyID(), request.Release());
    }

    async<TExistingSecret> NavigateSecret(TString path) {
        const auto reply = co_await WithTimeout(Op.Timeouts.Navigate, &TDelegationSecretActorBase::DescribeRequest, this, path);
        if (!reply) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot describe " << path;
        }
        const auto& record = (*reply)->Get()->GetRecord();
        TExistingSecret result;
        if (record.GetStatus() == NKikimrScheme::StatusPathDoesNotExist) {
            co_return result;
        }
        if (record.GetStatus() != NKikimrScheme::StatusSuccess) {
            throw TOrchestrationError(record.GetStatus() == NKikimrScheme::StatusAccessDenied
                ? Ydb::StatusIds::UNAUTHORIZED : Ydb::StatusIds::UNAVAILABLE) << record.GetReason();
        }
        result.Exists = true;
        result.Self = record.GetPathDescription().GetSelf();
        result.IsSecret = result.Self.GetPathType() == NKikimrSchemeOp::EPathTypeSecret;
        result.Description = record.GetPathDescription().GetSecretDescription();
        co_return result;
    }

    // Internal existence check only: the reply's metadata must never authorize
    // an operation or be used for a lifecycle identity.
    async<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr> ProbeSecretExistenceRequest(TString path) {
        auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
        request->DatabaseName = Op.Database;
        auto& entry = request->ResultSet.emplace_back();
        entry.Path = SplitPath(path);
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        entry.Access = NACLib::NoAccess;
        entry.RedirectRequired = false;
        entry.SyncVersion = true;
        co_return co_await ActorRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(
            MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
    }

    async<bool> SecretExists(TString path) {
        const auto reply = co_await WithTimeout(Op.Timeouts.Navigate,
            &TDelegationSecretActorBase::ProbeSecretExistenceRequest, this, path);
        if (!reply) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "timeout while resolving " << path;
        }
        const auto& entries = (*reply)->Get()->Request->ResultSet;
        AFL_ENSURE(entries.size() == 1)("path", path)("entries", entries.size());
        using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
        if (entries.front().Status == EStatus::PathErrorUnknown) {
            co_return false;
        }
        if (entries.front().Status != EStatus::Ok) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot resolve " << path << ": " << entries.front().Status;
        }
        co_return true;
    }

    // DDL metadata is authorized by the operation's permission, not by public
    // DescribeSchema. Identity and description come from the same synchronized
    // cache entry; the authenticated schema statement rechecks ACL and version.
    async<TExistingSecret> NavigateSecretForAlter(TString path) {
        if (!Op.UserToken) {
            throw TOrchestrationError(Ydb::StatusIds::UNAUTHORIZED) << "Access denied to " << path;
        }
        const auto entry = co_await Navigate(path, NACLib::AlterSchema);
        using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
        if (entry.Status == EStatus::PathErrorUnknown) {
            // The cache also hides an ACL denial as a missing path when the
            // caller lacks DescribeSchema. An internal existence-only probe
            // distinguishes absence from denial; no metadata leaves that probe.
            if (co_await SecretExists(path)) {
                throw TOrchestrationError(Ydb::StatusIds::UNAUTHORIZED) << "Access denied to " << path;
            }
            co_return TExistingSecret{};
        }
        if (entry.Status == EStatus::AccessDenied) {
            throw TOrchestrationError(Ydb::StatusIds::UNAUTHORIZED) << "Access denied to " << path;
        }
        if (entry.Status != EStatus::Ok || !entry.Self) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot resolve " << path << ": " << entry.Status;
        }
        TExistingSecret result;
        result.Exists = true;
        result.Self = entry.Self->Info;
        result.IsSecret = entry.Kind == NSchemeCache::TSchemeCacheNavigate::KindSecret;
        if (result.IsSecret) {
            if (!entry.SecretInfo) {
                throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot resolve secret metadata for " << path;
            }
            result.Description = entry.SecretInfo->Description;
        }
        co_return result;
    }

    async<void> Authorize(TString path, ui32 access) {
        const auto entry = co_await Navigate(path, access);
        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok || !Op.UserToken
            || !entry.SecurityObject || !entry.SecurityObject->CheckAccess(access, *Op.UserToken)) {
            throw TOrchestrationError(Ydb::StatusIds::UNAUTHORIZED) << "Access denied to " << path;
        }
    }

    // cloud_id attribute of the database; why is what made this the fallback for the cloud of the delegation
    async<TString> DatabaseCloudId(TString why) {
        const auto entry = co_await Navigate(Op.Database);
        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "cannot resolve database " << Op.Database << ": " << entry.Status;
        }
        const auto it = entry.Attributes.find("cloud_id");
        if (it == entry.Attributes.end() || it->second.empty()) {
            throw TOrchestrationError(Ydb::StatusIds::BAD_REQUEST)
                << "database " << Op.Database << " has no cloud_id attribute and " << why << "; specify RESOURCE explicitly";
        }
        co_return it->second;
    }

    // The cloud of the delegation when RESOURCE is omitted: the cloud of the service account, looked up
    // with the user's own IAM token (the system service account of YDB may not read service accounts).
    // The cloud_id attribute of the database is the fallback when there is no user token to forward,
    // the lookup is not configured, or it fails, e.g. the user lacks iam.serviceAccounts.get. The
    // fallback is reported as a warning: it is right only when the account and the database share
    // a cloud, and a wrong guess surfaces later as an IAM error.
    async<TString> ResolveCloudId(TString serviceAccountId) {
        TString why;
        const auto settings = DelegationSettings();
        const TString userToken = Op.UserToken ? Op.UserToken->GetOriginalUserToken() : TString();
        if (!settings.CanResolveCloud()) {
            why = "IamConfig.ResourceManagerEndpoint is not configured";
        } else if (userToken.empty()) {
            why = "the user has no IAM token to look the service account up with";
        } else {
            try {
                auto resolved = co_await WithTimeout(Op.Timeouts.CloudLookup, &ResolveCloud, settings, userToken, serviceAccountId);
                if (resolved) {
                    co_return resolved->CloudId;
                }
                why = "timeout while looking the service account up";
            } catch (const TIamCallError& e) {
                why = e.what();
            }
        }
        why = TStringBuilder() << "the cloud of service account " << serviceAccountId << " is unknown: " << why;
        YDB_LOG_WARN("Using the cloud of the database", {"database", Op.Database}, {"reason", why});
        const TString cloudId = co_await DatabaseCloudId(why);
        NYql::TIssue issue(TStringBuilder() << "RESOURCE " << cloudId << " was taken from the database, " << why);
        issue.SetCode(NYql::DEFAULT_ERROR, NYql::TSeverityIds::S_WARNING);
        Warnings.AddIssue(issue);
        co_return cloudId;
    }

    async<TEvPrivate::TEvSchemeOpDone::TPtr> WaitScheme(ui64 cookie) {
        co_return co_await ActorWaitForEvent<TEvPrivate::TEvSchemeOpDone>(cookie);
    }

    async<IEventHandle::TPtr> AllocateTransaction() {
        co_return co_await ActorRequest<IEventHandle>(MakeTxProxyID(), new TEvTxUserProxy::TEvAllocateTxId,
            IEventHandle::FlagTrackDelivery);
    }

    // Executes a scheme request through the regular scheme request handler. The flags of the statement
    // (IF EXISTS, IF NOT EXISTS) apply to its own request only.
    async<TGenericResult> RunSchemeOp(THolder<TEvTxUserProxy::TEvProposeTransaction> request, bool failedOnAlreadyExists = false, bool successOnNotExist = false, TActorId txProxy = {}) {
        // the handler completes the promise from its own turn or from another thread, so the result comes
        // back as a self-event; it cannot be handled before this turn ends, so the wait below
        // still intercepts it
        const ui64 cookie = AllocateWaitCookie();
        auto promise = NThreading::NewPromise<TGenericResult>();
        auto* actorSystem = TActivationContext::ActorSystem();
        const TActorId selfId = this->SelfId();
        promise.GetFuture().Subscribe([actorSystem, selfId, cookie](const NThreading::TFuture<TGenericResult>& future) {
            actorSystem->Send(new IEventHandle(selfId, selfId, new TEvPrivate::TEvSchemeOpDone(future.GetValue()), 0, cookie));
        });
        this->RegisterWithSameMailbox(new TSchemeOpRequestHandler(request.Release(), promise, failedOnAlreadyExists, successOnNotExist, NWilson::TTraceId{}, txProxy));
        auto ev = co_await WithTimeout(TDuration::Minutes(5), &TDelegationSecretActorBase::WaitScheme, this, cookie);
        if (!ev) {
            co_return MakeError(Ydb::StatusIds::UNAVAILABLE, "Scheme operation timed out; its outcome is unknown");
        }
        co_return (*ev)->Get()->Result;
    }

    async<TGenericResult> RunStatementSchemeOp() {
        co_return co_await RunSchemeOp(TakeRequest(), Op.FailedOnAlreadyExists, Op.SuccessOnNotExist);
    }

    async<P::TResponse> Tablet(P::TRequest request) {
        auto response = co_await CallIamDelegationTablet(TabletId, std::move(request));
        if (response.GetStatus() != P::SUCCESS) {
            throw TOrchestrationError(response.GetStatus() == P::CONFLICT || response.GetStatus() == P::PRECONDITION_FAILED
                ? Ydb::StatusIds::PRECONDITION_FAILED : Ydb::StatusIds::UNAVAILABLE)
                << "IAM delegation tablet: " << response.GetError();
        }
        co_return response;
    }

    async<P::TResponse> ReadOperation(TString id) {
        P::TRequest request;
        request.MutableGetDelegation()->SetOperationId(id);
        co_return co_await Tablet(std::move(request));
    }

    P::TSecretIdentity Identity(const TExistingSecret& schema) const {
        P::TSecretIdentity identity;
        identity.SetDatabaseIncarnation(schema.Description.GetIamDelegation().GetDatabaseIncarnation());
        identity.SetPathOwnerId(schema.Self.GetSchemeshardId());
        identity.SetPathLocalId(schema.Self.GetPathId());
        return identity;
    }

    async<P::TSecretRecord> ReadSecret(const TExistingSecret& schema) {
        P::TRequest request;
        *request.MutableGetSecret()->MutableSecret() = Identity(schema);
        const auto response = co_await Tablet(std::move(request));
        const auto& secret = response.GetSecret();
        if (secret.GetState() != P::SECRET_LIVE || secret.GetCreateTxId() != schema.Self.GetCreateTxId()
            || secret.GetCreateOperationId() != schema.Description.GetIamDelegation().GetCreateOperationId()) {
            throw TOrchestrationError(Ydb::StatusIds::PRECONDITION_FAILED) << "External secret identity changed";
        }
        co_return secret;
    }

    P::TIamBinding NewBinding(const TString& account, const TString& cloud) {
        const auto settings = DelegationSettings();
        P::TIamBinding binding;
        binding.SetServiceAccountId(account);
        binding.SetCloudId(cloud);
        binding.SetReferrerId(NewReferrerId());
        binding.SetServiceId(settings.Config.GetServiceId());
        binding.SetMicroserviceId(settings.Config.GetMicroserviceId());
        binding.SetResourceType(settings.Config.GetResourceType());
        binding.SetReferrerType(settings.ReferrerType);
        binding.SetReferencePolicy(P::WITHOUT_REFERENCES);
        return binding;
    }

    void FenceSchema(NKikimrSchemeOp::TModifyScheme& modify, const TExistingSecret& schema) {
        auto* condition = modify.AddApplyIf();
        condition->SetPathOwnerId(schema.Self.GetSchemeshardId());
        condition->SetPathId(schema.Self.GetPathId());
        condition->SetPathVersion(schema.Self.GetPathVersion());
    }

    async<P::TDelegation> SaveSetup(P::TDelegation delegation, const TDelegationOperationResult& result) {
        for (ui32 attempt = 0; attempt < 3; ++attempt) {
            P::TRequest update;
            auto* setup = update.MutableSetSetupResult();
            setup->SetOperationId(delegation.GetOperationId());
            setup->SetExpectedRevision(delegation.GetRevision());
            setup->SetOutcome(result.Outcome == EDelegationOperationOutcome::Succeeded ? P::SETUP_SUCCEEDED
                : result.Outcome == EDelegationOperationOutcome::Failed ? P::SETUP_FAILED : P::SETUP_UNKNOWN);
            setup->SetSetupOperationId(result.OperationId);
            const auto saved = co_await CallIamDelegationTablet(TabletId, std::move(update));
            if (saved.GetStatus() == P::SUCCESS) {
                co_return saved.GetDelegation();
            }
            delegation = (co_await ReadOperation(delegation.GetOperationId())).GetDelegation();
            if (delegation.GetSetupState() == P::SETUP_SUCCEEDED || delegation.GetSetupState() == P::SETUP_FAILED) {
                co_return delegation;
            }
        }
        throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot persist IAM setup outcome; retry after recovery";
    }

    async<TGenericResult> SetupAndPromote(P::TDelegation delegation, const TString& subject) {
        P::TRequest start;
        start.MutableStartSetup()->SetOperationId(delegation.GetOperationId());
        start.MutableStartSetup()->SetExpectedRevision(delegation.GetRevision());
        const auto started = co_await CallIamDelegationTablet(TabletId, std::move(start));
        TDelegationOperationResult result;
        if (started.GetStatus() == P::SUCCESS) {
            delegation = started.GetDelegation();
            result = co_await CallIamOperation(new TEvIamDelegation::TEvSubmitDelegationOperation(
                EDelegationOperationKind::Setup, ToIamBinding(delegation.GetBinding()), subject, delegation.GetOperationId()));
            delegation = co_await SaveSetup(delegation, result);
        } else {
            // A recovery worker may own dispatch, or our Start reply may have
            // been lost. Reading STARTED is never permission to send Setup again.
            delegation = (co_await ReadOperation(delegation.GetOperationId())).GetDelegation();
        }
        const auto deadline = TActivationContext::Monotonic() + TDuration::Minutes(1);
        while ((delegation.GetSetupState() == P::SETUP_UNKNOWN || delegation.GetSetupState() == P::SETUP_STARTED)
            && TActivationContext::Monotonic() < deadline) {
            co_await AsyncSleepFor(TDuration::Seconds(1));
            if (delegation.GetSetupOperationId()) {
                result = co_await CallIamOperation(new TEvIamDelegation::TEvPollDelegationOperation(delegation.GetSetupOperationId()));
                if (result.Outcome == EDelegationOperationOutcome::Succeeded || result.Outcome == EDelegationOperationOutcome::Failed) {
                    delegation = co_await SaveSetup(delegation, result);
                }
            }
            delegation = (co_await ReadOperation(delegation.GetOperationId())).GetDelegation();
        }
        if (delegation.GetSetupState() != P::SETUP_SUCCEEDED) {
            auto failure = MakeError(delegation.GetSetupState() == P::SETUP_FAILED && result.Status != Ydb::StatusIds::SUCCESS
                ? result.Status : Ydb::StatusIds::UNAVAILABLE,
                delegation.GetSetupState() == P::SETUP_FAILED ? "IAM refused delegation setup"
                    : "IAM setup outcome is not yet known; the durable intent is retained for recovery");
            for (const auto& issue : result.Issues) {
                failure.AddIssue(issue);
            }
            co_return failure;
        }
        const auto current = co_await ReadOperation(delegation.GetOperationId());
        P::TRequest promote;
        auto* command = promote.MutablePromote();
        command->SetOperationId(delegation.GetOperationId());
        command->SetExpectedRevision(current.GetDelegation().GetRevision());
        command->SetExpectedSecretRevision(current.GetSecret().GetRevision());
        co_await Tablet(std::move(promote));
        co_return MakeSuccess();
    }

    async<TGenericResult> Replace(TString path, const TExistingSecret& existing,
        NKikimrSchemeOp::TSecretSchemaOp& secretOp)
    {
        if (!existing.IsDelegation()) {
            throw TOrchestrationError(Ydb::StatusIds::BAD_REQUEST) << "Cannot change the source of a secret";
        }
        co_await Authorize(path, NACLib::AlterSchema);
        TableIdFrom(existing);
        auto secret = co_await ReadSecret(existing);
        const auto requested = secretOp.GetIamDelegation();
        P::TIamBinding previous;
        if (secret.GetCurrentOperationId()) {
            previous = (co_await ReadOperation(secret.GetCurrentOperationId())).GetDelegation().GetBinding();
        } else {
            previous = (co_await ReadOperation(secret.GetCreateOperationId())).GetDelegation().GetBinding();
        }
        TString account = requested.GetServiceAccountId() ? requested.GetServiceAccountId() : previous.GetServiceAccountId();
        TString cloud = requested.GetCloudId() ? requested.GetCloudId() : previous.GetCloudId();
        if (account.empty()) {
            throw TOrchestrationError(Ydb::StatusIds::BAD_REQUEST) << "SERVICE_ACCOUNT_ID is required";
        }
        if (cloud.empty()) {
            cloud = co_await ResolveCloudId(account);
        }
        const bool needsSetup = !secret.GetCurrentOperationId()
            || account != previous.GetServiceAccountId() || cloud != previous.GetCloudId();
        P::TDelegation pending;
        if (secret.GetPendingOperationId()) {
            pending = (co_await ReadOperation(secret.GetPendingOperationId())).GetDelegation();
            // Expiry permits detaching, never revoking an unknown setup.
            if ((pending.GetSetupState() == P::SETUP_STARTED || pending.GetSetupState() == P::SETUP_UNKNOWN)
                && TInstant::MicroSeconds(pending.GetStagedAtUs()) + TDuration::Minutes(10) > TActivationContext::Now()) {
                throw TOrchestrationError(Ydb::StatusIds::PRECONDITION_FAILED) << "A replacement is still in flight; retry ALTER after ten minutes";
            }
        }
        TString subject;
        P::TIamBinding binding;
        if (needsSetup) {
            subject = ResolveSubjectId();
            binding = NewBinding(account, cloud);
        }
        // Recheck ACL and immutable schema identity before changing lifecycle.
        // A no-op needs no IAM configuration and cancels a superseded pending
        // replacement so it cannot silently become current later.
        secretOp.ClearIamDelegation();
        *secretOp.MutableIamDelegationReference() = existing.Description.GetIamDelegation();
        FenceSchema(ModifyScheme(), existing);
        const auto authorized = co_await RunStatementSchemeOp();
        if (!authorized.Success()) {
            co_return authorized;
        }
        if (secret.GetPendingOperationId()) {
            P::TRequest retire;
            auto* command = retire.MutableRetirePending();
            *command->MutableSecret() = secret.GetIdentity();
            command->SetOperationId(pending.GetOperationId());
            command->SetExpectedRevision(pending.GetRevision());
            command->SetExpectedSecretRevision(secret.GetRevision());
            secret = (co_await Tablet(std::move(retire))).GetSecret();
        }
        if (!needsSetup) {
            co_return MakeSuccess();
        }
        P::TRequest stage;
        auto* command = stage.MutableStage();
        command->SetMode(P::ALTER);
        command->SetOperationId(CreateGuidAsString());
        command->SetDatabaseIncarnation(secret.GetIdentity().GetDatabaseIncarnation());
        command->SetSecretPath(path);
        command->SetCreateTxId(secret.GetCreateTxId());
        *command->MutableSecret() = secret.GetIdentity();
        command->SetExpectedSecretRevision(secret.GetRevision());
        *command->MutableBinding() = binding;
        command->SetSubjectId(subject);
        const auto staged = co_await Tablet(std::move(stage));
        co_return co_await SetupAndPromote(staged.GetDelegation(), subject);
    }

    void TableIdFrom(const TExistingSecret& existing) {
        TabletId = existing.Description.GetIamDelegation().GetTabletId();
        if (!TabletId) {
            throw TOrchestrationError(Ydb::StatusIds::PRECONDITION_FAILED) << "External secret has no delegation tablet";
        }
    }

    async<TGenericResult> RunCreate() {
        auto& op = *ModifyScheme().MutableCreateSecret();
        const TString name = op.GetName();
        const TString path = SecretPath(name);
        const auto existing = co_await (ModifyScheme().GetReplaceIfExists()
            ? NavigateSecretForAlter(path) : NavigateSecret(path));
        if (existing.Exists) {
            if (ModifyScheme().GetReplaceIfExists()) {
                co_return co_await Replace(path, existing, op);
            }
            co_await Authorize(RequestTemplate.GetTransaction().GetModifyScheme().GetWorkingDir(), NACLib::CreateTable);
            if (!ModifyScheme().GetFailOnExist() && existing.IsSecret) {
                co_return MakeSuccess();
            }
            throw TOrchestrationError(Ydb::StatusIds::ALREADY_EXISTS) << "Path already exists: " << path;
        }
        co_await Authorize(RequestTemplate.GetTransaction().GetModifyScheme().GetWorkingDir(), NACLib::CreateTable);
        const TString subject = ResolveSubjectId();
        const auto settings = DelegationSettings();
        TabletId = settings.Config.GetDelegationTabletId();
        if (!TabletId) {
            throw TOrchestrationError(Ydb::StatusIds::PRECONDITION_FAILED) << "IamConfig.DelegationTabletId is required";
        }
        const TString account = op.GetIamDelegation().GetServiceAccountId();
        if (account.empty()) {
            throw TOrchestrationError(Ydb::StatusIds::BAD_REQUEST) << "SERVICE_ACCOUNT_ID is required";
        }
        TString cloud = op.GetIamDelegation().GetCloudId();
        if (cloud.empty()) {
            cloud = co_await ResolveCloudId(account);
        }
        const auto database = co_await Navigate(Op.Database);
        if (database.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok || !database.DomainInfo) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot resolve immutable database identity";
        }
        const auto key = database.DomainInfo->DomainKey;
        const TString incarnation = TStringBuilder() << key.OwnerId << ':' << key.LocalPathId;
        const auto parent = co_await NavigateSecret(TString(ExtractParent(path)));
        if (!parent.Exists || !parent.Self.GetCreateFinished()) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot resolve completed secret parent";
        }
        const bool ifNotExists = !ModifyScheme().GetFailOnExist() && !ModifyScheme().GetReplaceIfExists();
        P::TRequest registration;
        auto* identity = registration.MutableRegisterDatabase()->MutableIdentity();
        identity->SetIncarnation(incarnation);
        identity->SetPath(Op.Database);
        const auto databaseId = database.Attributes.find("database_id");
        identity->SetDatabaseId(databaseId == database.Attributes.end() || databaseId->second.empty() ? incarnation : databaseId->second);
        co_await Tablet(std::move(registration));
        auto allocated = co_await WithTimeout(TDuration::Seconds(30), &TDelegationSecretActorBase::AllocateTransaction, this);
        if (!allocated || (*allocated)->GetTypeRewrite() != TEvTxUserProxy::TEvAllocateTxIdResult::EventType) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Cannot allocate schema transaction ID";
        }
        auto allocation = THolder<TEvTxUserProxy::TEvAllocateTxIdResult>((*allocated)->template Release<TEvTxUserProxy::TEvAllocateTxIdResult>().Release());
        const ui64 txId = allocation->TxId;
        const TString operationId = CreateGuidAsString();
        P::TRequest stage;
        auto* command = stage.MutableStage();
        command->SetMode(P::CREATE);
        command->SetOperationId(operationId);
        command->SetDatabaseIncarnation(incarnation);
        command->SetSecretPath(path);
        command->SetCreateTxId(txId);
        command->SetSubjectId(subject);
        *command->MutableBinding() = NewBinding(account, cloud);
        auto* schemaIntent = command->MutableSchemaCreate();
        schemaIntent->SetDatabasePath(Op.Database);
        schemaIntent->SetUserSID(Op.UserToken->GetUserSID());
        for (const auto& group : Op.UserToken->GetGroupSIDs()) {
            schemaIntent->AddGroupSIDs(group);
        }
        schemaIntent->SetParentPathOwnerId(parent.Self.GetSchemeshardId());
        schemaIntent->SetParentPathLocalId(parent.Self.GetPathId());
        schemaIntent->SetParentPathVersion(parent.Self.GetPathVersion());
        schemaIntent->SetDatabaseDomainOwnerId(key.OwnerId);
        schemaIntent->SetDatabaseDomainLocalId(key.LocalPathId);
        schemaIntent->SetInheritPermissions(op.HasInheritPermissions() ? op.GetInheritPermissions()
            : AppData()->AlwaysSetSystemOwner || AppData()->FeatureFlags.GetEnableIdmPermissionsManagement());
        if (ModifyScheme().GetModifyACL().HasDiffACL()) {
            schemaIntent->SetDiffACL(ModifyScheme().GetModifyACL().GetDiffACL());
        }
        auto delegation = (co_await Tablet(std::move(stage))).GetDelegation();
        auto request = MakeIamDelegationSchemaCreate(delegation, TabletId);
        request->Record.SetPeerName(TakeRequest()->Record.GetPeerName());
        const auto proxy = this->Register(CreateIamDelegationSchemaProxy(txId, std::move(allocation)));
        auto result = co_await RunSchemeOp(std::move(request), true, false, proxy);
        // Check parent before child. A reply alone cannot fence another copy
        // submitted by recovery or an earlier delayed transport attempt.
        const auto observation = co_await ObserveIamDelegationSchemaCreate(delegation, TabletId);
        TExistingSecret created;
        created.Exists = observation.Schema.GetStatus() == NKikimrScheme::StatusSuccess;
        created.Self = observation.Schema.GetPathDescription().GetSelf();
        created.IsSecret = created.Self.GetPathType() == NKikimrSchemeOp::EPathTypeSecret;
        created.Description = observation.Schema.GetPathDescription().GetSecretDescription();
        const bool ours = observation.State == ESchemaCreateState::Created
            || observation.State == ESchemaCreateState::Creating;
        if (!ours) {
            if (observation.State == ESchemaCreateState::Retire) {
                P::TRequest retire;
                retire.MutableDropSecret()->SetOperationId(operationId);
                retire.MutableDropSecret()->SetExpectedRevision(delegation.GetRevision());
                co_await Tablet(std::move(retire));
                if (ifNotExists && created.Exists && created.IsSecret) {
                    co_return MakeSuccess();
                }
                co_return result.Success() ? MakeError(Ydb::StatusIds::PRECONDITION_FAILED,
                    "Secret CREATE parent or object identity changed") : result;
            }
            // In particular, an ACL/quota rejection of this copy does not
            // reject a previously authorized request still queued in SchemeShard.
            co_return MakeError(Ydb::StatusIds::UNAVAILABLE,
                "Secret CREATE outcome is not yet known; the durable intent is retained for recovery");
        }
        if (!created.Self.GetCreateFinished()) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "Secret creation has not completed; intent retained";
        }
        // A worker may already have bound the verified shell after a lost reply.
        delegation = (co_await ReadOperation(operationId)).GetDelegation();
        if (!delegation.HasSecret()) {
            P::TRequest bind;
            bind.MutableBindSecret()->SetOperationId(operationId);
            bind.MutableBindSecret()->SetExpectedRevision(delegation.GetRevision());
            *bind.MutableBindSecret()->MutableSecret() = Identity(created);
            delegation = (co_await Tablet(std::move(bind))).GetDelegation();
        }
        result = co_await SetupAndPromote(delegation, subject);
        if (!result.Success()) {
            const auto state = co_await ReadOperation(operationId);
            if (state.GetDelegation().GetSetupState() == P::SETUP_FAILED) {
                auto drop = DropRequest(name);
                drop->Record.MutableTransaction()->MutableModifyScheme()->MutableDrop()->SetId(created.Self.GetPathId());
                FenceSchema(*drop->Record.MutableTransaction()->MutableModifyScheme(), created);
                const auto removed = co_await RunSchemeOp(std::move(drop));
                if (removed.Success()) {
                    P::TRequest retire;
                    *retire.MutableDropSecret()->MutableSecret() = Identity(created);
                    retire.MutableDropSecret()->SetExpectedRevision(state.GetSecret().GetRevision());
                    co_await Tablet(std::move(retire));
                }
            }
        } else {
            for (const auto& warning : Warnings) {
                result.AddIssue(warning);
            }
        }
        co_return result;
    }

    async<TGenericResult> RunAlter() {
        auto& op = *ModifyScheme().MutableAlterSecret();
        const TString path = SecretPath(op.GetName());
        const auto existing = co_await NavigateSecretForAlter(path);
        if (!existing.Exists && Op.SuccessOnNotExist) {
            co_return MakeSuccess();
        }
        if (!existing.Exists) {
            throw TOrchestrationError(Ydb::StatusIds::SCHEME_ERROR) << "Secret does not exist: " << path;
        }
        co_return co_await Replace(path, existing, op);
    }

    TIamDelegationSecretOperation Op;
    NKikimrTxUserProxy::TEvProposeTransaction RequestTemplate;
    NYql::TIssues Warnings;
    ui64 TabletId = 0;
};

class TCreator : public TDelegationSecretActorBase<TCreator> {
public:
    using TDelegationSecretActorBase::TDelegationSecretActorBase;
    async<TGenericResult> Run() { co_return co_await RunCreate(); }
};

class TAlterer : public TDelegationSecretActorBase<TAlterer> {
public:
    using TDelegationSecretActorBase::TDelegationSecretActorBase;
    async<TGenericResult> Run() { co_return co_await RunAlter(); }
};

} // namespace

TString NewReferrerId() {
    const TGUID guid = TGUID::Create();
    return TStringBuilder() << "ydb.delegation." << Sprintf("%08x%08x%08x%08x", guid.dw[0], guid.dw[1], guid.dw[2], guid.dw[3]);
}

TMaybe<TString> ExtractCloudSubjectId(const NACLib::TUserToken& userToken, TStringBuf accessServiceDomain) {
    if (userToken.IsSystemUser() || accessServiceDomain.empty()) {
        return Nothing();
    }
    const TString suffix = TStringBuilder() << '@' << accessServiceDomain;
    const TString sid = userToken.GetUserSID();
    if (sid.size() <= suffix.size() || !sid.EndsWith(suffix)) {
        return Nothing();
    }
    const TString subject = sid.substr(0, sid.size() - suffix.size());
    if (subject == "anonymous") {
        return Nothing();
    }
    return subject;
}

IActor* CreateIamDelegationSecretCreator(TIamDelegationSecretOperation op) {
    return new TCreator(std::move(op));
}
IActor* CreateIamDelegationSecretAlterer(TIamDelegationSecretOperation op) {
    return new TAlterer(std::move(op));
}

} // namespace NKikimr::NKqp
