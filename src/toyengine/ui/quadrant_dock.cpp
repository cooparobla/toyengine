#include <toyengine/ui/quadrant_dock.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <toyengine/ui/quadrant_layout.h>
#include <uicoopa/immediate/imm_theme.h>

namespace toy::ui {

using coopa::ui::imm::Anchor;
using coopa::ui::imm::Box;
using coopa::ui::imm::Context;
using coopa::ui::imm::GlassStyle;
using coopa::ui::imm::Icon;
using coopa::ui::imm::ItemRow;
using coopa::ui::imm::mix;
using coopa::ui::imm::with_alpha;

using coopa::ui::imm::IconPen;
using coopa::ui::imm::register_custom_icon;

namespace {
constexpr float kDockWidth = 76.0f;
constexpr float kDockButtonSize = 44.0f;
constexpr float kDockGap = 10.0f;
/** @brief Extra space between the Home button and the panel toggles below it. */
constexpr float kDockGroupGap = 20.0f;
/** @brief One stat bar's height inside the Character panel's scrolling region. */
constexpr float kStatBarHeight = 46.0f;
/** @brief Grab width of a divider between two panels. */
constexpr float kSplitterGrab = 10.0f;
/** @brief How long the minimise/restore transition takes, end to end. */
constexpr float kRevealSeconds = 0.22f;
/** @brief How far the overlay slides toward the dock as it minimises, in canvas units. */
constexpr float kRevealSlide = 70.0f;

// The four panel icons the shared set has no business carrying -- a bag, a figure, a folded
// map and a quest scroll are this game's vocabulary, not uicoopa's. Drawn on the same 16x16
// grid as every built-in through IconPen, so they sit at the same weight beside Home and Gear.

void draw_inventory_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    d.arc(8.0f, 6.0f, 3.0f, 3.14159f, 6.28318f);   // the bag's handle, arching above the mouth
    d.box(3.0f, 6.0f, 10.0f, 8.0f);
    d.l(3.0f, 9.0f, 13.0f, 9.0f);                  // the flap
}

void draw_character_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    d.ring(8.0f, 5.0f, 2.8f);
    d.arc(8.0f, 15.0f, 5.5f, 3.14159f, 6.28318f);  // shoulders
}

void draw_map_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    d.box(2.5f, 4.0f, 11.0f, 8.0f);
    d.l(6.2f, 4.0f, 6.2f, 12.0f);                  // the two folds
    d.l(9.8f, 4.0f, 9.8f, 12.0f);
}

void draw_quests_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    d.box(4.0f, 3.0f, 8.0f, 10.0f);
    d.l(6.0f, 6.0f, 10.0f, 6.0f);
    d.l(6.0f, 8.0f, 10.0f, 8.0f);
    d.l(6.0f, 10.0f, 9.0f, 10.0f);
    d.ring(4.0f, 3.0f, 1.4f);                      // the rolled ends
    d.ring(12.0f, 13.0f, 1.4f);
}

/** @brief The four registered ids, resolved once. register_custom_icon() is idempotent per name. */
struct PanelIconIds {
    Icon inventory = register_custom_icon("Inventory", &draw_inventory_icon);
    Icon character = register_custom_icon("Character", &draw_character_icon);
    Icon map       = register_custom_icon("Map", &draw_map_icon);
    Icon quests    = register_custom_icon("Quests", &draw_quests_icon);
};
const PanelIconIds& panel_icon_ids() {
    static const PanelIconIds ids;
    return ids;
}
}  // namespace

void QuadrantDock::on_theme_loaded(const coopa::ui::imm::Theme& theme) {
    glass_ = glass_theme_from(theme);
}

void QuadrantDock::on_ready() {
    assign_default_slots_();
    slots_ = default_slots_;
}

void QuadrantDock::assign_default_slots_() {
    default_slots_.fill(PanelKind::None);
    const std::pair<bool, PanelKind> wanted[] = {
        {open_inventory, PanelKind::Inventory},
        {open_character, PanelKind::Character},
        {open_map, PanelKind::Map},
        {open_quests, PanelKind::Quests},
        {open_settings, PanelKind::Settings},
    };
    int slot = 0;
    for (const auto& [want, kind] : wanted) {
        if (!want || slot >= 4) continue;
        default_slots_[static_cast<size_t>(slot++)] = kind;
    }
}

