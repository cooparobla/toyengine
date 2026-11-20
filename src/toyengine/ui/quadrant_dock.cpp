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
using coopa::ui::imm::with_alpha;

using coopa::ui::imm::IconPen;
using coopa::ui::imm::register_custom_icon;

namespace {
constexpr float kDockWidth = 76.0f;
constexpr float kDockButtonSize = 44.0f;
constexpr float kDockGap = 10.0f;
/** @brief Extra space around a divider in the dock column. */
constexpr float kDockGroupGap = 20.0f;
/**
 * @brief Space around the divider under Back, which is wider than kDockGroupGap on purpose.
 *
 * Back is not one of the level's icons -- it LEAVES the level -- so it needs to read as its own
 * band rather than as the first entry of the list under it. At the same gap as Home's divider it
 * still grouped with the panels it sits above.
 */
constexpr float kDockBackGap = 32.0f;
/** @brief Grab width of a divider between two panels. */
constexpr float kSplitterGrab = 10.0f;
/** @brief How long the minimise/restore transition takes, end to end. */
constexpr float kRevealSeconds = 0.22f;
/** @brief How long the dock's icons take to fade and slide in after a level change. */
constexpr float kLevelSeconds = 0.18f;
/** @brief How far the overlay slides toward the dock as it minimises, in canvas units. */
constexpr float kRevealSlide = 70.0f;
/** @brief How far the dock's icon column slides as a new level's icons arrive. */
constexpr float kLevelSlide = 14.0f;
/** @brief Alpha of the tinted scrim over the blurred world, at full reveal. */
constexpr float kScrimAlpha = 0.10f;
/** @brief Alpha of the two soft colour blobs over that scrim. */
constexpr float kBlobAlpha = 0.18f;

// The panel icons the shared set has no business carrying -- a bag, a figure, a folded map, a
// quest scroll, a skill star -- are this game's vocabulary, not uicoopa's. Drawn on the same
// 16x16 grid as every built-in through IconPen, so they sit at the same weight beside Home,
// Gear, World and Keyboard.

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

void draw_skills_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // A node with three branches: the shape every ability tree uses to say "this unlocks that".
    d.ring(8.0f, 4.0f, 2.0f);
    d.ring(4.0f, 12.0f, 1.8f);
    d.ring(12.0f, 12.0f, 1.8f);
    d.l(8.0f, 6.0f, 4.0f, 10.2f);
    d.l(8.0f, 6.0f, 12.0f, 10.2f);
}

void draw_equipment_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // A shield: two shoulders, two sides tapering to a point.
    d.l(3.0f, 3.5f, 13.0f, 3.5f);
    d.l(3.0f, 3.5f, 3.5f, 9.0f);
    d.l(13.0f, 3.5f, 12.5f, 9.0f);
    d.l(3.5f, 9.0f, 8.0f, 13.5f);
    d.l(12.5f, 9.0f, 8.0f, 13.5f);
}

void draw_bestiary_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // A paw: a pad and four toes.
    d.circle(8.0f, 11.0f, 3.1f);
    d.circle(4.0f, 6.5f, 1.5f);
    d.circle(7.0f, 4.8f, 1.5f);
    d.circle(10.4f, 5.4f, 1.5f);
    d.circle(12.8f, 8.2f, 1.4f);
}

void draw_journal_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // An open book: a spine with a page falling away on each side.
    d.l(8.0f, 4.0f, 8.0f, 13.0f);
    d.l(8.0f, 4.0f, 2.5f, 5.2f);
    d.l(2.5f, 5.2f, 2.5f, 12.4f);
    d.l(2.5f, 12.4f, 8.0f, 13.0f);
    d.l(8.0f, 4.0f, 13.5f, 5.2f);
    d.l(13.5f, 5.2f, 13.5f, 12.4f);
    d.l(13.5f, 12.4f, 8.0f, 13.0f);
}

