#pragma once

#include <ydb/core/base/events.h>
#include <ydb/library/actors/core/actorid.h>
#include <ydb/library/actors/core/event_local.h>
#include <ydb/public/api/protos/ydb_status_codes.pb.h>
#include <yql/essentials/public/issue/yql_issue.h>

#include <util/datetime/base.h>
#include <util/digest/multi.h>
#include <util/generic/hash.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/string/builder.h>

namespace NKikimr::NIamDelegation {

// Parameters of one IAM delegation: "the agent service account of YDB in cloud CloudId may impersonate
// ServiceAccountId; the delegation is referenced by ReferrerId". The IAM types of the cloud and of the
// referrer are constants of the service (TIamDelegationSettings), not part of a delegation's record.
struct TDelegationSpec {
    TString ServiceAccountId;
    TString CloudId;
    TString ReferrerId;

    TString ToString() const {
        return TStringBuilder() << "{sa: " << ServiceAccountId << ", cloud: " << CloudId << ", referrer: " << ReferrerId << "}";
    }
};

struct TDelegationResult {
    Ydb::StatusIds::StatusCode Status = Ydb::StatusIds::SUCCESS;
    NYql::TIssues Issues;

    bool IsSuccess() const {
        return Status == Ydb::StatusIds::SUCCESS;
    }

    static TDelegationResult Success() {
        return {};
    }

    static TDelegationResult Error(Ydb::StatusIds::StatusCode status, const TString& message) {
        TDelegationResult result;
        result.Status = status;
        result.Issues.AddIssue(message);
        return result;
    }
};

// Persisted identity of an IAM grant. Recovery must use these values, even if the node's
// current service identity changes. Transport endpoints and credentials remain node settings.
struct TDelegationBinding {
    TString ServiceAccountId;
    TString CloudId;
    TString ReferrerId;
    TString ServiceId;
    TString MicroserviceId;
    TString ResourceType;
    TString ReferrerType;
    bool WithReferences = false;
};

enum class EDelegationOperationKind { Setup, Revoke };
enum class EDelegationOperationOutcome { Unknown, Accepted, Succeeded, Failed };

// Allowlisted diagnostic groups safe to persist; no remote messages or descriptions.
enum class EDelegationFailureReason {
    WrongCloud,
    ServiceNotEnabled,
    CloudNotFound,
    ServiceAccountNotFound,
    SubjectNotDelegatable,
};

struct TDelegationOperationResult {
    EDelegationOperationOutcome Outcome = EDelegationOperationOutcome::Unknown;
    TString OperationId;
    Ydb::StatusIds::StatusCode Status = Ydb::StatusIds::UNAVAILABLE;
    NYql::TIssues Issues;
    TVector<EDelegationFailureReason> FailureReasons;
};

struct TTokenKey {
    TString ServiceAccountId;
    TString CloudId;
    // Old callers may omit the service identity and use the node configuration. Durable
    // delegation readers carry the complete persisted identity, which is part of the key.
    TString ServiceId;
    TString MicroserviceId;
    TString ResourceType;

    bool operator==(const TTokenKey&) const = default;

    size_t Hash() const { return MultiHash(ServiceAccountId, CloudId, ServiceId, MicroserviceId, ResourceType); }
    TString ToString() const { return TStringBuilder() << CloudId << "/" << ServiceAccountId; }
};

struct TEvIamDelegation {
    enum EEv {
        // delegation service
        EvSetupDelegation = EventSpaceBegin(TKikimrEvents::ES_IAM_DELEGATION),
        EvSetupDelegationResult,
        EvRevokeDelegation,
        EvRevokeDelegationResult,

        EvGetToken,
        EvGetTokenResult,
        EvSubmitDelegationOperation,
        EvPollDelegationOperation,
        EvDelegationOperationResult,
        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_IAM_DELEGATION), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_IAM_DELEGATION)");

    struct TEvSetupDelegation : NActors::TEventLocal<TEvSetupDelegation, EvSetupDelegation> {
        TDelegationSpec Spec;
        TString SubjectId; // IAM subject of the user on whose behalf the delegation is set up