int QuadrantDock::find_slot_(PanelKind k) const {
    for (int i = 0; i < 4; ++i) {
        if (slots_[static_cast<size_t>(i)] == k) return i;
    }
    return -1;
}

int QuadrantDock::first_free_slot_() const {
    for (int i = 0; i < 4; ++i) {
        if (slots_[static_cast<size_t>(i)] == PanelKind::None) return i;
    }
    return -1;
}

const char* QuadrantDock::panel_title_(PanelKind k) {
    switch (k) {
        case PanelKind::Inventory: return "Inventory";
        case PanelKind::Character: return "Character";
        case PanelKind::Map:       return "Map";
        case PanelKind::Quests:    return "Quests";
        case PanelKind::Settings:  return "Settings";
        case PanelKind::None:      return "";
    }
    return "";
}

Icon QuadrantDock::panel_icon_(PanelKind k) {
    const PanelIconIds& ids = panel_icon_ids();
    switch (k) {
        case PanelKind::Inventory: return ids.inventory;
        case PanelKind::Character: return ids.character;
        case PanelKind::Map:       return ids.map;
        case PanelKind::Quests:    return ids.quests;
        case PanelKind::Settings:  return Icon::Gear;
        case PanelKind::None:      return Icon::None;
    }
    return Icon::None;
}

void QuadrantDock::toggle_panel_(PanelKind k) {
    const int open_at = find_slot_(k);
    if (open_at >= 0) {
        slots_[static_cast<size_t>(open_at)] = PanelKind::None;
        drag_offset_[static_cast<size_t>(open_at)] = glm::vec2(0.0f);
        return;
    }
    const int free_slot = first_free_slot_();
    if (free_slot >= 0) slots_[static_cast<size_t>(free_slot)] = k;
}

float QuadrantDock::ease_hover_(Context& ctx, std::string_view id, bool hovered) {
    float& v = ctx.persistent(id);
    const float target = hovered ? 1.0f : 0.0f;
    if (reduced_motion) {
        v = target;
        return v;
    }
    // ~150ms time constant, matching the reference CSS's hover transition.
    const float rate = 1.0f - std::exp(-ctx.input().dt / 0.15f);
    v += (target - v) * rate;
    return v;
}

void QuadrantDock::draw(Context& ctx) {
    const Box workspace = tune().box(ctx, "workspace", Anchor::TopLeft, {0.0f, 0.0f}, ctx.canvas_size());
    const Box dock = tune().box(ctx, "dock", Anchor::TopRight, {0.0f, 0.0f}, {kDockWidth, workspace.h});
    const Box panel_area{workspace.x, workspace.y, workspace.w - dock.w, workspace.h};

    // A fixed-duration ramp, not an exponential chase: an exponential only APPROACHES its
    // target, so the overlay crawled through the last tenth of its fade and then cut to
    // nothing when it crossed the draw threshold. Stepping a 0..1 progress by dt/duration
    // reaches exactly 0 and exactly 1, and smoothstep puts the ease at both ends.
    const float target = minimised ? 0.0f : 1.0f;
    if (reduced_motion) {
        reveal_t_ = target;
    } else {
        const float step = ctx.input().dt / kRevealSeconds;
        reveal_t_ = std::clamp(reveal_t_ + (minimised ? -step : step), 0.0f, 1.0f);
    }
    reveal_ = reveal_t_ * reveal_t_ * (3.0f - 2.0f * reveal_t_);   // smoothstep

    std::array<bool, 4> open{};
    bool any_open = false;
    for (int i = 0; i < 4; ++i) {
        open[static_cast<size_t>(i)] = slots_[static_cast<size_t>(i)] != PanelKind::None;
        any_open = any_open || open[static_cast<size_t>(i)];
    }

    // Minimised: nothing of the overlay is drawn at all, so the camera view behind it is
    // untouched and -- just as importantly -- no invisible_button of a hidden panel is left
    // swallowing clicks meant for the scene.
    if (reveal_ > 0.0f) {
        const float slide = (1.0f - reveal_) * kRevealSlide;
        const Box anim_area{panel_area.x + slide, panel_area.y, panel_area.w, panel_area.h};
        // ONE fade for the whole overlay: backdrop, panes, borders, titles and every content
        // widget inside them scale together. Fading only the shells left the content popping
        // in at full opacity the instant the panes had finished arriving.
        ctx.push_alpha(reveal_);
        draw_backdrop_(ctx, anim_area);
        if (any_open) {
            const auto boxes = quadrant_layout(anim_area, open, split_);
            for (int i = 0; i < 4; ++i) {
                if (open[static_cast<size_t>(i)]) draw_panel_(ctx, i, boxes[static_cast<size_t>(i)], anim_area);
            }
            // After the panels, so a divider's grab strip sits above both of the panel edges it
            // divides; and only once settled, because a handle sliding under the pointer
            // mid-transition would grab on contact.
            if (reveal_ >= 1.0f) draw_splitters_(ctx, panel_area, open);
        }
        ctx.pop_alpha();
    }

    draw_dock_(ctx, dock, panel_area);
}

