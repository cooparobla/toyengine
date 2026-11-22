/**
 * @file quadrant_dock_panels.cpp
 * @brief What is INSIDE each of QuadrantDock's eleven panels.
 *
 * Split from quadrant_dock.cpp, which is about the workspace: the quadrants, the dock column,
 * the levels and the transitions. Nothing here knows about any of that -- every function takes
 * the content box its pane has already worked out and declares one panel's widgets into it, so
 * a panel can be read, changed or added without going near the layout.
 *
 * The data is a demo's: fixed item lists, fixed quests, three creatures. Everything a real game
 * would read from its own model is a local `static const` here, which is also what keeps these
 * panels honest about the one thing they are for -- showing that the widget set (imm_game.h) is
 * enough to build an interface of this size.
 */

#include <toyengine/ui/quadrant_dock.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <toyengine/render/sub_scene.h>

namespace toy::ui {

using coopa::ui::imm::Box;
using coopa::ui::imm::Context;
using coopa::ui::imm::Icon;
using coopa::ui::imm::ItemRow;
using coopa::ui::imm::SlotInfo;
using coopa::ui::imm::SlotStyle;
using coopa::ui::imm::with_alpha;

namespace {
/** @brief One stat bar's height inside the Character panel's scrolling region. */
constexpr float kStatBarHeight = 46.0f;
/** @brief Gap between the two columns a wide panel splits into. */
constexpr float kColumnGap = 8.0f;

/** @brief `value` with one decimal place -- the one number format these panels need. */
std::string one_dp(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(v));
    return buf;
}

/** @brief The top `height` of `c` (or all of it, if shorter) -- a card that fits its contents. */
Box card_top(const Box& c, float height) {
    return Box{c.x, c.y, c.w, std::min(c.h, height)};
}

/** @brief Splits `c` into a left column of `left_fraction` and a right one, with a gap between. */
std::pair<Box, Box> columns(const Box& c, float left_fraction) {
    const float lw = std::max(0.0f, c.w * left_fraction - kColumnGap * 0.5f);
    const Box left{c.x, c.y, lw, c.h};
    const Box right{left.right() + kColumnGap, c.y, std::max(0.0f, c.w - lw - kColumnGap), c.h};
    return {left, right};
}
}  // namespace

void QuadrantDock::draw_panel_content_(Context& ctx, PanelKind kind, const Box& content) {
    if (content.empty()) return;
    switch (kind) {
        case PanelKind::Inventory: draw_inventory_(ctx, content); break;
        case PanelKind::Character: draw_character_(ctx, content); break;
        case PanelKind::Skills:    draw_skills_(ctx, content);    break;
        case PanelKind::Equipment: draw_equipment_(ctx, content); break;
        case PanelKind::Map:       draw_map_(ctx, content);       break;
        case PanelKind::Quests:    draw_quests_(ctx, content);    break;
        case PanelKind::Bestiary:  draw_bestiary_(ctx, content);  break;
        case PanelKind::Journal:   draw_journal_(ctx, content);   break;
        case PanelKind::Settings:  draw_settings_(ctx, content);  break;
        case PanelKind::Audio:     draw_audio_(ctx, content);     break;
        case PanelKind::Controls:  draw_controls_(ctx, content);   break;
        case PanelKind::None:      break;
    }
}

