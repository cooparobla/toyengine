#include <toyengine/ui/inventory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include <coopa/input/keys.h>
#include <uicoopa/immediate/imm_icons.h>

namespace toy::ui {
namespace {

using coopa::ui::imm::Box;
using coopa::ui::imm::Context;
using coopa::ui::imm::SlotInfo;
using coopa::item::ItemCategory;

/// The payload type both halves of a drag agree on, so a drag from one grid cannot drop into
/// an unrelated drop target elsewhere on screen.
constexpr const char* k_drag_type = "toy::item_slot";

ItemCategory category_at(int index) {
    switch (index) {
        case 1: return ItemCategory::Weapon;
        case 2: return ItemCategory::Armor;
        case 3: return ItemCategory::Consumable;
        case 4: return ItemCategory::Material;
        case 5: return ItemCategory::Quest;
        default: return ItemCategory::Misc;
    }
}

}  // namespace

const std::vector<std::string>& inventory_categories() {
    // Index 0 is All; the rest line up with category_at() above, which is the only place the
    // order is stated.
    static const std::vector<std::string> names = {
        "All", "Weapons", "Armour", "Aid", "Materials", "Quest", "Misc",
    };
    return names;
}

glm::vec4 rarity_tint(const GameTheme& theme, coopa::item::ItemRarity rarity) {
    using R = coopa::item::ItemRarity;
    switch (rarity) {
        case R::Uncommon:  return theme.feedback.healing;
        case R::Rare:      return theme.feedback.notice;
        case R::Epic:      return theme.feedback.critical;
        case R::Legendary: return theme.feedback.damage;
        default:           return glm::vec4(1.0f);
    }
}

InventoryAction inventory_grid(Context& ctx, std::string_view id, const Box& b,
                               coopa::item::Inventory& inv, const coopa::item::ItemDatabase& db,
                               int cols, const GameTheme& theme, bool allow_split) {
    InventoryAction act;
    const int capacity = inv.capacity();
    if (capacity <= 0) return act;
    cols = std::max(1, cols);
    const int rows = (capacity + cols - 1) / cols;

    ctx.slot_grid(id, b, cols, rows, [&](Context& c, const SlotInfo& s) {
        if (s.index >= capacity) return;
        const coopa::item::ItemStack& stack = inv.at(s.index);
        const coopa::item::ItemDef* def = stack.item.is_valid() ? db.find(stack.item) : nullptr;

        if (def) {
            const glm::vec4 tint = def->tint.a >= 0.0f ? def->tint : rarity_tint(theme, def->rarity);
            if (!def->icon.empty()) {
                c.icon(coopa::ui::imm::icon_from_name(def->icon), s.box.shrink(4.0f), tint);
            } else {
                // No icon: the first letters stand in, so a slot is never blank and
                // indistinguishable from an empty one.
                c.text_in(s.box, def->name.substr(0, 3), tint, 0.0f, true);
            }
            if (stack.count > 1) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", stack.count);
                const float w = c.text.width(buf, c.style.font_size);
                c.draw_text({s.box.right() - w - 3.0f,
                             s.box.bottom() - c.text.line_height(c.style.font_size) - 2.0f},
                            buf, theme.slots.key_label);
            }
            // The drag is declared on the cell that has something to give. drag_source() keys
            // off the last registered item, which inside this callback is this cell's button.
            c.drag_source(k_drag_type, std::to_string(s.index), def->name);
        }

        // Every cell is a drop target, including empty ones -- an empty slot is where a stack
        // most often goes.
        if (std::optional<std::string> from = c.drop_target(k_drag_type, s.box)) {
            const int src = std::atoi(from->c_str());
            if (src >= 0 && src < capacity && src != s.index) {
                // The model owns the rules. Ctrl peels one off; otherwise merge-or-swap.
                const bool split = allow_split &&
                                   has(c.input().mods, coopa::input::Mods::Control);
                if (split) inv.split(src, 1, s.index);
                else inv.move_or_merge(src, s.index);
                act.moved_from = src;
                act.moved_to = s.index;
            }
        }
        if (s.pressed) act.clicked = s.index;
        if (s.right_pressed) act.right_clicked = s.index;
    }, slot_style(theme));
    return act;
}

InventoryAction inventory_list(Context& ctx, std::string_view id, const Box& b,
                               coopa::item::Inventory& inv, const coopa::item::ItemDatabase& db,
                               int* category, int* selected, const GameTheme& theme) {
    InventoryAction act;
    int cat = category ? *category : 0;
    int sel = selected ? *selected : 0;

    // Three columns: categories, the list, the detail card. Proportional rather than fixed so
    // the screen works at the resolutions a player actually has.
    const float gap = ctx.style.spacing;
    const float cat_w = std::max(96.0f, b.w * 0.20f);
    const float card_w = std::max(140.0f, b.w * 0.30f);
    const Box cat_box{b.x, b.y, cat_w, b.h};
    const Box list_box{b.x + cat_w + gap, b.y, std::max(0.0f, b.w - cat_w - card_w - gap * 2.0f), b.h};
    const Box card_box{b.right() - card_w, b.y, card_w, b.h};

    ctx.push_id(id);
    if (ctx.category_list("cats", cat_box, inventory_categories(), &cat) >= 0) sel = 0;

    // Built fresh each frame from the model, which is the point: an item used elsewhere
    // disappears here with nothing to keep in step.
    std::vector<coopa::ui::imm::ItemRow> rows;
    std::vector<int> slot_of;
    for (int i = 0; i < inv.capacity(); ++i) {
        const coopa::item::ItemStack& stack = inv.at(i);
        if (!stack.item.is_valid() || stack.count <= 0) continue;
        const coopa::item::ItemDef* def = db.find(stack.item);
        if (!def) continue;
        if (cat != 0 && def->category != category_at(cat)) continue;
        coopa::ui::imm::ItemRow r;
        r.label = def->name.empty() ? def->id.str() : def->name;
        r.count = stack.count;
        r.icon = def->icon.empty() ? coopa::ui::imm::Icon::None
                                   : coopa::ui::imm::icon_from_name(def->icon);
        r.tint = def->tint.a >= 0.0f ? def->tint : rarity_tint(theme, def->rarity);
        r.user = i;
        rows.push_back(std::move(r));
        slot_of.push_back(i);
    }

    coopa::ui::imm::ItemListColumns cols;
    cols.count = true;
    const int hit = ctx.item_list("rows", list_box, rows, &sel, cols);
    if (hit >= 0 && hit < static_cast<int>(slot_of.size())) act.clicked = slot_of[static_cast<size_t>(hit)];

    // The detail card follows the selection.
    std::string title, desc;
    std::vector<std::pair<std::string, std::string>> stats;
    if (sel >= 0 && sel < static_cast<int>(slot_of.size())) {
        const coopa::item::ItemStack& stack = inv.at(slot_of[static_cast<size_t>(sel)]);
        if (const coopa::item::ItemDef* def = db.find(stack.item)) {
            title = def->name.empty() ? def->id.str() : def->name;
            desc = def->description;
            stats.emplace_back("Count", std::to_string(stack.count));
            stats.emplace_back("Stack", std::to_string(def->max_stack));
        }
    }
    ctx.item_detail(card_box, title, desc, stats);
    ctx.pop_id();

    if (category) *category = cat;
    if (selected) *selected = sel;
    return act;
}

}  // namespace toy::ui
