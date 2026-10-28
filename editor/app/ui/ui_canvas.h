/**
 * @file ui_canvas.h
 * @brief The UI workspace: author a panel asset, and watch it run.
 *
 * A screen is data now (toyengine/ui/ui_panel.h) -- a title, a size, tabs of labelled rows --
 * so this edits that file and shows the result live, at whatever resolution a player might
 * have. It is deliberately its own workspace rather than part of the scene view: an interface
 * is built against a blank frame, not on top of the level, and a scene links to the finished
 * asset by reference the way a MeshRenderer links to a mesh.
 *
 *   Design   the rows, in a list beside a live preview
 *   Interact the preview takes the pointer, so a slider really slides
 *
 * The preview is the real runtime: a PanelScreen drawing the panel being edited, placed into
 * the frame by the engine at the chosen resolution. What you see is what the game draws.
 */

#ifndef TOYENGINE_EDITOR_APP_UI_UI_CANVAS_H
#define TOYENGINE_EDITOR_APP_UI_UI_CANVAS_H

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <uicoopa/immediate/imm.h>

#include "editor/app/editor_types.h"
#include "toyengine/core/engine.h"
#include "toyengine/ui/ui_panel.h"

namespace coopa::scene { class Scene; }
namespace toy::ui { class PanelScreen; class ImmScreen; }

namespace toy {
namespace editor {

class EditorApp;

/**
 * @class UiCanvasPanel
 * @brief Edits a UI panel asset and previews it.
 */
class UiCanvasPanel {
public:
    explicit UiCanvasPanel(EditorApp& app) : app_(app) {}
    ~UiCanvasPanel();

    /** @brief One entry of the preview resolution list. */
    struct UiResolution {
        const char* label;
        int w, h;
    };

    /**
     * @brief Which editor the workspace is showing. The FILE decides, not a toggle.
     *
     * `ui/settings.yaml` is a panel asset and opens the row editor; `ui/demo_hud.tune.yaml` is
     * a placement file for the `DemoHud` screen and opens the box editor. Both are forms over
     * YAML beside a live preview -- there is no canvas to click, and nothing to link.
     */
    enum class Mode {
        Panel,      ///< A panel asset: tabs, labelled rows, buttons.
        Placement,  ///< A screen's .tune.yaml: where each box the screen declares sits.
    };

    /** @brief Which editor is showing. */
    Mode mode() const { return mode_; }

    /** @brief The suffix that marks a placement file. */
    static constexpr const char* k_tune_suffix = ".tune.yaml";

    /** @brief Whether `path` is a placement file rather than a panel asset. */
    static bool is_placement_file(const std::filesystem::path& path);

    /** @brief The ImmScreen component keys the Placement editor can open, from the schemas. */
    static std::vector<std::string> screen_types();

    /** @brief Creates `<screen_name>.tune.yaml` (snake_case) for a screen type and opens it. */
    bool new_placement_asset(const std::string& screen_type);

    /** @brief True while a panel asset is open. */
    bool ui_mode() const;

    /** @brief Opens a panel asset. False (and logged) if it will not parse. */
    bool open_ui_asset(const std::filesystem::path& path);

    /** @brief Creates `name` from a template and opens it. */
    bool new_ui_asset(const std::string& name, const std::string& template_id);

    /** @brief Adds an object to the open SCENE carrying a PanelScreen for `ui_rel`. */
    ObjectId place_ui_asset(const std::string& ui_rel, ObjectId parent = 0);

    /** @brief The template names the "New UI" menu offers. */
    static std::vector<std::string> ui_templates();

    /** @brief The preview frame, in editor pixels. */
    imm::Box ui_frame() const { return ui_frame_; }

    /** @brief Whether the preview is taking the pointer. */
    bool ui_interacting() const { return ui_interact_; }
    void set_ui_interact(bool on) { ui_set_interact_(on); }

    /** @brief Where the engine should draw the previewed panel. */
    std::optional<core::Engine::ScreenUiPlacement> ui_preview_placement_();

    /** @brief The workspace's viewport: header, backdrop, preview frame. */
    void draw_ui_view_(imm::Context& ctx, const imm::Box& area);

    /** @brief The left column: the rows of the panel (or the screen's boxes), and what adds one. */
    void draw_ui_palette_(imm::Context& ctx, const imm::Box& area);

    /** @brief Properties of the panel itself -- title, size, anchor, theme. */
    void draw_ui_canvas_props_(imm::Context& ctx);

    /** @brief Properties of the selected row. */
    void draw_ui_object_props_(imm::Context& ctx);

    /** @brief Re-reads the document into the preview. Call after any edit. */
    void ui_refresh_preview_();

    /** @brief Saves the open panel back to its file. */
    void save();

