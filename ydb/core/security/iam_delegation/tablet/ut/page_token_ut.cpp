#include "../page_token.h"
#include "requests.h"

namespace NKikimr::NIamDelegation::NTests {

Y_UNIT_TEST_SUITE(IamDelegationInventoryTokenCodec) {
    Y_UNIT_TEST(SharedKeyStillBindsTheTabletIdentity) {
        const TString key(32, 'k');
        TInventoryPageTokenCodec first(key, 100);
        TInventoryPageTokenCodec second(key, 101);
        NProto::TDatabaseRecord database;
        *database.MutableIdentity() = DatabaseIdentity();
        database.SetInventoryRevision(7);
        const auto token = first.Encode(database, "operation-4");
        const auto decoded = first.Decode(token);
        UNIT_ASSERT(decoded);
        UNIT_ASSERT_VALUES_EQUAL(decoded->Database.GetIncarnation(), "database-incarnation-1");
        UNIT_ASSERT_VALUES_EQUAL(decoded->Database.GetPath(), "/Root/database");
        UNIT_ASSERT_VALUES_EQUAL(decoded->Database.GetDatabaseId(), "database-id-1");
        UNIT_ASSERT_VALUES_EQUAL(decoded->InventoryRevision, 7);
        UNIT_ASSERT_VALUES_EQUAL(decoded->AfterOperationId, "operation-4");
        UNIT_ASSERT(!second.Decode(token));
        UNIT_ASSERT(!TInventoryPageTokenCodec(TString(32, 'z'), 100).Decode(token));
    }

    Y_UNIT_TEST(OptionalDatabaseAliasAndUnknownRegistryFieldsDoNotChangeIdentity) {
        TInventoryPageTokenCodec codec(TString(32, 'k'), 100);
        NProto::TDatabaseRecord database;
        *database.MutableIdentity() = DatabaseIdentity("incarnation", "/Root/database", "");
        database.SetInventoryRevision(3);
        auto* identity = database.MutableIdentity();
        identity->GetReflection()->MutableUnknownFields(identity)->AddLengthDelimited(100, TString(32768, 'x'));
        const auto token = codec.Encode(database, "operation-1");
        UNIT_ASSERT(token.size() < 16384);
        const auto decoded = codec.Decode(token);
        UNIT_ASSERT(decoded);
        UNIT_ASSERT(decoded->Database.GetDatabaseId().empty());
        UNIT_ASSERT_VALUES_EQUAL(decoded->Database.GetIncarnation(), "incarnation");
        UNIT_ASSERT_VALUES_EQUAL(decoded->Database.GetReflection()->GetUnknownFields(decoded->Database).field_count(), 0);
    }
}

} // namespace NKikimr::NIamDelegation::NTests
