#pragma once

#include <ydb/core/security/iam_delegation/tablet/protos/iam_delegation.pb.h>

#include <util/generic/strbuf.h>
#include <util/generic/string.h>

#include <optional>

namespace NKikimr::NIamDelegation {

struct TInventoryCursor {
    NKikimrIamDelegation::TDatabaseIdentity Database;
    ui64 InventoryRevision = 0;
    TString AfterOperationId;
};

// The owning tablet loads one durable key before becoming active. Neither the
// key nor unauthenticated cursor fields cross the public RPC boundary.
class TInventoryPageTokenCodec {
public:
    static constexpr size_t KeySize = 32;
    static TString GenerateSigningKey();

    TInventoryPageTokenCodec(TString key, ui64 tabletId);

    TString Encode(const NKikimrIamDelegation::TDatabaseRecord& database,
        TStringBuf afterOperationId) const;
    std::optional<TInventoryCursor> Decode(TStringBuf token) const;

private:
    TString Sign(TStringBuf payload) const;

    const TString Key;
    const ui64 TabletId;
};

} // namespace NKikimr::NIamDelegation