void QuadrantDock::draw_splitters_(Context& ctx, const Box& area, const std::array<bool, 4>& open) {
    const QuadrantDividers d = quadrant_dividers(area, open, split_, kSplitterGrab);

    auto divider = [&](std::string_view id, const Box& grab, bool vertical) {
        if (grab.empty()) return;   // this row/column has no two panels to divide
        bool hov = false, held = false;
        // drag_region(), not invisible_button(): a splitter is pointer-only, so it must take no
        // keyboard focus. The accent focus ring a click leaves behind would outlive the drag
        // and sit on the divider until something else was focused.
        ctx.drag_region(id, grab, &hov, &held);
        // The divider jumps to the cursor on grab rather than tracking an offset from it: over
        // a strip this thin the jump is imperceptible, and it keeps the control stateless.
        if (held) {
            if (vertical && area.w > 1.0f) split_.x = std::clamp((ctx.mouse().x - area.x) / area.w, kMinSplit, kMaxSplit);
            if (!vertical && area.h > 1.0f) split_.y = std::clamp((ctx.mouse().y - area.y) / area.h, kMinSplit, kMaxSplit);
        }
        if (hov || held) {
            const glm::vec4 c = with_alpha(ctx.style.text, held ? 0.55f : 0.3f);
            const Box bar = vertical ? Box{grab.center().x - 1.5f, grab.y, 3.0f, grab.h}
                                     : Box{grab.x, grab.center().y - 1.5f, grab.w, 3.0f};
            ctx.fill_rounded_aa(bar, c, 1.5f);
        }
    };

    divider("qd_split_v", d.vertical, true);
    divider("qd_split_h", d.horizontal, false);
}

void QuadrantDock::draw_backdrop_(Context& ctx, const Box& area) {
    if (area.empty()) return;

    // Blue -> violet -> pink, as two stacked bilinear quads: opposite corners carry the two
    // end colours and the near corners their midpoint, which is what makes one quad's own
    // interpolation read as a diagonal ramp rather than a straight vertical or horizontal one.
    const Box top{area.x, area.y, area.w, area.h * 0.5f};
    const Box bottom{area.x, area.y + area.h * 0.5f, area.w, area.h * 0.5f};
    const glm::vec4 mid1 = mix(glass_.grad_blue, glass_.grad_violet, 0.5f);
    const glm::vec4 mid2 = mix(glass_.grad_violet, glass_.grad_pink, 0.5f);
    ctx.gradient(top, glass_.grad_blue, mid1, glass_.grad_violet, mid1);
    ctx.gradient(bottom, glass_.grad_violet, mid2, glass_.grad_pink, mid2);

    if (reduced_transparency) return;   // the blobs are only ever seen softened through glass

    const float side = std::min(area.w, area.h) * 0.38f;
    const Box blob_a{area.x + area.w * 0.12f, area.y + area.h * 0.08f, side, side};
    const Box blob_b{area.x + area.w * 0.58f, area.y + area.h * 0.56f, side, side};
    ctx.shadow_soft(blob_a, side * 0.6f, with_alpha(glass_.blob_teal, 0.45f), side * 0.5f);
    ctx.shadow_soft(blob_b, side * 0.6f, with_alpha(glass_.blob_pink, 0.45f), side * 0.5f);
}

