#include "page_token.h"
#include <ydb/core/security/iam_delegation/tablet/protos/inventory_page_token.pb.h>

#include <ydb/core/base/path.h>

#include <google/protobuf/io/coded_stream.h>
#include <library/cpp/string_utils/base64/base64.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <util/generic/yexception.h>

#include <array>

namespace NKikimr::NIamDelegation {
namespace {

constexpr size_t MaxTokenSize = 16384;
constexpr size_t MaxPayloadSize = 8192;
constexpr size_t SignatureSize = 32;

bool ValidPayload(const NKikimrIamDelegation::TInventoryPageToken& payload) {
    const auto& database = payload.GetDatabase();
    const auto& path = database.GetPath();
    if (!payload.GetTabletId() || !payload.GetInventoryRevision()
        || payload.GetAfterOperationId().empty() || payload.GetAfterOperationId().size() > 256
        || database.GetIncarnation().empty() || database.GetIncarnation().size() > 256
        || database.GetDatabaseId().size() > 256
        || path.empty() || path.size() > 4096 || path.front() != '/' || CanonizePath(path) != path
        || payload.GetReflection()->GetUnknownFields(payload).field_count()
        || database.GetReflection()->GetUnknownFields(database).field_count()) {
        return false;
    }
    for (const auto& part : SplitPath(path)) {
        if (IsPathPartContainsOnlyDots(part)) {
            return false;
        }
    }
    return true;
}

} // namespace

TString TInventoryPageTokenCodec::GenerateSigningKey() {
    TString key;
    key.ReserveAndResize(KeySize);
    Y_ABORT_UNLESS(RAND_bytes(reinterpret_cast<unsigned char*>(key.Detach()), key.size()) == 1,
        "Cannot generate IAM inventory signing key");
    return key;
}

TInventoryPageTokenCodec::TInventoryPageTokenCodec(TString key, ui64 tabletId)
    : Key(std::move(key))
    , TabletId(tabletId)
{
    Y_ABORT_UNLESS(Key.size() == KeySize && TabletId, "Invalid IAM inventory signing configuration");
}

TString TInventoryPageTokenCodec::Sign(TStringBuf payload) const {
    TString authenticated = "YDB/IAMDelegation/Inventory/v2:";
    authenticated.append(payload.data(), payload.size());
    std::array<unsigned char, SignatureSize> signature;
    unsigned int size = 0;
    Y_ABORT_UNLESS(HMAC(EVP_sha256(), Key.data(), Key.size(),
        reinterpret_cast<const unsigned char*>(authenticated.data()), authenticated.size(),
        signature.data(), &size) && size == SignatureSize, "Cannot authenticate IAM inventory cursor");
    return TString(reinterpret_cast<const char*>(signature.data()), signature.size());
}

TString TInventoryPageTokenCodec::Encode(const NKikimrIamDelegation::TDatabaseRecord& database,
    TStringBuf afterOperationId) const
{
    NKikimrIamDelegation::TInventoryPageToken payload;
    payload.SetTabletId(TabletId);
    // Registry rows can contain fields from another version. Bind the complete
    // known identity without serializing unbounded opaque unknown fields.
    auto* identity = payload.MutableDatabase();
    identity->SetIncarnation(database.GetIdentity().GetIncarnation());
    identity->SetPath(database.GetIdentity().GetPath());
    identity->SetDatabaseId(database.GetIdentity().GetDatabaseId());
    payload.SetInventoryRevision(database.GetInventoryRevision());
    payload.SetAfterOperationId(afterOperationId.data(), afterOperationId.size());
    Y_ABORT_UNLESS(ValidPayload(payload), "Invalid IAM inventory cursor identity");
    const auto bytes = payload.SerializeAsString();
    Y_ABORT_UNLESS(bytes.size() <= MaxPayloadSize);
    return "v2." + Base64Encode(bytes) + "." + Base64Encode(Sign(bytes));
}

std::optional<TInventoryCursor> TInventoryPageTokenCodec::Decode(TStringBuf token) const {
    if (token.size() > MaxTokenSize || !token.StartsWith("v2.")) {
        return std::nullopt;
    }
    const auto separator = token.find('.', 3);
    if (separator == TStringBuf::npos || token.find('.', separator + 1) != TStringBuf::npos) {
        return std::nullopt;
    }
    const auto encodedPayload = token.SubStr(3, separator - 3);
    const auto encodedSignature = token.SubStr(separator + 1);
    if (encodedPayload.empty() || encodedPayload.size() > (MaxPayloadSize + 2) / 3 * 4
        || encodedSignature.size() != (SignatureSize + 2) / 3 * 4) {
        return std::nullopt;
    }
    TString bytes;
    TString signature;
    try {
        bytes = Base64StrictDecode(encodedPayload);
        signature = Base64StrictDecode(encodedSignature);
    } catch (const yexception&) {
        return std::nullopt;
    }
    if (bytes.size() > MaxPayloadSize || signature.size() != SignatureSize
        || Base64Encode(bytes) != encodedPayload || Base64Encode(signature) != encodedSignature) {
        return std::nullopt;
    }
    const auto expected = Sign(bytes);
    if (CRYPTO_memcmp(signature.data(), expected.data(), SignatureSize) != 0) {
        return std::nullopt;
    }
    // Only authenticated bytes reach protobuf allocation or select a cursor.
    NProtoBuf::io::CodedInputStream input(reinterpret_cast<const ui8*>(bytes.data()), bytes.size());
    input.SetTotalBytesLimit(MaxPayloadSize);
    input.SetRecursionLimit(4);
    NKikimrIamDelegation::TInventoryPageToken payload;
    if (!payload.ParseFromCodedStream(&input) || !input.ConsumedEntireMessage()
        || !ValidPayload(payload) || payload.GetTabletId() != TabletId) {
        return std::nullopt;
    }
    return TInventoryCursor{payload.GetDatabase(), payload.GetInventoryRevision(), payload.GetAfterOperationId()};
}

} // namespace NKikimr::NIamDelegation