void draw_audio_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // A speaker cone and two waves leaving it.
    d.tri(3.0f, 6.0f, 3.0f, 10.0f, 7.5f, 13.0f);
    d.tri(3.0f, 6.0f, 7.5f, 13.0f, 7.5f, 3.0f);
    d.arc(7.5f, 8.0f, 3.2f, -1.2f, 1.2f);
    d.arc(7.5f, 8.0f, 5.4f, -1.1f, 1.1f);
}

void draw_sliders_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // Three tracks with a handle each, at different positions -- a settings sheet, not a gear,
    // so the panel does not wear the same icon as the group that holds it.
    d.l(2.5f, 4.5f, 13.5f, 4.5f);
    d.l(2.5f, 8.0f, 13.5f, 8.0f);
    d.l(2.5f, 11.5f, 13.5f, 11.5f);
    d.circle(5.5f, 4.5f, 1.7f);
    d.circle(10.5f, 8.0f, 1.7f);
    d.circle(7.0f, 11.5f, 1.7f);
}

void draw_player_group_icon(Context& ctx, const Box& b, const glm::vec4& c) {
    IconPen d(ctx, b, c);
    // The Character panel's bust inside a ring: the same subject, marked as a place that holds
    // more than one panel rather than as one panel.
    d.ring(8.0f, 8.0f, 7.0f);
    d.circle(8.0f, 6.0f, 1.9f);
    d.arc(8.0f, 13.5f, 3.6f, 3.14159f, 6.28318f);
}

/** @brief The registered ids, resolved once. register_custom_icon() is idempotent per name. */
struct PanelIconIds {
    Icon inventory = register_custom_icon("Inventory", &draw_inventory_icon);
    Icon character = register_custom_icon("Character", &draw_character_icon);
    Icon map        = register_custom_icon("Map", &draw_map_icon);
    Icon quests     = register_custom_icon("Quests", &draw_quests_icon);
    Icon skills     = register_custom_icon("Skills", &draw_skills_icon);
    Icon equipment  = register_custom_icon("Equipment", &draw_equipment_icon);
    Icon bestiary   = register_custom_icon("Bestiary", &draw_bestiary_icon);
    Icon journal    = register_custom_icon("Journal", &draw_journal_icon);
    Icon audio      = register_custom_icon("Audio", &draw_audio_icon);
    Icon sliders    = register_custom_icon("Sliders", &draw_sliders_icon);
    Icon player     = register_custom_icon("PlayerGroup", &draw_player_group_icon);
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
    seed_levels_();
    journal_ = {
        "Day 1 -- Left the harbour before dawn.",
        "Day 2 -- The pass is snowed in; took the ridge instead.",
        "Day 4 -- Found a shrine. Something is still living in it.",
    };
}

void QuadrantDock::seed_levels_() {
    for (auto& level : level_slots_) level.fill(PanelKind::None);

    // Declaration order is the authored order: the first panel asked for picks the level the
    // dock opens at, and each one lands in the first free quadrant of ITS OWN level, so a
    // request for a panel in another group is remembered there rather than dropped.
    const std::pair<bool, PanelKind> wanted[] = {
        {open_inventory, PanelKind::Inventory},
        {open_character, PanelKind::Character},
        {open_skills,    PanelKind::Skills},
        {open_equipment, PanelKind::Equipment},
        {open_map,       PanelKind::Map},
        {open_quests,    PanelKind::Quests},
        {open_bestiary,  PanelKind::Bestiary},
        {open_settings,  PanelKind::Settings},
        {open_audio,     PanelKind::Audio},
        {open_controls,  PanelKind::Controls},
        {open_journal,   PanelKind::Journal},
    };

    bool picked_level = false;
    for (const auto& [want, kind] : wanted) {
        if (!want) continue;
        const int group = panel_group_(kind);
        if (!picked_level) {
            level_ = group;
            picked_level = true;
        }
        std::array<PanelKind, 4>& store = level_slots_[static_cast<size_t>(level_store_(group))];
        for (PanelKind& s : store) {
            if (s != PanelKind::None) continue;
            s = kind;
            break;
        }
    }
    slots_ = level_slots_[static_cast<size_t>(level_store_(level_))];
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
        case PanelKind::Skills:    return "Skills";
        case PanelKind::Equipment: return "Equipment";
        case PanelKind::Map:       return "Map";
        case PanelKind::Quests:    return "Quests";
        case PanelKind::Bestiary:  return "Bestiary";
        case PanelKind::Settings:  return "Settings";
        case PanelKind::Audio:     return "Audio";
        case PanelKind::Controls:  return "Controls";
        case PanelKind::Journal:   return "Journal";
        case PanelKind::None:      return "";
    }
    return "";
}

