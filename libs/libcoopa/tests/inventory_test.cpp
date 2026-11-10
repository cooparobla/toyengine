/**
 * @file inventory_test.cpp
 * @brief coopa::item::Inventory and Hotbar: add() filling partial stacks before empty slots and
 *        returning leftovers, remove() across slots, the three-way move_or_merge (move / merge /
 *        swap, with partial merges keeping the remainder and the cap always coming from the
 *        ItemDatabase), the default cap for unknown items, change signals, split/clear, and
 *        hotbar selection wrap/emit rules.
 *
 * Item ids and the YAML database loader are in item_database_test.cpp.
 */
#include <coopa/testing/test.h>

#include <coopa/item/hotbar.h>
#include <coopa/item/inventory.h>
#include <coopa/item/item_database.h>
#include <coopa/item/item_def.h>
#include <coopa/item/item_id.h>
#include <coopa/item/item_stack.h>

COOPA_TEST_SUITE("inventory");

using namespace coopa::item;

namespace {

ItemDatabase make_test_db() {
    ItemDatabase db;
    auto define = [&db](const char* name, int max_stack) {
        ItemDef def;
        def.id = ItemId::from_name(name);
        def.max_stack = max_stack;
        db.define(def);
    };
    define("potion_health", 16);
    define("coin_gold", 999);
    define("sword_iron", 1);
    define("gem_ruby", 10);
    return db;
}

} // namespace

COOPA_TEST(add_tops_up_partial_stacks_spills_into_empty_slots_and_returns_leftover) {
    ItemDatabase db = make_test_db();
    ItemId potion = ItemId::from_name("potion_health");

    Inventory inv(2, &db);
    inv.set(0, ItemStack{potion, 10}, false);
    // slot0 (10/16) fills to 16 (uses 6 of the 20), the remaining 14 spills
    // into the empty slot1.
    EXPECT_EQ(inv.add(potion, 20), 0);
    EXPECT_EQ(inv.at(0).count, 16);
    EXPECT_TRUE(inv.at(1).item == potion);
    EXPECT_EQ(inv.at(1).count, 14);

    Inventory full(1, &db);
    full.set(0, ItemStack{potion, 16}, false); // already at max_stack
    EXPECT_EQ(full.add(potion, 5), 5);
    EXPECT_EQ(full.at(0).count, 16);
}

COOPA_TEST(remove_takes_across_slots_and_count_of_tracks_it) {
    ItemDatabase db = make_test_db();
    Inventory inv(3, &db);
    ItemId potion = ItemId::from_name("potion_health");
    inv.set(0, ItemStack{potion, 5}, false);
    inv.set(2, ItemStack{potion, 8}, false);

    EXPECT_EQ(inv.count_of(potion), 13);
    EXPECT_EQ(inv.remove(potion, 10), 10);
    EXPECT_TRUE(inv.at(0).empty());
    EXPECT_EQ(inv.at(2).count, 3);
    EXPECT_EQ(inv.count_of(potion), 3);
}

COOPA_TEST(move_or_merge_moves_merges_or_swaps) {
    ItemDatabase db = make_test_db();
    ItemId potion = ItemId::from_name("potion_health");
    ItemId sword  = ItemId::from_name("sword_iron");

    // Move into an empty slot.
    {
        Inventory inv(2, &db);
        inv.set(0, ItemStack{sword, 1}, false);
        ASSERT_TRUE(inv.move_or_merge(0, 1));
        EXPECT_TRUE(inv.at(0).empty());
        EXPECT_TRUE(inv.at(1).item == sword);
        EXPECT_EQ(inv.at(1).count, 1);
    }
    // Merge stacks of the same item.
    {
        Inventory inv(2, &db);
        inv.set(0, ItemStack{potion, 5}, false);
        inv.set(1, ItemStack{potion, 3}, false);
        ASSERT_TRUE(inv.move_or_merge(0, 1));
        EXPECT_TRUE(inv.at(0).empty());
        EXPECT_EQ(inv.at(1).count, 8);
    }
    // Swap two different items.
    {
        Inventory inv(2, &db);
        inv.set(0, ItemStack{potion, 4}, false);
        inv.set(1, ItemStack{sword, 1}, false);
        ASSERT_TRUE(inv.move_or_merge(0, 1));
        EXPECT_TRUE(inv.at(0).item == sword);
        EXPECT_TRUE(inv.at(1).item == potion);
        EXPECT_EQ(inv.at(1).count, 4);
    }
}

