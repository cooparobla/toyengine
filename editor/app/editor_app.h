/**
 * @file editor_app.h
 * @brief The toyengine editor: menus, tabs, panels, viewport tools -- everything above the
 *        embedded Engine.
 *
 * Tabs:
 *   Scene             hierarchy | viewport | inspector, asset browser below
 *   Asset             mesh & material editing against a private preview scene
 *   Render Settings   config.yaml's render block, applied live (or on renderer restart)
 *   Project Settings  window/physics/jobs/output, default scene, packaging
 *
 * Frame flow (see Engine::tick()):
 *   pre_scene_update  viewport camera/gizmo input is read inside the UI callback instead
 *   late_update       the overlay scene's ImmediateCanvas runs draw_() -- all UI, all
 *                     document edits; anything that restructures the engine's scenes is
 *                     queued in deferred_
 *   post_late_update  deferred_ runs: scene rebuilds, play/stop, tab scene switches
 *   pre_render        editor camera made main, display region set, grid lines pushed
 */

#ifndef TOYEDITOR_APP_EDITOR_APP_H
#define TOYEDITOR_APP_EDITOR_APP_H

#include "asset_documents.h"
#include "editor_theme.h"
#include "sculpt_preview.h"
#include "paint_preview.h"
#include "trackpad.h"
#include "../anim/clip_model.h"
#include "../anim/clip_pose.h"
#include "project.h"
#include "scene_sync.h"

#include "../build/build_pipeline.h"
#include "../build/packager.h"
#include "../core/naming.h"
#include "../core/process.h"
#include "../core/scene_document.h"
#include "../mesh/mesh_bvh.h"
#include "../mesh/mesh_loops.h"
#include "../mesh/mesh_mirror.h"
#include "../mesh/mesh_ops.h"
#include "../mesh/proportional.h"
#include "../mesh/mesh_subdivide.h"
#include "../mesh/mesh_topology.h"
#include "../mesh/sculpt.h"
#include "../mesh/shader_ball.h"
#include "../mesh/primitives.h"
#include "../schema/component_schema.h"
#include "../schema/inspector.h"
#include "../schema/ui_theme_schema.h"
#include "../schema/settings_schema.h"
#include "../viewport/editor_camera.h"
#include "../viewport/gizmo.h"
#include "../viewport/modal_transform.h"
#include "../viewport/rect_gizmo.h"
#include "../ui/ui_canvas_math.h"
#include "../ui/log_view.h"
#include "../ui/logo.h"
#include "../ui/ui_palette.h"

#include <toyengine/core/config.h>
#include <toyengine/core/branding.h>
#include <toyengine/core/engine.h>
#include <toyengine/render/passes/debug_line_pass.h>
#include <physxcoopa/components/collider.h>
#include <physxcoopa/debug/shape_draw.h>
#include <physxcoopa/util/transform_bridge.h>
#include <toyengine/particles/particle_system.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/data/mesh.h>

#include <uicoopa/immediate/imm_canvas.h>
#include <uicoopa/immediate/imm_file_dialog.h>
#include <uicoopa/ui_yaml.h>

#include <coopa/scene/scene_loader.h>

#include <root_directory.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

namespace fs = std::filesystem;

/**
 * @brief Render settings the editor always uses on top of the project's: the viewport fills
 *        its panel (resolution_mode "fill", keeping the project's vertical resolution so pixel
 *        density matches the game) and is fitted, never integer-letterboxed.
 */
void apply_editor_render_overrides(core::AppConfig& cfg);
using coopa::input::Key;
using coopa::input::Mods;
/// uicoopa's Finder-style picker (Open / Save / folders), shared with the hub.
using FileDialog = coopa::ui::imm::FileDialog;

/**
 * @brief The kind of asset the editor has open. Exactly one asset is open at a time, and its
 *        type decides what the viewer shows and what the Properties editor offers.
 */
enum class AssetType { None = 0, Scene, Object, Mesh, Material, Texture, UI, Theme, Audio };
/** @brief Blender's interaction modes for a mesh object (the viewport header's mode dropdown). */
enum class InteractionMode { Object = 0, Edit, Sculpt, VertexPaint, WeightPaint };
/** @brief Viewport shading, Blender's four buttons. */
enum class Shading { Wireframe = 0, Solid = 1, MaterialPreview = 2, Full = 3 };
/** @brief The Properties editor's tabs. */
enum class PropTab { Tool, Render, Output, Scene, World, Object, Components, Physics, Data, Material, Canvas, Bindings, Theme };

/** @brief State carried across a renderer restart (the Engine is rebuilt underneath). */
struct EditorState {
    std::optional<SceneDocument> scene;   ///< The open scene or object asset
    Shading shading = Shading::Solid;
    glm::vec3 cam_focus{0.0f};
    float cam_yaw = 35.0f, cam_pitch = 25.0f, cam_distance = 12.0f;
    std::optional<ConfigDocument> config;
};

class EditorApp {
public:
    /** @brief Asset panel orderings (ui/assets.inl). */
    enum class AssetSort { Name, Modified, Tags, Size };
    /** @brief An asset file's sort keys: last-modified (seconds) and size (bytes). */
    struct AssetStat { int64_t modified = 0; uintmax_t size = 0; };

    // =================================================================================
    // Construction
    // =================================================================================

    /**
     * @param engine  An Engine built with EngineOptions{.load_default_scene = false,
     *                .edit_mode = true, .escape_quits = false} and screen_ui_enabled.
     * @param scene   Scene to open; empty opens the config's default scene (or a new one).
     */
    EditorApp(core::Engine& engine, Project project, const fs::path& scene = {}, EditorState state = {});

    ~EditorApp();

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    /** @brief Hands the editor's documents over for a renderer restart. */
    EditorState take_state();

    // =================================================================================
    // Queries (main loop, tests)
    // =================================================================================

    SceneDocument& document() { return doc_; }
    SceneSync& sync() { return sync_; }
    MeshDocument& mesh_document() { return mesh_; }
    MaterialDocument& material_document() { return material_; }
    ConfigDocument& config_document() { return config_; }
    Project& project() { return project_; }
    EditorCamera& camera() { return camera_; }
    Gizmo& gizmo() { return gizmo_; }
    float grid_step() const { return grid_step_(); }
    /** @brief Tests: treat scrolls as trackpad gestures (or a wheel); queue a pinch. */
    void set_trackpad_for_test(std::optional<bool> trackpad) { trackpad_override_ = trackpad; }
    void pinch_for_test(double magnify) { pinch_override_ += magnify; }
    int grid_normal_axis() const { return grid_normal_axis_(); }
    /**
     * @brief Shows the scene / object document that is loaded (without reloading it) -- e.g.
     *        after a material was created and opened from it. Tests and internal flows.
     */
    void show_document_view();

