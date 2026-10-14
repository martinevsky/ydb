#include "iam_delegation_reader.h"

#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/settings.h>
#include <ydb/core/security/iam_delegation/lifecycle/client.h>
#include <ydb/core/security/iam_delegation/tablet/public/events.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

namespace NKikimr::NSecret {
namespace {
namespace P = NKikimrIamDelegation;
using TEvTablet = NIamDelegation::TEvIamDelegationTablet;

class TReader final : public NActors::TActorBootstrapped<TReader> {
    enum class EPhase { Secret, Delegation, Token, Recheck, Schema };
public:
    TReader(const NActors::TActorId& replyTo, ui64 cookie, TString path, TString database,
        TIntrusiveConstPtr<NACLib::TUserToken> userToken,
        NKqp::TEvDescribeSecretsResponse::TSecretReference reference)
        : ReplyTo(replyTo), Cookie(cookie), Path(std::move(path)), Database(std::move(database))
        , UserToken(std::move(userToken)), Reference(std::move(reference)) {}

    void Bootstrap() {
        if (!Reference.TabletId || Reference.DatabaseIncarnation.empty() || Reference.CreateOperationId.empty()
            || !Reference.PathOwnerId || !Reference.PathLocalId) {
            Finish(Ydb::StatusIds::NOT_FOUND, "Incomplete secret incarnation reference");
            return;
        }
        Become(&TReader::StateWork);
        SetPhase(EPhase::Secret);
        ReadSecret();
    }
private:
    STRICT_STFUNC(StateWork,
        hFunc(TEvTablet::TEvResponse, HandleTablet);
        hFunc(NIamDelegation::TEvIamDelegation::TEvGetTokenResult, HandleToken);
        hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, HandleSchema);
        hFunc(NActors::TEvents::TEvWakeup, HandleTimeout);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
    )

    void SetPhase(EPhase phase) {
        Phase = phase;
        // The token producer may already be backing off. Allow its complete next
        // attempt and bounded delivery slack, without extending storage/schema waits.
        const auto timeout = Phase == EPhase::Token
            ? NIamDelegation::TIamDelegationSettings::MintAttemptTimeout() + TDuration::Seconds(1)
            : TDuration::Seconds(30);
        Schedule(timeout, new NActors::TEvents::TEvWakeup(static_cast<ui64>(Phase)));
    }

    void Request(THolder<TEvTablet::TEvRequest> request) {
        Send(Register(NIamDelegation::CreateIamDelegationTabletRequestActor(Reference.TabletId)),
            request.Release(), 0, ++Sequence);
    }

    void ReadSecret() {
        auto request = MakeHolder<TEvTablet::TEvRequest>();
        auto* identity = request->Record.MutableGetSecret()->MutableSecret();
        identity->SetDatabaseIncarnation(Reference.DatabaseIncarnation);
        identity->SetPathOwnerId(Reference.PathOwnerId);
        identity->SetPathLocalId(Reference.PathLocalId);
        Request(std::move(request));
    }

    bool SameIdentity(const P::TSecretIdentity& identity) const {
        return identity.GetDatabaseIncarnation() == Reference.DatabaseIncarnation
            && identity.GetPathOwnerId() == Reference.PathOwnerId
            && identity.GetPathLocalId() == Reference.PathLocalId;
    }

    bool CheckSecret(const P::TSecretRecord& secret) {
        if (!SameIdentity(secret.GetIdentity()) || secret.GetCreateOperationId() != Reference.CreateOperationId
            || secret.GetState() != P::SECRET_LIVE) {
            Finish(Ydb::StatusIds::NOT_FOUND, "Secret incarnation is no longer live");
            return false;
        }
        if (secret.GetCurrentOperationId().empty()) {
            Finish(Ydb::StatusIds::UNAVAILABLE, "Secret delegation is not active");
            return false;
        }
        if (Phase != EPhase::Secret && (secret.GetRevision() != Secret.GetRevision()
            || secret.GetCurrentOperationId() != Secret.GetCurrentOperationId())) {
            Finish(Ydb::StatusIds::UNAVAILABLE, "Secret delegation changed during token resolution");
            return false;
        }
        return true;
    }

