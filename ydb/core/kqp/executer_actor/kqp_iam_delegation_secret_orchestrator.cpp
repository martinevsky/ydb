#include "kqp_iam_delegation_secret_orchestrator.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/path.h>
#include <ydb/core/kqp/gateway/actors/scheme.h>
#include <ydb/core/protos/auth.pb.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/security/iam_delegation/cloud_resolver.h>
#include <ydb/core/security/iam_delegation/events.h>
#include <ydb/core/security/iam_delegation/iam_actor_base.h>
#include <ydb/core/security/iam_delegation/services.h>
#include <ydb/core/security/iam_delegation/settings.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/async/timeout.h>
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

// What the scheme cache knows about the secret path.
struct TExistingSecret {
    bool Exists = false;
    bool IsSecret = false;
    NKikimrSchemeOp::TSecretDescription Description;

    bool IsDelegation() const {
        return Exists && IsSecret && Description.GetType() == NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION;
    }
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

TDelegationSpec ToSpec(const NKikimrSchemeOp::TIamDelegation& proto) {
    TDelegationSpec spec;
    spec.ServiceAccountId = proto.GetServiceAccountId();
    spec.CloudId = proto.GetCloudId();
    spec.ReferrerId = proto.GetReferrerId();
    return spec;
}

void FillProto(const TDelegationSpec& spec, NKikimrSchemeOp::TIamDelegation& proto) {
    proto.SetServiceAccountId(spec.ServiceAccountId);
    proto.SetCloudId(spec.CloudId);
    proto.SetReferrerId(spec.ReferrerId);
}

// A reply event of the delegation service carrying a TDelegationResult.
template <class TEv>
concept CDelegationResultEvent = requires (TEv ev) {
    { TEv::EventType } -> std::convertible_to<ui32>;
    { ev.Result } -> std::same_as<NIamDelegation::TDelegationResult&>;
};

// Building blocks shared by the two operations. Every method is a nested coroutine and keeps its data in
// its frame; the actor itself owns only the pending scheme request and the promise.
template <class TDerived>
class TDelegationSecretActorBase : public TActorBootstrapped<TDerived> {
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
        IgnoreFunc(TEvIamDelegation::TEvSetupDelegationResult);
        IgnoreFunc(NCloud::TEvServiceAccountService::TEvGetServiceAccountResponse); // the clients of ResolveCloud
        IgnoreFunc(NCloud::TEvFolderService::TEvResolveFoldersResponse);
        IgnoreFunc(TEvents::TEvUndelivered);
        IgnoreFunc(TEvPrivate::TEvSchemeOpDone);
        cFunc(TEvents::TEvPoison::EventType, BeginShutdown);
    )

    STFUNC(StateDying) {
        Y_UNUSED(ev); // PassAway unregisters the actor only when its coroutine tasks have unwound; events arriving meanwhile are dropped
    }

protected:
    // The executer waits for the promise: a cancelled operation must complete it rather than leave it hanging.
    // Whatever IAM has accepted by now is named by the secret (as its delegation or a staged one) and is revoked
    // by the schemeshard when the secret stops naming it.
    void BeginShutdown() {
        if (!Op.Promise.HasValue()) {
            YDB_LOG_WARN("Cancelled while in flight", {"database", Op.Database});
            Op.Promise.SetValue(MakeError(Ydb::StatusIds::CANCELLED, "IAM delegation secret operation was cancelled"));
        }
        this->Become(&TDelegationSecretActorBase::StateDying);
        this->PassAway();
    }

    NKikimrSchemeOp::TModifyScheme& ModifyScheme() {
        Y_ENSURE(Op.Request, "the scheme operation was already executed");
        return *Op.Request->Record.MutableTransaction()->MutableModifyScheme();
    }

    TString SecretPath(const TString& name) {
        return CanonizePath(JoinPath({RequestTemplate.GetTransaction().GetModifyScheme().GetWorkingDir(), name}));
    }