    /** @brief What's open: its type decides the viewer and the Properties tabs. */
    AssetType active_asset_type() const { return active_type_; }
    const fs::path& active_asset_path() const { return active_path_; }
    Shading shading() const { return shading_; }
    bool playing() const { return play_scene_ != nullptr; }
    /** @brief Selection outlines the viewport overlay drew last frame (tests / diagnostics). */
    int selection_outlines_drawn() const { return selection_outlines_drawn_; }
    /** @brief Collider wireframe lines drawn last frame (0 with the Colliders toggle off). */
    size_t collider_lines_drawn() const { return collider_lines_drawn_; }
    bool show_colliders() const { return show_colliders_; }
    void set_show_colliders(bool on) { show_colliders_ = on; }
    /** @brief The Colliders popover's choice: every collider (false) or the selection's (true). */
    void set_colliders_selected_only(bool selected_only) {
        collider_view_ = selected_only ? ColliderView::Selected : ColliderView::All;
    }
    /** @brief World > Weather > Preview's "Show Effects" (edit mode only; not saved). */
    void set_weather_preview_effects(bool show) { weather_preview_effects_ = show; }
    PropTab prop_tab() const { return prop_tab_; }
    void set_prop_tab(PropTab t) { prop_tab_ = t; }
    /** @brief The navigation gizmo's on-screen rect (canvas pixels), for tests. */
    imm::Box nav_gizmo_rect() const { return nav_gizmo_rect_(); }