    void HandleTablet(TEvTablet::TEvResponse::TPtr& ev) {
        if (ev->Cookie != Sequence) {
            return;
        }
        const auto& response = ev->Get()->Record;
        if (response.GetStatus() != P::SUCCESS) {
            Finish(response.GetStatus() == P::NOT_FOUND ? Ydb::StatusIds::NOT_FOUND : Ydb::StatusIds::UNAVAILABLE,
                "Cannot read secret delegation state");
            return;
        }
        if (!response.HasSecret() || !CheckSecret(response.GetSecret())) {
            if (!response.HasSecret()) Finish(Ydb::StatusIds::UNAVAILABLE, "Missing secret delegation state");
            return;
        }
        if (Phase == EPhase::Secret) {
            Secret = response.GetSecret();
            SetPhase(EPhase::Delegation);
            auto request = MakeHolder<TEvTablet::TEvRequest>();
            request->Record.MutableGetDelegation()->SetOperationId(Secret.GetCurrentOperationId());
            Request(std::move(request));
        } else if (Phase == EPhase::Delegation) {
            const auto& delegation = response.GetDelegation();
            if (!response.HasDelegation() || delegation.GetOperationId() != Secret.GetCurrentOperationId()
                || !SameIdentity(delegation.GetSecret()) || delegation.GetState() != P::ACTIVE
                || delegation.GetSetupState() != P::SETUP_SUCCEEDED) {
                Finish(Ydb::StatusIds::UNAVAILABLE, "Secret delegation is no longer active");
                return;
            }
            const auto& binding = delegation.GetBinding();
            TokenKey = {binding.GetServiceAccountId(), binding.GetCloudId(), binding.GetServiceId(),
                binding.GetMicroserviceId(), binding.GetResourceType()};
            SetPhase(EPhase::Token);
            Send(NIamDelegation::MakeIamDelegatedTokenServiceId(),
                new NIamDelegation::TEvIamDelegation::TEvGetToken(TokenKey), 0, ++Sequence);
        } else if (Phase == EPhase::Recheck) {
            SetPhase(EPhase::Schema);
            auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
            auto& entry = request->ResultSet.emplace_back();
            entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
            entry.Path = SplitPath(Path);
            entry.SyncVersion = true;
            entry.Access = NACLib::SelectRow;
            request->DatabaseName = Database;
            request->UserToken = UserToken;
            Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()), 0, ++Sequence);
        }
    }

    void HandleToken(NIamDelegation::TEvIamDelegation::TEvGetTokenResult::TPtr& ev) {
        if (Phase != EPhase::Token || ev->Cookie != Sequence) return;
        const auto& result = *ev->Get();
        if (!(result.Key == TokenKey) || !result.IsSuccess() || result.Token.empty()
            || NActors::TActivationContext::Now() >= result.UsableUntil) {
            Finish(result.IsSuccess() ? Ydb::StatusIds::UNAVAILABLE : result.Status,
                "Cannot obtain usable delegated token", result.Issues);
            return;
        }
        Token = result.Token;
        UsableUntil = result.UsableUntil;
        SetPhase(EPhase::Recheck);
        ReadSecret();
    }

    void HandleSchema(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        if (Phase != EPhase::Schema || ev->Cookie != Sequence) return;
        const auto& response = *ev->Get()->Request;
        if (response.ResultSet.size() != 1) {
            Finish(Ydb::StatusIds::UNAVAILABLE, "Missing secret schema response");
            return;
        }
        const auto& entry = response.ResultSet.front();
        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            Finish(entry.Status == NSchemeCache::TSchemeCacheNavigate::EStatus::AccessDenied ? Ydb::StatusIds::UNAUTHORIZED
                : entry.Status == NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError ? Ydb::StatusIds::UNAVAILABLE
                : Ydb::StatusIds::NOT_FOUND, "Secret is unavailable or access was denied");
            return;
        }
        const auto* description = entry.SecretInfo ? &entry.SecretInfo->Description : nullptr;
        if (entry.Kind != NSchemeCache::TSchemeCacheNavigate::KindSecret || !entry.Self || !description
            || !description->HasIamDelegation() || entry.Self->Info.GetSchemeshardId() != Reference.PathOwnerId
            || entry.Self->Info.GetPathId() != Reference.PathLocalId
            || description->GetIamDelegation().GetTabletId() != Reference.TabletId
            || description->GetIamDelegation().GetDatabaseIncarnation() != Reference.DatabaseIncarnation
            || description->GetIamDelegation().GetCreateOperationId() != Reference.CreateOperationId) {
            Finish(Ydb::StatusIds::NOT_FOUND, "Secret schema incarnation changed");
            return;
        }
        if (NActors::TActivationContext::Now() >= UsableUntil) {
            Finish(Ydb::StatusIds::UNAVAILABLE, "Delegated token expired during authorization");
            return;
        }
        Finish(Ydb::StatusIds::SUCCESS, {});
    }

    void HandleTimeout(NActors::TEvents::TEvWakeup::TPtr& ev) {
        if (ev->Get()->Tag == static_cast<ui64>(Phase)) {
            Finish(Ydb::StatusIds::UNAVAILABLE, "Secret delegation read timed out");
        }
    }
    void Finish(Ydb::StatusIds::StatusCode status, const TString& issue, const NYql::TIssues& details = {}) {
        auto result = MakeHolder<TEvIamDelegationSecretReadResult>();
        result->SecretPath = Path;
        result->Reference = Reference;
        result->Status = status;
        if (status == Ydb::StatusIds::SUCCESS) {
            result->Token = std::move(Token);
            result->UsableUntil = UsableUntil;
        } else {
            result->Issues.AddIssue(issue);
            result->Issues.AddIssues(details);
        }
        Send(ReplyTo, result.Release(), 0, Cookie);
        PassAway();
    }

    const NActors::TActorId ReplyTo;
    const ui64 Cookie;
    const TString Path;
    const TString Database;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const NKqp::TEvDescribeSecretsResponse::TSecretReference Reference;
    ui64 Sequence = 0;
    EPhase Phase = EPhase::Secret;
    P::TSecretRecord Secret;
    NIamDelegation::TTokenKey TokenKey;
    TString Token;
    TInstant UsableUntil;
};
} // namespace

NActors::IActor* CreateIamDelegationSecretReaderActor(const NActors::TActorId& replyTo, ui64 cookie,
    TString secretPath, TString database, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
    NKqp::TEvDescribeSecretsResponse::TSecretReference reference) {
    return new TReader(replyTo, cookie, std::move(secretPath), std::move(database),
        std::move(userToken), std::move(reference));
}
} // namespace NKikimr::NSecret
