#include "iam_delegation_reader.h"

#include <ydb/core/base/iam_delegation.h>
#include <ydb/core/security/iam_delegation/settings.h>
#include <ydb/core/security/iam_delegation/lifecycle/operations.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

namespace NKikimr::NSecret {
namespace {
namespace P = NKikimrIamDelegation;
using namespace NActors;
using TReference = NKqp::TEvDescribeSecretsResponse::TSecretReference;
using TTokenEvents = NIamDelegation::TEvIamDelegation;
using TTabletEvents = NIamDelegation::TEvIamDelegationTablet;
constexpr auto StorageTimeout = TDuration::Seconds(30);

class TReader final : public TActorBootstrapped<TReader> {
public:
    TReader(const TActorId& replyTo, ui64 cookie, TString path, TString database,
        TIntrusiveConstPtr<NACLib::TUserToken> userToken, TReference reference)
        : ReplyTo(replyTo), Cookie(cookie), Path(std::move(path)), Database(std::move(database))
        , UserToken(std::move(userToken)), Reference(std::move(reference)) {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        auto result = co_await Read();
        Send(ReplyTo, result.Release(), 0, Cookie);
        PassAway();
    }

private:
    STRICT_STFUNC(StateWork,
        IgnoreFunc(TTabletEvents::TEvResponse);
        IgnoreFunc(TTokenEvents::TEvGetTokenResult);
        IgnoreFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult);
        IgnoreFunc(TEvents::TEvUndelivered);
        IgnoreFunc(TEvents::TEvWakeup);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )

    P::TRequest SecretRequest() const {
        P::TRequest request;
        auto* identity = request.MutableGetSecret()->MutableSecret();
        identity->SetDatabaseIncarnation(Reference.DatabaseIncarnation);
        identity->SetPathOwnerId(Reference.PathOwnerId);
        identity->SetPathLocalId(Reference.PathLocalId);
        return request;
    }

    bool SameIdentity(const P::TSecretIdentity& identity) const {
        return identity.GetDatabaseIncarnation() == Reference.DatabaseIncarnation
            && identity.GetPathOwnerId() == Reference.PathOwnerId
            && identity.GetPathLocalId() == Reference.PathLocalId;
    }

    THolder<TEvIamDelegationSecretReadResult> Failure(Ydb::StatusIds::StatusCode status,
        TString issue, const NYql::TIssues& details = {}) const
    {
        auto result = MakeHolder<TEvIamDelegationSecretReadResult>();
        result->SecretPath = Path;
        result->Reference = Reference;
        result->Status = status;
        result->Issues.AddIssue(std::move(issue));
        result->Issues.AddIssues(details);
        return result;
    }

    THolder<TEvIamDelegationSecretReadResult> CheckSecret(
        const std::optional<P::TResponse>& response, const P::TSecretRecord* initial = nullptr) const
    {
        if (!response || response->GetStatus() != P::SUCCESS) {
            return Failure(response && response->GetStatus() == P::NOT_FOUND
                ? Ydb::StatusIds::NOT_FOUND : Ydb::StatusIds::UNAVAILABLE, "Cannot read secret delegation state");
        }
        if (!response->HasSecret()) {
            return Failure(Ydb::StatusIds::UNAVAILABLE, "Missing secret delegation state");
        }
        const auto& secret = response->GetSecret();
        if (!SameIdentity(secret.GetIdentity()) || secret.GetCreateOperationId() != Reference.CreateOperationId
            || secret.GetState() != P::SECRET_LIVE) {
            return Failure(Ydb::StatusIds::NOT_FOUND, "Secret incarnation is no longer live");
        }
        if (secret.GetCurrentOperationId().empty()) {
            return Failure(Ydb::StatusIds::UNAVAILABLE, "Secret delegation is not active");
        }
        if (initial && (secret.GetRevision() != initial->GetRevision()
            || secret.GetCurrentOperationId() != initial->GetCurrentOperationId())) {
            return Failure(Ydb::StatusIds::UNAVAILABLE, "Secret delegation changed during token resolution");
        }
        return {};
    }

