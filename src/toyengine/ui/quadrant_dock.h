/**
 * @file quadrant_dock.h
 * @brief A movable four-quadrant panel workspace, glassmorphism-styled: up to four panels
 *        sharing the screen's quadrants (dragged between them, each growing into any adjacent
 *        quadrant left empty -- see quadrant_layout.h), with an always-visible dock on the
 *        right whose icons are one level deep -- a group opens its own set of panels and a
 *        Back button returns to the root set.
 */

#ifndef TOYENGINE_UI_QUADRANT_DOCK_H
#define TOYENGINE_UI_QUADRANT_DOCK_H

#include <array>
#include <string>
#include <vector>

#include <toyengine/ui/glass_theme.h>
#include <toyengine/ui/imm_screen.h>
#include <toyengine/ui/quadrant_layout.h>

namespace toy::ui {

/**
 * @class QuadrantDock
 * @brief Eleven panels across a two-level dock, any four of which can be on screen together,
 *        dragged between the four quadrants and closed individually.
 *
 * ## The dock is a hierarchy, one level deep
 *
 * A dock icon is either a PANEL (it toggles that panel into a free quadrant) or a GROUP (it
 * replaces the dock's icons with its own members, and a Back button returns). Groups do not
 * nest: a group's members are always panels.
 *
 * Each level owns its own arrangement of the four quadrants and keeps it: entering a group
 * puts the level you left away whole -- which panels, in which quadrants -- and restores
 * whatever that group had open last time. So the four quadrants are not one workspace shared
 * by eleven panels, they are a workspace PER level, which is what makes a group feel like a
 * place rather than a filter.
 *
 * | Level | Icons |
 * |---|---|
 * | root | Player, World, System (groups), Journal (a panel) |
 * | Player | Inventory, Character, Skills, Equipment |
 * | World | Map, Quests, Bestiary |
 * | System | Settings, Audio, Controls |
 *
 * Home is above the divider at every level and never changes: it minimises the whole overlay
 * to the dock (and restores it), leaving the level and every level's arrangement untouched.
 *
 * ## The blur behind it
 *
 * There is no painted backdrop to hide the world: `set_scene_blur()` asks the renderer to fade
 * the frame toward its blurred copy (ToyRenderConfig::ui_scene_blur) in step with the overlay's
 * own reveal, and the overlay adds only a soft tinted scrim over it. So "the interface is open"
 * reads as the world going soft behind real glass, and minimising hands the sharp frame back.
 *
 * @code{.yaml}
 * - type: QuadrantDock
 *   theme_ref: ui/themes/glass
 *   open_inventory: true
 *   open_character: true
 *   open_skills: true
 *   open_map: true        # not shown at start -- remembered for when World is entered
 * @endcode
 */
class QuadrantDock : public ImmScreen {
public:
    std::string type_name() const override { return "QuadrantDock"; }

    /**
     * @brief Which panels start open. Each seeds the arrangement of ITS OWN level, and the dock
     *        opens at the level the first one named here belongs to.
     *
     * So the defaults below start the dock inside the Player group showing three of its four
     * panels, with the World group already holding Map and Quests and the System group holding
     * Settings for when they are entered. A level takes at most four, in the order listed here.
     */
    bool open_inventory = true;
    bool open_character = true;
    bool open_skills    = true;
    bool open_equipment = false;
    bool open_map       = true;
    bool open_quests    = true;
    bool open_bestiary  = false;
    bool open_settings  = true;
    bool open_audio     = false;
    bool open_controls  = false;
    bool open_journal   = false;

    /** @brief Pins every transition (dock hover, the minimise reveal, a level change) to its target. */
    bool reduced_motion = false;

    /**
     * @brief Swaps every glass pane for GlassTheme::opaque_fill, and asks for no scene blur.
     *
     * The blur goes with the glass deliberately: behind an opaque scrim there is nothing of the
     * world left to see softened, so paying for the fade would buy an invisible effect.
     */
    bool reduced_transparency = false;

    /** @brief Whether the panel overlay is minimised, leaving only the dock over the camera view. */
    bool minimised = false;

    void draw(coopa::ui::imm::Context& ctx) override;

    /** @brief How far the panel overlay is revealed: 1 fully, 0 minimised, between while easing. */
    float reveal() const { return reveal_; }

    /** @brief How many of the four quadrants hold a panel at the CURRENT level (0..4). */
    int open_panel_count() const {
        int n = 0;
        for (auto k : slots_) {
            if (k != PanelKind::None) ++n;
        }
        return n;
    }

    /** @brief Which group the dock is inside, or -1 at the root level. */
    int level() const { return level_; }

    /**
     * @brief Shows a level's own dock icons and restores the arrangement it had last time;
     *        -1 is the root level.
     *
     * What the dock's group buttons and its Back button do, for a game that wants a keybind or
     * a story beat to take the player somewhere ("M opens the map") rather than making them
     * walk the dock. Out-of-range values are ignored, so a caller may pass a group id it got
     * from data without checking it first.
     */
    void show_level(int group);