COOPA_TEST(partial_merge_leaves_the_remainder_in_the_source_slot) {
    ItemDatabase db = make_test_db();
    ItemId potion = ItemId::from_name("potion_health"); // max_stack 16
    Inventory inv(2, &db);
    inv.set(0, ItemStack{potion, 5}, false);
    inv.set(1, ItemStack{potion, 14}, false);

    ASSERT_TRUE(inv.move_or_merge(0, 1));
    EXPECT_EQ(inv.at(1).count, 16); // filled to max_stack
    EXPECT_FALSE(inv.at(0).empty());
    EXPECT_EQ(inv.at(0).count, 3);  // remainder stays behind
}

COOPA_TEST(merge_cap_comes_from_the_item_database_not_the_stack) {
    // ItemStack itself carries no max_stack -- the ItemDatabase (via ItemId)
    // is the only authority. An inventory pointed at a database with a
    // different max_stack for the same id must cap the merge there, proving
    // the cap comes from the database, not from any value cached alongside
    // the stack.
    ItemId ruby = ItemId::from_name("gem_ruby");
    ItemDatabase small_db;
    ItemDef small_def;
    small_def.id = ruby;
    small_def.max_stack = 10;
    small_db.define(small_def);

    Inventory inv(2, &small_db);
    inv.set(0, ItemStack{ruby, 8}, false);
    inv.set(1, ItemStack{ruby, 8}, false);
    ASSERT_TRUE(inv.move_or_merge(0, 1));
    EXPECT_EQ(inv.at(1).count, 10); // capped at small_db's max_stack
    EXPECT_EQ(inv.at(0).count, 6);  // 8 - (10-8) leftover in source
}

COOPA_TEST(unknown_item_without_a_database_uses_the_default_cap) {
    Inventory inv(1, nullptr); // no database at all
    ItemId mystery = ItemId::from_name("mystery_item");

    int leftover = inv.add(mystery, 150);
    EXPECT_EQ(inv.at(0).count, k_default_max_stack);
    EXPECT_EQ(leftover, 150 - k_default_max_stack);
}

COOPA_TEST(merge_signals_each_changed_slot_once_and_one_swap) {
    ItemDatabase db = make_test_db();
    ItemId potion = ItemId::from_name("potion_health");
    Inventory inv(2, &db);
    inv.set(0, ItemStack{potion, 5}, false);
    inv.set(1, ItemStack{potion, 3}, false);

    int changed_count = 0;
    int swapped_count = 0;
    auto c1 = inv.on_slot_changed.connect([&](int, const ItemStack&) { changed_count++; });
    auto c2 = inv.on_slots_swapped.connect([&](int, int) { swapped_count++; });

    ASSERT_TRUE(inv.move_or_merge(0, 1)); // merge -- both slots change, one swap notification
    EXPECT_EQ(changed_count, 2);
    EXPECT_EQ(swapped_count, 1);
}

COOPA_TEST(split_moves_part_of_a_stack_and_clear_slot_empties_it) {
    ItemDatabase db = make_test_db();
    ItemId potion = ItemId::from_name("potion_health");
    Inventory inv(2, &db);
    inv.set(0, ItemStack{potion, 10}, false);

    ASSERT_TRUE(inv.split(0, 4, 1));
    EXPECT_EQ(inv.at(0).count, 6);
    EXPECT_EQ(inv.at(1).count, 4);

    EXPECT_FALSE(inv.split(0, 0, 1)); // count must be > 0 -- and slot1 isn't empty at this point anyway

    inv.clear_slot(0);
    EXPECT_TRUE(inv.at(0).empty());
}

COOPA_TEST(hotbar_selection_wraps_and_emits_only_on_change) {
    ItemDatabase db = make_test_db();
    ItemId potion = ItemId::from_name("potion_health");
    Inventory inv(3, &db);
    inv.set(1, ItemStack{potion, 5}, false);
    Hotbar hb(&inv, 0, 3);

    int emit_count = 0;
    auto conn = hb.on_selection_changed.connect([&](int) { emit_count++; });

    hb.select(0);
    EXPECT_EQ(hb.selected(), 0);
    EXPECT_EQ(emit_count, 1);

    hb.select(0); // no change -- no emit
    EXPECT_EQ(emit_count, 1);

    hb.next();
    EXPECT_EQ(hb.selected(), 1);
    EXPECT_EQ(emit_count, 2);
    // The selected stack reads through to the inventory slot.
    EXPECT_TRUE(hb.selected_stack().item == potion);
    EXPECT_EQ(hb.selected_stack().count, 5);

    hb.select(2);
    hb.next(); // wraps past the end back to 0
    EXPECT_EQ(hb.selected(), 0);

    hb.prev(); // wraps before the start to count()-1
    EXPECT_EQ(hb.selected(), 2);

    hb.select(-1);
    EXPECT_EQ(hb.selected(), -1);
    EXPECT_EQ(hb.selected_slot(), -1);
    EXPECT_TRUE(hb.selected_stack().empty());
}
