#include "service.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/tx/scheme_board/subscriber.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/services/metadata/secret/fetcher.h>
#include <ydb/services/metadata/secret/snapshot.h>
#include <ydb/services/metadata/service.h>
#include <ydb/services/scheme_secret/resolver.h>

#include <ydb/library/actors/async/cancellation.h>
#include <ydb/library/actors/async/sleep.h>
#include <ydb/library/actors/async/task_group.h>
#include <ydb/library/actors/async/timeout.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <util/generic/scope.h>
#include <util/generic/hash_set.h>
#include <functional>
#include <limits>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::SCHEMA_SECRET_CACHE

namespace NKikimr::NSecret {

namespace {

class TDescribeSecretsActor: public NActors::TActorBootstrapped<TDescribeSecretsActor> {
    STRICT_STFUNC(StateFunc,
        hFunc(NMetadata::NProvider::TEvRefreshSubscriberData, Handle);
        cFunc(NActors::TEvents::TEvPoison::EventType, Cancel);
    )

    void Handle(NMetadata::NProvider::TEvRefreshSubscriberData::TPtr& ev) {
        auto snapshot = ev->Get()->GetSnapshotAs<NMetadata::NSecret::TSnapshot>();

        std::vector<TString> secretValues;
        secretValues.reserve(SecretIds.size());
        for (const auto& secretId: SecretIds) {
            auto secretValue = snapshot->GetSecretValue(NMetadata::NSecret::TSecretIdOrValue::BuildAsId(secretId));
            if (secretValue.IsSuccess()) {
                secretValues.push_back(secretValue.DetachResult());
                continue;
            }

            auto secretIds = snapshot->GetSecretIds(UserToken, secretId.GetSecretId());
            if (secretIds.size() > 1) {
                const auto err = TStringBuilder() << "several secrets with name '"
                    << secretId.GetSecretId() << "' were found";
                CompleteAndPassAway(
                    NKqp::TEvDescribeSecretsResponse::TDescription(
                        Ydb::StatusIds::BAD_REQUEST,
                        { NYql::TIssue(err) }
                    )
                );
                return;
            }

            if (!secretIds.empty()) {
                secretValue = snapshot->GetSecretValue(NMetadata::NSecret::TSecretIdOrValue::BuildAsId(secretIds[0]));
                if (secretValue.IsSuccess()) {
                    secretValues.push_back(secretValue.DetachResult());
                    continue;
                }
            }

            if (!AskSent) {
                AskSent = true;
                Send(NMetadata::NProvider::MakeServiceId(
                    SelfId().NodeId()), new NMetadata::NProvider::TEvAskSnapshot(GetSecretsSnapshotParser()
                ));
            } else {
                const auto err = TStringBuilder() << "secret with name '" << secretId.GetSecretId() << "' not found";
                CompleteAndPassAway(
                    NKqp::TEvDescribeSecretsResponse::TDescription(
                        Ydb::StatusIds::BAD_REQUEST,
                        { NYql::TIssue(err) }
                    )
                );
            }
            return;
        }

        CompleteAndPassAway(NKqp::TEvDescribeSecretsResponse::TDescription(secretValues));
    }

    void CompleteAndPassAway(const NKqp::TEvDescribeSecretsResponse::TDescription& response) {
        Promise.SetValue(response);
        Cancel();
    }

    void Cancel() {
        Send(NMetadata::NProvider::MakeServiceId(SelfId().NodeId()),
            new NMetadata::NProvider::TEvUnsubscribeExternal(GetSecretsSnapshotParser()));
        PassAway();
    }

    NMetadata::NFetcher::ISnapshotsFetcher::TPtr GetSecretsSnapshotParser() {
        return std::make_shared<NMetadata::NSecret::TSnapshotsFetcher>();
    }

public:
    TDescribeSecretsActor(const TString& ownerUserId, const std::vector<TString>& secretIds,
        NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription> promise
    )
        : UserToken(NACLib::TUserToken{ownerUserId, TVector<NACLib::TSID>{}})
        , SecretIds(CreateSecretIds(ownerUserId, secretIds))
        , Promise(promise)
    {
    }