void QuadrantDock::draw_inventory_(Context& ctx, const Box& c) {
    // tint is the rarity tier: the one piece of per-row styling the widget takes, and the
    // reason a list of eight items still reads as sorted by worth. Light tints, not saturated
    // ones: these sit on a frosted pane over the world, where a
    // mid-blue or mid-gold row goes unreadable against the backdrop behind it.
    static const glm::vec4 kCommon{1.0f, 1.0f, 1.0f, 1.0f};
    static const glm::vec4 kFine{0.72f, 0.90f, 1.0f, 1.0f};
    static const glm::vec4 kRare{1.0f, 0.90f, 0.55f, 1.0f};
    static const std::vector<ItemRow> kItems = {
        {"Health Potion", 3, Icon::None, kCommon, 0.5f, 25, false, false, 0},
        {"Iron Sword", 1, Icon::None, kCommon, 3.0f, 120, true, false, 1},
        {"Leather Boots", 1, Icon::None, kCommon, 1.5f, 40, false, true, 2},
        {"Hunting Bow", 1, Icon::None, kFine, 2.2f, 190, false, false, 3},
        {"Lockpick", 7, Icon::None, kCommon, 0.1f, 8, false, false, 4},
        {"Ancient Coin", 5, Icon::None, kRare, 0.1f, 15, false, false, 5},
        {"Shrine Key", 1, Icon::None, kRare, 0.2f, 0, false, true, 6},
        {"Dried Rations", 12, Icon::None, kCommon, 0.3f, 4, false, false, 7},
    };
    static const std::vector<std::string> kDescriptions = {
        "Three swallows and the bleeding stops.",
        "Notched, re-ground twice, still true.",
        "Soft soles. Quiet on stone, useless in snow.",
        "Draws heavy. Worth it at distance.",
        "Bent wire, patience, and no questions.",
        "Minted by a kingdom nobody can name.",
        "Warm to the touch, and it should not be.",
        "Food, technically.",
    };

    // The capacity bar is the bottom strip, so the list keeps the rest; a bag with no weight
    // budget shown is a list of names, and the budget is the whole decision.
    const float bar_h = 22.0f;
    const Box upper{c.x, c.y, c.w, std::max(0.0f, c.h - bar_h - 6.0f)};
    const auto [list, detail_column] = columns(upper, 0.6f);
    // A card the height of what it holds, not of its column: item_detail() fills whatever box
    // it is given, and a quadrant-high one is a black slab with a line of text at the top.
    const Box detail = card_top(detail_column, 150.0f);

    coopa::ui::imm::ItemListColumns cols;
    cols.weight = true;
    cols.value = true;
    ctx.item_list("qd_inv_list", list, kItems, &inventory_selected_, cols);
    if (inventory_selected_ >= 0 && inventory_selected_ < static_cast<int>(kItems.size())) {
        const size_t i = static_cast<size_t>(inventory_selected_);
        const ItemRow& r = kItems[i];
        ctx.item_detail(detail, r.label, kDescriptions[i],
                        {{"Weight", one_dp(r.weight)},
                         {"Value", std::to_string(r.value)},
                         {"Carried", std::to_string(r.count)}});
    } else {
        ctx.item_detail(detail, "Nothing selected", "Pick a line on the left.", {});
    }

    float carried = 0.0f;
    for (const ItemRow& r : kItems) carried += r.weight * static_cast<float>(r.count);
    const Box bar{c.x, c.bottom() - bar_h, c.w, bar_h};
    ctx.stat_bar("qd_inv_weight", bar, "Carried", carried, 60.0f,
                 bar_style(theme(), carried > 48.0f ? theme().feedback.damage : theme().bars.stamina));
}

void QuadrantDock::draw_character_(Context& ctx, const Box& c) {
    // A fixed bar height inside a scrolling region, rather than five bars squeezed to fit: in a
    // quarter-screen quadrant the squeezed version is unreadable, and this is what puts a
    // scrollbar there when the panel is small and none when it is large.
    ctx.begin_region("qd_char", c, true);
    ctx.heading("Tollen of Anhalt");
    ctx.label_dim("Level 12 Ranger");
    ctx.separator();
    auto bar = [&](const char* id, const char* label, float v, float mx, const glm::vec4& fill) {
        ctx.stat_bar(id, ctx.next_box(kStatBarHeight), label, v, mx, bar_style(theme(), fill));
    };
    bar("qd_hp", "Health", 82.0f, 100.0f, theme().bars.health);
    bar("qd_mp", "Mana", 46.0f, 100.0f, theme().bars.mana);
    bar("qd_sp", "Stamina", 70.0f, 100.0f, theme().bars.stamina);
    bar("qd_sh", "Shield", 25.0f, 100.0f, theme().bars.shield);
    bar("qd_xp", "Experience", 310.0f, 1000.0f, theme().bars.experience);
    ctx.separator();
    // Attributes as property rows: the same two-column alignment the inspector uses, which is
    // what keeps a wall of numbers scannable.
    static const std::pair<const char*, const char*> kAttributes[] = {
        {"Strength", "14"}, {"Agility", "18"}, {"Endurance", "12"},
        {"Insight", "11"},  {"Resolve", "15"},
    };
    for (const auto& [name, value] : kAttributes) {
        const Box v = ctx.property_row(name);
        ctx.text_in(v, value, ctx.style.text, 4.0f);
    }
    ctx.end_region();
}