        TEvSetupDelegation(TDelegationSpec spec, TString subjectId)
            : Spec(std::move(spec))
            , SubjectId(std::move(subjectId))
        {}
    };

    struct TEvSetupDelegationResult : NActors::TEventLocal<TEvSetupDelegationResult, EvSetupDelegationResult> {
        TDelegationResult Result;

        explicit TEvSetupDelegationResult(TDelegationResult result)
            : Result(std::move(result))
        {}
    };

    struct TEvRevokeDelegation : NActors::TEventLocal<TEvRevokeDelegation, EvRevokeDelegation> {
        TDelegationSpec Spec;

        explicit TEvRevokeDelegation(TDelegationSpec spec)
            : Spec(std::move(spec))
        {}
    };

    struct TEvRevokeDelegationResult : NActors::TEventLocal<TEvRevokeDelegationResult, EvRevokeDelegationResult> {
        TDelegationResult Result;

        explicit TEvRevokeDelegationResult(TDelegationResult result)
            : Result(std::move(result))
        {}
    };

    struct TEvGetToken : NActors::TEventLocal<TEvGetToken, EvGetToken> {
        TTokenKey Key;
        explicit TEvGetToken(TTokenKey key) : Key(std::move(key)) {}
    };

    struct TEvGetTokenResult : NActors::TEventLocal<TEvGetTokenResult, EvGetTokenResult> {
        TTokenKey Key;
        TString Token;
        TInstant ExpiresAt;
        TInstant UsableUntil;
        Ydb::StatusIds::StatusCode Status = Ydb::StatusIds::SUCCESS;
        NYql::TIssues Issues;

        bool IsSuccess() const { return Status == Ydb::StatusIds::SUCCESS; }
    };

    // A single submission. Unknown means the caller must retain the uncertain grant and
    // cannot assume that another submission or a revocation is safe. RequestId is correlation
    // metadata only; it does not provide remote idempotency.
    struct TEvSubmitDelegationOperation : NActors::TEventLocal<TEvSubmitDelegationOperation, EvSubmitDelegationOperation> {
        EDelegationOperationKind Kind;
        TDelegationBinding Binding;
        TString SubjectId;
        TString RequestId;

        TEvSubmitDelegationOperation(EDelegationOperationKind kind, TDelegationBinding binding,
                TString subjectId = {}, TString requestId = {})
            : Kind(kind), Binding(std::move(binding)), SubjectId(std::move(subjectId)), RequestId(std::move(requestId)) {}
    };

    // Polling never resubmits the mutation. A failed Get keeps Outcome=Unknown and the ID.
    struct TEvPollDelegationOperation : NActors::TEventLocal<TEvPollDelegationOperation, EvPollDelegationOperation> {
        TString OperationId;
        explicit TEvPollDelegationOperation(TString operationId) : OperationId(std::move(operationId)) {}
    };

    struct TEvDelegationOperationResult : NActors::TEventLocal<TEvDelegationOperationResult, EvDelegationOperationResult> {
        TDelegationOperationResult Result;
        explicit TEvDelegationOperationResult(TDelegationOperationResult result) : Result(std::move(result)) {}
    };

};

// Node-local service for privileged components performing SetupDelegation / RevokeDelegation.
inline NActors::TActorId MakeIamDelegationServiceId(ui32 nodeId = 0) {
    return NActors::TActorId(nodeId, "iam_dlg_srv");
}

// Node-local minting is available only to callers that already authorized access to the secret.
inline NActors::TActorId MakeIamDelegatedTokenServiceId(ui32 nodeId = 0) {
    return NActors::TActorId(nodeId, "iam_dlg_tok");
}
} // namespace NKikimr::NIamDelegation

template <>
struct THash<NKikimr::NIamDelegation::TTokenKey> {
    size_t operator()(const NKikimr::NIamDelegation::TTokenKey& key) const { return key.Hash(); }
};
