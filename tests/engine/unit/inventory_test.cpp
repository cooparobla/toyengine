/**
 * @file inventory_test.cpp
 * @brief The inventory helpers: the grid's drag through the model, and the categorised list.
 *
 * The old retained system had 16 cases over its inventory grid and the binding that fed it;
 * the replacement had two and none for dragging, which is the gap these close. The thing worth
 * testing is not that cells draw -- it is that a drag reaches `Inventory::move_or_merge()`, so
 * stacking and swapping behave exactly as they do everywhere else in the game rather than being
 * reimplemented in UI code.
 */

#include <coopa/testing/test.h>

#include <functional>
#include <string>
#include <vector>

#include <coopa/input/keys.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/render/draw_list.h>

#include <toyengine/ui/inventory.h>

COOPA_TEST_SUITE("inventory");

namespace {

using coopa::ui::imm::Box;
using coopa::ui::imm::Context;
using coopa::ui::imm::FrameInput;
using coopa::ui::imm::SlotInfo;

/** @brief Drives imm frames with a mouse that can press, hold and release across frames. */
struct Harness {
    Context ctx;
    coopa::ui::DrawList dl;
    glm::vec2 size{800.0f, 600.0f};
    glm::vec2 mouse{-1.0e6f};
    bool down = false;
    bool prev_down = false;
    coopa::input::Mods mods = coopa::input::Mods::None;

    void frame(const std::function<void(Context&)>& fn) {
        FrameInput in;
        in.mouse = mouse;
        in.dt = 1.0f / 60.0f;
        in.down[0] = down;
        in.pressed[0] = down && !prev_down;
        in.released[0] = !down && prev_down;
        in.mods = mods;
        prev_down = down;
        dl.begin(coopa::ui::Rect{glm::vec2(0.0f), size});
        ctx.begin_frame(dl, in, size);
        fn(ctx);
        ctx.end_frame();
    }

    /// Press at `from`, move to `to` while held, release there -- a complete drag gesture.
    void drag(glm::vec2 from, glm::vec2 to, const std::function<void(Context&)>& fn) {
        mouse = from;
        frame(fn);          // hover established (imm's hover comes from the previous frame)
        down = true;
        frame(fn);          // press
        mouse = to;
        frame(fn);          // moved while held -- the drag becomes active here
        frame(fn);          // and the target sees it hovering
        down = false;
        frame(fn);          // release -> the drop lands
    }

    void right_click(glm::vec2 at, const std::function<void(Context&)>& fn) {
        mouse = at;
        frame(fn);
        FrameInput in;
        in.mouse = at;
        in.dt = 1.0f / 60.0f;
        in.pressed[static_cast<int>(coopa::ui::imm::Mouse::Right)] = true;
        in.down[static_cast<int>(coopa::ui::imm::Mouse::Right)] = true;
        dl.begin(coopa::ui::Rect{glm::vec2(0.0f), size});
        ctx.begin_frame(dl, in, size);
        fn(ctx);
        ctx.end_frame();
    }
};

/// Two stackable items and one that does not stack, which is what makes merge vs swap visible.
coopa::item::ItemDatabase make_db() {
    coopa::item::ItemDatabase db;
    coopa::item::ItemDef potion;
    potion.id = coopa::item::ItemId::from_name("potion");
    potion.name = "Healing Potion";
    potion.description = "Restores health.";
    potion.max_stack = 10;
    potion.category = coopa::item::ItemCategory::Consumable;
    potion.rarity = coopa::item::ItemRarity::Uncommon;
    db.define(potion);

    coopa::item::ItemDef sword;
    sword.id = coopa::item::ItemId::from_name("sword");
    sword.name = "Iron Sword";
    sword.max_stack = 1;
    sword.category = coopa::item::ItemCategory::Weapon;
    sword.rarity = coopa::item::ItemRarity::Rare;
    db.define(sword);
    return db;
}

}  // namespace

/** @brief A rarity maps to a themed colour, and Common stays untinted. */
COOPA_TEST(rarity_tint_maps_each_tier_and_leaves_common_alone) {
    const toy::ui::GameTheme theme;
    using R = coopa::item::ItemRarity;
    ASSERT_VEC_NEAR(toy::ui::rarity_tint(theme, R::Common), glm::vec4(1.0f), 1e-5f);
    ASSERT_VEC_NEAR(toy::ui::rarity_tint(theme, R::Uncommon), theme.feedback.healing, 1e-5f);
    ASSERT_VEC_NEAR(toy::ui::rarity_tint(theme, R::Rare), theme.feedback.notice, 1e-5f);
    ASSERT_VEC_NEAR(toy::ui::rarity_tint(theme, R::Epic), theme.feedback.critical, 1e-5f);
    ASSERT_VEC_NEAR(toy::ui::rarity_tint(theme, R::Legendary), theme.feedback.damage, 1e-5f);
}

/**
 * @brief Dragging a stack onto an empty cell moves it, and onto a like stack merges it --
 *        through the model, not through arithmetic in the UI.
 */