void QuadrantDock::draw_skills_(Context& ctx, const Box& c) {
    static const std::array<const char*, 5> kAbilities = {"Volley", "Mark", "Snare", "Mend", "Vanish"};
    static const std::array<float, 5> kRecharge = {6.0f, 9.0f, 14.0f, 20.0f, 30.0f};

    // The cooldowns run: an ability clicked wipes over and recharges, which is the one thing a
    // still screenshot of a hotbar cannot show and the reason this panel moves at all.
    const float dt = ctx.input().dt;
    for (float& cd : skill_cooldown_) cd = std::max(0.0f, cd - dt);

    const float bar_h = std::min(64.0f, std::max(36.0f, c.h * 0.3f));
    const Box hot{c.x, c.y, c.w, bar_h};
    static const std::vector<std::string> kKeys = {"1", "2", "3", "4", "5"};
    ctx.hotbar("qd_skill_bar", hot, static_cast<int>(kAbilities.size()), kKeys,
               [&](Context& k, const SlotInfo& info) {
        const size_t i = static_cast<size_t>(info.index);
        if (i >= kAbilities.size()) return;
        k.icon(Icon::Play, info.box.shrink(std::max(4.0f, info.box.h * 0.26f)),
               skill_cooldown_[i] > 0.0f ? with_alpha(k.style.text, 0.4f) : k.style.text);
        if (skill_cooldown_[i] > 0.0f) k.cooldown(info.box, skill_cooldown_[i] / kRecharge[i]);
        if (info.pressed && skill_cooldown_[i] <= 0.0f) skill_cooldown_[i] = kRecharge[i];
        if (info.hovered) {
            k.tooltip(kAbilities[i], kKeys[i],
                      skill_cooldown_[i] > 0.0f ? one_dp(skill_cooldown_[i]) + "s left"
                                                : "Ready -- click to use");
        }
    }, slot_style(theme()));

    const Box rest{c.x, hot.bottom() + kColumnGap, c.w, std::max(0.0f, c.bottom() - hot.bottom() - kColumnGap)};
    if (rest.empty()) return;
    ctx.begin_region("qd_skills", rest, true);
    ctx.subhead("Mastery");
    static const std::pair<const char*, float> kTracks[] = {
        {"Archery", 0.78f}, {"Tracking", 0.55f}, {"Herbalism", 0.3f}, {"Stealth", 0.62f},
    };
    for (const auto& [name, t] : kTracks) {
        ctx.label(name);
        ctx.progress(t);
    }
    ctx.end_region();
}