    void Bootstrap() {
        if (!NMetadata::NProvider::TServiceOperator::IsEnabled()) {
            Promise.SetValue(
                NKqp::TEvDescribeSecretsResponse::TDescription(
                    Ydb::StatusIds::INTERNAL_ERROR,
                    { NYql::TIssue("metadata service is not active") }
                )
            );
            PassAway();
            return;
        }

        Send(NMetadata::NProvider::MakeServiceId(
            SelfId().NodeId()), new NMetadata::NProvider::TEvSubscribeExternal(GetSecretsSnapshotParser()
        ));
        Become(&TDescribeSecretsActor::StateFunc);
    }

private:
    static std::vector<NMetadata::NSecret::TSecretId> CreateSecretIds(const TString& ownerUserId,
        const std::vector<TString>& secretIds
    ) {
        std::vector<NMetadata::NSecret::TSecretId> result;
        for (const TString& secretId : secretIds) {
            result.emplace_back(ownerUserId, secretId);
        }
        return result;
    }

private:
    std::optional<NACLib::TUserToken> UserToken;
    const std::vector<NMetadata::NSecret::TSecretId> SecretIds;
    NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription> Promise;
    bool AskSent = false;
};

IActor* CreateDescribeSecretsActor(const TString& ownerUserId, const std::vector<TString>& secretIds,
    NThreading::TPromise<NKqp::TEvDescribeSecretsResponse::TDescription> promise
) {
    return new TDescribeSecretsActor(ownerUserId, secretIds, promise);
}

// It's a hack so we can simulate Scheme Cache retryable errors in tests
NSchemeCache::TSchemeCacheNavigate::EStatus GetSchemeCacheEntryStatus(
    const TDescribeSchemaSecretsService::ISchemeCacheStatusGetter* schemeCacheStatusGetter,
    NSchemeCache::TSchemeCacheNavigate::TEntry& entry)
{
    if (schemeCacheStatusGetter) {
        return schemeCacheStatusGetter->GetStatus(entry);
    }
    return entry.Status;
}

NKikimrScheme::EStatus GetSchemeShardPathStatus(
    const TDescribeSchemaSecretsService::ISchemeShardStatusGetter* schemeShardStatusGetter,
    const NKikimrScheme::TEvDescribeSchemeResult& record)
{
    if (schemeShardStatusGetter) {
        return schemeShardStatusGetter->GetStatus(record);
    }
    return record.GetStatus();
}

bool IsRetryableSchemeShardStatus(NKikimrScheme::EStatus status) {
    return status == NKikimrScheme::EStatus::StatusNotAvailable;
}

Ydb::StatusIds::StatusCode MapSchemeShardStatus(NKikimrScheme::EStatus status) {
    switch (status) {
        case NKikimrScheme::EStatus::StatusNotAvailable:
            return Ydb::StatusIds::UNAVAILABLE;
        default:
            return Ydb::StatusIds::BAD_REQUEST;
    }
}

TString MakeSchemeShardErrorMessage(NKikimrScheme::EStatus status, const TString& secretName) {
    switch (status) {
        case NKikimrScheme::EStatus::StatusNotAvailable:
            return TStringBuilder() << "Schemeshard is not available for secret `" << secretName << "`";
        case NKikimrScheme::EStatus::StatusPathDoesNotExist:
            return TStringBuilder() << "Secret `" << secretName << "` not found";
        case NKikimrScheme::EStatus::StatusAccessDenied:
            return TStringBuilder() << "Access denied to secret `" << secretName << "`";
        default:
            return TStringBuilder() << "Failed to resolve secret `" << secretName << "`";
    }
}

TString ListSecrets(const TVector<TString>& paths) {
    if (paths.empty()) {
        return "";
    }
    if (paths.size() == 1) {
        return TStringBuilder() << "secret `" << paths[0] << "`";
    }

    auto sb = TStringBuilder() << "secrets ";
    for (size_t i = 0; i < paths.size(); ++i) {
        sb << "`" << paths[i] << "`";
        if (i + 1 < paths.size()) {
            sb << ", ";
        }
    }
    return sb;
}

}  // anonymous namespace

namespace {
using TDescription = NKqp::TEvDescribeSecretsResponse::TDescription;
using namespace NActors;

struct TResolutionFailure {
    TDescription Description;
};

[[noreturn]] void FailResolution(Ydb::StatusIds::StatusCode status, TString issue) {
    throw TResolutionFailure{TDescription(status, {NYql::TIssue(std::move(issue))})};
}
} // namespace

void TDescribeSchemaSecretsService::HandleIncomingRequest(TEvResolveSecret::TPtr& ev) {
    auto request = std::move(ev);
    auto promise = request->Get()->Promise;
    TDescription result(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Secret resolver stopped before completion")});
    Y_DEFER { promise.TrySetValue(result); };
    auto cancellation = co_await TAsyncCancellationScope::WithCurrentHandler();
    const TRequestKey key{request->Sender, request->Cookie};
    const bool callerOwned = request->Get()->Ownership == EResolveOwnership::Caller;
    if (callerOwned && (!request->Sender || !RunningRequests.emplace(key, &cancellation).second)) {
        result = TDescription(Ydb::StatusIds::BAD_REQUEST, {NYql::TIssue("Duplicate secret request identity")});
        co_return;
    }
    Y_DEFER { if (callerOwned) RunningRequests.erase(key); };
    try {
        result = co_await Resolve(*request->Get());
    } catch (const TResolutionFailure& error) {
        result = error.Description;
    } catch (const std::exception&) {
        result = TDescription(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Secret resolution failed")});
    }
}

void TDescribeSchemaSecretsService::HandleCancellation(TEvCancelResolveSecret::TPtr& ev) {
    const auto it = RunningRequests.find({ev->Sender, ev->Cookie});
    if (it != RunningRequests.end()) it->second->Cancel();
}

void TDescribeSchemaSecretsService::Bootstrap() {
    Become(&TThis::StateWait);
}

void TDescribeSchemaSecretsService::PassAway() {
    for (const auto& [path, subscriber] : SchemeBoardSubscribers) {
        Send(subscriber, new TEvents::TEvPoison);
    }
    TActorBootstrapped::PassAway();
}

async<TAutoPtr<NSchemeCache::TSchemeCacheNavigate>> TDescribeSchemaSecretsService::Navigate(
    const TEvResolveSecret& request, const TVector<TString>& paths)
{
    auto retry = (request.Settings.RetryPolicy ? request.Settings.RetryPolicy : MakeShortRetryPolicy())->CreateRetryState();
    for (;;) {
        const auto navigate = [&]() -> async<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr> {
            auto body = MakeHolder<NSchemeCache::TSchemeCacheNavigate>();
            for (const auto& path : paths) {
                auto& entry = body->ResultSet.emplace_back();
                entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpPath;
                entry.SyncVersion = true;
                entry.Path = SplitPath(path);
                if (request.UserToken && request.UserToken->GetUserSID()) entry.Access = NACLib::SelectRow;
            }
            if (request.UserToken && request.UserToken->GetUserSID()) body->UserToken = request.UserToken;
            body->DatabaseName = request.Database;
            co_return co_await ActorRequest<TEvTxProxySchemeCache::TEvNavigateKeySetResult>(MakeSchemeCacheID(),
                new TEvTxProxySchemeCache::TEvNavigateKeySet(body.Release()));
        };
        auto response = co_await WithTimeout(TDuration::Seconds(30), navigate);
        if (!response) FailResolution(Ydb::StatusIds::UNAVAILABLE, "Secret schema navigation timed out");
        auto result = std::move((*response)->Get()->Request);
        if (!result || result->ResultSet.empty()) FailResolution(Ydb::StatusIds::BAD_REQUEST, "secrets were not found");
        TVector<TString> missing;
        TString retryPath;
        for (auto& entry : result->ResultSet) {
            const auto status = GetSchemeCacheEntryStatus(SchemeCacheStatusGetter, entry);
            if (status == NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError) {
                if (retryPath.empty()) retryPath = CanonizePath(entry.Path);
            } else if (status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
                missing.push_back(CanonizePath(entry.Path));
            }
        }
        if (!missing.empty()) FailResolution(Ydb::StatusIds::BAD_REQUEST, ListSecrets(missing) + " not found");
        if (retryPath.empty()) co_return std::move(result);
        const auto delay = retry->GetNextRetryDelay();
        if (!delay) FailResolution(Ydb::StatusIds::UNAVAILABLE,
            TStringBuilder() << "Retry limit exceeded for secret `" << retryPath << "`");
        co_await AsyncSleepFor(*delay);
    }
}

async<TDescribeSchemaSecretsService::TVersionedSecret> TDescribeSchemaSecretsService::ResolveOne(
    const TEvResolveSecret& request, NSchemeCache::TSchemeCacheNavigate::TEntry entry)
{
    const TString path = CanonizePath(entry.Path);
    TRetryPolicy::IRetryState::TPtr retry;
    for (;;) {
        if (entry.Kind != NSchemeCache::TSchemeCacheNavigate::KindSecret || !entry.SecretInfo || !entry.Self) {
            FailResolution(Ydb::StatusIds::BAD_REQUEST, TStringBuilder() << "Secret `" << path << "` not found");
        }
        const auto& description = entry.SecretInfo->Description;
        Y_ENSURE(!description.HasValue(), "SchemeCache must never contain secret values");
        if (description.HasIamDelegation()) {
            if (!AppData()->FeatureFlags.GetEnableIamDelegationSecrets()) {
                FailResolution(Ydb::StatusIds::UNSUPPORTED, "IAM delegation secrets are disabled");
            }
            const auto& locator = description.GetIamDelegation();
            const NKqp::TEvDescribeSecretsResponse::TSecretReference reference{locator.GetTabletId(),
                locator.GetDatabaseIncarnation(), locator.GetCreateOperationId(),
                entry.Self->Info.GetSchemeshardId(), entry.Self->Info.GetPathId()};
            if (request.Settings.ExpectedReference && *request.Settings.ExpectedReference != reference) {
                FailResolution(Ydb::StatusIds::NOT_FOUND, "Secret schema incarnation changed");
            }
            for (;;) {
                const auto cookie = AllocateWaitCookie();
                const auto reader = Register(CreateIamDelegationSecretReaderActor(SelfId(), cookie,
                    path, request.Database, request.UserToken, reference));
                auto* system = TActivationContext::ActorSystem();
                Y_DEFER { system->Send(reader, new TEvents::TEvPoison); };
                const auto wait = [cookie]() -> async<TEvIamDelegationSecretReadResult::TPtr> {
                    co_return co_await ActorWaitForEvent<TEvIamDelegationSecretReadResult>(cookie);
                };
                auto event = co_await WithTimeout(IamDelegationSecretReadTimeout(), wait);
                if (!event) FailResolution(Ydb::StatusIds::UNAVAILABLE, "Delegated secret acquisition timed out");
                const auto& result = *(*event)->Get();
                if ((*event)->Sender != reader || result.SecretPath != path || result.Reference != reference) {
                    FailResolution(Ydb::StatusIds::UNAVAILABLE, "Unexpected delegated secret response");
                }
                if (result.Status == Ydb::StatusIds::SUCCESS) {
                    co_return TVersionedSecret{.Name = path, .Value = result.Token,
                        .Reference = result.Reference, .UsableUntil = result.UsableUntil};
                }
                const bool retryable = result.Status == Ydb::StatusIds::UNAVAILABLE
                    || result.Status == Ydb::StatusIds::TIMEOUT || result.Status == Ydb::StatusIds::OVERLOADED
                    || result.Status == Ydb::StatusIds::INTERNAL_ERROR;
                if (retryable) {
                    if (!retry) retry = (request.Settings.RetryPolicy
                        ? request.Settings.RetryPolicy : MakeShortRetryPolicy())->CreateRetryState();
                    if (const auto delay = retry->GetNextRetryDelay()) {
                        co_await AsyncSleepFor(*delay);
                        continue;
                    }
                }
                throw TResolutionFailure{TDescription(result.Status, result.Issues)};
            }
        }
        if (request.Settings.ExpectedReference) {
            FailResolution(Ydb::StatusIds::NOT_FOUND, "Secret is no longer an IAM delegation");
        }
        if (const auto cached = VersionedSecrets.find(path); cached != VersionedSecrets.end()
            && cached->second.SecretVersion == description.GetVersion()
            && cached->second.PathId == entry.Self->Info.GetPathId()
            && cached->second.PathOwnerId == entry.Self->Info.GetSchemeshardId()) {
            co_return cached->second;
        }
        const auto read = [&]() -> async<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr> {
            auto body = MakeHolder<TEvTxUserProxy::TEvNavigate>();
            body->Record.SetDatabaseName(request.Database);
            if (request.UserToken) body->Record.SetUserToken(request.UserToken->SerializeAsString());
            auto* describe = body->Record.MutableDescribePath();
            describe->SetPath(path);
            describe->MutableOptions()->SetReturnSecretValue(true);
            co_return co_await ActorRequest<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>(MakeTxProxyID(), body.Release());
        };
        const auto response = co_await WithTimeout(TDuration::Seconds(30), read);
        if (!response) FailResolution(Ydb::StatusIds::UNAVAILABLE, "Secret value lookup timed out");
        const auto& record = (*response)->Get()->GetRecord();
        const auto status = GetSchemeShardPathStatus(SchemeShardStatusGetter, record);
        if (status != NKikimrScheme::StatusSuccess) {
            if (IsRetryableSchemeShardStatus(status) && request.Settings.RetryPolicy) {
                if (!retry) retry = request.Settings.RetryPolicy->CreateRetryState();
                if (const auto delay = retry->GetNextRetryDelay()) {
                    co_await AsyncSleepFor(*delay);
                    continue;
                }
                FailResolution(Ydb::StatusIds::UNAVAILABLE,
                    TStringBuilder() << "Retry limit exceeded for secret `" << path << "`");
            }
            FailResolution(MapSchemeShardStatus(status), MakeSchemeShardErrorMessage(status, path));
        }
        if (CanonizePath(record.GetPath()) != path) {
            FailResolution(Ydb::StatusIds::UNAVAILABLE, "Secret value response does not match its path");
        }
        if (record.GetPathDescription().GetSecretDescription().HasIamDelegation()) {
            // Source changed while reading a stored value. Reauthorize the current
            // schema before entering the delegated acquisition path.
            const auto current = co_await Navigate(request, {path});
            if (current->ResultSet.size() != 1) FailResolution(Ydb::StatusIds::UNAVAILABLE, "Unexpected secret schema response");
            entry = current->ResultSet.front();
            continue;
        }
        if (!SchemeBoardSubscribers.contains(path)) {
            SchemeBoardSubscribers[path] = Register(CreateSchemeBoardSubscriber(SelfId(), path));
        }
        TVersionedSecret secret{
            .SecretVersion = record.GetPathDescription().GetSecretDescription().GetVersion(),
            .PathId = record.GetPathId(),
            .PathOwnerId = record.GetPathDescription().GetSelf().GetSchemeshardId(),
            .Name = path,
            .Value = record.GetPathDescription().GetSecretDescription().GetValue(),
        };
        VersionedSecrets[path] = secret;
        co_return secret;
    }
}

async<TDescribeSchemaSecretsService::TDescription> TDescribeSchemaSecretsService::Resolve(const TEvResolveSecret& request) {
    if (request.SecretNames.empty()) FailResolution(Ydb::StatusIds::BAD_REQUEST, "empty secret names list");
    const auto navigation = co_await Navigate(request, request.SecretNames);
    co_return co_await WithTaskGroup<TVersionedSecret>([&](TTaskGroup<TVersionedSecret>& group) -> async<TDescription> {
        THashSet<TString> names;
        for (const auto& entry : navigation->ResultSet) {
            const auto path = CanonizePath(entry.Path);
            if (names.insert(path).second) group.Add(&TThis::ResolveOne, this, std::cref(request), entry);
        }
        THashMap<TString, TVersionedSecret> resolved;
        while (group) {
            auto secret = co_await group.Next();
            resolved.emplace(secret.Name, std::move(secret));
        }
        std::vector<TString> values;
        std::vector<NKqp::TEvDescribeSecretsResponse::TSecretReference> references;
        std::vector<TInstant> usableUntil;
        for (const auto& requestedPath : request.SecretNames) {
            const auto it = resolved.find(CanonizePath(requestedPath));
            if (it == resolved.end()) FailResolution(Ydb::StatusIds::UNAVAILABLE, "Missing secret schema response");
            const auto& secret = it->second;
            if (secret.Reference.TabletId && TActivationContext::Now() >= secret.UsableUntil) {
                FailResolution(Ydb::StatusIds::UNAVAILABLE, "Delegated token expired before the secret batch completed");
            }
            values.push_back(secret.Value);
            references.push_back(secret.Reference);
            usableUntil.push_back(secret.Reference.TabletId ? secret.UsableUntil : TInstant::Max());
        }
        co_return TDescription(std::move(values), std::move(references), std::move(usableUntil));
    });
}

IRetryPolicy<>::TPtr MakeShortRetryPolicy() {
    return IRetryPolicy<>::GetExponentialBackoffPolicy(
        [](){ return ERetryErrorClass::ShortRetry; },
        /* minDelay */ TDuration::MilliSeconds(100),
        /* minLongRetryDelay */ TDuration::MilliSeconds(500),
        /* maxDelay */ TDuration::Seconds(5),
        /* maxRetries */ 10,
        /* maxTime */ TDuration::Seconds(10)
    );
}

IRetryPolicy<>::TPtr MakeLongRetryPolicy() {
    return IRetryPolicy<>::GetExponentialBackoffPolicy(
        [](){ return ERetryErrorClass::LongRetry; },
        /* minDelay */ TDuration::Seconds(1),
        /* minLongRetryDelay */ TDuration::Seconds(10),
        /* maxDelay */ TDuration::Seconds(30),
        /* maxRetries */ std::numeric_limits<size_t>::max(),
        /* maxTime */ TDuration::Minutes(60)
    );
}

void TDescribeSchemaSecretsService::HandleNotifyUpdate(TSchemeBoardEvents::TEvNotifyUpdate::TPtr& ev) {
    Y_UNUSED(ev);
}

void TDescribeSchemaSecretsService::HandleNotifyDelete(TSchemeBoardEvents::TEvNotifyDelete::TPtr& ev) {
    const TString& secretName = CanonizePath(ev->Get()->Path);

    if (SecretUpdateListener) {
        SecretUpdateListener->HandleNotifyDelete(secretName);
    }

    VersionedSecrets.erase(secretName);

    const auto subscriberIt = SchemeBoardSubscribers.find(secretName);
    Y_ENSURE(subscriberIt != SchemeBoardSubscribers.end());
    Send(subscriberIt->second, new TEvents::TEvPoisonPill());
    SchemeBoardSubscribers.erase(subscriberIt);
}

namespace {
enum class ESecretSource { Schema, Legacy };
enum class EResolveState { Pending, Completed };

async<TDescription> ResolveSecretOwned(TVector<TString> names, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
    TString database, TDescribeSecretSettings settings, ESecretSource source)
{
    auto* system = TActivationContext::ActorSystem();
    const auto self = TActivationContext::AsActorContext().SelfID;
    const auto cookie = AllocateWaitCookie();
    auto promise = NThreading::NewPromise<TDescription>();
    promise.GetFuture().Subscribe([system, self, cookie](const NThreading::TFuture<TDescription>& ready) {
        TDescription response(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Secret resolution failed")});
        try { response = ready.GetValue(); } catch (const std::exception&) {}
        system->Send(new IEventHandle(self, {}, new NKqp::TEvDescribeSecretsResponse(std::move(response)), 0, cookie));
    });
    TActorId legacy;
    EResolveState state = EResolveState::Pending;
    const auto service = MakeDescribeSchemaSecretServiceId(self.NodeId());
    Y_DEFER {
        if (legacy) system->Send(legacy, new TEvents::TEvPoison);
        if (source == ESecretSource::Schema && state == EResolveState::Pending) {
            system->Send(new IEventHandle(service, self,
                new TDescribeSchemaSecretsService::TEvCancelResolveSecret, 0, cookie));
        }
    };
    if (source == ESecretSource::Schema) {
        system->Send(new IEventHandle(service, self,
            new TDescribeSchemaSecretsService::TEvResolveSecret(userToken, database, names, promise,
                std::move(settings), TDescribeSchemaSecretsService::EResolveOwnership::Caller),
            IEventHandle::FlagTrackDelivery, cookie));
    } else {
        if (AppData()->FeatureFlags.GetDisableOldSecrets()) {
            co_return TDescription(Ydb::StatusIds::BAD_REQUEST,
                {NYql::TIssue("Usage of old secrets is disabled now. Please use new secrets")});
        }
        legacy = TActivationContext::Register(CreateDescribeSecretsActor(
            userToken ? userToken->GetUserSID() : "", names, promise));
    }
    const auto response = co_await ActorWaitForEvent<IEventHandle>(cookie);
    state = EResolveState::Completed;
    if (response->GetTypeRewrite() != NKqp::TEvDescribeSecretsResponse::EventType) {
        co_return TDescription(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Secret resolver is unavailable")});
    }
    auto description = std::move(response->Get<NKqp::TEvDescribeSecretsResponse>()->Description);
    if (description.Status == Ydb::StatusIds::SUCCESS) {
        for (size_t i = 0; i < description.References.size(); ++i) {
            if (description.References[i].TabletId && (i >= description.UsableUntil.size()
                || TActivationContext::Now() >= description.UsableUntil[i])) {
                co_return TDescription(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Delegated token expired before acquisition completed")});
            }
        }
    }
    co_return description;
}

class TSecretPromiseAdapter final : public TActorBootstrapped<TSecretPromiseAdapter> {
public:
    TSecretPromiseAdapter(TVector<TString> names, TIntrusiveConstPtr<NACLib::TUserToken> userToken,
        TString database, TDescribeSecretSettings settings, ESecretSource source, NThreading::TPromise<TDescription> promise)
        : Names(std::move(names)), UserToken(std::move(userToken)), Database(std::move(database))
        , Settings(std::move(settings)), Source(source), Promise(std::move(promise)) {}

    void Bootstrap() {
        Become(&TThis::StateWork);
        auto promise = std::move(Promise);
        TDescription result(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Secret acquisition was cancelled")});
        Y_DEFER { promise.TrySetValue(result); };
        try {
            result = co_await ResolveSecretOwned(std::move(Names), std::move(UserToken),
                std::move(Database), std::move(Settings), Source);
        } catch (const std::exception&) {
            result = TDescription(Ydb::StatusIds::UNAVAILABLE, {NYql::TIssue("Secret resolution failed")});
        }
        PassAway();
    }
private:
    STRICT_STFUNC(StateWork,
        IgnoreFunc(NKqp::TEvDescribeSecretsResponse);
        IgnoreFunc(TEvents::TEvUndelivered);
        cFunc(TEvents::TEvPoison::EventType, PassAway);
    )
    TVector<TString> Names;
    TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    TString Database;
    TDescribeSecretSettings Settings;
    const ESecretSource Source;
    NThreading::TPromise<TDescription> Promise;
};

NThreading::TFuture<TDescription> StartSecretPromiseAdapter(TVector<TString> names,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, TString database, TDescribeSecretSettings settings,
    ESecretSource source, TActorSystem* system)
{
    auto promise = NThreading::NewPromise<TDescription>();
    auto future = promise.GetFuture();
    system->Register(new TSecretPromiseAdapter(std::move(names), std::move(userToken),
        std::move(database), std::move(settings), source, std::move(promise)));
    return future;
}
} // namespace

async<TDescription> DescribeSecretAsync(TVector<TString> secretNames,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, TString database, TDescribeSecretSettings settings)
{
    const auto source = UseSchemaSecrets(AppData()->FeatureFlags, secretNames) ? ESecretSource::Schema : ESecretSource::Legacy;
    co_return co_await ResolveSecretOwned(std::move(secretNames), std::move(userToken),
        std::move(database), std::move(settings), source);
}

NThreading::TFuture<TDescription> DescribeSchemaSecret(TVector<TString> secretNames,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, TString database, TActorSystem* system, TDescribeSecretSettings settings)
{
    return StartSecretPromiseAdapter(std::move(secretNames), std::move(userToken), std::move(database),
        std::move(settings), ESecretSource::Schema, system);
}

NThreading::TFuture<TDescription> DescribeSecret(const TVector<TString>& secretNames,
    const TIntrusiveConstPtr<NACLib::TUserToken> userToken, const TString& database,
    TActorSystem* system, TDescribeSecretSettings settings)
{
    const auto source = UseSchemaSecrets(AppData(system)->FeatureFlags, secretNames) ? ESecretSource::Schema : ESecretSource::Legacy;
    return StartSecretPromiseAdapter(secretNames, userToken, database, std::move(settings), source, system);
}

IActor* TDescribeSchemaSecretsServiceFactory::CreateService() {
    return new TDescribeSchemaSecretsService();
}

bool IsSchemeSecret(const TString& secretName) {
    return secretName.StartsWith('/');
}

bool UseSchemaSecrets(const NKikimr::TFeatureFlags& flags, const TVector<TString>& secretNames) {
    if (!flags.GetEnableSchemaSecrets()) {
        return false;
    }

    for (const auto& secretName : secretNames) {
        if (!IsSchemeSecret(secretName)) {
            return false;
        }
    }

    return true; // New secrets are enabled and all of them start with '/'
}

bool UseSchemaSecrets(const NKikimr::TFeatureFlags& flags, const TString& secretName) {
    return flags.GetEnableSchemaSecrets() && IsSchemeSecret(secretName);
}

}  // namespace NKikimr::NSecret