Icon QuadrantDock::panel_icon_(PanelKind k) {
    const PanelIconIds& ids = panel_icon_ids();
    switch (k) {
        case PanelKind::Inventory: return ids.inventory;
        case PanelKind::Character: return ids.character;
        case PanelKind::Skills:    return ids.skills;
        case PanelKind::Equipment: return ids.equipment;
        case PanelKind::Map:       return ids.map;
        case PanelKind::Quests:    return ids.quests;
        case PanelKind::Bestiary:  return ids.bestiary;
        case PanelKind::Settings:  return ids.sliders;
        case PanelKind::Audio:     return ids.audio;
        case PanelKind::Controls:  return Icon::Keyboard;
        case PanelKind::Journal:   return ids.journal;
        case PanelKind::None:      return Icon::None;
    }
    return Icon::None;
}

int QuadrantDock::panel_group_(PanelKind k) {
    switch (k) {
        case PanelKind::Inventory:
        case PanelKind::Character:
        case PanelKind::Skills:
        case PanelKind::Equipment: return static_cast<int>(Group::Player);
        case PanelKind::Map:
        case PanelKind::Quests:
        case PanelKind::Bestiary:  return static_cast<int>(Group::World);
        case PanelKind::Settings:
        case PanelKind::Audio:
        case PanelKind::Controls:  return static_cast<int>(Group::System);
        case PanelKind::Journal:
        case PanelKind::None:      return -1;
    }
    return -1;
}

const char* QuadrantDock::group_title_(int group) {
    switch (static_cast<Group>(group)) {
        case Group::Player: return "Player";
        case Group::World:  return "World";
        case Group::System: return "System";
        case Group::Count:  break;
    }
    return "";
}

Icon QuadrantDock::group_icon_(int group) {
    switch (static_cast<Group>(group)) {
        case Group::Player: return panel_icon_ids().player;
        // The built-in globe: a world group's subject, and the one icon in the set that already
        // means "everything out there" rather than one document.
        case Group::World:  return Icon::World;
        case Group::System: return Icon::Gear;
        case Group::Count:  break;
    }
    return Icon::None;
}