    // --- themes (Edit > Theme) ---
    /** @brief Switches to the theme `editor/themes/<id>.yaml` and remembers it; false if it failed to load. */
    bool set_theme(const std::string& id);
    const std::string& theme_id() const { return theme_id_; }
    const imm::Theme& theme() const { return theme_; }
    const EditorTheme& editor_theme() const { return et_; }
    /** @brief Unity's Pause toggle / single Step for the running game. */
    void pause() { toggle_pause_(); }
    void step() { step_simulation_(); }
    /** @brief Where a named widget was last drawn ("asset_row:<rel>", "asset_delete", "asset_add",
     *         "asset_new:<item>"), for tests driving the UI with real input. */
    /** @brief Tests: forget every recorded rect, so the next frame's are the only ones. */
    void clear_test_rects() { test_rects_.clear(); }
    std::optional<imm::Box> test_rect(const std::string& name) const;
    void set_asset_tab(AssetType t) { asset_tab_ = t; }
    /** @brief Window-space rect of an object's Outliner eye toggle (if its row is visible). */
    std::optional<imm::Box> outliner_eye_rect(ObjectId id) const;
    bool paused() const { return play_scene_ && !play_scene_->is_simulating() && step_countdown_ == 0; }
    /** @brief True while a scene object's mesh is in edit mode (Tab). */
    bool edit_mode_active() const { return in_edit_mode_(); }
    bool modal_active() const { return modal_.active(); }
    /** @brief Proportional editing (Edit Mode, O): its settings. */
    ProportionalSettings& proportional() { return proportional_; }
    /** @brief O: proportional editing on / off. */
    void toggle_proportional();
    /** @brief Shift O: the next falloff curve. */
    void cycle_proportional_falloff();
    /** @brief Mesh > Mirror on the current Edit Mode selection (axis 0-2; world or mesh axes). */
    void mirror_mesh_selection(int axis, bool global) { mirror_mesh_selection_(axis, global); }
    /** @brief H on these objects (editor-only hide). */
    void hide_objects(const std::vector<ObjectId>& ids) { hide_(ids); }
    // Prefab instances (ui/instances.inl).
    /** @brief What a viewport click on `hit` selects: the whole instance first, then its part. */
    ObjectId click_target(ObjectId hit) const { return click_target_(hit); }
    /** @brief `id`'s `type` component as the Components tab shows it (resolved in an instance). */
    Node shown_component(ObjectId id, const std::string& type) const;
    /** @brief A Components tab edit: saved in place, or in an instance as its smallest override. */
    void edit_component(ObjectId id, const Node& edited) { edit_component_(id, edited, "Edit " + component_type(edited)); }
    void revert_override_field(ObjectId id, const std::string& type, const std::string& key);
    void apply_component_to_asset(ObjectId id, const std::string& type);
    void apply_all_to_asset(ObjectId id) { apply_all_to_asset_(id); }
    void revert_all_overrides(ObjectId id) { revert_all_overrides_(id); }
    bool has_overrides(ObjectId id) const { return has_overrides_(id); }
    bool object_transform(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const { return get_object_transform_(id, p, r, s); }
    /** @brief Object Mode's Shade Smooth / Shade Flat on the selected objects' meshes. */
    void shade_selected(bool smooth) { shade_selected_(smooth); }
    /** @brief Sculpt / Edit Mode: Catmull-Clark the whole edited mesh `levels` times (one undo step). */
    void subdivide_smooth(int levels) { if (edit_object_ || mesh_edit_view_()) run_catmull_clark_(levels); }
    bool quit_requested() const { return quit_; }
    bool restart_requested() const { return restart_; }
    /** @brief Render > Rebuild Renderer: settings fixed at pipeline construction apply in place (ui/project_settings.inl). */
    void rebuild_renderer() { rebuild_renderer_(); }
    /** @brief Edit > Project Settings, on `category` (ui/project_settings.inl). */
    void open_project_settings(const std::string& category) { open_project_settings_(category); }
    /** @brief Closes the Project Settings modal on its next draw (as its Close button does). */
    void close_project_settings() { close_project_settings_ = true; }
    /** @brief A project switch was requested: the main loop rebuilds everything for it. */
    const std::optional<fs::path>& switch_project_requested() const { return switch_project_; }
    const std::deque<std::pair<int, std::string>>& log() const { return log_; }
    imm::Box viewport_box() const { return viewport_box_; }
    coopa::ui::imm::Context& ui() { return canvas_->context(); }
    /** @brief The editor UI's own canvas (its DrawList, scale), for tests. */
    coopa::ui::CanvasComponent* editor_canvas() { return canvas_canvas_(); }

    /** @brief True if anything is unsaved (scene, mesh, material, config). */
    bool has_unsaved() const;

    /**
     * @brief The window asked to close: returns true if it may (nothing unsaved), else opens
     *        the unsaved-changes prompt and returns false.
     */
    bool request_close();

    // =================================================================================
    // Logging
    // =================================================================================

    void log_info(const std::string& s) { push_log_(0, s); }
    void log_warn(const std::string& s) { push_log_(1, s); }
    void log_error(const std::string& s) { push_log_(2, s); }

    // =================================================================================
    // Scene actions
    // =================================================================================

    void new_scene();

    bool open_scene(const fs::path& path);

    bool save_scene();

    bool save_scene_as(const fs::path& path);

    /** @brief Creates an object with a primitive mesh (saving the mesh asset if new). */
    ObjectId create_primitive(const std::string& primitive, ObjectId parent = 0);

    ObjectId create_empty(ObjectId parent = 0);

    /** @brief Creates an object carrying one component of `type` (lights, cameras, ...), named
     *         `name` in snake_case ("Point Light" -> point_light). */
    ObjectId create_with_component(const std::string& type, const std::string& name, ObjectId parent = 0);

    /** @brief The selection minus inherited children (they come from an object asset: not deleted, moved or copied here). */
    std::vector<ObjectId> own_selection_();

    void delete_selected();

    void duplicate_selected();

    /** @brief Which document Ctrl+Z acts on: what the user is looking at / pointing at. */
    enum class UndoTarget { Scene, Mesh, Material, Theme, Config, Clip };
    UndoTarget undo_target_() const;

    void undo();
    void redo();

    /**
     * @brief Object Mode's Ctrl+Z: ONE timeline across the scene document and every mesh edited
     *        from its objects (the open one and the parked ones), newest step first -- so leaving
     *        Edit Mode does not strand its edits beyond undo. A mesh step undone here is applied
     *        to the live scene and saved, keeping the file in step (it was saved on leaving Edit
     *        Mode). Inside Edit/Sculpt Mode, Ctrl+Z stays on that mesh's own steps.
     */
    void scene_undo_();

    /** @brief scene_undo_()'s counterpart: re-applies the most recently undone step of that timeline. */
    void scene_redo_();

    /** @brief Makes `path`'s mesh history the open one (resuming it from the parked set). */
    bool activate_scene_mesh_(const fs::path& path);

    /**
     * @brief Object Mode's Shade Smooth / Shade Flat (Blender's): every face of each selected
     *        object's mesh. A mesh several selected objects share is changed once. Each mesh's
     *        change is a step in its own history, so Object Mode's Ctrl+Z undoes it.
     */
    void shade_selected_(bool smooth);

    /** @brief After an Object Mode undo/redo of a mesh step: save it and show it on every user. */
    void commit_scene_mesh_();

    // --- mesh histories ------------------------------------------------------------------

    /**
     * @brief Moves the open mesh document -- with its whole undo history -- aside, so switching
     *        to another mesh and back keeps it (load() would start a fresh history). A dirty
     *        document is saved first, as switching always did, so parked ones match their files.
     *        The most recent kMaxParkedMeshes are kept.
     */
    void park_mesh_doc_();

    /** @brief Opens `path` as the mesh document: its parked history if the file is unchanged, else a fresh load. */
    void switch_mesh_doc_(const fs::path& path);

    /** @brief Frames the selection (or everything) in the viewport. */
    void frame_selected();

    void frame_all();

    // --- play mode ---

    void play();

    void stop();

    /** @brief True while the running game owns the keyboard and mouse (see set_game_focus_). */
    bool game_focused() const { return game_focused_; }

    // --- shading / tabs ---

    void set_shading(Shading s);

    /**
     * @brief Switches what the viewer shows to asset type `t`: leaves mesh modes, swaps the
     *        camera between the scene view and the asset preview, and restores the shading
     *        this asset type last used.
     */
    void set_view_(AssetType t);

    // =================================================================================
    // Asset actions
    // =================================================================================

    bool open_mesh(const fs::path& path);

    void new_mesh(const std::string& primitive);

    bool save_mesh();

    bool open_material(const fs::path& path);

    /** @brief Creates materials/<name>.yaml (from `from` if given) and opens it. */
    bool create_material(const std::string& name, const Node& from = Node());

    bool save_material();

    // =================================================================================
    // Config actions
    // =================================================================================

    /** @brief Pushes the edited config's live-safe render values into the running renderer. */
    /**
     * @brief Overrides config.yaml's `section.key` for the open scene (null `value`: reverts to
     *        the project's setting) and applies it -- what editing a tinted settings row does.
     */
    void set_scene_setting(const std::string& section, const std::string& key, const Node* value);

    /**
     * @brief Applies the edited config.yaml (and the open scene's overrides on top) to the
     *        live renderer. The Engine does the layering -- see Engine::set_config_source().
     */
    void apply_config_live();

    /** @brief Re-reads the effective debug_view (config + overrides) the shading modes restore. */
    void refresh_config_debug_view_();

    bool save_config();

    /** @brief The edited config, for a renderer restart. */
    core::AppConfig config_for_restart() const { return core::AppConfig::from_node(config_.node); }

    PackageReport package(const fs::path& out_dir, bool keep_yaml = false);

    /** @brief Picks the document object under a canvas-pixel position (0 = nothing). */
    ObjectId pick_object(glm::vec2 px);

private:
    // =================================================================================
    // Small helpers
    // =================================================================================

    void push_log_(int level, const std::string& s);

    /**
     * @brief A free asset name for a new file in assets/<dir>: `base` in snake_case, then
     *        base_01, base_02... Free means no asset of the same TYPE has the name, in any tag
     *        folder -- references find assets by type and name (coopa::asset::AssetIndex).
     */
    std::string unique_asset_name_(const std::string& dir, const std::string& raw_base);

    static void set_object_position_(Node& obj, const glm::vec3& p);

    /** @brief Where new objects appear: the camera focus, snapped to the grid, on the ground. */
    glm::vec3 spawn_point_() const { return cursor3d_; }

    InspectorEnv inspector_env_();

    // =================================================================================
    // Document -> live scene
    // =================================================================================

    void rebuild_scene_();

    void queue_rebuild_();

    /** @brief Applies a document change to the live scene (deferred to a safe point). */
    void apply_(Change c);

    void after_structure_change_(ObjectId select);

    /** @brief Makes the scene the current tab wants the engine's active scene. */
    void ensure_active_scene_();

    // =================================================================================
    // Asset preview scene
    // =================================================================================

    coopa::gfx::engine::components::MeshRenderer* preview_renderer_();

    /** @brief Uploads `m` as the preview object's mesh (a runtime asset, no file). */
    void upload_preview_mesh_(const EditMesh& m, const std::string& id);

    void refresh_material_preview_();

    // =================================================================================
    // Frame hooks
    // =================================================================================

    void post_late_update_(float dt);

    /** @brief Scales the UI to the display's points (2x framebuffer pixels on Retina). */
    void sync_ui_scale_();

    /** @brief Places the scene image in viewport_box_ (framebuffer pixels). */
    void sync_display_region_();

    void pre_render_(float dt);

    /// What the viewport header's Colliders toggle draws (see push_collider_lines_()).
    enum class ColliderView { All, Selected };

    /**
     * @brief Physics collider wireframes, Unity-style: every Box/Sphere/Capsule/Mesh collider
     *        component of the viewed scene (the play scene while playing), or only those of the
     *        selection and its children (ColliderView::Selected). Drawn from the components and
     *        their transforms -- the shape PhysicsSystem builds from them (make_shape() at the
     *        object's world pose and scale) -- so it works with or without a simulation running.
     *        X-ray lines (always on top): green solid colliders, yellow triggers.
     */
    void push_collider_lines_();

    /**
     * @brief The live object a document object shows as in the viewed scene: its synced object
     *        while editing; while playing, the object at the same place in the play scene (found
     *        by its index path from the root, each step checked by name -- the play scene is
     *        loaded from the document, so the authored hierarchy matches; null if it doesn't).
     */
    coopa::scene::SceneObject* viewed_live_object_(ObjectId id) const;

    /**
     * @brief The emission shape of every selected ParticleSystem, as X-ray lines -- Unity's cone
     *        gizmo: a cone's base and spread, a sphere's three great circles, a box, a circle, an
     *        edge. A mesh emitter needs none (its mesh is the shape); every system also shows the
     *        bounds of its live particles, dimmer.
     */
    void push_particle_gizmos_();

    /**
     * @brief Keeps the open scene's screen-space UI (a HUD) inside the viewer rather than
     *        across the whole editor window: in the scene view it covers the rendered image
     *        (and reacts only while the running game has focus); the UI designer places it in
     *        its preview frame (see ui_canvas.inl).
     */
    void update_scene_ui_placement_();

    coopa::ui::CanvasComponent* canvas_canvas_();

    /** @brief The picker's toyengine bits: the project (and its assets/) as Favorites, toyengine's
     *         YAML / caml documents named as such, and this session's folders under Recent. */
    void setup_file_dialog_();

    /**
     * @brief Loads a theme by id and applies it to the UI (Style, font, editor colours). On
     *        failure the current look stays and the error goes to the console.
     */
    bool load_theme_(const std::string& id);

    /** @brief Newest modification time across the themes folder (a base theme may change too). */
    static fs::file_time_type themes_stamp_();

    /** @brief Hot reload: re-applies the active theme when any theme file changes on disk. */
    void poll_theme_(float dt);

    /**
     * @brief Scroll and pinch over the viewport, as Blender reads them.
     *   Wheel:     zoom; Shift pans vertically, Ctrl horizontally; sideways (tilt) orbits.
     *              Deltas are used as-is, so macOS smooth scrolling zooms smoothly.
     *   Trackpad:  two-finger swipe orbits (both ways, so the view can tilt up / down),
     *              Shift+swipe pans, Ctrl+swipe zooms, pinch zooms; the momentum coast
     *              after the fingers lift is ignored, so the view stops with them.
     */
    void scroll_navigate_(const imm::FrameInput& in, double pinch, bool shift, bool ctrl);

    /** @brief X-Ray is on and the shading has surfaces to see through (Blender: not in Rendered). */
    bool xray_surfaces_() const;

    /**
     * @brief The grid's current spacing: the smallest 1 / 5 x 10^k (down to 1 mm) keeping lines
     *        ~25+ px apart at the camera's zoom. Ctrl / Snap moves in these units, so -- as in
     *        Blender -- zooming in gives a finer grid and finer snapping.
     */
    float grid_step_() const;

    /** @brief The world axis (0 X, 1 Y, 2 Z) the grid is drawn perpendicular to: the view axis in
     *         an axis-aligned view (numpad 1 / 3: the XZ / YZ planes, as Blender), else Z (floor). */
    int grid_normal_axis_() const;

    /**
     * @brief The grid, as 3D lines in the scene's line pass -- occluded by geometry, as Blender's
     *        is. The floor (Z = 0) normally; looking straight down X or Y, the plane facing the
     *        view instead (through the origin in perspective; in ortho, behind the scene, as
     *        Blender's backdrop grid). Faint, adaptive spacing (grid_step_()), fading out with
     *        distance from the focus point; the axes through the origin tinted.
     */
    void push_grid_lines_();

    void apply_shading_();

public:
    /** @brief The Viewport Shading popover's "Ambient Occlusion" option (saved in prefs). */
    void set_viewport_ao(bool on);
    bool viewport_ao() const { return viewport_ao_; }

    /** @brief View > Stats Overlay: the engine's debug HUD in the viewport, off <-> full (F3
     *         in the viewport steps through fps as well). */
    void toggle_stats_overlay();

    /** @brief Local-space bounds of the geometry an object's outline is drawn from (see
     *        mesh_for_object_()); false for an object with no mesh. */
    bool outline_bounds(ObjectId id, glm::vec3& lo, glm::vec3& hi);

private:

    // =================================================================================
    // Viewport math
    // =================================================================================

    std::optional<ViewProj> view_proj_();

    struct CachedMesh {
        EditMesh mesh;
        std::vector<Edge> edges;
        glm::vec3 lo{0.0f}, hi{0.0f};
    };

    /**
     * @brief CPU geometry for an object's mesh (cached by resolved path) -- what selection
     *        outlines, wireframe/X-ray overlays and bounds draw. When that file is the one open in
     *        the mesh editor, this is the OPEN (possibly unsaved) mesh, not the file on disk: the
     *        scene already shows the edit (push_mesh_to_scene_()), so outlines read from disk would
     *        snap back to the saved shape the moment Edit Mode is left. push_mesh_to_scene_()
     *        clears the cache on every edit, so the copy here stays current.
     */
    const CachedMesh* mesh_for_object_(const Node& obj);

    bool object_bounds_(ObjectId id, glm::vec3& lo, glm::vec3& hi, bool meshes_only = false);

    // =================================================================================
    // UI (Blender layout) -- see editor/app/ui/*.inl
    // =================================================================================

#include "ui/layout.inl"
#include "ui/topbar.inl"
#include "ui/outliner.inl"
#include "ui/properties.inl"
#include "ui/browser.inl"
#include "ui/statusbar.inl"
#include "ui/viewport_chrome.inl"
#include "ui/mesh_tools.inl"
#include "ui/sculpt.inl"
#include "ui/paint.inl"
#include "ui/timeline.inl"
#include "ui/assets.inl"
#include "ui/instances.inl"
#include "ui/ui_canvas.inl"
#include "ui/theme_editor.inl"
#include "ui/build.inl"
#include "ui/weather.inl"
#include "ui/project_settings.inl"
#include "ui/preferences.inl"

    // --- helpers shared by the panels ---

    void extract_material_(ObjectId id, const Node& comp);

    void add_mesh_to_scene_(const std::string& item, std::optional<glm::vec3> at = std::nullopt);

    void assign_material_(const std::string& item, ObjectId only = 0);

    void draw_mesh_tools_(imm::Context& ctx);

    void extrude_();

    void draw_material_editor_(imm::Context& ctx);

    /**
     * @brief True while the settings UI edits the open scene's overrides (the Properties
     *        Render / World / Scene tabs with a scene open) rather than config.yaml (the
     *        Project Settings modal, ui/project_settings.inl, sets project_settings_mode_).
     */
    bool scene_settings_layer_() const {
        return !project_settings_mode_ && active_type_ == AssetType::Scene && !doc_.is_object_asset();
    }

    /**
     * @brief True if `f` (a key of config.yaml's `section`) is edited as a scene override: render
     *        and physics keys in the scene layer. Keys fixed at pipeline construction count --
     *        the engine rebuilds the renderer for them (Engine::apply_scene_settings_()); only
     *        project_only keys (baked in at engine start-up) and other sections stay project-wide.
     */
    bool scene_overridable_(const std::string& section, const FieldDesc& f) const {
        return (section == "render" || section == "physics") && !f.project_only && scene_settings_layer_();
    }

    /** @brief A setting's value as short text ("On", "0.5", "high"; `fallback` when unset). */
    static std::string setting_text_(const Node* n, const std::string& fallback = "default");

    /** @brief A scalar / vector setting value as numbers (bool 0/1; [a, b, c] or {x, y, z} / {r, g, b}); empty if not numeric. */
    static std::vector<double> setting_numbers_(const Node& n);

    /**
     * @brief True if `value` is what the project already uses for `f`: config.yaml's explicit
     *        value, else the schema default. A tier-driven key without an explicit value has no
     *        fixed project value, so it never matches (its override is kept).
     */
    static bool equals_project_value_(const FieldDesc& f, const Node& section, const Node& value);

    /** @brief How many of a settings group's keys (its switch and rows) the open scene overrides. */
    int group_override_count_(const SettingsGroup& g, const std::string& section) const;

    /**
     * @brief One settings row, over two layers: config.yaml (the project's value) and, where
     *        scene_overridable_(), the open scene's `scene.settings` override.
     *
     * A row showing the project's value looks as it always has. Editing it in a scene writes an
     * override instead, and an overridden row gets the theme's setting_override hue and edge
     * marker. Right-click: Revert to Project Setting (drop the override), Apply to Project
     * Settings (move it into config.yaml), or Reset to Default (drop config.yaml's key so the
     * preset / engine default applies). Rows that can't be overridden edit config.yaml directly.
     */
    void draw_setting_row_(imm::Context& ctx, const FieldDesc& f, Node& section, const InspectorEnv& env, const std::string& section_name);

    /**
     * @brief A settings row's right-click menu over `row`: Revert to Project Setting, Apply to
     *        Project Settings (scene layer), Reset to Default (config.yaml). Shared by rows and
     *        group header switches.
     */
    void setting_context_menu_(imm::Context& ctx, const imm::Box& row, const FieldDesc& f, Node& section, const std::string& section_name);

    /** @brief Records a config.yaml edit of `section_name.f.key` and applies it live. */
    void commit_setting_(const Node& before, const std::string& section_name, const FieldDesc& f, bool merging);

    // =================================================================================
    // UI: viewport (shared by the Scene and Asset tabs)
    // =================================================================================

    // =================================================================================
    // Viewport: Blender-style interaction
    //
    //   Navigation  MMB orbit, Shift+MMB pan, Ctrl+MMB / wheel zoom; Alt+LMB emulates MMB;
    //               Shift+wheel / Ctrl+wheel pan, horizontal scroll orbits; trackpad: swipe
    //               orbits, Shift+swipe pans, Ctrl+swipe / pinch zoom (scroll_navigate_)
    //   Views       numpad 1/3/7 (+Ctrl opposite), 5 ortho, 0 camera, 2/4/6/8 orbit,
    //               +/- zoom, numpad . / . / F frame selected (F: Object Mode), Home frame all,
    //               ` view menu
    //   Select      LMB click, Shift+LMB toggle, drag box (Shift add, Ctrl subtract),
    //               A all, Alt+A none, Ctrl+I invert
    //   Operators   G/R/S modal transform (X/Y/Z, Shift+axis, typed values, Ctrl snap),
    //               Shift+D duplicate, X/Del delete, Shift+A add, H/Alt+H/Shift+H hide,
    //               Ctrl+P / Alt+P parent, Alt+G/R/S clear, Tab edit mode, Z shading,
    //               Shift+S snap, Shift+RMB 3D cursor, RMB context menu, N/T panels,
    //               Ctrl+Space maximize
    //   Edit mode   1/2/3 vertex/edge/face, E extrude, I inset, Ctrl+B bevel, F fill,
    //               M merge, X delete, L/Ctrl+L linked, Shift+D duplicate, U UVs,
    //               Alt+N normals
    // =================================================================================

    enum class Tool { Select = 0, Move, Rotate, Scale, Cursor };
    /** @brief Blender's transform orientations (header dropdown); Normal applies in edit mode. */
    enum class Orientation { Global = 0, Local, Normal };

    /** @brief Editing a mesh: a scene/object's MeshRenderer (edit_object_) or a mesh asset. */
    bool mesh_mode_target_() const { return (!asset_view_() && edit_object_ != 0) || active_type_ == AssetType::Mesh; }
    bool in_edit_mode_() const { return mesh_mode_target_() && mode_ == InteractionMode::Edit; }
    bool in_sculpt_mode_() const { return mesh_mode_target_() && mode_ == InteractionMode::Sculpt; }

    /** @brief The edited mesh's object-to-world matrix (identity in the Asset tab). */
    glm::mat4 mesh_world_();

    void draw_viewport_(imm::Context& ctx, const imm::Box& area, bool mesh_edit);

    /** @brief Where a ray through `px` meets the ground plane (z = 0), or the 3D cursor. */
    glm::vec3 ground_point_(glm::vec2 px);

    /** @brief The first surface under `px` (objects' meshes), else the ground plane. */
    glm::vec3 surface_point_(glm::vec2 px);

    void handle_viewport_input_(imm::Context& ctx, bool mesh_edit);

    /** @brief Keys that act while the mouse is over the viewport (Blender's 3D View keymap). */
    void viewport_keymap_(imm::Context& ctx, const ViewProj& vp, bool mesh_edit, bool can_edit);

    /** @brief Blender's context-sensitive popups, declared every frame (opened by the keymap). */
    void draw_viewport_popups_(imm::Context& ctx, bool mesh_edit);

    glm::vec2 ctx_popup_origin_(glm::vec2 fallback) const { return fallback; }

    // --- modal operators ---

    void start_modal_(ModalKind kind, const ViewProj& vp, glm::vec2 mouse, bool mesh, std::optional<glm::vec3> axis = std::nullopt);

    /** @brief Is proportional editing driving the running mesh move / rotate / scale? */
    bool proportional_live_() const;

    /** @brief The proportional circle: the radius on screen, around the selection's current centre. */
    void draw_proportional_circle_(imm::Context& ctx, const ViewProj& vp, const glm::vec3& centre);

    void run_modal_(imm::Context& ctx, const ViewProj& vp, bool ctrl, bool shift);

    void apply_modal_(ModalKind kind, const ModalTransform::Result& r);

    void extrude_interactive_(const ViewProj& vp, glm::vec2 m);

    void invert_mesh_selection_();

    // --- object operations ---

    std::vector<ObjectId> visible_ids_() const;

    void hide_(const std::vector<ObjectId>& ids);
    void unhide_all_();
    void apply_hidden_();

    /** @brief World matrix of an object (live), or identity. */
    glm::mat4 world_of_(ObjectId id);

    /** @brief Splits a matrix into the Transform's (position, Euler degrees, scale). */
    static void decompose_(const glm::mat4& m, glm::vec3& pos, glm::vec3& rot, glm::vec3& scl);

    /** @brief Writes `id`'s transform so that it ends up at world matrix `world` under `parent`. */
    void set_world_transform_(ObjectId id, const glm::mat4& world, const glm::mat4& parent_world, const std::string& label);

    void parent_selection_to_active_();

    void clear_parent_keep_transform_();

    void clear_transform_(int what);

    void selection_to_cursor_();

    /** @brief Numpad 0: matches the editor view to the scene's main camera. */
    void view_through_scene_camera_();

    /** @brief Ctrl+Alt+Numpad 0: moves the scene camera to the current view. */
    void align_scene_camera_to_view_();

    // --- edit mode in the scene ---

    /** @brief Tab: Object <-> Edit Mode on the active mesh (from Sculpt Mode: back to Object). */
    void toggle_edit_mode_();

    /**
     * @brief The mesh key an object's geometry comes from: its MeshRenderer's `mesh_path`, or --
     *        when that is empty -- its WaterBody's. A water body's MeshRenderer names no mesh on
     *        purpose: toy::water::WaterSystem bakes the WaterBody's mesh and publishes it there at
     *        runtime, so for editing (and outlines, bounds) the WaterBody's key is the real one.
     */
    static std::string object_mesh_key_(const Node& obj);

    /** @brief A mesh key's file, resolved like the engine does (scene folder, then asset roots). */
    fs::path resolve_mesh_key_(const std::string& key);

    /**
     * @brief A WaterBody with no `mesh_path` draws a procedural grid (`size` x `resolution`). To edit
     *        it, write that exact grid out as a mesh file -- next to the scene, in its meshes/
     *        folder -- and point the WaterBody at it (one undo step). Returns the new key, or empty.
     */
    std::string water_grid_to_mesh_(ObjectId id, int water_ci);

    /** @brief The active object's mesh file, or empty (logging why). */
    fs::path active_mesh_path_(ObjectId id);

    /**
     * @brief Enters Edit or Sculpt Mode on the active mesh object -- the one way in. Switching
     *        between the two on the same object keeps the isolation and view.
     */
    bool enter_mesh_mode_(InteractionMode mode);

    /** @brief Back to Object Mode -- the one way out (restores isolation and view). */
    void exit_mesh_mode_();

    // --- Local-View-style isolation while editing a mesh ---

    /** @brief Does this object draw geometry (and is not a light / probe that lights the mesh)? */
    static bool isolate_candidate_(const Node& obj);

    /** @brief Hides every other renderable object (lights stay) and frames the mesh. */
    void isolate_(ObjectId keep);

    /** @brief Undoes isolate_(): objects come back (unless the user hid them) and the view returns. */
    void unisolate_();

public:
    /** @brief The viewport header's "Isolate in Edit Mode" toggle (saved in prefs). */
    void set_isolate_in_edit(bool on);
    bool isolate_in_edit() const { return isolate_in_edit_; }
    bool is_isolated(ObjectId id) const { return isolated_.count(id) > 0; }
    InteractionMode interaction_mode() const {
        return (edit_object_ || active_type_ == AssetType::Mesh) ? mode_ : InteractionMode::Object;
    }
    bool set_interaction_mode(InteractionMode m);

private:
    void tool_settings_restore_() {}

    /** @brief Shows the edited (unsaved) mesh on every live object that uses its file. */
    void push_mesh_to_scene_();

    /**
     * @brief World-space axes (columns) for a transform orientation: identity for Global,
     *        the object's (or edited mesh's) axes for Local, the selection's Normal basis in
     *        edit mode (Local outside it).
     */
    glm::mat3 orientation_basis_(bool mesh_edit, Orientation o);

    bool gizmo_target_(bool mesh_edit, glm::vec3& pivot, glm::mat3& basis);

    struct DragStart {
        glm::vec3 pos, rot, scl;
        glm::mat4 parent_world{1.0f};
        glm::vec3 world_pos;
    };

    /** @brief The proportional radius in screen pixels at world point `at` (the circle's size). */
    float proportional_radius_px_(const ViewProj& vp, const glm::vec3& at) const;

    /**
     * @brief For the transform about to run (or after the radius changes mid-way): which
     *        unselected vertices proportional editing drags and how much, then the symmetry
     *        partners of everything that moves -- all from the positions before the move.
     */
    void update_proportional_();

    /** @brief Captures the values a gizmo drag / modal operator applies its deltas to. */
    void begin_transform_(bool mesh_edit);

    /**
     * @brief Applies a cumulative world-space delta to what begin_transform_() captured.
     * @param kind 0 translate, 1 rotate, 2 scale.
     */
    void apply_transform_(bool mesh_edit, int kind, const glm::vec3& translate, const glm::vec3& rot_axis, float rot_deg,
                          const glm::vec3& scale, const glm::mat3& scale_basis = glm::mat3(1.0f));

    void box_select_(bool mesh_edit, glm::vec2 a, glm::vec2 b, bool additive, bool subtract);

    void draw_viewport_overlay_(imm::Context& ctx, bool mesh_edit);

    void draw_viewport_overlay_body_(imm::Context& ctx, bool mesh_edit, const ViewProj& vp_ref);

    // =================================================================================
    // Modals and dialogs
    // =================================================================================

    void draw_modals_(imm::Context& ctx);

    void confirm_unsaved_(std::function<void()> then);

    /** @brief Runs `fn` now if nothing is unsaved, else after the user saves or discards. */
    void guarded_(std::function<void()> fn);

    void discard_all_changes_();

    void save_active_() { save_all_(); }

    void save_all_();

    /** @brief Picks a sound file and imports it into assets/audio/<tags>/. */
    void import_audio_dialog_(std::vector<std::string> tags = {});

    void open_scene_dialog_();

    void save_scene_as_dialog_();

    void open_project_dialog_();

    void new_project_dialog_();

    void open_package_dialog_();

    std::string current_undo_label_(bool undo);

    // =================================================================================
    // Shortcuts
    // =================================================================================

    /** @brief Window-wide keys; the viewport and outliner keymaps live with those areas. */
    void handle_shortcuts_(imm::Context& ctx);

    // =================================================================================
    // State
    // =================================================================================

    core::Engine& engine_;
    Project project_;
    std::unique_ptr<coopa::scene::Scene> ui_scene_;
    coopa::ui::ImmediateCanvas* canvas_ = nullptr;
    EditorCamera camera_;
    float ui_scale_ = 1.0f;

    SceneDocument doc_;
    SceneSync sync_;
    MeshDocument mesh_;
    MaterialDocument material_;
    ThemeDocument game_theme_;   ///< The game UI theme open in the Theme tab (ui/theme_editor.inl).
    ConfigDocument config_;

    AssetType active_type_ = AssetType::Scene;   // what's open (see AssetType)
    fs::path active_path_;                       // its file (empty while unsaved)
    PropTab prop_tab_ = PropTab::Object;
    Shading shading_ = Shading::Solid;
    /// Shading per asset type (index = AssetType): materials open in the game's renderer.
    coopa::sfx::data::ClipImportSettings audio_import_;   ///< The open sound's .import sidecar.
    bool audio_import_dirty_ = false;
    coopa::sfx::mixer::VoiceHandle audio_preview_;
    Shading shading_by_type_[9] = {Shading::Solid, Shading::Solid, Shading::Solid, Shading::Solid, Shading::Full, Shading::MaterialPreview,
                                   Shading::Full, Shading::Full, Shading::MaterialPreview};
    std::string full_debug_view_;
    std::string config_base_debug_view_;

    coopa::scene::Scene* play_scene_ = nullptr;
    coopa::scene::Scene* preview_scene_ = nullptr;
    coopa::scene::SceneObject* preview_object_ = nullptr;
    uint64_t uploaded_revision_ = 0;

    std::vector<std::function<void()>> deferred_;
    bool rebuild_queued_ = false;

    // Layout.
    float left_w_ = 260, right_w_ = 330, bottom_h_ = 130, asset_left_w_ = 220, settings_w_ = 460;
    // Asset panel (ui/assets.inl).
    AssetType asset_tab_ = AssetType::Scene;
    std::string asset_context_, asset_rename_, asset_rename_to_, asset_delete_;
    // Asset panel ordering and tags (ui/assets.inl). Tags are the folders between an asset's
    // type folder and the asset (project.h asset_tags()).
    AssetSort asset_sort_ = AssetSort::Name;
    bool asset_sort_reverse_ = false;
    std::map<AssetType, std::vector<std::string>> asset_tag_filter_;   ///< per tab; empty = all
    bool asset_tag_match_all_ = false;                                 ///< filter: every selected tag vs any
    std::string asset_tags_target_;                    ///< Edit Tags: the asset (assets-relative)
    AssetType asset_tags_type_ = AssetType::None;
    std::vector<std::string> asset_tags_edit_;         ///< Edit Tags: the tags being edited
    std::string asset_tag_input_;                      ///< Edit Tags / New: a tag being typed
    std::vector<std::string> new_asset_tags_;          ///< + popup: the tags a new asset gets
    std::vector<std::string> creating_tags_;           ///< set while a create runs (with_tags_())
    std::unordered_map<std::string, AssetStat> asset_stats_;   ///< sort keys, dropped every few seconds
    std::chrono::steady_clock::time_point asset_stats_time_{};
    Lookdev lookdev_;
    coopa::scene::SceneObject* preview_ground_ = nullptr;
    std::vector<std::string> slot_preview_materials_;   // mesh viewer: material asset per slot (preview only)
    imm::Box viewport_box_;
    bool show_grid_ = true;
    bool show_colliders_ = false;                         ///< Viewport header: draw physics colliders.
    ColliderView collider_view_ = ColliderView::All;      ///< ...all of them, or the selection's.
    size_t collider_lines_drawn_ = 0;                     ///< Last frame's collider line count (tests).
    bool grid_wanted_ = false;
    coopa::input::CursorShape applied_cursor_ = coopa::input::CursorShape::Arrow;
    bool viewport_ao_ = true;    // Viewport Shading > Ambient Occlusion
    bool show_engine_assets_ = true;   // Asset panel: list toyengine's (read-only) assets too
    // Theme (see load_theme_): the file, the editor's typed colours, hot-reload state.
    imm::Theme theme_;
    EditorTheme et_;
    bool settings_edit_was_scene_ = false;   ///< The last settings-row edit was a scene override (undo_target_()).
    std::string theme_id_, default_font_, current_font_;
    fs::file_time_type theme_stamp_{};
    float theme_poll_ = 0.0f;   // set by the overlay pass, drawn in pre_render_ (line pass)
    bool show_gizmo_ = true;

    // Viewport interaction (Blender-style; see the viewport section).
    Tool tool_ = Tool::Select;
    ModalTransform modal_;
    bool modal_mesh_ = false;
    Orientation orient_ = Orientation::Global;   // header Transform Orientation
    std::vector<SlideRail> slide_rails_;          // Edge Slide in progress
    std::string slide_merge_key_ = "modal";      // "loopcut" folds a slide into its Loop Cut undo step
    TriangleBVH bvh_;                              // edited mesh, mesh-local (mesh_bvh_())
    uint64_t bvh_revision_ = 0;
    EditVisibility edit_vis_;
    LoopCutTool loopcut_;
    Edge preview_seed_{0, 0};
    LastOp last_op_;
    bool last_op_open_ = true;
    imm::Box last_op_rect_{};
    bool alt_click_ = false, alt_shift_ = false, alt_ctrl_ = false;   // Alt+click loop select, pending
    glm::vec2 alt_press_{0.0f};
    // Sculpt Mode (ui/sculpt.inl).
    SculptSettings sculpt_;
    SculptCache sculpt_cache_;
    VertexGrid sculpt_grid_;
    SculptPreview sculpt_preview_;
    bool sculpt_active_ = false;
    uint64_t sculpt_cache_geom_ = 0, sculpt_preview_pos_ = 0;
    Stroke stroke_;
    SculptResize sculpt_resize_;
    // Vertex / Weight Paint (ui/paint.inl); the cache and grid above are shared (exclusive modes).
    PaintSettings vpaint_;
    PaintSettings wpaint_;
    PaintPreview paint_preview_;
    bool paint_active_ = false;
    uint64_t paint_geom_ = 0, paint_pos_ = 0;
    PaintPreview::Show paint_shown_;
    PaintStroke paint_stroke_;
    SculptResize paint_resize_;
    uint32_t active_group_ = 0;
    float edit_assign_weight_ = 1.0f;
    std::map<ObjectId, std::string> paint_saved_shader_;   ///< Surface shaders swapped for editor_paint
    // Animation Timeline (ui/timeline.inl).
    ObjectId anim_rig_ = 0;
    bool close_project_settings_ = false;   ///< close_project_settings(): the modal closes on its next draw
    ClipDocument anim_clip_;
    float anim_time_ = 0.0f;
    bool anim_playing_ = false, anim_record_ = false, anim_show_rest_ = false, anim_posed_ = false, anim_scrubbing_ = false;
    std::shared_ptr<coopa::anim::AnimationClip> anim_runtime_;
    uint64_t anim_runtime_rev_ = 0, anim_pose_rev_ = 0;
    float anim_pose_time_ = -1.0f;
    std::set<ClipModel::KeyRef> anim_sel_keys_;
    KeyDrag anim_drag_;
    int anim_sel_event_ = -1;   ///< Timeline Events lane: the selected clip event (index), -1 none
    KeyDrag anim_event_drag_;
    int bottom_view_ = 0;   ///< Bottom area: 0 Console, 1 Timeline
    bool anim_clip_dirty_ = false;
    std::set<std::string> anim_expanded_;   ///< Timeline rows opened into channels (track paths)
    TimelineView anim_view_;
    float anim_menu_time_ = 0.0f;
    std::string anim_rename_buf_;
    bool anim_show_all_ = false;   ///< Timeline lists every rig object (else animated + selected)
    int anim_row_scroll_ = 0;
    fs::path anim_doc_path_;   ///< The document the Timeline's state belongs to
    bool timeline_hovered_ = false;
    MeshSelection modal_base_sel_;
    ModalKind pending_modal_kind_ = ModalKind::None;
    std::optional<glm::vec3> pending_modal_axis_;
    int nav_mode_ = 0;   // 0 orbit, 1 pan, 2 zoom
    ObjectId edit_object_ = 0;
    uint64_t scene_uploaded_revision_ = 0;
    bool force_scene_push_ = false;   ///< Push the mesh to the scene even though it is saved (Object Mode undo).
    /// Mesh documents switched away from, with their undo histories (see park_mesh_doc_()).
    static constexpr size_t kMaxParkedMeshes = 8;
    std::map<fs::path, MeshDocument> parked_meshes_;
    std::vector<fs::path> parked_order_;
    glm::vec3 cursor3d_{0.0f};
    std::set<ObjectId> hidden_;
    bool show_toolbar_ = true, show_sidebar_ = false, show_bottom_ = true, maximized_ = false;
    bool show_overlays_ = true, xray_ = false, snap_on_ = false;
    float xray_alpha_ = 0.5f;
    std::optional<bool> trackpad_override_;   ///< Tests: force trackpad (true) / wheel (false) scrolls.
    double pinch_override_ = 0.0;             ///< Tests: a pinch to apply next frame.   ///< X-Ray surface opacity (Blender's default 0.5).
    bool snap_adaptive_ = true;   ///< Move snapping follows the grid's zoom-dependent spacing (Blender); off: Move Increment.
    // Overlays popover (each also needs show_overlays_).
    bool show_glyphs_ = true, show_origins_ = true, show_cursor3d_ = true, show_stats_overlay_ = false;
    bool outliner_hovered_ = false, properties_hovered_ = false, nav_ball_dragged_ = false, pending_extrude_ = false;
    int sidebar_tab_ = 0, bottom_tab_ = 0, step_countdown_ = 0;
    bool pause_after_start_ = false;
    MirrorSettings edit_symmetry_;   ///< Edit Mode's X / Y / Z mirror toggles (off by default, as in Blender).
    MirrorMap mirror_map_;           ///< The current transform's mirror partners (begin_transform_).
    // Proportional editing (O): settings, and the current transform's weighted vertices.
    ProportionalSettings proportional_;
    std::vector<std::pair<uint32_t, float>> prop_weights_;   ///< unselected vertices it drags, with weights
    glm::vec3 prop_pivot_{0.0f};                            ///< world-space selection centre at the start
    float prop_radius_px_ = 0.0f;                           ///< the radius on screen at the pivot
    bool game_focused_ = false;   ///< Play mode: the game has the input (viewer clicked; Esc releases).

    void set_game_focus_(bool focused);
    std::unordered_map<ObjectId, imm::Box> eye_rects_;   // Outliner eye toggles (tests)
    std::map<std::string, imm::Box> test_rects_;           // Where named widgets were last drawn (tests)
    bool open_add_menu_ = false;
    float outliner_h_ = 260;
    int selection_outlines_drawn_ = 0;
    std::string outliner_filter_, add_component_filter_, browser_dir_;
    std::vector<std::string> asset_dirs_cache_;
    bool console_show_[3] = {true, true, true};
    bool viewport_hovered_ = false, hierarchy_hovered_ = false;
    struct CameraPose { glm::vec3 focus{0.0f}; float yaw = 35, pitch = 25, distance = 12; bool ortho = false; bool valid = false; };
    CameraPose scene_pose_, asset_pose_, pre_isolate_pose_;
    InteractionMode mode_ = InteractionMode::Object;
    std::set<ObjectId> isolated_;        // hidden by Edit / Sculpt Mode isolation (not saved, not hidden_)
    bool isolate_in_edit_ = true;        // header toggle, prefs "isolate_edit_mode"
    Gizmo gizmo_;
    bool nav_active_ = false;
    int nav_button_ = 2;
    bool nav_moved_ = false;
    bool fly_used_ = false;
    bool select_pending_ = false;
    bool box_selecting_ = false;
    glm::vec2 select_press_{0.0f};
    std::map<ObjectId, DragStart> drag_starts_;
    EditMesh mesh_drag_base_;
    std::map<std::string, CachedMesh> mesh_cache_;
    std::pair<ObjectId, uint64_t> parent_choices_key_{0, ~0ull};         // Relations > Parent list cache (properties.inl)
    std::vector<std::string> parent_choice_names_;
    std::vector<ObjectId> parent_choice_ids_;
    std::unordered_map<std::string, std::string> mesh_resolve_cache_;   // (dir, mesh key) -> resolved path; see mesh_for_object_()
    std::string stats_cache_;                                           // see scene_stats_()
    std::tuple<uint64_t, uint64_t, size_t, size_t> stats_key_{~0ull, 0, 0, 0};

    // Hierarchy / browser.
    ObjectId context_target_ = 0;
    ObjectId rename_id_ = 0;
    int rename_frames_ = 0;
    int weather_sel_ = 0;              ///< World > Weather: the condition being edited.
    bool weather_preview_effects_ = false;   ///< World > Weather > Preview: show effects in edit mode.
    int weather_preview_speed_ = 0;    ///< World > Weather > Preview: transition fast-forward (0 = 1x .. 3 = instant).
    int weather_palette_ = 0;          ///< World > Weather > Sky Colours: the palette being edited (0 day, 1 twilight, 2 night).
    // Edit > Preferences (ui/preferences.inl); saved in ~/.toyengine_editor.yaml.
    float ui_scale_pref_ = 1.0f;          ///< Interface Scale, on top of the display's scale.
    float tooltip_delay_pref_ = 0.6f;
    bool invert_zoom_ = false;            ///< Wheel up zooms out.
    bool trackpad_swipe_pans_ = false;    ///< A two-finger swipe pans (Shift+swipe orbits).
    bool prefs_dirty_ = false;            ///< A preference changed; written when the mouse lets go.
    std::string prefs_category_ = "Interface";
    std::string prefs_keymap_filter_;
    std::vector<ThemeSwatch> prefs_themes_;   ///< Theme tiles, read while the window is open.
    std::string runtime_sel_;          ///< The selected runtime object's ':' path (ui/weather.inl); empty = none.
    int asset_cat_ = 1;
    std::string asset_filter_;
    std::string render_settings_filter_;   ///< Render tab: settings search
    std::string selected_asset_;
    double asset_click_time_ = 0.0;

    // Mesh tool parameters.
    float extrude_dist_ = 0.5f, inset_amount_ = 0.2f, bevel_width_ = 0.1f, merge_dist_ = 0.001f, uv_scale_ = 1.0f;

    // Dialogs.
    FileDialog file_dialog_;
    std::string pending_modal_;
    std::function<void()> pending_after_confirm_;
    std::string new_project_path_;
    std::string package_dir_;
    bool package_keep_yaml_ = false;

    // Log / status.
    std::deque<std::pair<int, std::string>> log_;
    std::string status_;
    int status_level_ = 0;
    std::chrono::steady_clock::time_point status_time_ = std::chrono::steady_clock::now();

    bool quit_ = false;
    bool force_quit_ = false;
    bool restart_ = false;
    bool project_settings_mode_ = false;          ///< drawing the Project Settings modal: rows edit config.yaml
    std::string project_settings_category_ = "General";
    std::string project_settings_filter_;
    std::optional<fs::path> switch_project_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_EDITOR_APP_H