void QuadrantDock::draw_panel_(Context& ctx, int slot, const Box& box, const Box& panel_area) {
    const size_t si = static_cast<size_t>(slot);
    const PanelKind kind = slots_[si];

    const Box dragged{box.x + drag_offset_[si].x, box.y + drag_offset_[si].y, box.w, box.h};

    Box content;
    if (reduced_transparency) {
        ctx.shadow_soft(dragged, 10.0f, ctx.style.shadow, 16.0f);
        ctx.fill_rounded_aa(dragged, glass_.opaque_fill, 16.0f);
        content = dragged;
    } else {
        GlassStyle gs;
        gs.rounding = 16.0f;
        gs.rim = glass_.border;
        gs.shadow_size = 10.0f;
        content = ctx.glass_panel(dragged, gs);
    }

    const float row_h = ctx.style.row_height + 4.0f;
    const Box title_row{content.x, content.y, content.w, row_h};
    const Box close_box{title_row.right() - row_h, title_row.y, row_h, row_h};
    const Box drag_area{title_row.x, title_row.y, title_row.w - row_h, title_row.h};

    const std::string drag_id = "qd_drag_" + std::to_string(slot);
    const bool held = ctx.drag_handle(drag_id, drag_area, &drag_offset_[si]);
    if (dragging_prev_[si] && !held) {
        const Slot target = nearest_slot(panel_area, dragged.center());
        const int ti = static_cast<int>(target);
        if (ti != slot) std::swap(slots_[si], slots_[static_cast<size_t>(ti)]);
        drag_offset_[si] = glm::vec2(0.0f);
    }
    dragging_prev_[si] = held;

    ctx.icon(panel_icon_(kind), {drag_area.x + 8.0f, drag_area.y + 4.0f, row_h - 8.0f, row_h - 8.0f}, ctx.style.text_dim);
    ctx.draw_text({drag_area.x + row_h + 4.0f, drag_area.y + 2.0f}, panel_title_(kind), ctx.style.text);

    bool close_hov = false;
    const bool close_clicked = ctx.invisible_button("qd_close_" + std::to_string(slot), close_box, &close_hov);
    if (close_hov) ctx.fill_rounded_aa(close_box, ctx.style.button_hover, 8.0f);
    const Box x = close_box.shrink(8.0f);
    const glm::vec4 xcol = close_hov ? ctx.style.text : ctx.style.text_dim;
    ctx.line(x.pos(), {x.right(), x.bottom()}, xcol, 1.5f);
    ctx.line({x.right(), x.y}, {x.x, x.bottom()}, xcol, 1.5f);
    if (close_clicked) {
        slots_[si] = PanelKind::None;
        drag_offset_[si] = glm::vec2(0.0f);
        return;   // the slot is empty now; nothing left here to draw content into
    }

    const Box body{content.x + 8.0f, content.y + row_h + 4.0f, content.w - 16.0f,
                   std::max(0.0f, content.h - row_h - 12.0f)};
    draw_panel_content_(ctx, kind, body);
}