    /** @brief True if there are unsaved edits. */
    bool dirty() const { return dirty_; }

    /** @brief The open panel's path, or empty. */
    const std::filesystem::path& path() const { return path_; }

    /** @brief Undo / redo one edit. */
    void undo();
    void redo();

    /** @brief Frames the preview on the next draw. */
    bool ui_fit_ = true;

    /** @brief The preview is taking input. */
    bool ui_interact_ = false;

    void ui_set_interact_(bool on);

    /** @brief Fits the preview to the viewport. */
    void ui_frame_selection_() { ui_fit_ = true; }

    /** @brief Saves the workspace's view preferences. */
    void ui_store_editor_extras_();

    /** @brief Releases the preview scene, before the device goes away. */
    void release();

    /**
     * @brief Attaches or detaches the preview's overlay layer.
     *
     * Driven every frame from `EditorApp::update_scene_ui_placement_()` with "is the UI
     * workspace showing", so whether the panel is drawn is a pure function of where the editor
     * is -- not of whether a file was once opened. The engine holds overlay layers NON-OWNING
     * and draws them every frame until something removes them, so attaching on open and
     * detaching only on teardown leaves the panel painted over every later workspace.
     */
    void set_preview_visible(bool on);

    /** @brief Moves the selected row by `delta`, duplicates it, or removes it. */
    void move_row_(int delta);
    void duplicate_row_();
    void remove_row_();

    /** @brief The screen whose boxes Placement mode is editing, or null. */
    toy::ui::ImmScreen* placed_screen() const { return placed_; }

    /** @brief The PanelScreen drawing the open panel asset, or null. */
    toy::ui::PanelScreen* panel_screen() const { return screen_; }

    /** @brief Whether the attached preview layer is taking the pointer. */
    bool preview_takes_input() const { return preview_attached_ && preview_input_; }

    /** @brief Which row the Panel editor has selected, or -1. */
    int selected_row_index() const { return row_; }
    void select_row(int i) { row_ = i; }

    /** @brief Records that the open file has edits the user has not saved. */
    void mark_dirty() { dirty_ = true; }

private:
    using Node = fkyaml::node;

    void ui_layout_frame_();
    void ui_zoom_at_(glm::vec2 m, float factor);
    void draw_ui_header_(imm::Context& ctx, const imm::Box& b);
    void handle_ui_input_(imm::Context& ctx);
    void draw_ui_overlays_(imm::Context& ctx);
    static const std::vector<UiResolution>& ui_resolutions_();

    /** @brief The rows of the selected tab, for the list and the inspector. */
    Node* rows_();
    Node* selected_row_();
    void push_undo_();
    void ensure_preview_();
    /** @brief Tears the preview down and builds the one the current mode needs. */
    void rebuild_preview_();

    /** @brief The left column in Placement mode: the boxes the live screen declared. */
    void draw_placement_list_(imm::Context& ctx);
    /** @brief The inspector in Placement mode: anchor, pos and size of the selected box. */
    void draw_placement_props_(imm::Context& ctx);

    EditorApp& app_;

    // --- the document ---
    Node doc_;
    std::filesystem::path path_;
    bool open_ = false;
    bool dirty_ = false;
    std::vector<Node> undo_, redo_;

    int tab_ = 0;        ///< Tab being edited.
    int row_ = -1;       ///< Selected row within that tab, or -1.

    Mode mode_ = Mode::Panel;
    /// Placement mode: which screen's boxes are being placed ("DemoHud").
    std::string screen_type_;
    /// Placement mode: index into the live screen's declared() list, or -1.
    int box_ = -1;

    // --- the preview ---
    std::unique_ptr<coopa::scene::Scene> preview_;
    /// Whether `preview_` is currently registered as an engine overlay layer.
    bool preview_attached_ = false;
    /// Whether the attached layer was added as interactive. The engine reads the LAYER's own
    /// input flag (Engine::overlay_layer_placement_), so changing Interact has to re-add it.
    bool preview_input_ = false;
    toy::ui::PanelScreen* screen_ = nullptr;
    /// Placement mode: the screen component whose tune() is being edited.
    toy::ui::ImmScreen* placed_ = nullptr;

    // --- the view ---
    imm::Box ui_frame_{};
    glm::vec2 ui_res_{1920.0f, 1080.0f};
    glm::vec2 ui_pan_{0.0f};
    float ui_zoom_ = 0.0f;
    bool ui_panning_ = false;
    int ui_backdrop_ = 0;   ///< 0 dark, 1 light, 2 the scene behind.
};

}  // namespace editor
}  // namespace toy

#endif  // TOYENGINE_EDITOR_APP_UI_UI_CANVAS_H