void QuadrantDock::draw_equipment_(Context& ctx, const Box& c) {
    // Twelve worn slots, named rather than numbered: a grid of unlabelled cells is a puzzle,
    // and the name is also what the detail column keys off.
    static const std::array<const char*, 12> kSlots = {
        "Head", "Cloak", "Amulet", "Chest", "Gloves", "Belt",
        "Legs", "Boots", "Ring", "Ring", "Main Hand", "Off Hand",
    };
    static const std::array<const char*, 12> kWorn = {
        "", "Ranger's Cloak", "", "Studded Leather", "Fingerless Wraps", "Tool Belt",
        "Patched Breeches", "Leather Boots", "Copper Band", "", "Iron Sword", "",
    };

    const auto [grid, detail_column] = columns(c, 0.62f);
    const Box detail = card_top(detail_column, 140.0f);
    ctx.slot_grid("qd_equip", grid, 3, 4, [&](Context& k, const SlotInfo& info) {
        const size_t i = static_cast<size_t>(info.index);
        if (i >= kSlots.size()) return;
        const bool worn = kWorn[i][0] != '\0';
        if (static_cast<int>(i) == equipment_selected_) {
            k.outline_rounded_aa(info.box, theme().slots.selected, k.style.rounding, 1.5f);
        }
        if (worn) {
            k.icon(Icon::Package, info.box.shrink(std::max(3.0f, info.box.h * 0.28f)), k.style.text);
        } else {
            // An empty slot says WHICH slot it is; a filled one shows the item and says so in
            // its tooltip, because the name rarely fits a cell this size.
            k.text_in(info.box, kSlots[i], with_alpha(k.style.text, 0.35f), 2.0f, true);
        }
        if (info.pressed) equipment_selected_ = static_cast<int>(i);
        if (info.hovered) k.tooltip(kSlots[i], "", worn ? kWorn[i] : "Empty");
    }, slot_style(theme()));

    if (equipment_selected_ >= 0 && equipment_selected_ < static_cast<int>(kSlots.size())) {
        const size_t i = static_cast<size_t>(equipment_selected_);
        const bool worn = kWorn[i][0] != '\0';
        ctx.item_detail(detail, kSlots[i], worn ? kWorn[i] : "Nothing worn here.",
                        {{"Armour", worn ? "12" : "0"}, {"Condition", worn ? "Fair" : "--"}});
    } else {
        ctx.item_detail(detail, "Worn gear", "Pick a slot to see what is in it.",
                        {{"Armour", "143"}, {"Weight", "18.4"}});
    }
}

void QuadrantDock::draw_map_(Context& ctx, const Box& c) {
    static const std::vector<std::string> kZooms = {"Region", "Local", "Close"};
    const float row_h = ctx.style.row_height;
    const Box zoom_row{c.x, c.y, c.w, row_h};
    const Box body{c.x, zoom_row.bottom() + 6.0f, c.w, std::max(0.0f, c.bottom() - zoom_row.bottom() - 6.0f)};
    ctx.segmented_in(ctx.get_id("qd_map_zoom"), zoom_row, &map_zoom_, kZooms);
    if (body.empty()) return;

    // The world behind the map: a SubScene on this scene renders map_world_demo into a texture
    // every frame, and the three zoom segments drive THAT camera's orthographic half-height.
    // Metres of world across the view, per zoom step -- Region, Local, Close.
    static constexpr float kZoomSizes[] = {90.0f, 40.0f, 16.0f};
    render::SubScene* sub = nullptr;
    if (scene != nullptr) {
        for (render::SubScene* s : scene->get_components<render::SubScene>()) {
            if (s->view_name == "map" || s->view_name.empty()) { sub = s; break; }
        }
    }
    const int zoom = std::clamp(map_zoom_, 0, static_cast<int>(std::size(kZoomSizes)) - 1);
    if (sub != nullptr) sub->ortho_size = kZoomSizes[zoom];

    ctx.push_clip(body);
    if (sub != nullptr && sub->ready && sub->texture.valid()) {
        ctx.image(sub->texture, body);
        // Where this view ended up, in WINDOW pixels -- the space coopa::input::Input speaks,
        // which is what lets the engine remap the cursor into the view when it is hovered.
        // A screen lays out in canvas units, hence the scale.
        const float s = canvas_scale();
        sub->set_panel_rect(glm::vec4(body.x * s, body.y * s, body.w * s, body.h * s),
                            ctx.is_hovered(body));
    } else {
        // No sub-scene (or it has not finished loading): the grid still reads as a map, and
        // tightening it with the zoom keeps the three segments from being a dead control.
        const int cells = 4 + zoom * 3;
        const glm::vec4 grid_col = with_alpha(ctx.style.text, 0.13f);
        for (int i = 1; i < cells; ++i) {
            const float f = static_cast<float>(i) / static_cast<float>(cells);
            ctx.line({body.x + body.w * f, body.y}, {body.x + body.w * f, body.bottom()}, grid_col);
            ctx.line({body.x, body.y + body.h * f}, {body.right(), body.y + body.h * f}, grid_col);
        }
    }

    // Points of interest at fractions of the box, so they stay put at any panel size.
    static const struct { float x, y; const char* label; int kind; } kPois[] = {
        {0.22f, 0.30f, "Harbour", 0},
        {0.63f, 0.21f, "The Pass", 1},
        {0.78f, 0.68f, "Shrine", 2},
        {0.40f, 0.80f, "Camp", 0},
    };
    for (const auto& poi : kPois) {
        const glm::vec2 at{body.x + body.w * poi.x, body.y + body.h * poi.y};
        const glm::vec4 col = poi.kind == 0 ? theme().feedback.notice
                            : poi.kind == 1 ? theme().feedback.critical
                                            : theme().feedback.healing;
        ctx.circle(at, 4.0f, col);
        ctx.ring(at, 7.0f, 1.0f, with_alpha(col, 0.5f));
        ctx.draw_text({at.x + 10.0f, at.y - ctx.style.font_size * 0.5f}, poi.label,
                      with_alpha(ctx.style.text, 0.8f));
    }

    // The player, turning slowly: a map with a heading on it reads as live, and the turn is the
    // cheapest possible proof that this panel is drawn fresh every frame.
    const float heading = static_cast<float>(ctx.time()) * 0.6f;
    const glm::vec2 me = body.center();
    const glm::vec2 fwd{std::cos(heading), std::sin(heading)};
    const glm::vec2 side{-fwd.y, fwd.x};
    ctx.triangle(me + fwd * 9.0f, me - fwd * 5.0f + side * 6.0f, me - fwd * 5.0f - side * 6.0f,
                 ctx.style.accent);
    ctx.pop_clip();
}

