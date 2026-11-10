/**
 * @file item_database_test.cpp
 * @brief coopa::item ids and the item database YAML loader: ItemId normalisation/hashing (the
 *        identity every inventory and save file keys on), and parse_item_database() reading
 *        every field, defaulting omitted ones, degrading on unknown enum strings, and merging
 *        into an existing database instead of replacing it.
 */
#include <coopa/testing/test.h>

#include <coopa/item/item_database.h>
#include <coopa/item/item_database_loader.h>
#include <coopa/item/item_def.h>
#include <coopa/item/item_id.h>

#include <cmath>
#include <string>
#include <unordered_map>

COOPA_TEST_SUITE("item_database");

using namespace coopa::item;

COOPA_TEST(item_id_normalizes_case_and_whitespace_and_hashes_consistently) {
    ItemId a = ItemId::from_name("Potion_Health");
    ItemId b = ItemId::from_name("  potion_health  ");
    EXPECT_TRUE(a == b);
    EXPECT_EQ(a.hash(), b.hash());
    EXPECT_TRUE(a.is_valid());
    EXPECT_EQ(a.str(), std::string("potion_health"));
    EXPECT_FALSE(ItemId().is_valid());

    std::unordered_map<ItemId, int> map;
    map[a] = 5;
    EXPECT_EQ(map[b], 5); // same normalized identity as a key
}

COOPA_TEST(loader_parses_fields_defaults_omissions_and_merges_into_existing) {
    std::string yaml =
        "items:\n"
        "  - id: potion_health\n"
        "    name: Health Potion\n"
        "    description: Restores health.\n"
        "    icon: potion\n"
        "    max_stack: 16\n"
        "    category: consumable\n"
        "    rarity: common\n"
        "    tint: { r: 0.9, g: 0.2, b: 0.3, a: 0.95 }\n"
        "  - id: mystery_box\n";
    ItemDatabase db;
    ItemDef existing;
    existing.id = ItemId::from_name("coin_gold");
    existing.max_stack = 999;
    db.define(existing);

    parse_item_database(fkyaml::node::deserialize(yaml), db);
    EXPECT_EQ(db.size(), 3u); // merged into, not replaced
    EXPECT_TRUE(db.contains(ItemId::from_name("coin_gold")));

    const ItemDef* potion = db.find(ItemId::from_name("potion_health"));
    ASSERT_TRUE(potion != nullptr);
    EXPECT_EQ(potion->name, "Health Potion");
    EXPECT_EQ(potion->max_stack, 16);
    EXPECT_TRUE(potion->category == ItemCategory::Consumable);
    EXPECT_TRUE(potion->rarity == ItemRarity::Common);
    EXPECT_NEAR(potion->tint.a, 0.95f, 1e-5f);

    // Fields omitted entirely fall back to ItemDef's own defaults.
    const ItemDef* mystery = db.find(ItemId::from_name("mystery_box"));
    ASSERT_TRUE(mystery != nullptr);
    EXPECT_EQ(mystery->max_stack, k_default_max_stack);
    EXPECT_EQ(db.max_stack_of(ItemId::from_name("never_defined")), k_default_max_stack);
    EXPECT_TRUE(mystery->category == ItemCategory::Misc);
    EXPECT_TRUE(mystery->rarity == ItemRarity::Common);

    // Unrecognized category/rarity strings degrade rather than throw.
    ItemDatabase db2;
    parse_item_database(
        fkyaml::node::deserialize(std::string("items:\n  - id: weird_item\n    category: nonsense\n    rarity: bogus\n")),
        db2);
    const ItemDef* weird = db2.find(ItemId::from_name("weird_item"));
    ASSERT_TRUE(weird != nullptr);
    EXPECT_TRUE(weird->category == ItemCategory::Misc);
    EXPECT_TRUE(weird->rarity == ItemRarity::Common);
}
