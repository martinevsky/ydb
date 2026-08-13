#include "iam_delegation_ddl.h"

#include "iam_delegation_ddl_actor.h"

namespace NKikimr::NKqp::NExternalDataSource {
namespace {

using TContext = TExternalDataSourceManager::TExternalModificationContext;
using TStatus = TExternalDataSourceManager::TYqlConclusionStatus;
using TAsyncStatus = TExternalDataSourceManager::TAsyncStatus;

// Local real-IAM protocol backdoor. Paste a short-lived raw user IAM token and
// its matching IAM subject SID here for a manual build, and never commit a
// non-empty token. The synthetic initiating identity is serialized through
// SchemeShard, and its original token is used as the ServiceControl bearer.
// When the token is empty, this code has no effect.
constexpr TStringBuf IamDelegationTestToken = R"()";
constexpr TStringBuf IamDelegationTestUserSid = "replace-with-iam-subject-id@as";

TStatus ApplyIamDelegationTestIdentityIfConfigured(TContext& context) {
    if (IamDelegationTestToken.empty()) {
        return TStatus::Success();
    }
    return ApplyIamDelegationTestIdentity(
        context, IamDelegationTestToken, IamDelegationTestUserSid);
}

} // anonymous namespace

TAsyncStatus ExecuteIamDelegationDdl(
    const NKikimrSchemeOp::TModifyScheme& schemeTx,
    const TContext& context,
    NKqpProto::TKqpSchemeOperation::OperationCase operationCase)
{
    TContext effectiveContext = context;
    auto* actorSystem = effectiveContext.GetActorSystem();
    if (!actorSystem) {
        return NThreading::MakeFuture(TStatus::Fail(
            NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
            "IAM delegation DDL requires an actor system"));
    }
    if (auto status = ApplyIamDelegationTestIdentityIfConfigured(effectiveContext);
        status.IsFail())
    {
        return NThreading::MakeFuture(std::move(status));
    }
    auto promise = NThreading::NewPromise<TStatus>();
    auto future = promise.GetFuture();
    auto* actor = CreateIamDelegationDdlActor(
        schemeTx, effectiveContext, operationCase, std::move(promise));
    if (!actor) {
        return NThreading::MakeFuture(TStatus::Fail(
            NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
            "Unsupported EXTERNAL_DATA_SOURCE operation"));
    }
    actorSystem->Register(actor);
    return future;
}

TAsyncStatus ExecuteLegacyDdlWithIamCleanup(
    const NKikimrSchemeOp::TModifyScheme& schemeTx,
    const TContext& context,
    NKqpProto::TKqpSchemeOperation::OperationCase operationCase,
    TLegacyDdlExecutor executeLegacyDdl)
{
    TContext effectiveContext = context;
    auto* actorSystem = effectiveContext.GetActorSystem();
    if (!actorSystem) {
        return NThreading::MakeFuture(TStatus::Fail(
            NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
            "IAM delegation cleanup requires an actor system"));
    }
    if (auto status = ApplyIamDelegationTestIdentityIfConfigured(effectiveContext);
        status.IsFail())
    {
        return NThreading::MakeFuture(std::move(status));
    }
    auto promise = NThreading::NewPromise<TStatus>();
    auto future = promise.GetFuture();
    auto* actor = CreateLegacyDdlWithIamCleanupActor(
        schemeTx,
        effectiveContext,
        operationCase,
        std::move(executeLegacyDdl),
        std::move(promise));
    if (!actor) {
        return NThreading::MakeFuture(TStatus::Fail(
            NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
            "Unsupported EXTERNAL_DATA_SOURCE IAM cleanup operation"));
    }
    actorSystem->Register(actor);
    return future;
}

} // namespace NKikimr::NKqp::NExternalDataSource