    // The statement's own scheme request, executed once.
    THolder<TEvTxUserProxy::TEvProposeTransaction> TakeRequest() {
        Y_ENSURE(Op.Request, "the scheme operation was already executed");
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

    // ALTER of the delegation only: the promotion or the cancellation of a staged delegation.
    THolder<TEvTxUserProxy::TEvProposeTransaction> AlterDelegationRequest(const TString& name, const TDelegationSpec& spec, NKikimrSchemeOp::EIamDelegationAlter action) {
        auto request = MakeRequest(NKikimrSchemeOp::ESchemeOpAlterSecret);
        auto& alter = *request->Record.MutableTransaction()->MutableModifyScheme()->MutableAlterSecret();
        alter.SetName(name);
        alter.SetType(NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION);
        FillProto(spec, *alter.MutableIamDelegation());
        alter.SetIamDelegationAlter(action);
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

    // A new delegation for the service account in the cloud, with a fresh referrer.
    TDelegationSpec NewSpec(const TString& serviceAccountId, const TString& cloudId) {
        DelegationSettings(); // the control plane must be configured before anything is set up
        TDelegationSpec spec;
        spec.ServiceAccountId = serviceAccountId;
        spec.CloudId = cloudId;
        spec.ReferrerId = NewReferrerId();
        return spec;
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

    async<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr> NavigateRequest(TString path) {
        auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
        request->DatabaseName = Op.Database;
        auto& entry = request->ResultSet.emplace_back();
        entry.Path = SplitPath(path);
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
        entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByPath;
        entry.RedirectRequired = false;
        entry.SyncVersion = true;
        co_return co_await ActorRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(
            MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
    }

    async<NSchemeCache::TSchemeCacheNavigate::TEntry> Navigate(TString path) {
        auto ev = co_await WithTimeout(Op.Timeouts.Navigate, &TDelegationSecretActorBase::NavigateRequest, this, path);
        if (!ev) {
            throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "timeout while resolving " << path;
        }
        auto& result = *(*ev)->Get()->Request;
        Y_ENSURE(result.ResultSet.size() == 1);
        co_return std::move(result.ResultSet.front());
    }

    async<TExistingSecret> NavigateSecret(TString path) {
        using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
        const auto entry = co_await Navigate(path);
        TExistingSecret existing;
        switch (entry.Status) {
            case EStatus::Ok:
                existing.Exists = true;
                existing.IsSecret = entry.Kind == NSchemeCache::TSchemeCacheNavigate::KindSecret && entry.SecretInfo;
                if (existing.IsSecret) {
                    existing.Description = entry.SecretInfo->Description;
                }
                co_return existing;
            case EStatus::RootUnknown:
            case EStatus::PathErrorUnknown:
                co_return existing;
            case EStatus::AccessDenied:
                throw TOrchestrationError(Ydb::StatusIds::UNAUTHORIZED) << "access denied to " << path;
            default:
                throw TOrchestrationError(Ydb::StatusIds::UNAVAILABLE) << "cannot resolve " << path << ": " << entry.Status;
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
                auto resolved = co_await WithTimeout(Op.Timeouts.Delegation, &ResolveCloud, settings, userToken, serviceAccountId);
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

    // Waits for a reply of any type: the typed result, or TEvUndelivered when the service is not running.
    async<IEventHandle::TPtr> DelegationRequest(IEventBase* request) {
        co_return co_await ActorRequest<IEventHandle>(MakeIamDelegationServiceId(), request, IEventHandle::FlagTrackDelivery);
    }

    // Sends a request to the delegation service and returns its result (or an error on undelivery / timeout).
    template <CDelegationResultEvent TResult>
    async<TDelegationResult> CallDelegationService(IEventBase* request, TStringBuf method) {
        auto ev = co_await WithTimeout(Op.Timeouts.Delegation, &TDelegationSecretActorBase::DelegationRequest, this, request);
        if (!ev) {
            co_return TDelegationResult::Error(Ydb::StatusIds::TIMEOUT, TStringBuilder() << method << ": timeout");
        }
        if ((*ev)->GetTypeRewrite() == TEvents::TEvUndelivered::EventType) {
            co_return TDelegationResult::Error(Ydb::StatusIds::UNAVAILABLE, "IAM delegation service is not running on this node");
        }
        if ((*ev)->GetTypeRewrite() != TResult::EventType) {
            co_return TDelegationResult::Error(Ydb::StatusIds::INTERNAL_ERROR, TStringBuilder() << method << ": unexpected reply " << (*ev)->GetTypeName());
        }
        co_return (*ev)->template Get<TResult>()->Result;
    }

    // Sets the delegation up on behalf of the subject. The result is returned, not thrown: the caller has to
    // undo the schema change that named the delegation first, and a coroutine cannot await in a handler.
    async<TDelegationResult> Setup(TDelegationSpec spec, TString subjectId) {
        YDB_LOG_INFO("SetupDelegation", {"spec", spec.ToString()}, {"subjectId", subjectId});
        co_return co_await CallDelegationService<TEvIamDelegation::TEvSetupDelegationResult>(
            new TEvIamDelegation::TEvSetupDelegation(spec, subjectId), "SetupDelegation");
    }

    TString SetupError(const TDelegationSpec& spec, const TDelegationResult& setup) {
        return TStringBuilder() << "SetupDelegation for service account " << spec.ServiceAccountId << " failed: " << setup.Issues.ToOneLineString();
    }

    // Executes a scheme request through the regular scheme request handler. The flags of the statement
    // (IF EXISTS, IF NOT EXISTS) apply to its own request only.
    async<TGenericResult> RunSchemeOp(THolder<TEvTxUserProxy::TEvProposeTransaction> request, bool failedOnAlreadyExists = false, bool successOnNotExist = false) {
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
        this->RegisterWithSameMailbox(new TSchemeOpRequestHandler(request.Release(), promise, failedOnAlreadyExists, successOnNotExist));
        auto ev = co_await ActorWaitForEvent<TEvPrivate::TEvSchemeOpDone>(cookie);
        co_return ev->Get()->Result;
    }

    async<TGenericResult> RunStatementSchemeOp() {
        co_return co_await RunSchemeOp(TakeRequest(), Op.FailedOnAlreadyExists, Op.SuccessOnNotExist);
    }

    // Replaces the delegation of an existing secret (ALTER, CREATE OR REPLACE). The statement's own request
    // stages the new delegation (and checks the user's rights); once IAM has set it up, a second request
    // promotes it and the schemeshard revokes the old one. When IAM refuses, the second request cancels
    // the staged delegation instead. secretOp is the CreateSecret or AlterSecret of the statement.
    async<TGenericResult> Replace(TString path, TDelegationSpec oldSpec, NKikimrSchemeOp::TSecretSchemaOp& secretOp) {
        // secretOp lives in the statement's request, which is gone once that request has run
        const TString name = secretOp.GetName();
        auto& requested = *secretOp.MutableIamDelegation();
        TString serviceAccountId = requested.GetServiceAccountId() ? requested.GetServiceAccountId() : oldSpec.ServiceAccountId;
        TString cloudId = requested.GetCloudId() ? requested.GetCloudId() : oldSpec.CloudId;
        if (serviceAccountId == oldSpec.ServiceAccountId && cloudId == oldSpec.CloudId) {
            // nothing changes in IAM: keep the existing delegation
            FillProto(oldSpec, requested);
            secretOp.SetIamDelegationAlter(NKikimrSchemeOp::IAM_DELEGATION_ALTER_NONE);
            co_return co_await RunStatementSchemeOp();
        }

        const TString subjectId = ResolveSubjectId();
        const TDelegationSpec newSpec = NewSpec(serviceAccountId, cloudId);
        YDB_LOG_INFO("Replacing delegation", {"path", path}, {"old", oldSpec.ToString()}, {"new", newSpec.ToString()});
        FillProto(newSpec, requested);
        secretOp.SetIamDelegationAlter(NKikimrSchemeOp::IAM_DELEGATION_ALTER_STAGE);
        const TGenericResult staged = co_await RunStatementSchemeOp();
        if (!staged.Success()) {
            co_return staged; // nothing was sent to IAM
        }

        const TDelegationResult setup = co_await Setup(newSpec, subjectId);
        const auto action = setup.IsSuccess() ? NKikimrSchemeOp::IAM_DELEGATION_ALTER_PROMOTE : NKikimrSchemeOp::IAM_DELEGATION_ALTER_CANCEL;
        TGenericResult result = co_await RunSchemeOp(AlterDelegationRequest(name, newSpec, action));
        if (!setup.IsSuccess()) {
            TStringBuilder message;
            message << SetupError(newSpec, setup);
            if (!result.Success()) {
                message << "; the delegation " << newSpec.ReferrerId << " stays staged for the secret until its next ALTER or DROP: "
                    << result.Issues().ToOneLineString();
            }
            throw TOrchestrationError(setup.Status) << message;
        }
        if (!result.Success()) {
            // IAM knows the new delegation and the secret names it as staged: the next ALTER or DROP of the
            // secret revokes it, the readers keep the old one meanwhile
            result.AddIssue(NYql::TIssue(TStringBuilder() << "IAM delegation " << newSpec.ReferrerId << " for service account "
                << newSpec.ServiceAccountId << " was set up but the secret still uses the previous one; retry ALTER SECRET"));
        }
        co_return result;
    }

    TIamDelegationSecretOperation Op;
    NKikimrTxUserProxy::TEvProposeTransaction RequestTemplate;
    NYql::TIssues Warnings; // reported with the result of a successful operation
};

class TCreator : public TDelegationSecretActorBase<TCreator> {
public:
    using TDelegationSecretActorBase::TDelegationSecretActorBase;

    async<TGenericResult> Run() {
        auto& op = *ModifyScheme().MutableCreateSecret();
        Y_ENSURE(op.GetType() == NKikimrSchemeOp::SECRET_TYPE_IAM_DELEGATION);
        const TString name = op.GetName();
        const TString path = SecretPath(name);

        const TExistingSecret existing = co_await NavigateSecret(path);
        if (existing.Exists) {
            if (ModifyScheme().GetReplaceIfExists() && existing.IsSecret) {
                if (!existing.IsDelegation()) {
                    throw TOrchestrationError(Ydb::StatusIds::BAD_REQUEST)
                        << "Cannot replace secret " << path << " of type VALUE with a secret of type IAM_DELEGATION";
                }
                co_return co_await Replace(path, ToSpec(existing.Description.GetIamDelegation()), op);
            }
            if (!ModifyScheme().GetFailOnExist() && !ModifyScheme().GetReplaceIfExists() && existing.IsSecret) {
                co_return MakeSuccess(); // IF NOT EXISTS
            }
            co_return co_await RunStatementSchemeOp(); // the schemeshard reports the conflict (or the non-secret path)
        }

        const TString subjectId = ResolveSubjectId();
        TString cloudId = op.GetIamDelegation().GetCloudId();
        if (cloudId.empty()) {
            cloudId = co_await ResolveCloudId(op.GetIamDelegation().GetServiceAccountId());
        }
        const TDelegationSpec spec = NewSpec(op.GetIamDelegation().GetServiceAccountId(), cloudId);
        FillProto(spec, *op.MutableIamDelegation());

        // the secret names the delegation before IAM knows it; the schemeshard checks the user's rights here
        TGenericResult result = co_await RunStatementSchemeOp();
        if (!result.Success()) {
            co_return result;
        }
        const TDelegationResult setup = co_await Setup(spec, subjectId);
        if (!setup.IsSuccess()) {
            // the secret names a delegation IAM refused: drop it (the revocation the schemeshard makes is a no-op)
            YDB_LOG_WARN("SetupDelegation failed after CREATE SECRET, dropping the secret", {"path", path}, {"spec", spec.ToString()});
            const TGenericResult drop = co_await RunSchemeOp(DropRequest(name));
            TStringBuilder message;
            message << SetupError(spec, setup);
            if (!drop.Success()) {
                message << "; secret " << path << " was created and could not be dropped: " << drop.Issues().ToOneLineString() << "; drop it";
            }
            throw TOrchestrationError(setup.Status) << message;
        }
        for (const auto& warning : Warnings) {
            result.AddIssue(warning);
        }
        co_return result;
    }
};

class TAlterer : public TDelegationSecretActorBase<TAlterer> {
public:
    using TDelegationSecretActorBase::TDelegationSecretActorBase;

    async<TGenericResult> Run() {
        auto& op = *ModifyScheme().MutableAlterSecret();
        const TString path = SecretPath(op.GetName());

        const TExistingSecret existing = co_await NavigateSecret(path);
        if (!existing.Exists && Op.SuccessOnNotExist) {
            co_return MakeSuccess(); // IF EXISTS
        }
        if (!existing.IsDelegation()) {
            co_return co_await RunStatementSchemeOp(); // the schemeshard reports a missing path or a type change
        }
        co_return co_await Replace(path, ToSpec(existing.Description.GetIamDelegation()), op);
    }
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
        // the ticket parser maps the anonymous account of the access service to "anonymous@<domain>"
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