void QuadrantDock::draw_quests_(Context& ctx, const Box& c) {
    static const std::vector<std::string> kCats = {"Active", "Completed"};
    static const std::vector<std::string> kActive = {
        "Find the lost amulet",
        "Defeat the bandit camp",
        "Map the ridge road",
    };
    static const std::vector<std::string> kDone = {"Deliver the letter", "Escort the surveyor"};

    const auto [cats, right] = columns(c, 0.35f);
    ctx.category_list("qd_quest_cats", cats, kCats, &quest_category_);

    // The objective bar under the log, not inside it: a log scrolls and an objective does not,
    // so stacking them keeps the newest line and the current goal both visible at once.
    const bool active = quest_category_ == 0;
    const float bar_h = active ? 22.0f : 0.0f;
    const Box log{right.x, right.y, right.w, std::max(0.0f, right.h - bar_h - (active ? 6.0f : 0.0f))};
    ctx.message_log(log, active ? kActive : kDone, nullptr, false);
    if (!active) return;
    ctx.stat_bar("qd_quest_obj", {right.x, right.bottom() - bar_h, right.w, bar_h},
                 "Bandits", 4.0f, 7.0f, bar_style(theme(), theme().feedback.critical));
}

void QuadrantDock::draw_bestiary_(Context& ctx, const Box& c) {
    static const std::vector<std::string> kNames = {"Ridge Wolf", "Bog Lurker", "Shrine Warden"};
    static const std::array<const char*, 3> kNotes = {
        "Hunts in threes. Breaks off if two go down.",
        "Waits under the water for something to step wrong.",
        "Has not moved in a century. Still warm.",
    };
    static const std::array<std::array<const char*, 3>, 3> kStats = {{
        {{"42", "Fast", "Cold"}},
        {{"88", "Slow", "Fire"}},
        {{"310", "None", "Unknown"}},
    }};

    const auto [list, detail_column] = columns(c, 0.44f);
    const Box detail = card_top(detail_column, 150.0f);
    ctx.category_list("qd_bestiary", list, kNames, &bestiary_selected_);
    const size_t i = static_cast<size_t>(std::clamp(bestiary_selected_, 0, static_cast<int>(kNames.size()) - 1));
    ctx.item_detail(detail, kNames[i], kNotes[i],
                    {{"Health", kStats[i][0]}, {"Speed", kStats[i][1]}, {"Weak to", kStats[i][2]}});
}