void QuadrantDock::draw_panel_content_(Context& ctx, PanelKind kind, const Box& content) {
    if (content.empty()) return;
    switch (kind) {
        case PanelKind::Inventory: {
            static const std::vector<ItemRow> kItems = {
                {"Health Potion", 3, Icon::None, glm::vec4(1.0f), 0.5f, 25, false, false, 0},
                {"Iron Sword", 1, Icon::None, glm::vec4(1.0f), 3.0f, 120, true, false, 1},
                {"Leather Boots", 1, Icon::None, glm::vec4(1.0f), 1.5f, 40, false, true, 2},
                {"Ancient Coin", 5, Icon::None, glm::vec4(0.84f, 0.78f, 0.31f, 1.0f), 0.1f, 15, false, false, 3},
            };
            const Box list{content.x, content.y, content.w * 0.6f, content.h};
            const Box detail{list.right() + 8.0f, content.y, content.w * 0.4f - 8.0f, content.h};
            ctx.item_list("qd_inv_list", list, kItems, &inventory_selected_);
            if (inventory_selected_ >= 0 && inventory_selected_ < static_cast<int>(kItems.size())) {
                const ItemRow& r = kItems[static_cast<size_t>(inventory_selected_)];
                ctx.item_detail(detail, r.label, "A reliable piece of adventuring gear.",
                               {{"Weight", std::to_string(r.weight)}, {"Value", std::to_string(r.value)}});
            }
            break;
        }
        case PanelKind::Character: {
            // A fixed bar height inside a scrolling region, rather than five bars squeezed to
            // fit: in a quarter-screen quadrant the squeezed version is unreadable, and this is
            // what puts a scrollbar there when the panel is small and none when it is large.
            ctx.begin_region("qd_char", content, true);
            auto bar = [&](const char* id, const char* label, float v, float mx, const glm::vec4& fill) {
                ctx.stat_bar(id, ctx.next_box(kStatBarHeight), label, v, mx, bar_style(theme(), fill));
            };
            bar("qd_hp", "Health", 82.0f, 100.0f, theme().bars.health);
            bar("qd_mp", "Mana", 46.0f, 100.0f, theme().bars.mana);
            bar("qd_sp", "Stamina", 70.0f, 100.0f, theme().bars.stamina);
            bar("qd_sh", "Shield", 25.0f, 100.0f, theme().bars.shield);
            bar("qd_xp", "Experience", 310.0f, 1000.0f, theme().bars.experience);
            ctx.end_region();
            break;
        }
        case PanelKind::Map: {
            constexpr int kCells = 6;
            const glm::vec4 grid_col = with_alpha(ctx.style.text, 0.15f);
            for (int i = 1; i < kCells; ++i) {
                const float fx = content.x + content.w * static_cast<float>(i) / kCells;
                const float fy = content.y + content.h * static_cast<float>(i) / kCells;
                ctx.line({fx, content.y}, {fx, content.bottom()}, grid_col);
                ctx.line({content.x, fy}, {content.right(), fy}, grid_col);
            }
            ctx.circle(content.center(), 5.0f, theme().feedback.notice);
            break;
        }
        case PanelKind::Quests: {
            static const std::vector<std::string> kCats = {"Active", "Completed"};
            static const std::vector<std::string> kActive = {"Find the lost amulet", "Defeat the bandit camp"};
            static const std::vector<std::string> kDone = {"Deliver the letter"};
            const Box cats{content.x, content.y, content.w * 0.35f, content.h};
            const Box log{cats.right() + 8.0f, content.y, content.w * 0.65f - 8.0f, content.h};
            ctx.category_list("qd_quest_cats", cats, kCats, &quest_category_);
            ctx.message_log(log, quest_category_ == 0 ? kActive : kDone, nullptr, false);
            break;
        }
        case PanelKind::Settings: {
            ctx.begin_region("qd_settings", content, true);
            ctx.checkbox("Reduced motion", &reduced_motion);
            ctx.checkbox("Reduced transparency", &reduced_transparency);
            ctx.end_region();
            break;
        }
        case PanelKind::None:
            break;
    }
}