    /** @brief The name of the level the dock is on ("Player", or "" at the root). */
    const char* level_name() const;

protected:
    void on_ready() override;
    void on_theme_loaded(const coopa::ui::imm::Theme& theme) override;

private:
    /** @brief Every panel the dock can open. Which group each belongs to is panel_group_(). */
    enum class PanelKind {
        None,
        Inventory, Character, Skills, Equipment,   // the Player group
        Map, Quests, Bestiary,                     // the World group
        Settings, Audio, Controls,                 // the System group
        Journal,                                   // a root-level panel, in no group
    };

    /** @brief The dock's groups. Values index draw_dock_()'s item tables and level_slots_. */
    enum class Group { Player = 0, World = 1, System = 2, Count = 3 };

    /** @brief One level's worth of quadrants: the root's, plus one per group. */
    static constexpr int kLevelCount = static_cast<int>(Group::Count) + 1;

    /**
     * @brief One button in the dock's column: a panel it toggles, or a group it opens.
     *
     * `group >= 0` is what makes it a group button; the two are deliberately one type, because
     * the dock draws, hovers and animates them identically and only the click differs. Defined
     * in quadrant_dock.cpp, beside the table of them.
     */
    struct DockItem;

    GlassTheme glass_;

    /**
     * @brief Eases toward `minimised ? 0 : 1`; the overlay's whole opacity and slide, and the
     *        scene blur asked of the renderer.
     *
     * The dock is deliberately NOT faded by it -- it is the only way back into the UI, so it
     * stays at full opacity over the camera view at every point in the transition.
     */
    float reveal_ = 1.0f;

    /** @brief Linear 0..1 progress behind reveal_; reveal_ is its smoothstep. */
    float reveal_t_ = 1.0f;

    /** @brief 0..1 progress of the dock's level change; the icons below Home fade and slide in on it. */
    float level_t_ = 1.0f;

    /** @brief Where the two dividers sit; dragged by the splitter handles between panels. */
    QuadrantSplit split_{};

    /** @brief Which panel (if any) occupies each Slot at the CURRENT level; see quadrant_layout.h. */
    std::array<PanelKind, 4> slots_{};

    /**
     * @brief Every level's arrangement, indexed by level_store_(): what entering a level
     *        restores and what leaving one saves.
     */
    std::array<std::array<PanelKind, 4>, kLevelCount> level_slots_{};

    /** @brief Which group the dock is inside, or -1 for the root level. */
    int level_ = -1;

    /** @brief A dragged panel's current offset from its slot's box; zeroed on release. */
    std::array<glm::vec2, 4> drag_offset_{};
    /** @brief Whether each slot's drag_handle() was held last frame, to detect release. */
    std::array<bool, 4> dragging_prev_{};

    // --- What the panels show. A demo's model, which is the panels' own business. ---
    int inventory_selected_ = -1;
    int quest_category_ = 0;
    int equipment_selected_ = -1;
    int bestiary_selected_ = 0;
    int map_zoom_ = 1;
    /** @brief Seconds left on each ability in the Skills panel; ticks down so the demo moves. */
    std::array<float, 5> skill_cooldown_{};
    std::array<float, 3> volumes_{0.8f, 0.55f, 0.7f};
    bool muted_ = false;
    /** @brief How strong a fully revealed overlay's scene blur is -- the Settings panel's slider. */
    float blur_strength_ = 1.0f;
    std::vector<std::string> journal_;

    void seed_levels_();
    int find_slot_(PanelKind k) const;
    int first_free_slot_() const;
    /** @brief Opens `k` into the first free slot, or closes it if already open. */
    void toggle_panel_(PanelKind k);
    /** @brief Saves this level's arrangement and switches to `next` (-1 = root), restoring its own. */
    void enter_level_(int next);

    void draw_backdrop_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& area);
    void draw_panel_(coopa::ui::imm::Context& ctx, int slot, const coopa::ui::imm::Box& box,
                     const coopa::ui::imm::Box& panel_area);
    void draw_panel_content_(coopa::ui::imm::Context& ctx, PanelKind kind, const coopa::ui::imm::Box& content);
    void draw_dock_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& dock, const coopa::ui::imm::Box& panel_area);
    void draw_splitters_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& area,
                         const std::array<bool, 4>& open);
    /** @brief The empty-workspace line, shown where a level has no panel open at all. */
    void draw_empty_hint_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& area);

    // --- One panel's contents each (quadrant_dock_panels.cpp) ---
    void draw_inventory_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_character_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_skills_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_equipment_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_map_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_quests_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_bestiary_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_journal_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_settings_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_audio_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);
    void draw_controls_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& c);

    float ease_hover_(coopa::ui::imm::Context& ctx, std::string_view id, bool hovered);

    /** @brief level_ (or a panel's group) as an index into level_slots_; the root is 0. */
    static int level_store_(int level) { return level + 1; }

    static const char* panel_title_(PanelKind k);
    static coopa::ui::imm::Icon panel_icon_(PanelKind k);
    /** @brief Which group owns `k`, or -1 for a root-level panel. */
    static int panel_group_(PanelKind k);
    static const char* group_title_(int group);
    static coopa::ui::imm::Icon group_icon_(int group);
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_QUADRANT_DOCK_H