    async<THolder<TEvIamDelegationSecretReadResult>> Read() {
        if (!Reference.TabletId || Reference.DatabaseIncarnation.empty() || Reference.CreateOperationId.empty()
            || !Reference.PathOwnerId || !Reference.PathLocalId) {
            co_return Failure(Ydb::StatusIds::NOT_FOUND, "Incomplete secret incarnation reference");
        }
        const auto first = co_await WithTimeout(StorageTimeout,
            &NIamDelegation::CallIamDelegationTablet, Reference.TabletId, SecretRequest());
        if (auto failure = CheckSecret(first)) co_return failure;
        const auto secret = first->GetSecret();
        P::TRequest delegationRequest;
        delegationRequest.MutableGetDelegation()->SetOperationId(secret.GetCurrentOperationId());
        const auto saved = co_await WithTimeout(StorageTimeout,
            &NIamDelegation::CallIamDelegationTablet, Reference.TabletId, std::move(delegationRequest));
        if (auto failure = CheckSecret(saved, &secret)) co_return failure;
        const auto& delegation = saved->GetDelegation();
        if (!saved->HasDelegation() || delegation.GetOperationId() != secret.GetCurrentOperationId()
            || !SameIdentity(delegation.GetSecret()) || delegation.GetState() != P::ACTIVE
            || delegation.GetSetupState() != P::SETUP_SUCCEEDED) {
            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Secret delegation is no longer active");
        }
        const auto& binding = delegation.GetBinding();
        const NIamDelegation::TTokenKey key{binding.GetServiceAccountId(), binding.GetCloudId(), binding.GetServiceId(),
            binding.GetMicroserviceId(), binding.GetResourceType()};
        const auto tokenWait = [&]() -> async<IEventHandle::TPtr> {
            co_return co_await ActorRequest<IEventHandle>(NIamDelegation::MakeIamDelegatedTokenServiceId(),
                new TTokenEvents::TEvGetToken(key), IEventHandle::FlagTrackDelivery);
        };
        // Cancelling this wait never cancels the account's shared producer.
        auto tokenReply = co_await WithTimeout(
            NIamDelegation::TIamDelegationSettings::MintAttemptTimeout() + TDuration::Seconds(1), tokenWait);
        if (!tokenReply || (*tokenReply)->GetTypeRewrite() != TTokenEvents::TEvGetTokenResult::EventType) {
            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Secret delegation read timed out or producer is unavailable");
        }
        const auto& token = *(*tokenReply)->Get<TTokenEvents::TEvGetTokenResult>();
        if (!(token.Key == key) || !token.IsSuccess() || token.Token.empty()
            || TActivationContext::Now() >= token.UsableUntil) {
            co_return Failure(token.IsSuccess() ? Ydb::StatusIds::UNAVAILABLE : token.Status,
                "Cannot obtain usable delegated token", token.Issues);
        }
        const auto rechecked = co_await WithTimeout(StorageTimeout,
            &NIamDelegation::CallIamDelegationTablet, Reference.TabletId, SecretRequest());
        if (auto failure = CheckSecret(rechecked, &secret)) co_return failure;
        const auto schemaWait = [&]() -> async<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr> {
            auto request = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
            auto& entry = request->ResultSet.emplace_back();
            entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
            entry.Path = SplitPath(Path);
            entry.SyncVersion = true;
            entry.Access = NACLib::SelectRow;
            request->DatabaseName = Database;
            request->UserToken = UserToken;
            co_return co_await ActorRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(MakeSchemeCacheID(),
                new TEvTxProxySchemeCache::TEvNavigateKeySet(request.Release()));
        };
        const auto schema = co_await WithTimeout(StorageTimeout, schemaWait);
        if (!schema || !(*schema)->Get()->Request || (*schema)->Get()->Request->ResultSet.size() != 1) {
            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Missing secret schema response");
        }
        const auto& entry = (*schema)->Get()->Request->ResultSet.front();
        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            co_return Failure(entry.Status == NSchemeCache::TSchemeCacheNavigate::EStatus::AccessDenied
                ? Ydb::StatusIds::UNAUTHORIZED
                : entry.Status == NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError
                ? Ydb::StatusIds::UNAVAILABLE : Ydb::StatusIds::NOT_FOUND,
                "Secret is unavailable or access was denied");
        }
        const auto* description = entry.SecretInfo ? &entry.SecretInfo->Description : nullptr;
        if (entry.Kind != NSchemeCache::TSchemeCacheNavigate::KindSecret || !entry.Self || !description
            || !description->HasIamDelegation() || entry.Self->Info.GetSchemeshardId() != Reference.PathOwnerId
            || entry.Self->Info.GetPathId() != Reference.PathLocalId
            || description->GetIamDelegation().GetTabletId() != Reference.TabletId
            || description->GetIamDelegation().GetDatabaseIncarnation() != Reference.DatabaseIncarnation
            || description->GetIamDelegation().GetCreateOperationId() != Reference.CreateOperationId) {
            co_return Failure(Ydb::StatusIds::NOT_FOUND, "Secret schema incarnation changed");
        }
        if (TActivationContext::Now() >= token.UsableUntil) {
            co_return Failure(Ydb::StatusIds::UNAVAILABLE, "Delegated token expired during authorization");
        }
        auto result = MakeHolder<TEvIamDelegationSecretReadResult>();
        result->SecretPath = Path;
        result->Reference = Reference;
        result->Status = Ydb::StatusIds::SUCCESS;
        result->Token = token.Token;
        result->UsableUntil = token.UsableUntil;
        co_return result;
    }

    const TActorId ReplyTo;
    const ui64 Cookie;
    const TString Path;
    const TString Database;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    const TReference Reference;
};
} // namespace

TDuration IamDelegationSecretReadTimeout() {
    return StorageTimeout * 4
        + NIamDelegation::TIamDelegationSettings::MintAttemptTimeout() + TDuration::Seconds(1);
}

IActor* CreateIamDelegationSecretReaderActor(const TActorId& replyTo, ui64 cookie,
    TString secretPath, TString database, TIntrusiveConstPtr<NACLib::TUserToken> userToken, TReference reference)
{
    return new TReader(replyTo, cookie, std::move(secretPath), std::move(database),
        std::move(userToken), std::move(reference));
}
} // namespace NKikimr::NSecret