void QuadrantDock::draw_dock_(Context& ctx, const Box& dock, const Box& panel_area) {
    // Never faded by reveal_: the dock is the only way back into a minimised UI, so it stays at
    // full opacity over the camera view at every point in the transition.
    if (reduced_transparency) {
        ctx.fill_rounded_aa(dock, glass_.opaque_fill, 0.0f);
    } else {
        ctx.glass(dock, glm::vec4(1.0f), 0.0f);
    }
    ctx.line({panel_area.right(), panel_area.y}, {panel_area.right(), panel_area.bottom()}, glass_.border, 1.0f);

    const PanelKind kinds[] = {PanelKind::Inventory, PanelKind::Character, PanelKind::Map,
                               PanelKind::Quests, PanelKind::Settings};
    constexpr int kPanelButtons = 5;

    // Home, a gap, then one toggle per panel -- the whole column centred in the dock's height
    // rather than hung from its top, so it stays centred at any window size.
    const float total_h = kDockButtonSize * (kPanelButtons + 1) + kDockGap * (kPanelButtons - 1) + kDockGroupGap;
    const float x = dock.x + (dock.w - kDockButtonSize) * 0.5f;
    float y = dock.y + (dock.h - total_h) * 0.5f;

    // Every dock affordance is a CIRCLE: the hover ring, the selected disc and the pressed
    // state alike. imm's icon_button() paints its own rounded-SQUARE for `on` and for hover,
    // which read as a different shape language from the round ring around them, so the dock
    // draws its own and uses invisible_button() purely for the behaviour and the tooltip.
    auto dock_button = [&](const std::string& id, Icon ic, const std::string& tip, bool on,
                           const Box& b, bool enabled) {
        if (!enabled) {
            ctx.icon(ic, b.shrink(10.0f), with_alpha(ctx.style.text, 0.3f));
            if (ctx.is_hovered(b)) ctx.tooltip(tip);
            return false;
        }
        bool hov = false, held = false;
        const bool clicked = ctx.invisible_button(id, b, &hov, &held);
        const glm::vec2 c = b.center();
        const float r = b.w * 0.5f;
        const float t = ease_hover_(ctx, id + "_hover", hov);

        if (on) ctx.circle(c, r, held ? with_alpha(ctx.style.accent, 0.8f) : ctx.style.accent);
        else if (held) ctx.circle(c, r, ctx.style.button_active);
        else if (t > 0.01f) ctx.circle(c, r, with_alpha(ctx.style.hover_tint, ctx.style.hover_tint.a * t));

        if (t > 0.01f) ctx.ring(c, r + 2.0f, 1.5f, with_alpha(glass_.border, t));
        ctx.icon(ic, b.shrink(std::max(2.0f, b.h * 0.16f)), ctx.style.text);
        if (!tip.empty() && hov) ctx.tooltip(tip);
        return clicked;
    };

    const Box home_box{x, y, kDockButtonSize, kDockButtonSize};
    const std::string home_tip = minimised ? "Home\nShow the panels" : "Home\nMinimise to the scene";
    if (dock_button("qd_dock_home", Icon::Home, home_tip, minimised, home_box, true)) {
        minimised = !minimised;
    }
    y += kDockButtonSize + kDockGroupGap;

    // The partition between Home and the panel toggles: Home acts on the whole overlay, each
    // button below it on one panel.
    ctx.line({dock.x + 14.0f, y - kDockGroupGap * 0.5f}, {dock.right() - 14.0f, y - kDockGroupGap * 0.5f},
             glass_.border, 1.0f);

    for (int i = 0; i < kPanelButtons; ++i) {
        const PanelKind kind = kinds[i];
        const Box b{x, y, kDockButtonSize, kDockButtonSize};
        y += kDockButtonSize + kDockGap;

        const bool is_open = find_slot_(kind) >= 0;
        // Openable when it is already open (the click closes it) or a quadrant is free. A
        // minimised overlay is not full -- its panels are still assigned, just not shown.
        const bool enabled = is_open || first_free_slot_() >= 0;
        const std::string id = std::string("qd_dock_panel_") + std::to_string(i);

        // Disabled draws dimmed rather than silently doing nothing: every quadrant is taken,
        // so there is nowhere for this panel to open into.
        const std::string tip = std::string(panel_title_(kind)) +
                                (enabled ? (is_open ? "\nClose" : "\nOpen") : "\nClose a panel first");
        if (dock_button(id, panel_icon_(kind), tip, is_open, b, enabled)) {
            toggle_panel_(kind);
            // Reaching for a panel is a request to see it: opening one from a minimised overlay
            // restores the overlay rather than toggling a panel the user cannot see.
            if (minimised) minimised = false;
        }
    }
}

}  // namespace toy::ui
