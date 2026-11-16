/**
 * @file quadrant_dock.h
 * @brief A movable four-quadrant panel workspace, glassmorphism-styled: up to four panels
 *        sharing the screen's quadrants (dragged between them, each growing into any adjacent
 *        quadrant left empty -- see quadrant_layout.h), with an always-visible dock on the
 *        right holding Home and Settings.
 */

#ifndef TOYENGINE_UI_QUADRANT_DOCK_H
#define TOYENGINE_UI_QUADRANT_DOCK_H

#include <array>
#include <string>

#include <toyengine/ui/glass_theme.h>
#include <toyengine/ui/imm_screen.h>
#include <toyengine/ui/quadrant_layout.h>

namespace toy::ui {

/**
 * @class QuadrantDock
 * @brief Inventory, Character, Map, Quests and Settings panels, any four of which can be on
 *        screen together, dragged between the four quadrants and closed individually; the
 *        dock's Home button closes all of them (or restores the default arrangement when
 *        nothing is open), and its Settings button toggles the Settings panel into whichever
 *        quadrant is free.
 *
 * @code{.yaml}
 * - type: QuadrantDock
 *   theme_ref: ui/themes/glass
 *   open_inventory: true
 *   open_character: true
 *   open_quests: true
 * @endcode
 */
class QuadrantDock : public ImmScreen {
public:
    std::string type_name() const override { return "QuadrantDock"; }

    /** @brief Which panels are open when the screen starts. Three true by default, leaving one quadrant free. */
    bool open_inventory = true;
    bool open_character = true;
    bool open_map = false;
    bool open_quests = true;
    bool open_settings = false;

    /** @brief Pins every transition (dock hover, the minimise reveal) to its target instead of easing toward it. */
    bool reduced_motion = false;

    /** @brief Swaps every glass pane for GlassTheme::opaque_fill and skips the backdrop blur entirely. */
    bool reduced_transparency = false;

    /** @brief Whether the panel overlay is minimised, leaving only the dock over the camera view. */
    bool minimised = false;

    void draw(coopa::ui::imm::Context& ctx) override;

    /** @brief How far the panel overlay is revealed: 1 fully, 0 minimised, between while easing. */
    float reveal() const { return reveal_; }

    /** @brief How many of the four quadrants currently hold a panel (0..4). */
    int open_panel_count() const {
        int n = 0;
        for (auto k : slots_) {
            if (k != PanelKind::None) ++n;
        }
        return n;
    }

protected:
    void on_ready() override;
    void on_theme_loaded(const coopa::ui::imm::Theme& theme) override;

private:
    enum class PanelKind { None, Inventory, Character, Map, Quests, Settings };

    GlassTheme glass_;

    /**
     * @brief Eases toward `minimised ? 0 : 1`; the overlay's whole opacity and slide.
     *
     * The dock is deliberately NOT faded by it -- it is the only way back into the UI, so it
     * stays at full opacity over the camera view at every point in the transition.
     */
    float reveal_ = 1.0f;

    /** @brief Linear 0..1 progress behind reveal_; reveal_ is its smoothstep. */
    float reveal_t_ = 1.0f;

    /** @brief Where the two dividers sit; dragged by the splitter handles between panels. */
    QuadrantSplit split_{};

    /** @brief Which panel (if any) currently occupies each Slot; see quadrant_layout.h. */
    std::array<PanelKind, 4> slots_{};
    /** @brief slots_ as assembled from the open_* fields in on_ready() -- what Home restores. */
    std::array<PanelKind, 4> default_slots_{};

    /** @brief A dragged panel's current offset from its slot's box; zeroed on release. */
    std::array<glm::vec2, 4> drag_offset_{};
    /** @brief Whether each slot's drag_handle() was held last frame, to detect release. */
    std::array<bool, 4> dragging_prev_{};

    int inventory_selected_ = -1;
    int quest_category_ = 0;

    void assign_default_slots_();
    int find_slot_(PanelKind k) const;
    int first_free_slot_() const;
    /** @brief Opens `k` into the first free slot, or closes it if already open. */
    void toggle_panel_(PanelKind k);

    void draw_backdrop_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& area);
    void draw_panel_(coopa::ui::imm::Context& ctx, int slot, const coopa::ui::imm::Box& box,
                     const coopa::ui::imm::Box& panel_area);
    void draw_panel_content_(coopa::ui::imm::Context& ctx, PanelKind kind, const coopa::ui::imm::Box& content);
    void draw_dock_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& dock, const coopa::ui::imm::Box& panel_area);
    void draw_splitters_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& area,
                         const std::array<bool, 4>& open);

    float ease_hover_(coopa::ui::imm::Context& ctx, std::string_view id, bool hovered);

    static const char* panel_title_(PanelKind k);
    static coopa::ui::imm::Icon panel_icon_(PanelKind k);
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_QUADRANT_DOCK_H