const char* QuadrantDock::level_name() const {
    return level_ < 0 ? "" : group_title_(level_);
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

void QuadrantDock::show_level(int group) {
    if (group < -1 || group >= static_cast<int>(Group::Count)) return;
    enter_level_(group);
}

void QuadrantDock::enter_level_(int next) {
    if (next == level_) return;
    // Put the level being left away WHOLE -- which panels, in which quadrants -- so coming back
    // to it is coming back to the workspace, not to a list of panels in a different order.
    level_slots_[static_cast<size_t>(level_store_(level_))] = slots_;
    level_ = next;
    slots_ = level_slots_[static_cast<size_t>(level_store_(level_))];
    // A drag cannot survive the switch: the panel under the pointer is gone, and a stale offset
    // would be applied to whichever panel arrived in that quadrant.
    drag_offset_.fill(glm::vec2(0.0f));
    dragging_prev_.fill(false);
    level_t_ = reduced_motion ? 1.0f : 0.0f;
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
        level_t_ = 1.0f;
    } else {
        const float step = ctx.input().dt / kRevealSeconds;
        reveal_t_ = std::clamp(reveal_t_ + (minimised ? -step : step), 0.0f, 1.0f);
        level_t_ = std::clamp(level_t_ + ctx.input().dt / kLevelSeconds, 0.0f, 1.0f);
    }
    reveal_ = reveal_t_ * reveal_t_ * (3.0f - 2.0f * reveal_t_);   // smoothstep

    // What replaces the painted backdrop this screen used to hide the world behind: the
    // renderer fades the whole frame toward its blurred copy in step with the reveal, so the
    // world goes soft as the overlay arrives and comes back sharp as it leaves. Nothing to ask
    // for under reduced transparency -- the opaque panes leave none of it visible.
    set_scene_blur(reduced_transparency ? 0.0f : reveal_ * std::clamp(blur_strength_, 0.0f, 1.0f));

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
        // ONE fade for the whole overlay: scrim, panes, borders, titles and every content
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
        } else {
            draw_empty_hint_(ctx, anim_area);
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

    if (reduced_transparency) {
        // No blur was asked for, so there is nothing softened behind this: an honest opaque
        // sheet, the same fill the panes take.
        ctx.fill_rounded_aa(area, glass_.opaque_fill, 0.0f);
        return;
    }

    // A SCRIM, not a backdrop: the world behind it is already blurred by the renderer, and what
    // the overlay adds is the thin coloured film that lifts the panes off it -- the reference
    // CSS's point that glass needs something colourful behind it, at an alpha that tints the
    // world rather than replacing it.
    //
    // ONE bilinear quad, not two stacked halves: the diagonal blue -> violet -> pink ramp comes
    // from putting the end colours on opposite corners and their midpoint on the other two, and
    // a second quad under it can only agree along the shared edge at one point -- which drew a
    // hard horizontal line across the middle of the screen wherever no panel covered it.
    const glm::vec4 blue = with_alpha(glass_.grad_blue, kScrimAlpha);
    const glm::vec4 violet = with_alpha(glass_.grad_violet, kScrimAlpha);
    const glm::vec4 pink = with_alpha(glass_.grad_pink, kScrimAlpha);
    ctx.gradient(area, blue, violet, pink, violet);

    const float side = std::min(area.w, area.h) * 0.38f;
    const Box blob_a{area.x + area.w * 0.12f, area.y + area.h * 0.08f, side, side};
    const Box blob_b{area.x + area.w * 0.58f, area.y + area.h * 0.56f, side, side};
    ctx.shadow_soft(blob_a, side * 0.6f, with_alpha(glass_.blob_teal, kBlobAlpha), side * 0.5f);
    ctx.shadow_soft(blob_b, side * 0.6f, with_alpha(glass_.blob_pink, kBlobAlpha), side * 0.5f);
}