COOPA_TEST(dragging_between_cells_moves_and_merges_through_the_inventory) {
    const coopa::item::ItemDatabase db = make_db();
    const toy::ui::GameTheme theme;
    const coopa::item::ItemId potion = coopa::item::ItemId::from_name("potion");

    coopa::item::Inventory inv(4);
    inv.set(0, coopa::item::ItemStack{potion, 3});

    // A 2x2 grid of 100px cells at the origin: cell 0 is centred near (50,50), cell 1 (150,50).
    const Box grid{0.0f, 0.0f, 200.0f, 200.0f};
    toy::ui::InventoryAction last;
    auto draw = [&](Context& c) {
        const toy::ui::InventoryAction a =
            toy::ui::inventory_grid(c, "bag", grid, inv, db, 2, theme);
        if (a.moved_from >= 0) last = a;
        if (a.clicked >= 0 || a.right_clicked >= 0) last = a;
    };

    Harness h;
    h.drag({50.0f, 50.0f}, {150.0f, 50.0f}, draw);
    coopa::test::expect(last.moved_from == 0 && last.moved_to == 1,
           "the drag reported moving slot 0 to slot 1 (" + std::to_string(last.moved_from) +
               " -> " + std::to_string(last.moved_to) + ")");
    coopa::test::expect(inv.at(1).count == 3 && inv.at(1).item == potion, "the stack is in slot 1");
    coopa::test::expect(inv.at(0).count == 0, "and slot 0 is empty");

    // Now a merge: put 4 more in slot 0 and drag them onto the 3 already in slot 1.
    inv.set(0, coopa::item::ItemStack{potion, 4});
    last = {};
    Harness h2;
    h2.drag({50.0f, 50.0f}, {150.0f, 50.0f}, draw);
    coopa::test::expect(inv.at(1).count == 7, "the stacks merged to 7 (got " + std::to_string(inv.at(1).count) + ")");
    coopa::test::expect(inv.at(0).count == 0, "and the source emptied");
}

/** @brief A right-click is reported, which is how "use" and "equip" reach the game. */
COOPA_TEST(a_right_click_on_a_cell_is_reported) {
    const coopa::item::ItemDatabase db = make_db();
    const toy::ui::GameTheme theme;
    coopa::item::Inventory inv(4);
    inv.set(2, coopa::item::ItemStack{coopa::item::ItemId::from_name("sword"), 1});

    int right = -1;
    auto draw = [&](Context& c) {
        const toy::ui::InventoryAction a =
            toy::ui::inventory_grid(c, "bag", Box{0.0f, 0.0f, 200.0f, 200.0f}, inv, db, 2, theme);
        if (a.right_clicked >= 0) right = a.right_clicked;
    };
    Harness h;
    h.right_click({50.0f, 150.0f}, draw);   // bottom-left cell = index 2
    coopa::test::expect(right == 2, "the right-clicked cell was reported (" + std::to_string(right) + ")");
}

/**
 * @brief The list filters by category and reports the SLOT, not the row.
 *
 * The row index is an artefact of filtering; a game needs the slot to act on the item, and
 * getting that mapping wrong would silently use the wrong item whenever a filter is active.
 */
COOPA_TEST(the_list_filters_by_category_and_reports_the_slot_behind_a_row) {
    const coopa::item::ItemDatabase db = make_db();
    const toy::ui::GameTheme theme;
    coopa::item::Inventory inv(6);
    inv.set(0, coopa::item::ItemStack{coopa::item::ItemId::from_name("potion"), 2});
    inv.set(3, coopa::item::ItemStack{coopa::item::ItemId::from_name("sword"), 1});

    const std::vector<std::string>& cats = toy::ui::inventory_categories();
    ASSERT_TRUE(!cats.empty());
    ASSERT_EQ(cats[0], std::string("All"));

    // Weapons is index 1, and the sword lives in slot 3 -- so the single row shown must map
    // back to 3, not to 0.
    int category = 1;
    int selected = 0;
    int clicked = -1;
    auto draw = [&](Context& c) {
        const toy::ui::InventoryAction a = toy::ui::inventory_list(
            c, "inv", Box{0.0f, 0.0f, 600.0f, 400.0f}, inv, db, &category, &selected, theme);
        if (a.clicked >= 0) clicked = a.clicked;
    };

    Harness h;
    h.mouse = {300.0f, 12.0f};        // the first row of the list column
    h.frame(draw);
    h.down = true;
    h.frame(draw);
    h.down = false;
    h.frame(draw);
    coopa::test::expect(clicked == 3, "clicking the only Weapons row reported slot 3 (got " +
                             std::to_string(clicked) + ")");

    // Switching category resets the selection rather than leaving it past the end of a
    // shorter list.
    category = 3;                      // Aid
    clicked = -1;
    h.frame(draw);
    coopa::test::expect(selected == 0, "a shorter list clamps the selection");
}