void QuadrantDock::draw_journal_(Context& ctx, const Box& c) {
    // The one panel that WRITES: a dock level is a place you come back to, and a journal that
    // kept nothing from the last visit would not prove that.
    const float row_h = ctx.style.row_height;
    const Box log{c.x, c.y, c.w, std::max(0.0f, c.h - row_h - 6.0f)};
    const Box row{c.x, c.bottom() - row_h, c.w, row_h};
    // A scrolling region rather than message_log(): a log pins its newest line to the BOTTOM of
    // its box, which is right for combat chatter and wrong for a journal -- in a panel grown to
    // the whole workspace it leaves the entries stranded along the bottom edge.
    ctx.begin_region("qd_journal", log, true);
    for (const std::string& entry : journal_) ctx.paragraph(entry);
    if (journal_.empty()) ctx.label_dim("No entries yet.");
    ctx.end_region();

    const float bw = std::min(row.w * 0.5f, 120.0f);
    if (ctx.button("Add entry", {row.x, row.y, bw, row_h})) {
        journal_.push_back("Day " + std::to_string(journal_.size() + 2) + " -- Nothing worth the ink.");
    }
    if (ctx.button("Clear", {row.x + bw + kColumnGap, row.y, bw * 0.6f, row_h}, !journal_.empty())) {
        journal_.clear();
    }
}

void QuadrantDock::draw_settings_(Context& ctx, const Box& c) {
    ctx.begin_region("qd_settings", c, true);
    ctx.subhead("Appearance");
    ctx.checkbox("Reduced motion", &reduced_motion);
    ctx.checkbox("Reduced transparency", &reduced_transparency);
    // The renderer's side of this screen, from inside the screen: the slider scales what
    // draw() asks for every frame (ToyRenderConfig::ui_scene_blur), so dragging it fades the
    // world between sharp and soft while the panel it is in stays put.
    ctx.slider_float("World blur", &blur_strength_, 0.0f, 1.0f, "%.2f");
    if (reduced_transparency) ctx.label_dim("Opaque panels: the blur is off");
    ctx.separator();
    ctx.subhead("Workspace");
    ctx.label_dim("Drag a panel's title bar to move it between quadrants.");
    if (ctx.button("Reset the dividers")) split_ = QuadrantSplit{};
    ctx.end_region();
}

void QuadrantDock::draw_audio_(Context& ctx, const Box& c) {
    static const std::array<const char*, 3> kBuses = {"Master", "Music", "Effects"};
    ctx.begin_region("qd_audio", c, true);
    ctx.subhead("Buses");
    for (size_t i = 0; i < kBuses.size(); ++i) {
        // Muted dims the sliders rather than hiding them: a mute that erases the mix is a mute
        // nobody trusts.
        float shown = volumes_[i];
        if (ctx.slider_float(kBuses[i], &shown, 0.0f, 1.0f, "%.2f") && !muted_) volumes_[i] = shown;
    }
    ctx.checkbox("Mute everything", &muted_);
    ctx.separator();
    ctx.subhead("Output");
    ctx.label_dim(muted_ ? "Silent" : "Stereo -- 48 kHz");
    ctx.end_region();
}

void QuadrantDock::draw_controls_(Context& ctx, const Box& c) {
    static const std::pair<const char*, const char*> kBindings[] = {
        {"Move", "W A S D"}, {"Sprint", "Shift"}, {"Jump", "Space"}, {"Interact", "E"},
        {"Attack", "LMB"},   {"Aim", "RMB"},      {"Inventory", "I"}, {"Map", "M"},
        {"Journal", "J"},    {"Minimise UI", "Esc"},
    };
    ctx.begin_region("qd_controls", c, true);
    ctx.subhead("Bindings");
    for (const auto& [action, key] : kBindings) {
        const Box v = ctx.property_row(action);
        // The key as a chip, not as text: it is a thing you press, and the chip is the shape
        // this widget set already uses for one.
        const float w = std::min(v.w, ctx.chip_width(key));
        ctx.chip_in({v.x, v.y, w, v.h}, key, ctx.style.button);
    }
    ctx.end_region();
}

}  // namespace toy::ui