void QuadrantDock::draw_empty_hint_(Context& ctx, const Box& area) {
    // A level with nothing open is a legitimate state, not a bug to hide: the world is blurred
    // and this says whose dock the icons on the right belong to, which is the one thing the
    // blur alone cannot say.
    const std::string where = level_ < 0 ? std::string("Pick a panel from the dock")
                                         : std::string(level_name()) + " -- pick a panel from the dock";
    const float w = ctx.text_width(where);
    const glm::vec2 at{area.center().x - w * 0.5f, area.center().y - ctx.style.font_size};
    ctx.draw_text({at.x + 1.0f, at.y + 1.0f}, where, glass_.text_shadow);
    ctx.draw_text(at, where, with_alpha(ctx.style.text, 0.75f));
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
    // The group as a dim breadcrumb before the title: a panel dragged into a quadrant says
    // which level it belongs to without the dock having to be read.
    float tx = drag_area.x + row_h + 4.0f;
    const int group = panel_group_(kind);
    if (group >= 0) {
        const std::string crumb = std::string(group_title_(group)) + " / ";
        ctx.draw_text({tx, drag_area.y + 2.0f}, crumb, ctx.style.text_dim);
        tx += ctx.text_width(crumb);
    }
    ctx.draw_text({tx, drag_area.y + 2.0f}, panel_title_(kind), ctx.style.text);

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

/** @brief See the declaration in quadrant_dock.h; the table of them is in draw_dock_() below. */
struct QuadrantDock::DockItem {
    PanelKind panel = PanelKind::None;
    int group = -1;
    bool is_group() const { return group >= 0; }
};

void QuadrantDock::draw_dock_(Context& ctx, const Box& dock, const Box& panel_area) {
    // Never faded by reveal_: the dock is the only way back into a minimised UI, so it stays at
    // full opacity over the camera view at every point in the transition.
    if (reduced_transparency) {
        ctx.fill_rounded_aa(dock, glass_.opaque_fill, 0.0f);
    } else {
        ctx.glass(dock, glm::vec4(1.0f), 0.0f);
    }
    ctx.line({panel_area.right(), panel_area.y}, {panel_area.right(), panel_area.bottom()}, glass_.border, 1.0f);

    // This level's icons: the root's mixed set, or one group's panels. The ONE place the
    // hierarchy is written down -- everything below is level-agnostic.
    static const std::vector<DockItem> kRootItems = {
        {PanelKind::None, static_cast<int>(Group::Player)},
        {PanelKind::None, static_cast<int>(Group::World)},
        {PanelKind::None, static_cast<int>(Group::System)},
        {PanelKind::Journal, -1},
    };
    static const std::vector<DockItem> kGroupItems[static_cast<int>(Group::Count)] = {
        {{PanelKind::Inventory, -1}, {PanelKind::Character, -1}, {PanelKind::Skills, -1}, {PanelKind::Equipment, -1}},
        {{PanelKind::Map, -1}, {PanelKind::Quests, -1}, {PanelKind::Bestiary, -1}},
        {{PanelKind::Settings, -1}, {PanelKind::Audio, -1}, {PanelKind::Controls, -1}},
    };
    const std::vector<DockItem>& items = level_ < 0 ? kRootItems : kGroupItems[level_];
    const bool has_back = level_ >= 0;
    const int count = static_cast<int>(items.size());

    // Home, a gap, (Back, a gap,) then one button per item -- the whole column centred in the
    // dock's height rather than hung from its top, so it stays centred at any window size.
    const float total_h = kDockButtonSize * static_cast<float>(count + 1 + (has_back ? 1 : 0)) +
                          kDockGap * static_cast<float>(count - 1) +
                          kDockGroupGap + (has_back ? kDockBackGap : 0.0f);
    const float x = dock.x + (dock.w - kDockButtonSize) * 0.5f;
    float y = dock.y + (dock.h - total_h) * 0.5f;

    // Every dock affordance is a CIRCLE: the hover ring, the selected disc and the pressed
    // state alike. imm's icon_button() paints its own rounded-SQUARE for `on` and for hover,
    // which read as a different shape language from the round ring around them, so the dock
    // draws its own and uses invisible_button() purely for the behaviour and the tooltip.
    auto dock_button = [&](const std::string& id, Icon ic, const std::string& tip, bool on,
                           const Box& b, bool enabled, bool group_badge, Icon second = Icon::None) {
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
        if (second == Icon::None) {
            ctx.icon(ic, b.shrink(std::max(2.0f, b.h * 0.16f)), ctx.style.text);
        } else {
            // Two icons side by side, each on its own half: "back" plus WHAT you are backing out
            // of. One 44px button says both, which neither a tooltip nor a separate marker does
            // without being hunted for.
            const float side = b.h * 0.46f;
            const float cy = c.y - side * 0.5f;
            ctx.icon(ic, {b.x + b.w * 0.10f, cy, side, side}, ctx.style.text);
            ctx.icon(second, {b.x + b.w * 0.48f, cy, side, side}, with_alpha(ctx.style.text, 0.7f));
        }
        // A group's badge: a small chevron in the corner, the one mark that separates "this
        // opens a panel" from "this opens more icons" at a glance.
        if (group_badge) {
            // On the circle's lower-right diagonal, clear of the icon rather than over it: at
            // this size a mark that touches the glyph reads as part of it.
            const glm::vec2 p = c + glm::vec2(r * 0.68f, r * 0.68f);
            const glm::vec4 col = with_alpha(ctx.style.text, 0.85f);
            ctx.line({p.x - 3.5f, p.y - 4.0f}, {p.x + 0.5f, p.y}, col, 1.6f);
            ctx.line({p.x + 0.5f, p.y}, {p.x - 3.5f, p.y + 4.0f}, col, 1.6f);
        }
        if (!tip.empty() && hov) ctx.tooltip(tip);
        return clicked;
    };

    const Box home_box{x, y, kDockButtonSize, kDockButtonSize};
    const std::string home_tip = minimised ? "Home\nShow the panels" : "Home\nMinimise to the scene";
    if (dock_button("qd_dock_home", Icon::Home, home_tip, minimised, home_box, true, false)) {
        minimised = !minimised;
    }
    y += kDockButtonSize + kDockGroupGap;

    // The partition between Home and everything below it: Home acts on the whole overlay, each
    // button below it on one panel or one group.
    ctx.line({dock.x + 14.0f, y - kDockGroupGap * 0.5f}, {dock.right() - 14.0f, y - kDockGroupGap * 0.5f},
             glass_.border, 1.0f);

    // Everything below the partition belongs to the level, so it is what fades and slides when
    // the level changes -- Home does not move, because it means the same thing at every level.
    ctx.push_alpha(level_t_);
    const float level_dx = (1.0f - level_t_) * kLevelSlide;

    if (has_back) {
        const Box back_box{x + level_dx, y, kDockButtonSize, kDockButtonSize};
        if (dock_button("qd_dock_back", Icon::ArrowLeft,
                        std::string("Back\nLeave ") + level_name(), false, back_box, true, false,
                        group_icon_(level_))) {
            enter_level_(-1);
        }
        y += kDockButtonSize + kDockBackGap;
        // A full-strength rule at the same inset as Home's, not a fainter one: these are the two
        // partitions of the column (leave the overlay / leave the level), and a half-drawn second
        // one reads as a hairline inside the list rather than as a boundary of it.
        ctx.line({dock.x + 14.0f, y - kDockBackGap * 0.5f}, {dock.right() - 14.0f, y - kDockBackGap * 0.5f},
                 glass_.border, 1.0f);
    }

    for (int i = 0; i < count; ++i) {
        const DockItem& item = items[static_cast<size_t>(i)];
        const Box b{x + level_dx, y, kDockButtonSize, kDockButtonSize};
        y += kDockButtonSize + kDockGap;
        // The LEVEL is part of the id: two levels' icons share a position in the column, and an
        // id that did not distinguish them would hand the new button the old one's hover ease
        // (and its held state) on the frame the level changed.
        const std::string id = "qd_dock_item_" + std::to_string(level_) + "_" + std::to_string(i);

        if (item.is_group()) {
            // How many of this group's quadrants are filled, so the dock says what is waiting
            // inside it before it is entered.
            const std::array<PanelKind, 4>& store = level_slots_[static_cast<size_t>(level_store_(item.group))];
            int held_count = 0;
            for (PanelKind k : store) {
                if (k != PanelKind::None) ++held_count;
            }
            const std::string tip = std::string(group_title_(item.group)) + "\n" +
                                    (held_count > 0 ? std::to_string(held_count) + " panel" +
                                                          (held_count == 1 ? "" : "s") + " open inside"
                                                    : "Open its panels");
            if (dock_button(id, group_icon_(item.group), tip, false, b, true, true)) {
                enter_level_(item.group);
                // Reaching into a group is a request to see it, so it restores a minimised
                // overlay rather than switching a level the user cannot see.
                if (minimised) minimised = false;
            }
            continue;
        }

        const bool is_open = find_slot_(item.panel) >= 0;
        // Openable when it is already open (the click closes it) or a quadrant is free. A
        // minimised overlay is not full -- its panels are still assigned, just not shown.
        const bool enabled = is_open || first_free_slot_() >= 0;
        // Disabled draws dimmed rather than silently doing nothing: every quadrant of this
        // level is taken, so there is nowhere for this panel to open into.
        const std::string tip = std::string(panel_title_(item.panel)) +
                                (enabled ? (is_open ? "\nClose" : "\nOpen") : "\nClose a panel first");
        if (dock_button(id, panel_icon_(item.panel), tip, is_open, b, enabled, false)) {
            toggle_panel_(item.panel);
            // Reaching for a panel is a request to see it: opening one from a minimised overlay
            // restores the overlay rather than toggling a panel the user cannot see.
            if (minimised) minimised = false;
        }
    }
    ctx.pop_alpha();
}

}  // namespace toy::ui
