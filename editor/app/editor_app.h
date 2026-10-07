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
inline void apply_editor_render_overrides(core::AppConfig& cfg) {
    auto& r = cfg.render;
    if (r.resolution_mode == "divisor") r.render_height = std::max(1u, cfg.window.height / std::max(1u, r.scale_divisor));
    r.resolution_mode = "fill";
    r.upscale_mode = "fit";
}
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
    EditorApp(core::Engine& engine, Project project, const fs::path& scene = {}, EditorState state = {})
        : engine_(engine), project_(std::move(project)) {
        ui_scene_ = std::make_unique<coopa::scene::Scene>("EditorUI");
        canvas_ = coopa::ui::build_immediate_canvas(*ui_scene_, "EditorUI", 100000);
        canvas_->on_draw = [this](coopa::ui::imm::Context& ctx) { draw_(ctx); };
        // While the running game has the mouse (play mode, after a click in the viewer), the
        // editor UI sees nothing but the keys that hand it back (Esc) or stop play (F5).
        trackpad::install();   // pinch and trackpad-vs-wheel scrolls (macOS)
        canvas_->filter_input = [this](imm::FrameInput& in) {
            if (!game_focused_) return;
            in.mouse = glm::vec2(-1e6f);
            in.mouse_delta = glm::vec2(0.0f);
            for (int b = 0; b < 3; ++b) in.down[b] = in.pressed[b] = in.released[b] = false;
            in.scroll = glm::vec2(0.0f);
            in.chars.clear();
            std::erase_if(in.keys, [](const coopa::input::KeyEvent& e) {
                return e.key != coopa::input::Key::Escape && e.key != coopa::input::Key::F5;
            });
        };
        const std::string font_path = std::string(PROJ_DIR) + "/uicoopa/assets/fonts/Inter-Regular.ttf";
        default_font_ = current_font_ = font_path;
        canvas_->context().text.set_font(coopa::ui::UIResourceCache::instance().font_for_path(font_path));
        sync_ui_scale_();   // before the first frame, or it lays out at 1x and then jumps
        {
            const Node prefs = Project::load_prefs();
            std::string id = default_theme_id();
            if (prefs.contains("theme") && prefs.at("theme").is_string()) id = prefs.at("theme").get_value<std::string>();
            if (!load_theme_(id)) load_theme_(default_theme_id());
            if (prefs.contains("viewport_ao") && prefs.at("viewport_ao").is_boolean()) viewport_ao_ = prefs.at("viewport_ao").get_value<bool>();
            if (prefs.contains("show_engine_assets") && prefs.at("show_engine_assets").is_boolean()) {
                show_engine_assets_ = prefs.at("show_engine_assets").get_value<bool>();
            }
            if (prefs.contains("asset_sort") && prefs.at("asset_sort").is_string()) {
                asset_sort_ = asset_sort_from_name_(prefs.at("asset_sort").get_value<std::string>());
            }
            if (prefs.contains("asset_sort_reverse") && prefs.at("asset_sort_reverse").is_boolean()) {
                asset_sort_reverse_ = prefs.at("asset_sort_reverse").get_value<bool>();
            }
            if (prefs.contains("isolate_edit_mode") && prefs.at("isolate_edit_mode").is_boolean()) {
                isolate_in_edit_ = prefs.at("isolate_edit_mode").get_value<bool>();
            }
        }
        camera_.create(*ui_scene_);
        camera_.focus = state.cam_focus;
        camera_.yaw_deg = state.cam_yaw;
        camera_.pitch_deg = state.cam_pitch;
        camera_.distance = state.cam_distance;
        camera_.apply();

        engine_.set_overlay_scene(ui_scene_.get());
        engine_.set_edit_mode(true);
        engine_.assets().set_hot_reload(true);
        engine_.assets().set_poll_interval(0.5f);
        core::FrameHooks hooks;
        hooks.post_late_update = [this](float dt) { post_late_update_(dt); };
        hooks.pre_render = [this](float dt) { pre_render_(dt); };
        engine_.set_frame_hooks(std::move(hooks));

        sync_.fallback_path = project_.assets() / "scenes" / "untitled" / "scene.yaml";
        shading_ = state.shading;
        if (state.config) config_ = std::move(*state.config);
        else config_.load(project_.config_path());
        // Scenes run with config.yaml plus their own `scene.settings` overrides; the Engine
        // layers them, on the edited config.yaml rather than the one it started with.
        engine_.set_config_source(config_.node, apply_editor_render_overrides);

        if (state.scene) {
            doc_ = std::move(*state.scene);
            active_type_ = doc_.is_object_asset() ? AssetType::Object : AssetType::Scene;
            active_path_ = doc_.path();
            rebuild_scene_();
        } else if (!scene.empty()) {
            open_scene(scene);
        } else {
            const std::string def = get_string(config_.node.contains("scene") ? config_.node.at("scene") : Node::mapping(),
                                               "default_scene");
            const fs::path p = def.empty() ? fs::path() : project_.root() / def;
            if (!p.empty() && coopa::yaml::document_exists(p)) open_scene(p);
            else new_scene();
        }
        Project::remember(project_.root());
        log_info("Opened project " + project_.root().string());
        setup_file_dialog_();
    }

    ~EditorApp() {
        engine_.set_frame_hooks({});
        engine_.set_overlay_scene(nullptr);
        engine_.wait_idle();
    }

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    /** @brief Hands the editor's documents over for a renderer restart. */
    EditorState take_state() {
        EditorState s;
        s.scene = std::move(doc_);
        s.shading = shading_;
        s.cam_focus = camera_.focus;
        s.cam_yaw = camera_.yaw_deg;
        s.cam_pitch = camera_.pitch_deg;
        s.cam_distance = camera_.distance;
        s.config = std::move(config_);
        return s;
    }

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
    void show_document_view() { set_view_(doc_.is_object_asset() ? AssetType::Object : AssetType::Scene); active_path_ = doc_.path(); }

    /** @brief What's open: its type decides the viewer and the Properties tabs. */
    AssetType active_asset_type() const { return active_type_; }
    const fs::path& active_asset_path() const { return active_path_; }
    Shading shading() const { return shading_; }
    bool playing() const { return play_scene_ != nullptr; }
    PropTab prop_tab() const { return prop_tab_; }
    void set_prop_tab(PropTab t) { prop_tab_ = t; }
    /** @brief The navigation gizmo's on-screen rect (canvas pixels), for tests. */
    imm::Box nav_gizmo_rect() const { return nav_gizmo_rect_(); }

    // --- themes (Edit > Theme) ---
    /** @brief Switches to the theme `editor/themes/<id>.yaml` and remembers it; false if it failed to load. */
    bool set_theme(const std::string& id) {
        if (!load_theme_(id)) return false;
        Node prefs = Project::load_prefs();
        prefs["theme"] = Node(id);
        Project::save_prefs(prefs);
        return true;
    }
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
    std::optional<imm::Box> test_rect(const std::string& name) const {
        auto it = test_rects_.find(name);
        if (it == test_rects_.end()) return std::nullopt;
        return it->second;
    }
    void set_asset_tab(AssetType t) { asset_tab_ = t; }
    /** @brief Window-space rect of an object's Outliner eye toggle (if its row is visible). */
    std::optional<imm::Box> outliner_eye_rect(ObjectId id) const {
        auto it = eye_rects_.find(id);
        if (it == eye_rects_.end()) return std::nullopt;
        return it->second;
    }
    bool paused() const { return play_scene_ && !play_scene_->is_simulating() && step_countdown_ == 0; }
    /** @brief True while a scene object's mesh is in edit mode (Tab). */
    bool edit_mode_active() const { return in_edit_mode_(); }
    bool modal_active() const { return modal_.active(); }
    /** @brief Proportional editing (Edit Mode, O): its settings. */
    ProportionalSettings& proportional() { return proportional_; }
    /** @brief O: proportional editing on / off. */
    void toggle_proportional() {
        proportional_.enabled = !proportional_.enabled;
        log_info(std::string("Proportional Editing ") + (proportional_.enabled ? "on" : "off"));
    }
    /** @brief Shift O: the next falloff curve. */
    void cycle_proportional_falloff() {
        proportional_.falloff = static_cast<Falloff>((static_cast<int>(proportional_.falloff) + 1) % kFalloffCount);
        log_info(std::string("Proportional Falloff: ") + falloff_name(proportional_.falloff));
    }
    /** @brief Mesh > Mirror on the current Edit Mode selection (axis 0-2; world or mesh axes). */
    void mirror_mesh_selection(int axis, bool global) { mirror_mesh_selection_(axis, global); }
    /** @brief H on these objects (editor-only hide). */
    void hide_objects(const std::vector<ObjectId>& ids) { hide_(ids); }
    // Prefab instances (ui/instances.inl).
    /** @brief What a viewport click on `hit` selects: the whole instance first, then its part. */
    ObjectId click_target(ObjectId hit) const { return click_target_(hit); }
    /** @brief `id`'s `type` component as the Components tab shows it (resolved in an instance). */
    Node shown_component(ObjectId id, const std::string& type) const { Node c; shown_component_(id, type, c); return c; }
    /** @brief A Components tab edit: saved in place, or in an instance as its smallest override. */
    void edit_component(ObjectId id, const Node& edited) { edit_component_(id, edited, "Edit " + component_type(edited)); }
    void revert_override_field(ObjectId id, const std::string& type, const std::string& key) {
        Node m = Node::mapping(); m["type"] = Node(type); revert_override_field_(id, m, key);
    }
    void apply_component_to_asset(ObjectId id, const std::string& type) {
        Node m = Node::mapping(); m["type"] = Node(type); apply_component_to_asset_(id, m);
    }
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
    /** @brief A project switch was requested: the main loop rebuilds everything for it. */
    const std::optional<fs::path>& switch_project_requested() const { return switch_project_; }
    const std::deque<std::pair<int, std::string>>& log() const { return log_; }
    imm::Box viewport_box() const { return viewport_box_; }
    coopa::ui::imm::Context& ui() { return canvas_->context(); }
    /** @brief The editor UI's own canvas (its DrawList, scale), for tests. */
    coopa::ui::CanvasComponent* editor_canvas() { return canvas_canvas_(); }

    /** @brief True if anything is unsaved (scene, mesh, material, config). */
    bool has_unsaved() const {
        return doc_.dirty() || (mesh_.open() && mesh_.dirty()) || (material_.open() && material_.dirty()) ||
               (game_theme_.open() && game_theme_.dirty()) || config_.dirty();
    }

    /**
     * @brief The window asked to close: returns true if it may (nothing unsaved), else opens
     *        the unsaved-changes prompt and returns false.
     */
    bool request_close() {
        if (!has_unsaved() || force_quit_) return true;
        confirm_unsaved_([this] { force_quit_ = true; quit_ = true; });
        return false;
    }

    // =================================================================================
    // Logging
    // =================================================================================

    void log_info(const std::string& s) { push_log_(0, s); }
    void log_warn(const std::string& s) { push_log_(1, s); }
    void log_error(const std::string& s) { push_log_(2, s); }

    // =================================================================================
    // Scene actions
    // =================================================================================

    void new_scene() {
        stop();
        doc_.reset("untitled");
        Node starter = Project::default_scene_node("untitled");
        for (const auto& o : starter.at("scene").at("root_objects").as_seq()) {
            Node copy = o;
            doc_.add_object(copy, 0, -1, "New Scene");
        }
        doc_.undo_stack().clear();
        rebuild_scene_();
        frame_all();
    }

    bool open_scene(const fs::path& path) {
        if (refuse_engine_asset_(path)) return false;   // toyengine's scenes are read-only here
        stop();
        try {
            doc_.load(path);
        } catch (const std::exception& e) {
            log_error(std::string("Open failed: ") + e.what());
            return false;
        }
        if (doc_.is_object_asset()) { return open_object_asset(path); }
        set_view_(AssetType::Scene);
        active_path_ = path;
        sync_.fallback_path = path;
        rebuild_scene_();
        frame_all();
        log_info("Opened " + project_.relative(path));
        return true;
    }

    bool save_scene() {
        if (doc_.path().empty()) { save_scene_as_dialog_(); return false; }
        return save_scene_as(doc_.path());
    }

    bool save_scene_as(const fs::path& path) {
        if (active_type_ == AssetType::UI) ui_store_editor_extras_();
        try {
            doc_.save(path);
            sync_.fallback_path = path;
            // Instances elsewhere override this asset's parts by name: follow any renames.
            if (doc_.is_object_asset() && active_type_ == AssetType::Object) follow_asset_renames_(path);
            project_.refresh();
            log_info("Saved " + project_.relative(path));
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Save failed: ") + e.what());
            return false;
        }
    }

    /** @brief Creates an object with a primitive mesh (saving the mesh asset if new). */
    ObjectId create_primitive(const std::string& primitive, ObjectId parent = 0) {
        std::string key = primitive;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        for (char& c : key) if (c == ' ') c = '_';
        const fs::path mesh_path = project_.assets() / "meshes" / (key + ".yaml");
        if (!coopa::yaml::document_exists(mesh_path)) {
            try {
                coopa::yaml::save_document(mesh_path, mesh_to_node(make_primitive(primitive)));
                project_.refresh();
            } catch (const std::exception& e) {
                log_error(std::string("Could not write mesh: ") + e.what());
            }
        }
        Node obj = doc_.make_object(doc_.unique_name(key, parent));
        set_object_position_(obj, spawn_point_());
        Node mr = default_component("MeshRenderer");
        mr["mesh_path"] = Node(key);
        obj["components"].as_seq().push_back(mr);
        const ObjectId id = doc_.add_object(obj, parent, -1, "Create " + primitive);
        after_structure_change_(id);
        // Adjust Last Operation: the primitive's parameters (sizes, subdivisions...).
        LastOp op;
        op.kind = LastOp::Kind::AddObjectPrimitive;
        op.object = id;
        op.prim = PrimitiveParams::defaults(primitive);
        op.doc_revision_after = doc_.undo_revision();
        last_op_ = op;
        return id;
    }

    ObjectId create_empty(ObjectId parent = 0) {
        Node obj = doc_.make_object(doc_.unique_name("empty", parent));
        set_object_position_(obj, parent ? glm::vec3(0.0f) : spawn_point_());
        const ObjectId id = doc_.add_object(obj, parent, -1, "Create Empty");
        after_structure_change_(id);
        return id;
    }

    /** @brief Creates an object carrying one component of `type` (lights, cameras, ...), named
     *         `name` in snake_case ("Point Light" -> point_light). */
    ObjectId create_with_component(const std::string& type, const std::string& name, ObjectId parent = 0) {
        Node obj = doc_.make_object(doc_.unique_name(snake_case(name), parent));
        set_object_position_(obj, spawn_point_() + glm::vec3(0, 0, type == "Camera" ? 2.0f : 3.0f));
        obj["components"].as_seq().push_back(default_component(type));
        const ObjectId id = doc_.add_object(obj, parent, -1, "Create " + name);
        after_structure_change_(id);
        return id;
    }

    /** @brief The selection minus inherited children (they come from an object asset: not deleted, moved or copied here). */
    std::vector<ObjectId> own_selection_() {
        std::vector<ObjectId> out;
        for (ObjectId id : doc_.selection()) if (!doc_.is_inherited(id)) out.push_back(id);
        if (out.size() != doc_.selection().size()) log_warn("Parts of an object asset stay: open the asset to delete or copy them (or Revert their overrides)");
        return out;
    }

    void delete_selected() {
        if (doc_.selection().empty()) return;
        apply_(doc_.delete_objects(own_selection_()));
    }

    void duplicate_selected() {
        if (doc_.selection().empty()) return;
        const auto own = own_selection_();
        if (own.empty()) return;
        auto ids = doc_.duplicate_objects(own);
        doc_.clear_selection();
        for (ObjectId id : ids) doc_.select(id, true);
        queue_rebuild_();
    }

    /** @brief Which document Ctrl+Z acts on: what the user is looking at / pointing at. */
    enum class UndoTarget { Scene, Mesh, Material, Theme, Config, Clip };
    UndoTarget undo_target_() const {
        if ((mesh_edit_view_() || in_brush_mode_()) && mesh_.open()) return UndoTarget::Mesh;
        // The Timeline's clip: while the panel is under the mouse, or while recording keys.
        if (anim_clip_.open() && show_bottom_ && bottom_view_ == 1 && (timeline_hovered_ || anim_record_)) return UndoTarget::Clip;
        if (active_type_ == AssetType::Material && material_.open()) return UndoTarget::Material;
        if (prop_tab_ == PropTab::Theme && game_theme_.open() && properties_hovered_) return UndoTarget::Theme;
        const bool config_tab = prop_tab_ == PropTab::Render || prop_tab_ == PropTab::Output || prop_tab_ == PropTab::World;
        // Settings rows edit config.yaml or the scene's overrides: undo whichever changed last.
        if (properties_hovered_ && config_tab) return settings_edit_was_scene_ ? UndoTarget::Scene : UndoTarget::Config;
        return UndoTarget::Scene;
    }

    void undo() {
        switch (undo_target_()) {
            case UndoTarget::Mesh: mesh_.do_undo(); break;
            case UndoTarget::Material: material_.do_undo(); refresh_material_preview_(); break;
            case UndoTarget::Theme: game_theme_.do_undo(); theme_changed_(); break;
            case UndoTarget::Config: config_.do_undo(); apply_config_live(); break;
            case UndoTarget::Scene: scene_undo_(); break;
            case UndoTarget::Clip:
                if (const ClipModel* m = anim_clip_.undo.undo()) { anim_clip_.model = *m; anim_save_clip_(); anim_sel_keys_.clear(); }
                break;
        }
    }
    void redo() {
        switch (undo_target_()) {
            case UndoTarget::Mesh: mesh_.do_redo(); break;
            case UndoTarget::Material: material_.do_redo(); refresh_material_preview_(); break;
            case UndoTarget::Theme: game_theme_.do_redo(); theme_changed_(); break;
            case UndoTarget::Config: config_.do_redo(); apply_config_live(); break;
            case UndoTarget::Scene: scene_redo_(); break;
            case UndoTarget::Clip:
                if (const ClipModel* m = anim_clip_.undo.redo()) { anim_clip_.model = *m; anim_save_clip_(); anim_sel_keys_.clear(); }
                break;
        }
    }

    /**
     * @brief Object Mode's Ctrl+Z: ONE timeline across the scene document and every mesh edited
     *        from its objects (the open one and the parked ones), newest step first -- so leaving
     *        Edit Mode does not strand its edits beyond undo. A mesh step undone here is applied
     *        to the live scene and saved, keeping the file in step (it was saved on leaving Edit
     *        Mode). Inside Edit/Sculpt Mode, Ctrl+Z stays on that mesh's own steps.
     */
    void scene_undo_() {
        if (playing()) return;
        uint64_t best = doc_.undo_stack().top_undo_seq();
        fs::path mesh_path;
        if (mesh_.scene_owned && mesh_.undo.top_undo_seq() > best) { best = mesh_.undo.top_undo_seq(); mesh_path = mesh_.path; }
        for (const auto& [p, d] : parked_meshes_) {
            if (d.scene_owned && d.undo.top_undo_seq() > best) { best = d.undo.top_undo_seq(); mesh_path = p; }
        }
        if (best == 0) return;
        if (mesh_path.empty()) { doc_.undo(); queue_rebuild_(); return; }
        if (!activate_scene_mesh_(mesh_path)) return;
        mesh_.do_undo();
        commit_scene_mesh_();
    }

    /** @brief scene_undo_()'s counterpart: re-applies the most recently undone step of that timeline. */
    void scene_redo_() {
        if (playing()) return;
        uint64_t best = 0;
        bool scene = false;
        fs::path mesh_path;
        auto consider = [&](uint64_t seq, bool is_scene, const fs::path& p) {
            if (seq != 0 && (best == 0 || seq < best)) { best = seq; scene = is_scene; mesh_path = p; }
        };
        if (doc_.undo_stack().redo_fresh()) consider(doc_.undo_stack().top_redo_seq(), true, {});
        if (mesh_.scene_owned && mesh_.undo.redo_fresh()) consider(mesh_.undo.top_redo_seq(), false, mesh_.path);
        for (const auto& [p, d] : parked_meshes_) {
            if (d.scene_owned && d.undo.redo_fresh()) consider(d.undo.top_redo_seq(), false, p);
        }
        if (best == 0) return;
        if (scene) { doc_.redo(); queue_rebuild_(); return; }
        if (!activate_scene_mesh_(mesh_path)) return;
        mesh_.do_redo();
        commit_scene_mesh_();
    }

    /** @brief Makes `path`'s mesh history the open one (resuming it from the parked set). */
    bool activate_scene_mesh_(const fs::path& path) {
        if (mesh_.open() && mesh_.path == path) return true;
        try { switch_mesh_doc_(path); } catch (const std::exception& e) { log_error(e.what()); return false; }
        mesh_.scene_owned = true;
        return true;
    }

    /**
     * @brief Object Mode's Shade Smooth / Shade Flat (Blender's): every face of each selected
     *        object's mesh. A mesh several selected objects share is changed once. Each mesh's
     *        change is a step in its own history, so Object Mode's Ctrl+Z undoes it.
     */
    void shade_selected_(bool smooth) {
        if (playing() || asset_view_() || edit_object_) return;
        std::set<fs::path> done;
        for (ObjectId id : doc_.selection()) {
            const Node* obj = doc_.find(id);
            const std::string key = obj ? object_mesh_key_(effective_(*obj)) : std::string();
            if (key.empty()) continue;
            const fs::path path = resolve_mesh_key_(key);
            if (!done.insert(path).second || !coopa::yaml::document_exists(path) || refuse_engine_asset_(path)) continue;
            if (!activate_scene_mesh_(path)) continue;
            mesh_.edit(smooth ? "Shade Smooth" : "Shade Flat", [smooth](EditMesh& m, MeshSelection&) {
                set_smooth(m, MeshSelection{}, smooth);   // the whole mesh, whatever Edit Mode had selected
            });
            commit_scene_mesh_();
        }
    }

    /** @brief After an Object Mode undo/redo of a mesh step: save it and show it on every user. */
    void commit_scene_mesh_() {
        if (!edit_object_) save_mesh();
        force_scene_push_ = true;
        mesh_cache_.clear();
    }

    // --- mesh histories ------------------------------------------------------------------

    /**
     * @brief Moves the open mesh document -- with its whole undo history -- aside, so switching
     *        to another mesh and back keeps it (load() would start a fresh history). A dirty
     *        document is saved first, as switching always did, so parked ones match their files.
     *        The most recent kMaxParkedMeshes are kept.
     */
    void park_mesh_doc_() {
        if (mesh_.open() && mesh_.dirty() && !mesh_.path.empty()) save_mesh();
        if (mesh_.open() && !mesh_.path.empty()) {
            const fs::path p = mesh_.path;
            parked_order_.erase(std::remove(parked_order_.begin(), parked_order_.end(), p), parked_order_.end());
            parked_meshes_.erase(p);
            parked_meshes_.emplace(p, std::move(mesh_));
            parked_order_.push_back(p);
            while (parked_order_.size() > kMaxParkedMeshes) {
                parked_meshes_.erase(parked_order_.front());
                parked_order_.erase(parked_order_.begin());
            }
        }
        mesh_ = MeshDocument{};
    }

    /** @brief Opens `path` as the mesh document: its parked history if the file is unchanged, else a fresh load. */
    void switch_mesh_doc_(const fs::path& path) {
        if (mesh_.open() && mesh_.path == path) return;
        park_mesh_doc_();
        auto it = parked_meshes_.find(path);
        if (it != parked_meshes_.end()) {
            std::error_code ec;
            const auto now = fs::last_write_time(coopa::yaml::resolve_variant(path), ec);
            if (!ec && now == it->second.file_time) {
                mesh_ = std::move(it->second);
                ++mesh_.geometry_revision;
            }
            parked_meshes_.erase(it);
            parked_order_.erase(std::remove(parked_order_.begin(), parked_order_.end(), path), parked_order_.end());
            if (mesh_.open() && mesh_.path == path) return;
        }
        mesh_.load(path);
    }

    /** @brief Frames the selection (or everything) in the viewport. */
    void frame_selected() {
        if (active_type_ == AssetType::UI) { ui_frame_selection_(); return; }
        glm::vec3 lo(1e30f), hi(-1e30f);
        bool any = false;
        if (mesh_edit_view_() && mesh_.open()) {
            const glm::mat4 w = mesh_world_();
            const auto vs = mesh_.selection.affected_vertices(mesh_.mesh);
            auto add = [&](const glm::vec3& p) { const glm::vec3 q(w * glm::vec4(p, 1.0f)); lo = glm::min(lo, q); hi = glm::max(hi, q); };
            if (vs.empty()) for (const auto& p : mesh_.mesh.positions) add(p);
            else for (uint32_t v : vs) add(mesh_.mesh.positions[v]);
            any = true;
        } else {
            for (ObjectId id : doc_.selection()) any |= object_bounds_(id, lo, hi);
        }
        if (any) camera_.frame(lo, hi);
        else frame_all();
    }

    void frame_all() {
        if (active_type_ == AssetType::UI) { ui_fit_ = true; return; }
        glm::vec3 lo(1e30f), hi(-1e30f);
        bool any = false;
        if (asset_view_()) {
            if (active_type_ == AssetType::Mesh) mesh_.mesh.bounds(lo, hi);
            else if (active_type_ == AssetType::Texture) { lo = glm::vec3(-0.55f, -0.55f, 0.0f); hi = glm::vec3(0.55f, 0.55f, 0.0f); }
            else { lo = glm::vec3(-0.6f, -0.6f, -0.1f); hi = glm::vec3(0.6f, 0.6f, 1.1f); }
            any = true;
        } else {
            for (ObjectId id : doc_.all_ids()) any |= object_bounds_(id, lo, hi, /*meshes_only=*/true);
        }
        if (any && glm::length(hi - lo) < 1e5f) camera_.frame(lo, hi);
    }

    // --- play mode ---

    void play() {
        if (playing() || active_type_ != AssetType::Scene) return;   // play runs the open scene
        exit_mesh_mode_();   // the play scene is built from the document; isolation stays in the editor
        deferred_.push_back([this] {
            const std::string anchor = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).string();
            try {
                engine_.assets().reload_changed();   // clip files the Timeline wrote since they were loaded
                coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc_.node(), anchor);
                play_scene_ = &engine_.push_scene(std::move(scene), true, doc_.scene_settings());
                engine_.set_edit_mode(false);
                play_scene_->set_simulating(true);
                // The editor viewport camera held main while the scene started, so hand it to the
                // scene's `main: true` camera (else its first camera), as a standalone run would.
                coopa::gfx::engine::components::CameraComponent* game_cam = nullptr;
                for (auto* c : play_scene_->get_components<coopa::gfx::engine::components::CameraComponent>())
                    if (!game_cam || (c->is_main && !game_cam->is_main)) game_cam = c;
                if (game_cam) game_cam->make_main();
                // The game runs, but gets no input until the viewer is clicked (Esc releases).
                set_game_focus_(false);
                log_info("Play  (click the viewer to control the game, Esc to release)");
            } catch (const std::exception& e) {
                play_scene_ = nullptr;
                log_error(std::string("Play failed: ") + e.what());
            }
        });
    }

    void stop() {
        if (!play_scene_) return;
        set_game_focus_(false);
        engine_.audio().resume_all();   // a paused game's audio must not stay frozen
        engine_.audio().stop_all();
        engine_.remove_scene(play_scene_);
        play_scene_ = nullptr;
        engine_.set_edit_mode(true);          // releases the cursor if the game still held it
        // Back to the engine default (edit mode drives nothing anyway). After set_edit_mode():
        // before it, this would re-capture the cursor for the still-active play scene's camera.
        engine_.set_game_input_focus(true);
        if (sync_.scene()) engine_.activate_scene(sync_.scene());
        camera_.make_main();
        log_info("Stop");
    }

    /** @brief True while the running game owns the keyboard and mouse (see set_game_focus_). */
    bool game_focused() const { return game_focused_; }

    // --- shading / tabs ---

    void set_shading(Shading s) {
        shading_ = s;
        apply_shading_();
    }

    /**
     * @brief Switches what the viewer shows to asset type `t`: leaves mesh modes, swaps the
     *        camera between the scene view and the asset preview, and restores the shading
     *        this asset type last used.
     */
    void set_view_(AssetType t) {
        const bool was_asset = asset_view_();
        shading_by_type_[static_cast<int>(active_type_)] = shading_;
        if (edit_object_ || mode_ != InteractionMode::Object) exit_mesh_mode_();
        if (t != AssetType::UI && ui_interact_) ui_set_interact_(false);
        active_type_ = t;
        set_shading(shading_by_type_[static_cast<int>(t)]);
        prop_tab_ = t == AssetType::Material ? PropTab::Material : t == AssetType::Mesh ? PropTab::Data
                  : t == AssetType::UI ? PropTab::Object : PropTab::Tool;
        const bool now_asset = asset_view_();
        if (was_asset != now_asset) {
            // The asset preview and the scene each keep their own view.
            gizmo_ = Gizmo{};
            CameraPose& out = was_asset ? asset_pose_ : scene_pose_;
            const CameraPose& in = now_asset ? asset_pose_ : scene_pose_;
            out = {camera_.focus, camera_.yaw_deg, camera_.pitch_deg, camera_.distance, camera_.ortho, true};
            if (in.valid && !now_asset) {
                camera_.focus = in.focus; camera_.yaw_deg = in.yaw; camera_.pitch_deg = in.pitch;
                camera_.distance = in.distance; camera_.ortho = in.ortho; camera_.auto_ortho = false;
                camera_.apply();
            }
        }
        deferred_.push_back([this] { ensure_active_scene_(); });
    }

    // =================================================================================
    // Asset actions
    // =================================================================================

    bool open_mesh(const fs::path& path) {
        try {
            switch_mesh_doc_(path);
            mesh_.scene_owned = false;
        } catch (const std::exception& e) {
            log_error(std::string("Open mesh failed: ") + e.what());
            return false;
        }
        set_view_(AssetType::Mesh);
        active_path_ = path;
        uploaded_revision_ = 0;
        deferred_.push_back([this] { ensure_preview_(); frame_all(); });
        log_info("Opened mesh " + project_.relative(path) + " (Tab: Edit Mode)");
        return true;
    }

    void new_mesh(const std::string& primitive) {
        std::string key = primitive;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        for (char& c : key) if (c == ' ') c = '_';
        exit_mesh_mode_();
        park_mesh_doc_();
        mesh_.reset(make_primitive(primitive), unique_asset_name_(new_asset_dir_("meshes"), key));
        if (!creating_tags_.empty()) mesh_.path = project_.assets() / new_asset_dir_("meshes") / (mesh_.name + ".yaml");
        set_view_(AssetType::Mesh);
        active_path_.clear();
        uploaded_revision_ = 0;
        deferred_.push_back([this] { ensure_preview_(); frame_all(); });
    }

    bool save_mesh() {
        if (!mesh_.open()) return false;
        if (mesh_.path.empty()) mesh_.path = project_.assets() / new_asset_dir_("meshes") / (mesh_.name + ".yaml");
        try {
            fs::create_directories(mesh_.path.parent_path());
            mesh_.save();
            project_.refresh();
            // Every user of the file -- render meshes, MeshColliders, water sources -- re-reads
            // it now, not a hot-reload poll later: a Play started this frame must not build its
            // scene from the previous version.
            engine_.assets().reload_changed();
            log_info("Saved mesh " + project_.relative(mesh_.path));
            if (coopa::yaml::document_exists(fs::path(mesh_.path).replace_extension(".lod.yaml"))) {
                log_warn("This mesh has a .lod.yaml sidecar: its simplified LODs regenerate at load, "
                         "but hand-authored LOD meshes may be stale.");
            }
            mesh_cache_.clear();
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Save mesh failed: ") + e.what());
            return false;
        }
    }

    bool open_material(const fs::path& path) {
        // Referenced by its short form (no tag folders): found by name wherever it's tagged.
        const std::string ref = strip_yaml_ext(short_ref(project_.relative(path)));
        try {
            material_.load(path, ref);
        } catch (const std::exception& e) {
            log_error(std::string("Open material failed: ") + e.what());
            return false;
        }
        set_view_(AssetType::Material);
        active_path_ = path;
        uploaded_revision_ = 0;
        deferred_.push_back([this] { ensure_preview_(); refresh_material_preview_(); frame_all(); });
        log_info("Opened material " + ref);
        return true;
    }

    /** @brief Creates materials/<name>.yaml (from `from` if given) and opens it. */
    bool create_material(const std::string& name, const Node& from = Node()) {
        const std::string dir = new_asset_dir_("materials");
        const std::string n = unique_asset_name_(dir, name);
        const fs::path path = project_.assets() / dir / (n + ".yaml");
        Node m = from.is_mapping() ? from : Node::mapping();
        erase_key(m, "base");
        if (m.size() == 0) {
            m["albedo"] = make_color(glm::vec3(0.8f));
            m["metallic"] = make_float(0.0);
            m["roughness"] = make_float(0.5);
        }
        try {
            fs::create_directories(path.parent_path());
            coopa::yaml::save_document(path, m);
            project_.refresh();
        } catch (const std::exception& e) {
            log_error(std::string("Create material failed: ") + e.what());
            return false;
        }
        return open_material(path);
    }

    bool save_material() {
        if (!material_.open()) return false;
        try {
            material_.save();
            log_info("Saved material " + material_.ref);
            queue_rebuild_();   // every renderer referencing it re-parses
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Save material failed: ") + e.what());
            return false;
        }
    }

    // =================================================================================
    // Config actions
    // =================================================================================

    /** @brief Pushes the edited config's live-safe render values into the running renderer. */
    /**
     * @brief Overrides config.yaml's `section.key` for the open scene (null `value`: reverts to
     *        the project's setting) and applies it -- what editing a tinted settings row does.
     */
    void set_scene_setting(const std::string& section, const std::string& key, const Node* value) {
        settings_edit_was_scene_ = true;
        apply_(doc_.set_scene_setting(section, key, value, (value ? "Override " : "Revert ") + section + "." + key));
    }

    /**
     * @brief Applies the edited config.yaml (and the open scene's overrides on top) to the
     *        live renderer. The Engine does the layering -- see Engine::set_config_source().
     */
    void apply_config_live() {
        engine_.set_config_source(config_.node, apply_editor_render_overrides);
        if (coopa::scene::Scene* s = sync_.scene()) engine_.set_scene_settings(*s, doc_.scene_settings());
        refresh_config_debug_view_();
    }

    /** @brief Re-reads the effective debug_view (config + overrides) the shading modes restore. */
    void refresh_config_debug_view_() {
        // The viewport shading mode owns the live debug_view while the editor runs.
        coopa::scene::Scene* s = sync_.scene();
        config_base_debug_view_ = s ? engine_.scene_config(*s).render.debug_view
                                    : core::AppConfig::from_node(config_.node).render.debug_view;
        apply_shading_();
    }

    bool save_config() {
        try {
            config_.save();
            log_info("Saved config.yaml");
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Save config failed: ") + e.what());
            return false;
        }
    }

    /** @brief The edited config, for a renderer restart. */
    core::AppConfig config_for_restart() const { return core::AppConfig::from_node(config_.node); }

    PackageReport package(const fs::path& out_dir, bool keep_yaml = false) {
        PackageOptions opt;
        opt.out_dir = out_dir;
        opt.keep_yaml = keep_yaml;
        opt.engine_assets = fs::path(ROOT_DIR) / "assets";
        opt.library_layers = default_library_layers();
#ifdef TOY_GAME_BINARY
        // This editor's own game executable (cmake/ToyProject.cmake). toyengine's own is the
        // generic player any assets-only project runs on; a game project's binary carries that
        // project's code, so it only ships with that project.
        const fs::path game = TOY_GAME_BINARY;
        std::error_code same_ec;
        const bool generic = std::string(TOY_PROJECT_ROOT) == std::string(ROOT_DIR);
        if (fs::exists(game) && (generic || fs::equivalent(project_.root(), fs::path(TOY_PROJECT_ROOT), same_ec))) {
            opt.game_binary = game;
        }
#else
        const fs::path game = fs::path(ROOT_DIR) / "build" / "toyengine";
        if (fs::exists(game)) opt.game_binary = game;
#endif
        PackageReport rep = package_project(project_, opt);
        if (rep.ok()) log_info("Packaged " + std::to_string(rep.files) + " files (" + std::to_string(rep.encoded) +
                               " encoded) to " + out_dir.string());
        else for (const auto& e : rep.errors) log_error(e);
        return rep;
    }

    /** @brief Picks the document object under a canvas-pixel position (0 = nothing). */
    ObjectId pick_object(glm::vec2 px) {
        const auto vp = view_proj_();
        if (!vp) return 0;
        glm::vec3 o, d;
        vp->ray(px, o, d);
        ObjectId best = 0, best_marker = 0;
        float best_t = 1e30f, best_marker_px = 1e30f, best_marker_t = 1e30f;
        for (const auto& [id, live] : sync_.live_objects()) {
            const Node* node = doc_.find(id);
            if (!node || !live || !live->get_transform()) continue;
            // Not drawn, not clickable: hidden (H, isolation, `active: false`) here or above.
            bool shown = true;
            for (const auto* o = live; o && shown; o = o->parent()) shown = o->active();
            if (!shown) continue;
            const glm::mat4 world = live->get_transform()->transform().get_world_matrix();
            if (const CachedMesh* cm = mesh_for_object_(*node)) {
                const glm::mat4 inv = glm::inverse(world);
                const glm::vec3 lo_ = glm::vec3(inv * glm::vec4(o, 1.0f));
                const glm::vec3 ld = glm::vec3(inv * glm::vec4(d, 0.0f));
                if (!ray_aabb(lo_, ld, cm->lo - glm::vec3(1e-3f), cm->hi + glm::vec3(1e-3f))) continue;
                for (const auto& f : cm->mesh.faces) {
                    for (size_t k = 1; k + 1 < f.corners.size(); ++k) {
                        auto t = ray_triangle(lo_, ld, cm->mesh.positions[f.corners[0].v], cm->mesh.positions[f.corners[k].v],
                                              cm->mesh.positions[f.corners[k + 1].v]);
                        if (!t) continue;
                        const float wt = glm::dot(glm::vec3(world * glm::vec4(lo_ + ld * *t, 1.0f)) - o, d);
                        if (wt < best_t) { best_t = wt; best = id; }
                    }
                }
            } else {
                // No mesh: pick by its origin marker on screen.
                const auto p = vp->project(glm::vec3(world[3]));
                if (p && glm::distance(*p, px) < 8.0f && glm::distance(*p, px) < best_marker_px) {
                    best_marker_px = glm::distance(*p, px);
                    best_marker = id;
                    best_marker_t = glm::dot(glm::vec3(world[3]) - o, d);
                }
            }
        }
        // A marker clicked dead-on wins unless a surface hides it; otherwise surfaces take
        // priority over markers.
        if (best_marker && (best == 0 || (best_marker_px < 4.0f && best_marker_t < best_t))) return best_marker;
        return best;
    }

private:
    // =================================================================================
    // Small helpers
    // =================================================================================

    void push_log_(int level, const std::string& s) {
        log_.push_back({level, s});
        if (log_.size() > 200) log_.pop_front();
        status_ = s;
        status_level_ = level;
        status_time_ = std::chrono::steady_clock::now();
        if (level >= 1) std::fprintf(stderr, "[editor] %s\n", s.c_str());
    }

    /**
     * @brief A free asset name for a new file in assets/<dir>: `base` in snake_case, then
     *        base_01, base_02... Free means no asset of the same TYPE has the name, in any tag
     *        folder -- references find assets by type and name (coopa::asset::AssetIndex).
     */
    std::string unique_asset_name_(const std::string& dir, const std::string& raw_base) {
        std::string base = snake_case(raw_base);
        if (base.empty()) base = "untitled";
        const std::string type = asset_type_dir(dir + "/x");
        auto exists = [&](const std::string& n) {
            return coopa::yaml::document_exists(project_.assets() / dir / (n + ".yaml")) || asset_name_taken_(type, n);
        };
        if (!exists(base)) return base;
        for (int i = 1; i < 1000; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "_%02d", i);
            if (!exists(base + buf)) return base + buf;
        }
        return base;
    }

    static void set_object_position_(Node& obj, const glm::vec3& p) {
        for (auto& c : obj["components"].as_seq()) if (component_type(c) == "Transform") c["position"] = make_vec3(p);
    }

    /** @brief Where new objects appear: the camera focus, snapped to the grid, on the ground. */
    glm::vec3 spawn_point_() const { return cursor3d_; }

    InspectorEnv inspector_env_() {
        InspectorEnv env;
        env.list_assets = [this](const std::string& dir, const std::string& ext) {
            std::vector<std::string> out = project_.list(dir, ext);
            // toyengine's assets resolve under the project's: offer them too (while shown).
            if (show_engine_assets_) for (const auto& e : project_.list_engine(dir, ext)) out.push_back(e);
            // Scene-local assets (assets/scenes/<name>/meshes/...) resolve first at load time.
            if (!doc_.path().empty()) {
                const fs::path local = doc_.path().parent_path() / dir;
                std::error_code ec;
                if (fs::is_directory(local, ec)) {
                    for (const auto& e : fs::directory_iterator(local, ec)) {
                        if (!e.is_regular_file()) continue;
                        const std::string fe = e.path().extension().string();
                        const std::string fname = e.path().filename().string();
                        if (fname.find(".lod.") != std::string::npos) continue;
                        if (!ext.empty() && fe != ext && !(ext == ".yaml" && fe == ".caml")) continue;
                        const std::string rel = dir + "/" + fname;
                        if (std::find(out.begin(), out.end(), rel) == out.end()) out.push_back(rel);
                    }
                }
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        // ChildRef fields (a Slider's fill, a reactor's target): the selected object's
        // descendants first -- what such a name usually refers to -- then every other object.
        env.object_names = [this] {
            std::vector<std::string> out;
            const ObjectId sel = doc_.primary();
            std::function<void(const Node&)> walk = [&](const Node& o) {
                if (!o.contains("children") || !o.at("children").is_sequence()) return;
                for (const auto& c : o.at("children").as_seq()) { out.push_back(get_string(c, "name")); walk(c); }
            };
            if (const Node* s = sel ? doc_.find(sel) : nullptr) walk(*s);
            doc_.for_each_object([&](const Node& o, int) {
                const std::string n = get_string(o, "name");
                if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
            });
            return out;
        };
        return env;
    }

    // =================================================================================
    // Document -> live scene
    // =================================================================================

    void rebuild_scene_() {
        stop();   // set_scene() below replaces every engine scene, a playing one included
        mesh_cache_.clear();
        // Composites fall back to the library's active theme when no Theme component is above
        // them; reset it so one file's theme never leaks into the next.
        coopa::ui::ThemeLibrary::instance().set_active(coopa::ui::UITheme::builtin_dark());
        apply_theme_preview_();   // an open theme's unsaved edits shadow its file (ui/theme_editor.inl)
        // Instances' inherited children get their nodes first (ui/instances.inl): the ids the
        // live objects map to. Asset files may have changed, so the resolved view is stale too.
        sync_instance_placeholders_();
        ++scene_gen_;
        sync_.rebuild(engine_, doc_);
        if (!sync_.last_error.empty()) { log_error("Scene: " + sync_.last_error); sync_.last_error.clear(); }
        add_object_view_lights_();
        ui_after_rebuild_();
        preview_scene_ = nullptr;   // set_scene() replaced every engine scene
        preview_object_ = nullptr;
        preview_ground_ = nullptr;
        uploaded_revision_ = 0;
        scene_uploaded_revision_ = 0;   // re-show an unsaved edited mesh on the new live objects
        if (edit_object_ && !doc_.find(edit_object_)) exit_mesh_mode_();
        for (auto it = hidden_.begin(); it != hidden_.end();) it = doc_.find(*it) ? std::next(it) : hidden_.erase(it);
        for (auto it = isolated_.begin(); it != isolated_.end();) it = doc_.find(*it) ? std::next(it) : isolated_.erase(it);
        ensure_active_scene_();
        apply_hidden_();
    }

    void queue_rebuild_() {
        if (rebuild_queued_) return;
        rebuild_queued_ = true;
        deferred_.push_back([this] { rebuild_queued_ = false; rebuild_scene_(); });
    }

    /** @brief Applies a document change to the live scene (deferred to a safe point). */
    void apply_(Change c) {
        if (c.scope == ChangeScope::None) return;
        // A change inside a prefab instance (an override) rebuilds that instance, not the scene:
        // SceneSync rebuilds an outermost instance root from its node alone.
        if (c.scope == ChangeScope::Object && c.object) {
            if (const ObjectId root = doc_.outermost_instance_of(c.object)) c.object = root;
        }
        if (c.scope == ChangeScope::Settings) {   // config overrides: re-apply, rebuild nothing
            if (coopa::scene::Scene* s = sync_.scene()) engine_.set_scene_settings(*s, doc_.scene_settings());
            if (play_scene_) engine_.set_scene_settings(*play_scene_, doc_.scene_settings());   // render: live
            refresh_config_debug_view_();
            return;
        }
        if (c.scope == ChangeScope::Structure) { queue_rebuild_(); return; }
        if (c.scope == ChangeScope::Rect && sync_.patch_rect(doc_, c.object)) return;   // likewise
        if (c.scope == ChangeScope::Transform) {   // cheap and safe immediately
            sync_.apply(engine_, doc_, c);
            return;
        }
        deferred_.push_back([this, c] {
            if (rebuild_queued_) return;
            sync_.apply(engine_, doc_, c);
            apply_hidden_();               // the rebuilt objects start visible
            scene_uploaded_revision_ = 0;  // ...and from their files: re-show an unsaved edited mesh
        });
    }

    void after_structure_change_(ObjectId select) {
        doc_.select(select);
        queue_rebuild_();
    }

    /** @brief Makes the scene the current tab wants the engine's active scene. */
    void ensure_active_scene_() {
        coopa::scene::Scene* want = nullptr;
        if (asset_view_()) {
            ensure_preview_();
            want = preview_scene_;
        } else {
            want = play_scene_ ? play_scene_ : sync_.scene();
        }
        if (want && engine_.has_scene() && &engine_.scene() != want) engine_.activate_scene(want);
        if (!play_scene_ || asset_view_()) camera_.make_main();
        apply_shading_();
    }

    // =================================================================================
    // Asset preview scene
    // =================================================================================

    coopa::gfx::engine::components::MeshRenderer* preview_renderer_() {
        return preview_object_ ? preview_object_->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    }

    /** @brief Uploads `m` as the preview object's mesh (a runtime asset, no file). */
    void upload_preview_mesh_(const EditMesh& m, const std::string& id) {
        auto* mr = preview_renderer_();
        if (!mr || m.faces.empty()) return;
        try {
            auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(mesh_to_node(m));
            auto mesh = std::make_shared<coopa::gfx::engine::data::Mesh>(
                coopa::gfx::engine::data::Mesh::from_cpu(engine_.device(), engine_.allocator(), std::move(cpu)));
            mr->set_mesh(engine_.assets().create<coopa::gfx::engine::data::Mesh>(id, std::move(mesh)));
        } catch (const std::exception& e) {
            log_error(std::string("Preview upload failed: ") + e.what());
        }
    }

    void refresh_material_preview_() {
        auto* mr = preview_renderer_();
        if (!mr) return;
        coopa::scene::SceneLoader::ParseContext ctx;
        ctx.scene_path = (project_.assets() / "__preview__.yaml").string();
        ctx.scene_dir = project_.assets().string();
        ctx.search_dirs = {ctx.scene_dir};
        mr->material = coopa::gfx::engine::components::PBRMaterial{};
        try {
            if (active_type_ == AssetType::Material) {
                coopa::gfx::engine::components::parse_material_value_(material_.node, mr->material, engine_.assets(), ctx);
            } else {
                Node m = Node::mapping();
                m["albedo"] = make_color(glm::vec3(0.72f));
                m["roughness"] = make_float(0.55);
                coopa::gfx::engine::components::parse_material_value_(m, mr->material, engine_.assets(), ctx);
            }
        } catch (const std::exception& e) {
            log_error(std::string("Material: ") + e.what());
        }
    }

    // =================================================================================
    // Frame hooks
    // =================================================================================

    void post_late_update_(float dt) {
        poll_theme_(dt);
        poll_build_();
        // The cursor the UI asked for this frame (resize arrows on panel borders, I-beam in text).
        const coopa::input::CursorShape want = canvas_->context().mouse_cursor();
        if (want != applied_cursor_) { engine_.input_state().set_cursor_shape(want); applied_cursor_ = want; }
        // Run queued actions; actions may queue more (run those next frame).
        auto pending = std::move(deferred_);
        deferred_.clear();
        for (auto& fn : pending) fn();
        // Keep live objects showing an edited, unsaved mesh; keep hidden objects hidden.
        if (!asset_view_() && !playing()) { push_mesh_to_scene_(); apply_hidden_(); }
        if (game_focused_ && (!playing() || asset_view_())) set_game_focus_(false);
        // Unity's Step: one simulated tick, then paused again.
        if (step_countdown_ > 0 && --step_countdown_ == 0 && play_scene_) play_scene_->set_simulating(false);
        if (pause_after_start_ && play_scene_) { pause_after_start_ = false; play_scene_->set_simulating(true); step_countdown_ = 2; }
        // Keep the asset preview's mesh current.
        if (asset_view_() && preview_scene_) update_preview_();
    }

    /** @brief Scales the UI to the display's points (2x framebuffer pixels on Retina). */
    void sync_ui_scale_() {
        ui_scale_ = std::max(1.0f, engine_.display_scale());
        if (auto* canvas = canvas_canvas_()) canvas->scaler.scale_factor = ui_scale_;
    }

    /** @brief Places the scene image in viewport_box_ (framebuffer pixels). */
    void sync_display_region_() {
        if (viewport_box_.empty()) return;
        render::LetterboxRect r;
        r.x = static_cast<int32_t>(viewport_box_.x * ui_scale_);
        r.y = static_cast<int32_t>(viewport_box_.y * ui_scale_);
        r.w = static_cast<uint32_t>(std::max(1.0f, viewport_box_.w * ui_scale_));
        r.h = static_cast<uint32_t>(std::max(1.0f, viewport_box_.h * ui_scale_));
        engine_.set_display_region(r);
    }

    void pre_render_(float dt) {
        sync_ui_scale_();
        sync_display_region_();
        update_scene_ui_placement_();
        if (!playing() || asset_view_()) camera_.make_main();
        sculpt_frame_();
        paint_frame_();
        timeline_frame_(dt);
        // X-Ray: surfaces turn translucent over the backdrop (Solid / Material Preview; paint
        // modes look through the surface they paint on, so they stay opaque).
        engine_.render_config().editor_xray_alpha = xray_surfaces_() ? xray_alpha_ : 1.0f;
        if (grid_wanted_) push_grid_lines_();
        if (!playing()) push_particle_gizmos_();
    }

    /**
     * @brief The emission shape of every selected ParticleSystem, as X-ray lines -- Unity's cone
     *        gizmo: a cone's base and spread, a sphere's three great circles, a box, a circle, an
     *        edge. A mesh emitter needs none (its mesh is the shape); every system also shows the
     *        bounds of its live particles, dimmer.
     */
    void push_particle_gizmos_() {
        using toy::particles::EmitShape;
        std::vector<render::DebugLine>& out = engine_.pipeline().debug_lines();
        const uint32_t shape_col = 0xE0'40'B0'FFu;   // ABGR bytes: warm orange, mostly opaque
        const uint32_t bounds_col = 0x60'40'B0'FFu;
        for (ObjectId id : doc_.selection()) {
            auto it = sync_.live_objects().find(id);
            if (it == sync_.live_objects().end() || !it->second) continue;
            auto* ps = it->second->get_component<toy::particles::ParticleSystem>();
            auto* tc = it->second->get_transform();
            if (!ps || !tc) continue;
            const glm::mat4 m = tc->transform().get_world_matrix();
            auto line = [&](glm::vec3 a, glm::vec3 b, uint32_t c) {
                out.push_back(render::DebugLine{glm::vec3(m * glm::vec4(a, 1.0f)), glm::vec3(m * glm::vec4(b, 1.0f)), c, false});
            };
            auto circle = [&](glm::vec3 centre, glm::vec3 u, glm::vec3 v, float r, float arc_deg) {
                const int seg = 32;
                const float arc = glm::radians(std::clamp(arc_deg, 0.0f, 360.0f));
                for (int i = 0; i < seg; ++i) {
                    const float a0 = arc * i / seg, a1 = arc * (i + 1) / seg;
                    line(centre + (u * std::cos(a0) + v * std::sin(a0)) * r, centre + (u * std::cos(a1) + v * std::sin(a1)) * r, shape_col);
                }
            };
            const auto& sh = ps->settings.shape;
            const glm::vec3 o = sh.offset, X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
            switch (sh.type) {
                case EmitShape::Cone: {
                    const float spread = std::tan(glm::radians(std::clamp(sh.angle_deg, 0.0f, 89.0f)));
                    const float h = 1.0f;
                    circle(o, X, Y, sh.radius, sh.arc_deg);
                    circle(o + Z * h, X, Y, sh.radius + spread * h, sh.arc_deg);
                    for (int k = 0; k < 4; ++k) {
                        const float a = glm::radians(90.0f * k);
                        const glm::vec3 d(std::cos(a), std::sin(a), 0.0f);
                        line(o + d * sh.radius, o + d * (sh.radius + spread * h) + Z * h, shape_col);
                    }
                    break;
                }
                case EmitShape::Sphere:
                case EmitShape::Hemisphere:
                    circle(o, X, Y, sh.radius, 360.0f);
                    circle(o, X, Z, sh.radius, sh.type == EmitShape::Hemisphere ? 180.0f : 360.0f);
                    circle(o, Y, Z, sh.radius, sh.type == EmitShape::Hemisphere ? 180.0f : 360.0f);
                    break;
                case EmitShape::Circle:
                    circle(o, X, Y, sh.radius, sh.arc_deg);
                    break;
                case EmitShape::Box: {
                    const glm::vec3 h = sh.box * 0.5f;
                    for (int e = 0; e < 12; ++e) {
                        const int axis = e / 4, a = (e & 1) ? 1 : -1, b = (e & 2) ? 1 : -1;
                        glm::vec3 p0(0.0f), p1(0.0f);
                        p0[axis] = -h[axis]; p1[axis] = h[axis];
                        p0[(axis + 1) % 3] = p1[(axis + 1) % 3] = a * h[(axis + 1) % 3];
                        p0[(axis + 2) % 3] = p1[(axis + 2) % 3] = b * h[(axis + 2) % 3];
                        line(o + p0, o + p1, shape_col);
                    }
                    break;
                }
                case EmitShape::Edge:
                    line(o - X * (0.5f * sh.length), o + X * (0.5f * sh.length), shape_col);
                    break;
                case EmitShape::Point:
                    for (const glm::vec3& d : {X, Y, Z}) line(o - d * 0.1f, o + d * 0.1f, shape_col);
                    break;
                case EmitShape::Mesh:
                    break;
            }
            if (ps->particle_count() > 0) {
                const glm::vec3 lo = ps->bounds_min(), hi = ps->bounds_max();
                for (int e = 0; e < 12; ++e) {
                    const int axis = e / 4;
                    glm::vec3 p0((e & 1) ? hi.x : lo.x, (e & 2) ? hi.y : lo.y, lo.z);
                    if (axis == 0) p0 = glm::vec3(lo.x, (e & 1) ? hi.y : lo.y, (e & 2) ? hi.z : lo.z);
                    if (axis == 1) p0 = glm::vec3((e & 1) ? hi.x : lo.x, lo.y, (e & 2) ? hi.z : lo.z);
                    glm::vec3 p1 = p0;
                    p1[axis] = hi[axis];
                    out.push_back(render::DebugLine{p0, p1, bounds_col, false});
                }
            }
        }
    }

    /**
     * @brief Keeps the open scene's screen-space UI (a HUD) inside the viewer rather than
     *        across the whole editor window: in the scene view it covers the rendered image
     *        (and reacts only while the running game has focus); the UI designer places it in
     *        its preview frame (see ui_canvas.inl).
     */
    void update_scene_ui_placement_() {
        if (active_type_ == AssetType::UI) { engine_.set_scene_ui_placement(ui_preview_placement_()); return; }
        if (asset_view_() || viewport_box_.empty()) { engine_.set_scene_ui_placement(std::nullopt); return; }
        core::Engine::ScreenUiPlacement p;
        p.rect = engine_.display_rect();
        p.input = playing() && game_focused_;
        engine_.set_scene_ui_placement(p);
    }

    coopa::ui::CanvasComponent* canvas_canvas_() {
        coopa::scene::SceneObject* o = canvas_->owner ? canvas_->owner->parent() : nullptr;
        return o ? o->get_component<coopa::ui::CanvasComponent>() : nullptr;
    }

    /** @brief The picker's toyengine bits: the project (and its assets/) as Favorites, toyengine's
     *         YAML / caml documents named as such, and this session's folders under Recent. */
    void setup_file_dialog_() {
        file_dialog_.favorites = {
            {project_.name(), project_.root(), imm::Icon::Package, "This project"},
            {"assets", project_.assets(), imm::Icon::Folder, "This project's assets"},
        };
        file_dialog_.kind_name = [](const std::string& e) -> std::string {
            if (e == ".yaml" || e == ".yml") return "toyengine Document";
            if (e == ".caml") return "toyengine Document (caml)";
            return {};
        };
        file_dialog_.format_group = [](const std::string& e) -> std::string {
            return e == ".yaml" || e == ".yml" || e == ".caml" ? "toyengine Document" : std::string();
        };
        file_dialog_.on_folder_used = [this](const fs::path& dir, FileDialog::Mode) {
            auto& r = file_dialog_.recent_dirs;
            r.erase(std::remove(r.begin(), r.end(), dir), r.end());
            r.insert(r.begin(), dir);
            if (r.size() > 8) r.resize(8);
        };
    }

    /**
     * @brief Loads a theme by id and applies it to the UI (Style, font, editor colours). On
     *        failure the current look stays and the error goes to the console.
     */
    bool load_theme_(const std::string& id) {
        fs::path path = editor_themes_dir() / (id + ".yaml");
        try {
            imm::Theme t = imm::load_theme(path);
            theme_ = std::move(t);
        } catch (const std::exception& e) {
            log_error(std::string("Theme: ") + e.what());
            return false;
        }
        theme_id_ = id;
        et_ = editor_theme_from(theme_);
        imm::Context& ctx = canvas_->context();
        ctx.style = theme_.style;
        const std::string font = theme_.font.empty() ? default_font_ : theme_.font;
        if (font != current_font_) {
            if (auto* f = coopa::ui::UIResourceCache::instance().font_for_path(font)) { ctx.text.set_font(f); current_font_ = font; }
        }
        theme_stamp_ = themes_stamp_();
        return true;
    }

    /** @brief Newest modification time across the themes folder (a base theme may change too). */
    static fs::file_time_type themes_stamp_() {
        fs::file_time_type t{};
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(editor_themes_dir(), ec)) t = std::max(t, e.last_write_time(ec));
        return t;
    }

    /** @brief Hot reload: re-applies the active theme when any theme file changes on disk. */
    void poll_theme_(float dt) {
        theme_poll_ += dt;
        if (theme_poll_ < 0.5f) return;
        theme_poll_ = 0.0f;
        if (themes_stamp_() != theme_stamp_ && load_theme_(theme_id_)) log_info("Theme reloaded: " + theme_.name);
        theme_stamp_ = themes_stamp_();   // a broken edit isn't retried until the next save
    }

    /**
     * @brief Scroll and pinch over the viewport, as Blender reads them.
     *   Wheel:     zoom; Shift pans vertically, Ctrl horizontally; sideways (tilt) orbits.
     *              Deltas are used as-is, so macOS smooth scrolling zooms smoothly.
     *   Trackpad:  two-finger swipe orbits (both ways, so the view can tilt up / down),
     *              Shift+swipe pans, Ctrl+swipe zooms, pinch zooms; the momentum coast
     *              after the fingers lift is ignored, so the view stops with them.
     */
    void scroll_navigate_(const imm::FrameInput& in, double pinch, bool shift, bool ctrl) {
        const auto& tp = trackpad::state();
        const bool trackpad = trackpad_override_ ? *trackpad_override_ : tp.scroll_is_trackpad;
        if (pinch != 0.0) camera_.dolly(static_cast<float>(pinch) * 7.5f);   // +10% spread ~ one notch closer
        if (in.scroll == glm::vec2(0.0f)) return;
        if (trackpad) {
            if (tp.momentum && !trackpad_override_) return;
            if (shift) camera_.pan(in.scroll * glm::vec2(-20.0f, -20.0f), viewport_box_.h);
            else if (ctrl) camera_.dolly(in.scroll.y);
            else camera_.orbit(in.scroll * -6.0f);
            return;
        }
        if (in.scroll.y != 0.0f) {
            if (shift) camera_.pan(glm::vec2(0.0f, in.scroll.y * -20.0f), viewport_box_.h);
            else if (ctrl) camera_.pan(glm::vec2(in.scroll.y * -20.0f, 0.0f), viewport_box_.h);
            else camera_.dolly(in.scroll.y);
        }
        if (in.scroll.x != 0.0f && !shift && !ctrl) camera_.orbit(glm::vec2(in.scroll.x * -6.0f, 0.0f));
    }

    /** @brief X-Ray is on and the shading has surfaces to see through (Blender: not in Rendered). */
    bool xray_surfaces_() const {
        return xray_ && !playing() && !in_brush_mode_() &&
               (shading_ == Shading::Solid || shading_ == Shading::MaterialPreview);
    }

    /**
     * @brief The grid's current spacing: the smallest 1 / 5 x 10^k (down to 1 mm) keeping lines
     *        ~25+ px apart at the camera's zoom. Ctrl / Snap moves in these units, so -- as in
     *        Blender -- zooming in gives a finer grid and finer snapping.
     */
    float grid_step_() const {
        const float target = std::max(camera_.distance / 20.0f, 0.001f);
        const float base = std::pow(10.0f, std::floor(std::log10(target)));
        if (target <= base * 1.0001f) return base;
        return target <= base * 5.0001f ? base * 5.0f : base * 10.0f;
    }

    /** @brief The world axis (0 X, 1 Y, 2 Z) the grid is drawn perpendicular to: the view axis in
     *         an axis-aligned view (numpad 1 / 3: the XZ / YZ planes, as Blender), else Z (floor). */
    int grid_normal_axis_() const {
        const glm::vec3 f = camera_.forward();
        if (std::abs(f.x) > 0.999f) return 0;
        if (std::abs(f.y) > 0.999f) return 1;
        return 2;
    }

    /**
     * @brief The grid, as 3D lines in the scene's line pass -- occluded by geometry, as Blender's
     *        is. The floor (Z = 0) normally; looking straight down X or Y, the plane facing the
     *        view instead (through the origin in perspective; in ortho, behind the scene, as
     *        Blender's backdrop grid). Faint, adaptive spacing (grid_step_()), fading out with
     *        distance from the focus point; the axes through the origin tinted.
     */
    void push_grid_lines_() {
        auto pack = [](glm::vec4 c) {
            auto b = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(c.a) << 24);
        };
        std::vector<render::DebugLine>& out = engine_.pipeline().debug_lines();
        const imm::Style& st = canvas_->context().style;
        const glm::vec4* axis_col[3] = {&st.axis_x, &st.axis_y, &st.axis_z};
        const float step = grid_step_();
        const float half = step * 30.0f;
        const bool depth_tested = !xray_surfaces_();   // X-Ray: seen through the translucent surfaces
        // The plane: normal axis `nax`, spanned by `ua` / `va`, at `w` along the normal.
        const int nax = grid_normal_axis_();
        const int ua = nax == 0 ? 1 : 0, va = nax == 2 ? 1 : 2;
        const bool aligned_ortho = camera_.ortho && (nax != 2 || std::abs(camera_.forward().z) > 0.999f);
        const float w = aligned_ortho ? camera_.focus[nax] + camera_.forward()[nax] * camera_.distance * 10.0f : 0.0f;
        auto at3 = [&](float u, float v) { glm::vec3 p; p[ua] = u; p[va] = v; p[nax] = w; return p; };
        const glm::vec2 f2(camera_.focus[ua], camera_.focus[va]);
        const glm::vec2 c = glm::round(f2 / step) * step;
        const int n = 30;
        auto seg_line = [&](glm::vec2 a, glm::vec2 b, glm::vec4 col) {
            // Segmented so the radial fade (per-vertex alpha) stays smooth.
            const int k = 24;
            auto at = [&](int i) {
                const glm::vec2 p = glm::mix(a, b, i / float(k));
                const float fade = 1.0f - std::min(1.0f, glm::length(p - f2) / half);
                return std::pair{at3(p.x, p.y), pack(imm::with_alpha(col, col.a * fade))};
            };
            auto prev = at(0);
            for (int i = 1; i <= k; ++i) {
                auto cur = at(i);
                out.push_back(render::DebugLine{prev.first, cur.first, prev.second, depth_tested});
                prev = cur;
            }
        };
        for (int i = -n; i <= n; ++i) {
            const float u = c.x + i * step, v = c.y + i * step;
            const bool major_u = std::abs(std::remainder(u, step * 10.0f)) < step * 0.01f;
            const bool major_v = std::abs(std::remainder(v, step * 10.0f)) < step * 0.01f;
            glm::vec4 cu = major_u ? et_.viewport.grid_major : et_.viewport.grid;   // the line u = const runs along v
            glm::vec4 cv = major_v ? et_.viewport.grid_major : et_.viewport.grid;
            if (std::abs(u) < step * 0.01f) cu = imm::with_alpha(*axis_col[va], 0.6f);
            if (std::abs(v) < step * 0.01f) cv = imm::with_alpha(*axis_col[ua], 0.6f);
            seg_line({u, c.y - half}, {u, c.y + half}, cu);
            seg_line({c.x - half, v}, {c.x + half, v}, cv);
        }
    }

    void apply_shading_() {
        std::string view;
        switch (shading_) {
            case Shading::Wireframe: view = "wireframe"; break;
            // Painting shows its colours / weights in full: Solid tints only lightly by albedo,
            // so a paint mode looks at Solid through Material Preview's studio rig instead.
            case Shading::Solid: view = paint_active_ ? "material_preview" : "solid"; break;
            case Shading::MaterialPreview: view = "material_preview"; break;
            case Shading::Full: view = full_debug_view_.empty() ? std::string("off") : full_debug_view_; break;
        }
        engine_.render_config().debug_view = view;
        engine_.render_config().editor_ssao = viewport_ao_;
    }

public:
    /** @brief The Viewport Shading popover's "Ambient Occlusion" option (saved in prefs). */
    void set_viewport_ao(bool on) {
        viewport_ao_ = on;
        apply_shading_();
        Node prefs = Project::load_prefs();
        prefs["viewport_ao"] = Node(on);
        Project::save_prefs(prefs);
    }
    bool viewport_ao() const { return viewport_ao_; }

    /** @brief Local-space bounds of the geometry an object's outline is drawn from (see
     *        mesh_for_object_()); false for an object with no mesh. */
    bool outline_bounds(ObjectId id, glm::vec3& lo, glm::vec3& hi) {
        const Node* node = doc_.find(id);
        const CachedMesh* cm = node ? mesh_for_object_(*node) : nullptr;
        if (!cm) return false;
        lo = cm->lo;
        hi = cm->hi;
        return true;
    }

private:

    // =================================================================================
    // Viewport math
    // =================================================================================

    std::optional<ViewProj> view_proj_() {
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        if (!cam) return std::nullopt;
        const uint32_t rw = engine_.pipeline().render_width(), rh = engine_.pipeline().render_height();
        const render::LetterboxRect box = engine_.display_rect();
        ViewProj vp;
        vp.view = cam->get_view_matrix();
        vp.proj = cam->get_projection_matrix(static_cast<float>(rw) / std::max(1u, rh));
        vp.rect = imm::Box{box.x / ui_scale_, box.y / ui_scale_, box.w / ui_scale_, box.h / ui_scale_};
        vp.ortho = cam->type == coopa::gfx::engine::components::CameraType::Orthographic;
        return vp;
    }

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
    const CachedMesh* mesh_for_object_(const Node& obj) {
        if (!obj.contains("components")) return nullptr;
        {
            const std::string key = object_mesh_key_(effective_(obj));   // MeshRenderer's, else a WaterBody's (an instance's: its asset's)
            if (key.empty()) return nullptr;
            const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
            const std::string resolved = engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string());
            auto it = mesh_cache_.find(resolved);
            if (it != mesh_cache_.end()) return it->second.mesh.faces.empty() ? nullptr : &it->second;
            CachedMesh cm;
            if (mesh_.open() && !mesh_.path.empty() && coopa::yaml::resolve_variant(resolved) == mesh_.path) {
                cm.mesh = mesh_.mesh;
                cm.edges = cm.mesh.edges();
                cm.mesh.bounds(cm.lo, cm.hi);
                auto& slot = mesh_cache_[resolved] = std::move(cm);
                return slot.mesh.faces.empty() ? nullptr : &slot;
            }
            try {
                if (coopa::yaml::document_exists(resolved)) {
                    cm.mesh = mesh_from_node(coopa::yaml::load_document(coopa::yaml::resolve_variant(resolved)));
                    cm.edges = cm.mesh.edges();
                    cm.mesh.bounds(cm.lo, cm.hi);
                }
            } catch (...) {}
            auto& slot = mesh_cache_[resolved] = std::move(cm);
            return slot.mesh.faces.empty() ? nullptr : &slot;
        }
        return nullptr;
    }

    bool object_bounds_(ObjectId id, glm::vec3& lo, glm::vec3& hi, bool meshes_only = false) {
        auto* live = sync_.live(id);
        const Node* node = doc_.find(id);
        if (!live || !node || !live->get_transform()) return false;
        const glm::mat4 world = live->get_transform()->transform().get_world_matrix();
        if (const CachedMesh* cm = mesh_for_object_(*node)) {
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 p((i & 1) ? cm->hi.x : cm->lo.x, (i & 2) ? cm->hi.y : cm->lo.y, (i & 4) ? cm->hi.z : cm->lo.z);
                const glm::vec3 w = glm::vec3(world * glm::vec4(p, 1.0f));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
            return true;
        }
        if (meshes_only) return false;
        const glm::vec3 p(world[3]);
        lo = glm::min(lo, p - glm::vec3(0.5f));
        hi = glm::max(hi, p + glm::vec3(0.5f));
        return true;
    }

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

    // --- helpers shared by the panels ---

    void extract_material_(ObjectId id, const Node& comp) {
        const std::string base = get_string(*doc_.find(id), "name", "material");
        std::string name = base;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        for (char& ch : name) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        const std::string n = unique_asset_name_("materials", name);
        const fs::path path = project_.assets() / "materials" / (n + ".yaml");
        Node m = comp.at("material");
        erase_key(m, "base");
        try {
            coopa::yaml::save_document(path, m);
            project_.refresh();
        } catch (const std::exception& e) {
            log_error(std::string("Extract failed: ") + e.what());
            return;
        }
        Node c = comp;
        c["material"] = Node("materials/" + n);
        edit_component_(id, c, "Extract Material");
        log_info("Extracted material to materials/" + n + ".yaml");
    }

    void add_mesh_to_scene_(const std::string& item, std::optional<glm::vec3> at = std::nullopt) {
        if (playing()) return;
        std::string key = fs::path(item).stem().string();
        Node obj = doc_.make_object(doc_.unique_name(key));
        set_object_position_(obj, at.value_or(spawn_point_()));
        Node mr = default_component("MeshRenderer");
        mr["mesh_path"] = Node(mesh_ref(item));
        obj["components"].as_seq().push_back(mr);
        after_structure_change_(doc_.add_object(obj, 0, -1, "Add " + key));
    }

    void assign_material_(const std::string& item, ObjectId only = 0) {
        if (playing()) return;
        std::string ref = item.substr(0, item.size() - fs::path(item).extension().string().size());
        std::vector<ObjectId> targets = only ? std::vector<ObjectId>{only} : doc_.selection();
        for (ObjectId id : targets) {
            const int ci = doc_.find_component(id, "MeshRenderer");
            if (ci < 0) continue;
            Node c = doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
            c["material"] = Node(ref);
            apply_(doc_.set_component(id, ci, c, "Assign Material"));
        }
    }

    void draw_mesh_tools_(imm::Context& ctx) {
        auto& md = mesh_;
        std::string name = md.name;
        if (ctx.input_text("Name", &name) && !name.empty()) md.name = name;
        ctx.label_dim(md.path.empty() ? std::string("(not saved yet: meshes/") + md.name + ".yaml)" : project_.relative(md.path));
        ctx.label_dim(std::to_string(md.mesh.positions.size()) + " verts  " + std::to_string(md.mesh.faces.size()) + " faces  " +
                      std::to_string(md.mesh.triangle_count()) + " tris" + (md.dirty() ? "  (modified)" : ""));
        if (ctx.button("Save Mesh", 120)) save_mesh();
        ctx.same_line();
        if (ctx.button("Make Object Asset", 150)) {
            if (md.path.empty() || md.dirty()) save_mesh();
            if (!md.path.empty()) create_object_asset_from_mesh_(project_.relative(md.path));
        }
        ctx.tooltip("Make Object Asset\nWrites objects/<mesh>.yaml: an object with a MeshRenderer using this mesh, ready to place in scenes");
        ctx.spacing();
        if (ctx.collapsing_header("Select")) {
            if (ctx.button("All (A)", 90)) md.selection.select_all(md.mesh);
            ctx.same_line();
            if (ctx.button("None", 70)) md.selection.clear();
            ctx.same_line();
            if (ctx.button("Linked (L)", 90)) select_linked(md.mesh, md.selection);
        }
        if (ctx.collapsing_header("Modelling")) {
            ctx.drag_float("Extrude dist", &extrude_dist_, 0.01f, -100.0f, 100.0f);
            if (ctx.button("Extrude (Ctrl+E)", -1)) extrude_();
            ctx.drag_float("Inset amount", &inset_amount_, 0.005f, 0.0f, 0.99f);
            if (ctx.button("Inset (I)", -1)) md.edit("Inset", [&](EditMesh& m, MeshSelection& s) { inset_faces(m, s, inset_amount_); });
            ctx.drag_float("Bevel width", &bevel_width_, 0.005f, 0.0f, 10.0f);
            if (ctx.button("Bevel (Ctrl+B)", -1)) md.edit("Bevel", [&](EditMesh& m, MeshSelection& s) { bevel_edges(m, s, bevel_width_); });
            if (ctx.button("Delete (X)", -1)) md.edit("Delete", [](EditMesh& m, MeshSelection& s) { delete_selection(m, s); });
            if (ctx.button("Merge at Center (M)", -1)) md.edit("Merge", [](EditMesh& m, MeshSelection& s) { merge_at_center(m, s); });
            ctx.drag_float("Merge distance", &merge_dist_, 0.0005f, 0.0f, 10.0f, "%.4f");
            if (ctx.button("Merge by Distance", -1)) {
                size_t removed = 0;
                md.edit("Merge by Distance", [&](EditMesh& m, MeshSelection& s) { removed = merge_by_distance(m, s, merge_dist_); });
                log_info("Merged " + std::to_string(removed) + " vertices");
            }
            if (ctx.button("Flip Normals", -1)) md.edit("Flip Normals", [](EditMesh& m, MeshSelection& s) { flip_normals(m, s); });
            if (ctx.button("Shade Smooth", -1)) md.edit("Shade Smooth", [](EditMesh& m, MeshSelection& s) { set_smooth(m, s, true); });
            if (ctx.button("Shade Flat", -1)) md.edit("Shade Flat", [](EditMesh& m, MeshSelection& s) { set_smooth(m, s, false); });
        }
        if (ctx.collapsing_header("UVs")) {
            ctx.drag_float("Box scale", &uv_scale_, 0.01f, 0.001f, 1000.0f);
            if (ctx.button("Box Project", -1)) md.edit("Box UV", [&](EditMesh& m, MeshSelection& s) { uv_box_project(m, s, uv_scale_); });
            if (ctx.button("Planar X", 80)) md.edit("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 0); });
            ctx.same_line();
            if (ctx.button("Planar Y", 80)) md.edit("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 1); });
            ctx.same_line();
            if (ctx.button("Planar Z", 80)) md.edit("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 2); });
        }
        if (ctx.collapsing_header("Selection Transform", false)) {
            const auto vs = md.selection.affected_vertices(md.mesh);
            if (!vs.empty()) {
                glm::vec3 c = selection_center(md.mesh, md.selection);
                glm::vec3 nc = c;
                if (ctx.drag_floatn("Center", &nc.x, 3, 0.01f)) {
                    const glm::vec3 d = nc - c;
                    md.edit("Move Vertices", [&](EditMesh& m, MeshSelection& s) { translate_selection(m, s, d); }, "sel_center");
                }
                if (ctx.last_deactivated()) md.undo.end_merge();
            } else {
                ctx.label_dim("Nothing selected.");
            }
        }
    }

    void extrude_() {
        auto& md = mesh_;
        if (md.selection.mode == SelectMode::Edge) {
            glm::vec3 n(0, 0, extrude_dist_);
            md.edit("Extrude Edges", [&](EditMesh& m, MeshSelection& s) { extrude_edges(m, s, n); });
        } else {
            md.edit("Extrude", [&](EditMesh& m, MeshSelection& s) { extrude_faces(m, s, extrude_dist_); });
        }
    }

    void draw_material_editor_(imm::Context& ctx) {
        auto& md = material_;
        ctx.label_dim(md.ref + (md.dirty() ? "  (modified)" : ""));
        if (ctx.button("Save Material", 130)) save_material();
        ctx.same_line();
        if (ctx.button("Assign to Selected", 150, true, imm::Icon::Material)) assign_material_(md.ref + ".yaml");
        ctx.spacing();
        Node before = md.node;
        InspectorEnv env = inspector_env_();
        EditResult r = draw_material_block(ctx, md.node, env);
        if (r.changed) {
            md.commit("Edit " + r.key, before, r.active ? "m:" + r.key : std::string());
            refresh_material_preview_();
        }
        if (r.finished) md.undo.end_merge();
    }

    /**
     * @brief True if `f` (a key of config.yaml's `section`) can be overridden by the open scene:
     *        render and physics keys that apply live, while a scene (not an object asset) is open.
     *        Startup-only keys and every other section stay project-wide.
     */
    bool scene_overridable_(const std::string& section, const FieldDesc& f) const {
        return (section == "render" || section == "physics") && !f.startup_only &&
               active_type_ == AssetType::Scene && !doc_.is_object_asset();
    }

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
    void draw_setting_row_(imm::Context& ctx, const FieldDesc& f, Node& section, const InspectorEnv& env, const std::string& section_name) {
        using I = imm::Icon;
        ctx.push_id(f.key);
        const bool layered = scene_overridable_(section_name, f);
        const Node* over = layered ? doc_.scene_setting(section_name, f.key) : nullptr;
        const bool in_config = section.contains(f.key);

        const glm::vec2 top = ctx.cursor();
        const imm::Box band{top.x - 4.0f, top.y, ctx.available_width() + 4.0f, ctx.style.row_height};
        if (over) {
            ctx.fill(band, et_.chrome.setting_override);
            ctx.fill(imm::Box{band.x, band.y + 1.0f, 2.0f, band.h - 2.0f}, et_.chrome.setting_override_bar);
        }

        if (layered) {
            // Edit a view of the project's section with this scene's value on top.
            Node view = section;
            if (over) view[f.key] = *over;
            const EditResult r = draw_field(ctx, f, view, env);
            if (over) ctx.tooltip(f.display() + "\nOverridden by this scene" + (in_config ? "" : " (the project uses the default)") +
                                  "\nRight-click to revert or apply to the project");
            if (r.changed) {
                const Node* value = view.contains(f.key) ? &view.at(f.key) : nullptr;
                settings_edit_was_scene_ = true;
                apply_(doc_.set_scene_setting(section_name, f.key, value, "Override " + section_name + "." + f.key,
                                              r.active ? "set:" + section_name + "." + f.key : std::string()));
            }
            if (r.finished) doc_.end_merge();
        } else {
            const Node before = config_.node;
            const EditResult r = draw_field(ctx, f, section, env);
            if (r.changed) commit_setting_(before, section_name, f, r.active);
            if (r.finished) config_.undo.end_merge();
        }

        const imm::Box row{band.x, band.y, band.w, std::max(band.h, ctx.cursor().y - top.y)};
        if (ctx.is_hovered(row) && ctx.input().released[1]) ctx.open_popup("setting_ctx");
        if (ctx.begin_popup("setting_ctx", 230)) {
            if (layered) {
                if (ctx.menu_item("Revert to Project Setting", "", nullptr, over != nullptr, I::Restart)) {
                    settings_edit_was_scene_ = true;
                    apply_(doc_.set_scene_setting(section_name, f.key, nullptr, "Revert " + section_name + "." + f.key));
                }
                if (ctx.menu_item("Apply to Project Settings", "", nullptr, over != nullptr, I::Save)) {
                    // Into config.yaml (the project's value), then drop the now-redundant override.
                    const Node value = *over;
                    const Node before = config_.node;
                    section[f.key] = value;
                    commit_setting_(before, section_name, f, false);
                    settings_edit_was_scene_ = true;
                    apply_(doc_.set_scene_setting(section_name, f.key, nullptr, "Apply " + section_name + "." + f.key + " to project"));
                }
                ctx.separator();
            }
            if (ctx.menu_item("Reset to Default", "", nullptr, in_config, I::Restart)) {
                const Node before = config_.node;
                erase_key(section, f.key);
                commit_setting_(before, section_name, f, false);
            }
            ctx.tooltip("Reset to Default\nRemoves the key from config.yaml so its quality preset / engine default applies");
            ctx.end_popup();
        }
        ctx.pop_id();
    }

    /** @brief Records a config.yaml edit of `section_name.f.key` and applies it live. */
    void commit_setting_(const Node& before, const std::string& section_name, const FieldDesc& f, bool merging) {
        settings_edit_was_scene_ = false;
        config_.commit("Edit " + section_name + "." + f.key, before, merging ? "cfg:" + f.key : std::string());
        if (section_name == "render" || section_name == "physics") apply_config_live();
        if (f.startup_only) log_info(f.key + " changes on renderer restart");
    }

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
    glm::mat4 mesh_world_() {
        if (!asset_view_() && edit_object_) {
            if (auto* live = sync_.live(edit_object_); live && live->get_transform()) return live->get_transform()->transform().get_world_matrix();
        }
        return glm::mat4(1.0f);
    }

    void draw_viewport_(imm::Context& ctx, const imm::Box& area, bool mesh_edit) {
        if (active_type_ == AssetType::UI) { draw_ui_view_(ctx, area); return; }   // the UI designer (ui/ui_canvas.inl)
        const imm::Box hb = area_header_(ctx, area);
        draw_viewport_header_(ctx, hb, mesh_edit);
        viewport_box_ = imm::Box{area.x, hb.bottom(), area.w, std::max(1.0f, area.h - hb.h)};
        // Now, not only in pre_render_: the bars below (and picking) read the display rect, and
        // one from last frame's box leaves a sliver of the black overlay clear uncovered.
        sync_display_region_();
        ctx.push_clip(viewport_box_);
        // In fill mode the render matches the panel; bars only show while a resize waits out
        // the pipeline-rebuild debounce (Engine::update_fill_extent_). Painted like Blender's
        // viewport background rather than leaving them black.
        if (auto vpr = view_proj_()) {
            const imm::Box r = vpr->rect.intersect(viewport_box_);
            if (!r.empty()) {
                const glm::vec4 bars{0.23f, 0.23f, 0.23f, 1.0f};
                ctx.fill({viewport_box_.x, viewport_box_.y, viewport_box_.w, r.y - viewport_box_.y}, bars);
                ctx.fill({viewport_box_.x, r.bottom(), viewport_box_.w, viewport_box_.bottom() - r.bottom()}, bars);
                ctx.fill({viewport_box_.x, r.y, r.x - viewport_box_.x, r.h}, bars);
                ctx.fill({r.right(), r.y, viewport_box_.right() - r.right(), r.h}, bars);
            }
        }
        handle_viewport_input_(ctx, mesh_edit);
        draw_viewport_overlay_(ctx, mesh_edit);
        if (modal_.active()) {
            std::string h = modal_.header();
            if (proportional_live_()) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "   Proportional: %.3f (%s, wheel resizes)", proportional_.radius,
                              falloff_name(proportional_.falloff));
                h += buf;
            }
            const imm::Box b{viewport_box_.x + (show_toolbar_ ? kToolSize + 20 : 10), viewport_box_.bottom() - 34, ctx.text_width(h) + 18, 24};
            ctx.fill_rounded(b, et_.viewport.modal_backdrop);
            ctx.text_in(b, h, ctx.style.text);
        }
        if (playing() && !asset_view_()) {
            const std::string h = game_focused_ ? "Esc to release the mouse" : "Click to control the game";
            const float w = ctx.text_width(h) + 18;
            const imm::Box b{viewport_box_.center().x - w * 0.5f, viewport_box_.y + 8, w, 24};
            ctx.fill_rounded(b, et_.viewport.modal_backdrop);
            ctx.text_in(b, h, ctx.style.text);
        }
        // Furniture last, so it draws over the overlays (its rects were excluded from input).
        draw_last_op_panel_(ctx, viewport_box_);
        if (show_toolbar_ && !preview_only_view_()) draw_toolbar_(ctx, mesh_edit);
        if (show_overlays_ || true) draw_nav_gizmo_(ctx);
        if (show_sidebar_) draw_sidebar_(ctx, mesh_edit);
        ctx.pop_clip();
        draw_viewport_popups_(ctx, mesh_edit);
        // Asset drops into the viewport.
        if (!mesh_edit && !asset_view_()) {
            if (auto dropped = ctx.drop_target("asset", viewport_box_)) {
                const std::string& item = *dropped;
                switch (asset_type_of_(item)) {
                    case AssetType::Object: place_object_asset(item, ground_point_(ctx.mouse())); break;
                    case AssetType::UI: place_ui_asset(item); break;   // a HUD / menu the scene starts with
                    case AssetType::Mesh: add_mesh_to_scene_(item, ground_point_(ctx.mouse())); break;
                    case AssetType::Material: {
                        const ObjectId hit = pick_object(ctx.mouse());
                        if (hit) assign_material_(item, hit);
                        break;
                    }
                    default: open_asset(asset_type_of_(item), item); break;
                }
            }
        } else if (active_type_ == AssetType::Mesh) {
            // A material dropped on the mesh previews on the slot under the cursor.
            if (auto dropped = ctx.drop_target("asset", viewport_box_); dropped && asset_type_of_(*dropped) == AssetType::Material) {
                const std::string ref = dropped->substr(0, dropped->size() - fs::path(*dropped).extension().string().size());
                uint32_t slot = 0;
                if (auto vp2 = view_proj_()) {
                    glm::vec3 o, d;
                    mesh_local_ray_(*vp2, ctx.mouse(), o, d);
                    if (auto hit = mesh_bvh_().raycast(mesh_.mesh, o, d)) slot = mesh_.mesh.faces[hit->face].slot;
                }
                if (slot_preview_materials_.size() <= slot) slot_preview_materials_.resize(slot + 1);
                slot_preview_materials_[slot] = ref;
                apply_slot_preview_materials_();
            }
        }
    }

    /** @brief Where a ray through `px` meets the ground plane (z = 0), or the 3D cursor. */
    glm::vec3 ground_point_(glm::vec2 px) {
        auto vp = view_proj_();
        if (!vp) return spawn_point_();
        glm::vec3 o, d;
        vp->ray(px, o, d);
        if (std::abs(d.z) < 1e-5f) return spawn_point_();
        const float t = -o.z / d.z;
        if (t < 0) return spawn_point_();
        const glm::vec3 p = o + d * t;
        return glm::vec3(std::round(p.x * 4) / 4, std::round(p.y * 4) / 4, 0.0f);
    }

    /** @brief The first surface under `px` (objects' meshes), else the ground plane. */
    glm::vec3 surface_point_(glm::vec2 px) {
        auto vp = view_proj_();
        if (!vp) return spawn_point_();
        glm::vec3 o, d;
        vp->ray(px, o, d);
        float best = 1e30f;
        for (const auto& [id, live] : sync_.live_objects()) {
            const Node* node = doc_.find(id);
            if (!node || !live || !live->active() || !live->get_transform()) continue;
            const CachedMesh* cm = mesh_for_object_(*node);
            if (!cm) continue;
            const glm::mat4 world = live->get_transform()->transform().get_world_matrix();
            for (const auto& f : cm->mesh.faces) {
                for (size_t k = 1; k + 1 < f.corners.size(); ++k) {
                    const glm::vec3 a = glm::vec3(world * glm::vec4(cm->mesh.positions[f.corners[0].v], 1));
                    const glm::vec3 b = glm::vec3(world * glm::vec4(cm->mesh.positions[f.corners[k].v], 1));
                    const glm::vec3 c = glm::vec3(world * glm::vec4(cm->mesh.positions[f.corners[k + 1].v], 1));
                    if (auto t = ray_triangle(o, d, a, b, c); t && *t < best) best = *t;
                }
            }
        }
        if (best < 1e29f) return o + d * best;
        if (std::abs(d.z) > 1e-5f && -o.z / d.z > 0) return o + d * (-o.z / d.z);
        return spawn_point_();
    }

    void handle_viewport_input_(imm::Context& ctx, bool mesh_edit) {
        const auto& in = ctx.input();
        const glm::vec2 m = ctx.mouse();
        const bool alt = has(in.mods, Mods::Alt);
        const bool shift = has(in.mods, Mods::Shift);
        const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
        // The toolbar, navigation gizmo and sidebar are drawn after this (on top); clicks on
        // them must not select or navigate.
        const bool hovered = ctx.is_hovered(viewport_box_) && !over_viewport_chrome_(m, mesh_edit);
        viewport_hovered_ = hovered;
        // Pinches are taken every frame, so one made over another panel never lands here later.
        const double pinch = trackpad::take_magnify() + std::exchange(pinch_override_, 0.0);
        // Play mode: a click in the viewer hands the game the keyboard and mouse.
        if (playing() && !asset_view_()) {
            if (!game_focused_ && hovered && in.pressed[0]) set_game_focus_(true);
            return;
        }
        const auto vp = view_proj_();
        if (!vp) return;

        // --- a running modal operator owns all input ---
        if (modal_.active()) {
            run_modal_(ctx, *vp, ctrl, shift);
            return;
        }
        if (pending_extrude_ && mesh_edit) {
            pending_extrude_ = false;
            extrude_interactive_(*vp, ctx.is_hovered(viewport_box_) ? m : viewport_box_.center());
            return;
        }
        if (pending_modal_kind_ != ModalKind::None && (hovered || ctx.is_hovered(viewport_box_))) {
            const ModalKind k = pending_modal_kind_;
            pending_modal_kind_ = ModalKind::None;
            start_modal_(k, *vp, m, mesh_edit, pending_modal_axis_);
            pending_modal_axis_.reset();
            return;
        }

        // --- Alt+click loop / ring select (edit mode); Alt+drag still navigates ---
        if (mesh_edit && hovered && in.pressed[0] && alt && !loopcut_.active) {
            alt_click_ = true;
            alt_press_ = m;
            alt_shift_ = shift;
            alt_ctrl_ = ctrl;
        }
        if (alt_click_) {
            if (in.released[0]) {
                alt_click_ = false;
                if (glm::distance(m, alt_press_) <= 4.0f) loop_select_at_(alt_press_, alt_ctrl_, alt_shift_);
                return;
            }
            if (!in.down[0]) { alt_click_ = false; }
            else if (glm::distance(m, alt_press_) > 4.0f) {
                alt_click_ = false;
                nav_active_ = true;
                nav_button_ = 0;
                nav_mode_ = alt_shift_ ? 1 : alt_ctrl_ ? 2 : 0;
            } else {
                return;
            }
        }

        // --- navigation ---
        const bool nav_press = hovered && (in.pressed[2] || (in.pressed[0] && alt && !mesh_edit));
        if (nav_press) {
            nav_active_ = true;
            nav_button_ = in.pressed[2] ? 2 : 0;
            nav_mode_ = shift ? 1 : ctrl ? 2 : 0;
        }
        if (nav_active_) {
            if (!in.down[nav_button_]) nav_active_ = false;
            else if (in.mouse_delta != glm::vec2(0.0f)) {
                if (nav_mode_ == 1) camera_.pan(in.mouse_delta, viewport_box_.h);
                else if (nav_mode_ == 2) camera_.dolly(-in.mouse_delta.y * 0.05f);
                else camera_.orbit(in.mouse_delta);
            }
            return;
        }
        if (loopcut_.active) {
            if (mesh_edit) run_loop_cut_(ctx, *vp, hovered);
            else loopcut_.active = false;
            return;
        }
        if (in_brush_mode_()) {
            // Scroll navigation stays (below is skipped), so handle it here first.
            if (hovered && !ctx.popup_hovered() && !stroke_.active && !paint_stroke_.active) scroll_navigate_(in, pinch, shift, ctrl);
            if (hovered && in.pressed[1]) ctx.open_popup("mode_menu_vp", m);
            if (in_sculpt_mode_()) handle_sculpt_input_(ctx, *vp, hovered);
            else handle_paint_input_(ctx, *vp, hovered);
            return;
        }
        if (hovered && !ctx.popup_hovered()) scroll_navigate_(in, pinch, shift, ctrl);

        // --- gizmo (the Move / Rotate / Scale tools) ---
        bool gizmo_took_mouse = false;
        const bool can_edit = !(playing() && !asset_view_());
        if (tool_ != Tool::Select && show_gizmo_ && can_edit) {
            gizmo_.mode = tool_ == Tool::Move ? GizmoMode::Translate : tool_ == Tool::Rotate ? GizmoMode::Rotate : GizmoMode::Scale;
            glm::vec3 pivot;
            glm::mat3 basis(1.0f);
            if (gizmo_target_(mesh_edit, pivot, basis)) {
                if (snap_adaptive_) gizmo_.translate_snap = grid_step_();
                const GizmoDelta g = gizmo_.update(*vp, pivot, basis, m, in.pressed[0] && !alt, in.down[0], in.released[0], ctrl != snap_on_,
                                                   hovered && !ctx.popup_hovered());
                gizmo_took_mouse = g.active || gizmo_.hot_axis() >= 0;
                if (g.started) begin_transform_(mesh_edit);
                if (g.active) {
                    const int kind = gizmo_.mode == GizmoMode::Translate ? 0 : gizmo_.mode == GizmoMode::Rotate ? 1 : 2;
                    apply_transform_(mesh_edit, kind, g.translate, g.rotate_axis, g.rotate_deg, g.scale, basis);
                    if (mesh_edit && proportional_.enabled) {
                        draw_proportional_circle_(ctx, *vp, prop_pivot_ + (kind == 0 ? g.translate : glm::vec3(0.0f)));
                    }
                }
                if (g.finished) { if (mesh_edit) mesh_.undo.end_merge(); else doc_.end_merge(); }
            }
        }

        // --- 3D cursor (Shift+RMB) and context menu (RMB) ---
        if (hovered && in.pressed[1]) {
            if (shift) cursor3d_ = surface_point_(m);
            else ctx.open_popup(mesh_edit ? "vp_mesh_ctx" : "vp_obj_ctx", m);
        }

        // --- click / box select ---
        if (tool_ == Tool::Cursor && hovered && in.pressed[0] && !alt) {
            cursor3d_ = surface_point_(m);   // the Cursor tool: click places the 3D cursor
            return;
        }
        if (!gizmo_took_mouse && hovered && in.pressed[0] && !alt) {
            select_press_ = m;
            select_pending_ = true;
        }
        if (select_pending_ && in.down[0] && glm::distance(m, select_press_) > 5.0f) box_selecting_ = true;
        if (select_pending_ && in.released[0]) {
            if (box_selecting_) box_select_(mesh_edit, select_press_, m, shift, ctrl);
            else if (mesh_edit) pick_mesh_element_(m, shift);
            else {
                const ObjectId id = click_target_(pick_object(m));   // an instance as a whole first
                if (id == 0) { if (!shift) doc_.clear_selection(); }
                else if (shift) {
                    // Blender: shift-click makes an unselected object active-selected, and an
                    // already active one deselected.
                    if (doc_.is_selected(id) && doc_.primary() == id) doc_.select(id, true);
                    else { if (doc_.is_selected(id)) doc_.select(id, true); doc_.select(id, true); }
                } else doc_.select(id);
            }
            select_pending_ = box_selecting_ = false;
        }
        if (!in.down[0] && !in.released[0]) select_pending_ = box_selecting_ = false;
        if (box_selecting_) {
            const imm::Box r{std::min(select_press_.x, m.x), std::min(select_press_.y, m.y),
                             std::abs(m.x - select_press_.x), std::abs(m.y - select_press_.y)};
            ctx.fill(r, et_.viewport.box_select_fill);
            ctx.outline(r, et_.viewport.box_select_outline);
        }

        if (hovered && !ctx.wants_keyboard() && !ctx.any_popup_open()) viewport_keymap_(ctx, *vp, mesh_edit, can_edit);
    }

    /** @brief Keys that act while the mouse is over the viewport (Blender's 3D View keymap). */
    void viewport_keymap_(imm::Context& ctx, const ViewProj& vp, bool mesh_edit, bool can_edit) {
        const glm::vec2 m = ctx.mouse();
        // Blender's I in Object Mode: key the selected rig objects on the Timeline's clip.
        if (!mesh_edit && can_edit && !in_brush_mode_() && anim_rig_ && ctx.shortcut(Key::I)) anim_insert_keys_();
        // --- views (any mode) ---
        if (ctx.shortcut(Key::Kp1)) camera_.axis_view('f', false);
        if (ctx.shortcut(Key::Kp1, Mods::Control)) camera_.axis_view('f', true);
        if (ctx.shortcut(Key::Kp3)) camera_.axis_view('r', false);
        if (ctx.shortcut(Key::Kp3, Mods::Control)) camera_.axis_view('r', true);
        if (ctx.shortcut(Key::Kp7)) camera_.axis_view('t', false);
        if (ctx.shortcut(Key::Kp7, Mods::Control)) camera_.axis_view('t', true);
        if (ctx.shortcut(Key::Kp5)) camera_.set_ortho(!camera_.ortho);
        if (ctx.shortcut(Key::Kp0)) view_through_scene_camera_();
        if (ctx.shortcut(Key::Kp0, Mods::Control | Mods::Alt)) align_scene_camera_to_view_();
        if (ctx.shortcut(Key::Kp4)) camera_.orbit(glm::vec2(15.0f / 0.35f, 0));
        if (ctx.shortcut(Key::Kp6)) camera_.orbit(glm::vec2(-15.0f / 0.35f, 0));
        if (ctx.shortcut(Key::Kp8)) camera_.orbit(glm::vec2(0, -15.0f / 0.35f));
        if (ctx.shortcut(Key::Kp2)) camera_.orbit(glm::vec2(0, 15.0f / 0.35f));
        if (ctx.shortcut(Key::KpAdd) || ctx.shortcut(Key::Equal)) camera_.dolly(1.0f);
        if (ctx.shortcut(Key::KpSubtract) || ctx.shortcut(Key::Minus)) camera_.dolly(-1.0f);
        // Frame Selected: numpad . (Blender), . on the main keyboard (no numpad needed) in every
        // mode, and F where F is free -- Edit Mode's F is Make Face, the brush modes' F is radius.
        if (ctx.shortcut(Key::KpDecimal) || ctx.shortcut(Key::Period)) frame_selected();
        if (!mesh_edit && !in_brush_mode_() && ctx.shortcut(Key::F)) frame_selected();
        if (ctx.shortcut(Key::Home)) frame_all();
        if (ctx.shortcut(Key::GraveAccent)) ctx.open_popup("vp_view_menu", m);
        if (ctx.shortcut(Key::Z)) ctx.open_popup("vp_shading_menu", m);
        if (ctx.shortcut(Key::Z, Mods::Alt)) {
            xray_ = !xray_;
            log_info(xray_ ? "X-Ray on (Alt Z toggles)" : "X-Ray off");
        }
        if (ctx.shortcut(Key::Z, Mods::Shift)) set_shading(shading_ == Shading::Wireframe ? Shading::Solid : Shading::Wireframe);
        if (ctx.shortcut(Key::N)) show_sidebar_ = !show_sidebar_;
        if (ctx.shortcut(Key::T)) show_toolbar_ = !show_toolbar_;
        if (ctx.shortcut(Key::B)) tool_ = Tool::Select;
        if (ctx.shortcut(Key::Space, Mods::Control)) maximized_ = !maximized_;
        if (ctx.shortcut(Key::S, Mods::Shift)) ctx.open_popup("vp_snap_menu", m);
        if (ctx.shortcut(Key::C, Mods::Shift)) { cursor3d_ = glm::vec3(0.0f); frame_all(); }
        if (ctx.shortcut(Key::Space, Mods::Shift)) ctx.open_popup("vp_tool_menu", m);
        if (ctx.shortcut(Key::Tab, Mods::Control)) ctx.open_popup("mode_menu_vp", m);   // Blender's mode pie
        if (!can_edit) return;

        if (mesh_edit) {
            auto& md = mesh_;
            // Switching select mode converts the selection; pressing the current mode is a no-op.
            auto set_mode = [&](SelectMode sm) { if (md.selection.mode != sm) convert_selection(md.mesh, md.selection, sm); };
            if (ctx.shortcut(Key::Num1)) set_mode(SelectMode::Vertex);
            if (ctx.shortcut(Key::Num2)) set_mode(SelectMode::Edge);
            if (ctx.shortcut(Key::Num3)) set_mode(SelectMode::Face);
            if (ctx.shortcut(Key::A)) md.selection.select_all(md.mesh);
            if (ctx.shortcut(Key::A, Mods::Alt)) md.selection.clear();
            if (ctx.shortcut(Key::I, Mods::Control)) invert_mesh_selection_();
            if (ctx.shortcut(Key::L, Mods::Control)) select_linked(md.mesh, md.selection);
            if (ctx.shortcut(Key::L)) { pick_mesh_element_(m, true); select_linked(md.mesh, md.selection); }
            if (ctx.shortcut(Key::G)) start_modal_(ModalKind::Grab, vp, m, true);
            if (ctx.shortcut(Key::R)) start_modal_(ModalKind::Rotate, vp, m, true);
            if (ctx.shortcut(Key::S)) start_modal_(ModalKind::Scale, vp, m, true);
            if (ctx.shortcut(Key::O)) toggle_proportional();
            if (ctx.shortcut(Key::O, Mods::Shift)) cycle_proportional_falloff();
            if (ctx.shortcut(Key::E)) extrude_interactive_(vp, m);
            if (ctx.shortcut(Key::I)) start_modal_(ModalKind::Inset, vp, m, true);
            if (ctx.shortcut(Key::B, Mods::Control)) start_modal_(ModalKind::Bevel, vp, m, true);
            if (ctx.shortcut(Key::F)) { bool ok = false; md.edit("Make Face", [&](EditMesh& mm, MeshSelection& s) { ok = fill_face(mm, s); }); if (!ok) log_warn("Make Face needs 3+ selected vertices"); }
            if (ctx.shortcut(Key::D, Mods::Shift)) {
                md.edit("Duplicate", [](EditMesh& mm, MeshSelection& s) { duplicate_faces(mm, s); });
                start_modal_(ModalKind::Grab, vp, m, true);
            }
            if (ctx.shortcut(Key::R, Mods::Control)) begin_loop_cut();
            if (ctx.shortcut(Key::T, Mods::Control)) md.edit("Triangulate", [](EditMesh& mm, MeshSelection& s) { triangulate(mm, s); });
            if (ctx.shortcut(Key::J, Mods::Alt)) md.edit("Tris to Quads", [](EditMesh& mm, MeshSelection& s) { tris_to_quads(mm, s); });
            if (ctx.shortcut(Key::E, Mods::Control)) bridge_selected_();
            if (ctx.shortcut(Key::A, Mods::Shift)) ctx.open_popup("vp_mesh_add", m);
            if (ctx.shortcut(Key::W)) ctx.open_popup("vp_mesh_ctx", m);
            if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) ctx.open_popup("vp_mesh_delete", m);
            if (ctx.shortcut(Key::M)) ctx.open_popup("vp_mesh_merge", m);
            if (ctx.shortcut(Key::M, Mods::Control)) ctx.open_popup("vp_mesh_mirror", m);
            if (ctx.shortcut(Key::U)) ctx.open_popup("vp_mesh_uv", m);
            if (ctx.shortcut(Key::N, Mods::Alt)) ctx.open_popup("vp_mesh_normals", m);
            if (ctx.shortcut(Key::Tab)) toggle_edit_mode_();
            return;
        }
        if (active_type_ == AssetType::Mesh && ctx.shortcut(Key::Tab)) toggle_edit_mode_();
        if (asset_view_()) return;
        if (ctx.shortcut(Key::A)) { doc_.clear_selection(); for (ObjectId id : visible_ids_()) doc_.select(id, true); }
        if (ctx.shortcut(Key::A, Mods::Alt)) doc_.clear_selection();
        if (ctx.shortcut(Key::I, Mods::Control)) {
            std::vector<ObjectId> inv;
            for (ObjectId id : visible_ids_()) if (!doc_.is_selected(id)) inv.push_back(id);
            doc_.clear_selection();
            for (ObjectId id : inv) doc_.select(id, true);
        }
        if (ctx.shortcut(Key::G)) start_modal_(ModalKind::Grab, vp, m, false);
        if (ctx.shortcut(Key::R)) start_modal_(ModalKind::Rotate, vp, m, false);
        if (ctx.shortcut(Key::S)) start_modal_(ModalKind::Scale, vp, m, false);
        if (ctx.shortcut(Key::G, Mods::Alt)) clear_transform_(0);
        if (ctx.shortcut(Key::R, Mods::Alt)) clear_transform_(1);
        if (ctx.shortcut(Key::S, Mods::Alt)) clear_transform_(2);
        if (ctx.shortcut(Key::D, Mods::Shift) && !doc_.selection().empty()) {
            duplicate_selected();
            pending_modal_kind_ = ModalKind::Grab;
        }
        if (ctx.shortcut(Key::X)) { if (!doc_.selection().empty()) ctx.open_popup("vp_delete_confirm", m); }
        if (ctx.shortcut(Key::Delete)) delete_selected();
        if (ctx.shortcut(Key::A, Mods::Shift)) ctx.open_popup("vp_add_menu", m);
        if (ctx.shortcut(Key::H)) hide_(doc_.selection());
        if (ctx.shortcut(Key::H, Mods::Shift)) {
            std::vector<ObjectId> others;
            for (ObjectId id : doc_.all_ids()) if (!doc_.is_selected(id) && !doc_.is_ancestor(id, doc_.primary())) others.push_back(id);
            hide_(others);
        }
        if (ctx.shortcut(Key::H, Mods::Alt)) unhide_all_();
        if (ctx.shortcut(Key::P, Mods::Control)) parent_selection_to_active_();
        if (ctx.shortcut(Key::P, Mods::Alt)) ctx.open_popup("vp_clear_parent", m);
        if (ctx.shortcut(Key::Tab)) toggle_edit_mode_();
    }

    /** @brief Blender's context-sensitive popups, declared every frame (opened by the keymap). */
    void draw_viewport_popups_(imm::Context& ctx, bool mesh_edit) {
        auto vp = view_proj_();
        const glm::vec2 m = ctx.mouse();
        if (open_add_menu_) { open_add_menu_ = false; ctx.open_popup("vp_add_menu", m); }
        if (ctx.begin_popup("mode_menu_vp", 170)) {
            draw_mode_menu_items_(ctx);
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_view_menu", 170)) {
            if (ctx.menu_item("Front", "Num1")) camera_.axis_view('f', false);
            if (ctx.menu_item("Back", "Ctrl Num1")) camera_.axis_view('f', true);
            if (ctx.menu_item("Right", "Num3")) camera_.axis_view('r', false);
            if (ctx.menu_item("Left", "Ctrl Num3")) camera_.axis_view('r', true);
            if (ctx.menu_item("Top", "Num7")) camera_.axis_view('t', false);
            if (ctx.menu_item("Bottom", "Ctrl Num7")) camera_.axis_view('t', true);
            ctx.menu_separator();
            if (ctx.menu_item("Camera", "Num0")) view_through_scene_camera_();
            if (ctx.menu_item("Align Camera to View", "Ctrl Alt Num0")) align_scene_camera_to_view_();
            if (ctx.menu_item("Frame Selected", "F / .")) frame_selected();
            if (ctx.menu_item("Frame All", "Home")) frame_all();
            if (ctx.menu_item(camera_.ortho ? "Perspective" : "Orthographic", "Num5")) camera_.set_ortho(!camera_.ortho);
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_shading_menu", 150)) {
            bool w = shading_ == Shading::Wireframe, s = shading_ == Shading::Solid, f = shading_ == Shading::Full;
            bool mp = shading_ == Shading::MaterialPreview;
            if (ctx.menu_item("Wireframe", "", &w)) set_shading(Shading::Wireframe);
            if (ctx.menu_item("Solid", "", &s)) set_shading(Shading::Solid);
            if (ctx.menu_item("Material Preview", "", &mp)) set_shading(Shading::MaterialPreview);
            if (ctx.menu_item("Rendered", "", &f)) set_shading(Shading::Full);
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_tool_menu", 140)) {
            if (ctx.menu_item("Select")) tool_ = Tool::Select;
            if (ctx.menu_item("Move")) tool_ = Tool::Move;
            if (ctx.menu_item("Rotate")) tool_ = Tool::Rotate;
            if (ctx.menu_item("Scale")) tool_ = Tool::Scale;
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_snap_menu", 210)) {
            if (ctx.menu_item("Cursor to Selected")) { glm::vec3 p; glm::mat3 b; if (gizmo_target_(mesh_edit, p, b)) cursor3d_ = p; }
            if (ctx.menu_item("Cursor to World Origin")) cursor3d_ = glm::vec3(0.0f);
            if (ctx.menu_item("Cursor to Grid")) { const float g = grid_step_(); cursor3d_ = glm::round(cursor3d_ / g) * g; }
            if (ctx.menu_item("Selection to Cursor", "", nullptr, !mesh_edit && !asset_view_())) selection_to_cursor_();
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_add_menu", 180)) {
            ctx.label_dim("Add");
            if (ctx.begin_menu("Mesh")) {
                for (const auto& p : primitive_names()) if (ctx.menu_item(p)) create_primitive(p);
                ctx.end_menu();
            }
            if (ctx.begin_menu("Light")) {
                if (ctx.menu_item("Sun")) create_with_component("DirectionalLight", "Sun");
                if (ctx.menu_item("Point")) create_with_component("PointLight", "Point Light");
                if (ctx.menu_item("Spot")) create_with_component("SpotLight", "Spot Light");
                if (ctx.menu_item("Environment")) create_with_component("EnvironmentLight", "Environment");
                ctx.end_menu();
            }
            if (ctx.menu_item("Camera")) create_with_component("Camera", "Camera");
            if (ctx.menu_item("Particle System")) create_with_component("ParticleSystem", "Particle System");
            if (ctx.menu_item("Empty")) create_empty();
            if (ctx.menu_item("Reflection Probe")) create_with_component("ReflectionProbe", "Reflection Probe");
            if (ctx.menu_item("Terrain")) create_with_component("Terrain", "Terrain");
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_delete_confirm", 150)) {
            if (ctx.menu_item("Delete")) delete_selected();
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_clear_parent", 230)) {
            if (ctx.menu_item("Clear Parent")) { for (ObjectId id : doc_.selection()) apply_(doc_.reparent(id, 0)); }
            if (ctx.menu_item("Clear and Keep Transformation")) clear_parent_keep_transform_();
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_obj_ctx", 230)) {
            const bool any = !doc_.selection().empty();
            draw_instance_menu_items_(ctx, doc_.primary());   // Open Object Asset, Revert / Apply overrides
            if (ctx.menu_item("Edit Mode", "Tab", nullptr, any)) toggle_edit_mode_();
            ctx.menu_separator();
            if (ctx.menu_item("Duplicate", "Shift D", nullptr, any)) { duplicate_selected(); pending_modal_kind_ = ModalKind::Grab; }
            if (ctx.menu_item("Delete", "X", nullptr, any)) delete_selected();
            if (ctx.menu_item("Rename", "F2", nullptr, any && !doc_.is_inherited(doc_.primary()))) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
            ctx.menu_separator();
            if (ctx.menu_item("Shade Smooth", "", nullptr, any)) shade_selected_(true);
            if (ctx.menu_item("Shade Flat", "", nullptr, any)) shade_selected_(false);
            ctx.menu_separator();
            if (ctx.menu_item("Parent to Active", "Ctrl P", nullptr, doc_.selection().size() > 1)) parent_selection_to_active_();
            if (ctx.menu_item("Clear Parent (keep transform)", "Alt P", nullptr, any)) clear_parent_keep_transform_();
            ctx.menu_separator();
            if (ctx.menu_item("Hide", "H", nullptr, any)) hide_(doc_.selection());
            if (ctx.menu_item("Unhide All", "Alt H")) unhide_all_();
            if (ctx.menu_item("Frame Selected", "F / .", nullptr, any)) frame_selected();
            if (ctx.menu_item("Set 3D Cursor Here", "Shift RMB")) cursor3d_ = surface_point_(ctx_popup_origin_(m));
            ctx.end_popup();
        }
        auto mesh_op = [&](const char* label, auto&& fn) { mesh_.edit(label, fn); };
        if (ctx.begin_popup("vp_mesh_ctx", 220)) {
            if (ctx.menu_item("Subdivide")) run_subdivide_(1);
            if (ctx.menu_item("Subdivide Smooth")) run_catmull_clark_(1);
            if (ctx.menu_item("Loop Cut and Slide", "Ctrl R")) begin_loop_cut();
            if (ctx.menu_item("Edge Slide", "G G") && vp) begin_edge_slide_from_selection_(*vp, m);
            ctx.menu_separator();
            if (ctx.menu_item("Extrude", "E") && vp) extrude_interactive_(*vp, m);
            if (ctx.menu_item("Inset", "I")) pending_modal_kind_ = ModalKind::Inset;
            if (ctx.menu_item("Bevel", "Ctrl B")) pending_modal_kind_ = ModalKind::Bevel;
            if (ctx.menu_item("Make Face", "F")) mesh_op("Make Face", [](EditMesh& mm, MeshSelection& s) { fill_face(mm, s); });
            if (ctx.menu_item("Merge at Center", "M")) mesh_op("Merge", [](EditMesh& mm, MeshSelection& s) { merge_at_center(mm, s); });
            if (ctx.menu_item("Flip Normals", "Alt N")) mesh_op("Flip Normals", [](EditMesh& mm, MeshSelection& s) { flip_normals(mm, s); });
            if (ctx.menu_item("Shade Smooth")) mesh_op("Shade Smooth", [](EditMesh& mm, MeshSelection& s) { set_smooth(mm, s, true); });
            if (ctx.menu_item("Shade Flat")) mesh_op("Shade Flat", [](EditMesh& mm, MeshSelection& s) { set_smooth(mm, s, false); });
            ctx.menu_separator();
            if (ctx.menu_item("Triangulate Faces", "Ctrl T")) mesh_op("Triangulate", [](EditMesh& mm, MeshSelection& s) { triangulate(mm, s); });
            if (ctx.menu_item("Tris to Quads", "Alt J")) mesh_op("Tris to Quads", [](EditMesh& mm, MeshSelection& s) { tris_to_quads(mm, s); });
            if (ctx.menu_item("Bridge Edge Loops", "Ctrl E")) bridge_selected_();
            ctx.menu_separator();
            if (ctx.menu_item("Dissolve Edges")) dissolve_selected_edges_();
            if (ctx.menu_item("Delete Faces", "X")) mesh_op("Delete", [](EditMesh& mm, MeshSelection& s) { delete_selection(mm, s); });
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_add", 180)) {
            ctx.label_dim("Add Mesh");
            for (const auto& p : primitive_names()) if (ctx.menu_item(p)) add_mesh_primitive_(p);
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_delete", 170)) {
            auto del = [&](SelectMode as) {
                mesh_op("Delete", [as](EditMesh& mm, MeshSelection& s) { convert_selection(mm, s, as); delete_selection(mm, s); });
            };
            if (ctx.menu_item("Vertices")) del(SelectMode::Vertex);
            if (ctx.menu_item("Edges")) del(SelectMode::Edge);
            if (ctx.menu_item("Faces")) del(SelectMode::Face);
            ctx.menu_separator();
            if (ctx.menu_item("Dissolve Vertices")) mesh_op("Dissolve Vertices", [](EditMesh& mm, MeshSelection& s) { dissolve_verts(mm, s); });
            if (ctx.menu_item("Dissolve Edges")) dissolve_selected_edges_();
            if (ctx.menu_item("Dissolve Faces")) dissolve_selected_faces_();
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_mirror", 170)) {
            draw_mirror_menu_items_(ctx);
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_merge", 170)) {
            if (ctx.menu_item("At Center")) mesh_op("Merge", [](EditMesh& mm, MeshSelection& s) { merge_at_center(mm, s); });
            if (ctx.menu_item("By Distance")) {
                size_t n = 0;
                mesh_op("Merge by Distance", [&](EditMesh& mm, MeshSelection& s) { n = merge_by_distance(mm, s, merge_dist_); });
                log_info("Removed " + std::to_string(n) + " vertices");
            }
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_uv", 170)) {
            if (ctx.menu_item("Cube Projection")) mesh_op("Box UV", [&](EditMesh& mm, MeshSelection& s) { uv_box_project(mm, s, uv_scale_); });
            if (ctx.menu_item("Project X")) mesh_op("Planar UV", [](EditMesh& mm, MeshSelection& s) { uv_planar_project(mm, s, 0); });
            if (ctx.menu_item("Project Y")) mesh_op("Planar UV", [](EditMesh& mm, MeshSelection& s) { uv_planar_project(mm, s, 1); });
            if (ctx.menu_item("Project Z")) mesh_op("Planar UV", [](EditMesh& mm, MeshSelection& s) { uv_planar_project(mm, s, 2); });
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_normals", 170)) {
            if (ctx.menu_item("Flip")) mesh_op("Flip Normals", [](EditMesh& mm, MeshSelection& s) { flip_normals(mm, s); });
            if (ctx.menu_item("Smooth")) mesh_op("Shade Smooth", [](EditMesh& mm, MeshSelection& s) { set_smooth(mm, s, true); });
            if (ctx.menu_item("Flat")) mesh_op("Shade Flat", [](EditMesh& mm, MeshSelection& s) { set_smooth(mm, s, false); });
            ctx.end_popup();
        }
    }

    glm::vec2 ctx_popup_origin_(glm::vec2 fallback) const { return fallback; }

    // --- modal operators ---

    void start_modal_(ModalKind kind, const ViewProj& vp, glm::vec2 mouse, bool mesh, std::optional<glm::vec3> axis = std::nullopt) {
        if (playing() && !asset_view_()) return;
        glm::vec3 pivot;
        glm::mat3 basis(1.0f);
        if (!gizmo_target_(mesh, pivot, basis)) return;
        if ((kind == ModalKind::Inset || kind == ModalKind::Bevel) && !mesh) return;
        // A second axis press uses the header orientation (Local when it says Global).
        const Orientation second = orient_ == Orientation::Global || (orient_ == Orientation::Normal && !mesh) ? Orientation::Local : orient_;
        const glm::mat3 second_basis = orientation_basis_(mesh, second);
        begin_transform_(mesh);
        modal_mesh_ = mesh;
        modal_base_sel_ = mesh_.selection;
        slide_merge_key_ = "modal";
        modal_.begin(kind, vp, pivot, second_basis, mouse, axis, second == Orientation::Normal ? "normal" : "local",
                     mesh && kind == ModalKind::Grab);
    }

    /** @brief Is proportional editing driving the running mesh move / rotate / scale? */
    bool proportional_live_() const {
        const ModalKind k = modal_.kind();
        return proportional_.enabled && modal_mesh_ && (k == ModalKind::Grab || k == ModalKind::Rotate || k == ModalKind::Scale);
    }

    /** @brief The proportional circle: the radius on screen, around the selection's current centre. */
    void draw_proportional_circle_(imm::Context& ctx, const ViewProj& vp, const glm::vec3& centre) {
        const auto c = vp.project(centre);
        const float r = proportional_radius_px_(vp, centre);
        if (!c || r < 1.0f) return;
        ctx.ring(*c, r, 1.5f, imm::with_alpha(ctx.style.text, 0.85f), 96);
        ctx.ring(*c, r + 1.5f, 1.0f, imm::with_alpha(glm::vec4(0, 0, 0, 1), 0.35f), 96);
    }

    void run_modal_(imm::Context& ctx, const ViewProj& vp, bool ctrl, bool shift) {
        const auto& in = ctx.input();
        const ModalKind kind = modal_.kind();
        // Proportional editing: the wheel / Page Up / Page Down resize the radius, as in Blender.
        if (proportional_live_()) {
            float factor = 1.0f;
            if (in.scroll.y != 0.0f) factor *= std::exp(in.scroll.y * 0.1f);
            for (const auto& e : in.keys) {
                if (e.action == coopa::input::KeyAction::Release) continue;
                if (e.key == coopa::input::Key::PageUp) factor *= 1.1f;
                if (e.key == coopa::input::Key::PageDown) factor /= 1.1f;
            }
            if (factor != 1.0f) {
                proportional_.set_radius(proportional_.radius * factor);
                update_proportional_();
            }
        }
        modal_.snap_step = snap_adaptive_ ? grid_step_() : gizmo_.translate_snap;
        const auto outcome = modal_.update(vp, ctx.mouse(), in.keys, in.pressed[0], in.pressed[1], ctrl != snap_on_, shift,
                                           in.down[2], in.pressed[2]);
        ctx.consume_keyboard();
        select_pending_ = box_selecting_ = false;
        nav_active_ = false;   // MMB belongs to the operator (auto constraint) while it runs
        if (outcome == ModalTransform::Outcome::SwitchToSlide) {
            // G G: put the grab back and slide the selected edges instead.
            apply_modal_(kind, ModalTransform::Result{});
            mesh_.undo.end_merge();
            begin_edge_slide_from_selection_(vp, ctx.mouse());
            return;
        }
        if (outcome == ModalTransform::Outcome::Cancelled) {
            ModalTransform::Result zero;
            apply_modal_(kind, zero);
            if (modal_mesh_) mesh_.undo.end_merge(); else doc_.end_merge();
            note_slide_finished_(kind, 0.0f);
            return;
        }
        apply_modal_(kind, modal_.result());
        if (outcome == ModalTransform::Outcome::Confirmed) {
            if (modal_mesh_) mesh_.undo.end_merge(); else doc_.end_merge();
            note_slide_finished_(kind, modal_.result().amount);
        }
        if (outcome == ModalTransform::Outcome::Running && proportional_live_()) {
            draw_proportional_circle_(ctx, vp, prop_pivot_ + (kind == ModalKind::Grab ? modal_.result().translate : glm::vec3(0.0f)));
        }
        // Guide lines: the constraint axis, or both axes of a constraint plane (dimmer).
        if (kind == ModalKind::EdgeSlide) return;
        for (const auto& [axis_v, primary] : modal_.guide_axes()) {
            const glm::vec3 p = modal_.pivot();
            auto a = vp.project(p - axis_v * 1000.0f), b = vp.project(p + axis_v * 1000.0f);
            auto c = vp.project(p);
            if (!c) continue;
            if (!a) a = c;
            if (!b) b = c;
            glm::vec4 col = ctx.style.accent;
            const int ax = modal_.axis();
            if (modal_.axis_is_second() || ax < 0) {
                // Local / normal axes: colour by the closest world axis, as Blender does.
                const glm::vec3 av = glm::abs(axis_v);
                col = av.x >= av.y && av.x >= av.z ? ctx.style.axis_x : av.y >= av.z ? ctx.style.axis_y : ctx.style.axis_z;
                if (ax < 0) col = ctx.style.accent;
            } else {
                col = std::abs(axis_v.x) > 0.9f ? ctx.style.axis_x : std::abs(axis_v.y) > 0.9f ? ctx.style.axis_y : ctx.style.axis_z;
            }
            ctx.line(*a, *b, primary ? col : imm::with_alpha(col, 0.55f), primary ? 1.5f : 1.0f);
        }
    }

    void apply_modal_(ModalKind kind, const ModalTransform::Result& r) {
        if (kind == ModalKind::EdgeSlide) {
            const EditMesh base = mesh_drag_base_;
            mesh_.edit("Edge Slide", [&](EditMesh& mm, MeshSelection&) {
                mm = base;
                apply_edge_slide(mm, slide_rails_, r.amount);
            }, slide_merge_key_);
            return;
        }
        if (kind == ModalKind::Inset || kind == ModalKind::Bevel) {
            const EditMesh base = mesh_drag_base_;
            const MeshSelection base_sel = modal_base_sel_;
            mesh_.edit(kind == ModalKind::Inset ? "Inset" : "Bevel", [&](EditMesh& mm, MeshSelection& s) {
                mm = base;
                s = base_sel;
                if (kind == ModalKind::Inset) inset_faces(mm, s, r.amount);
                else if (r.amount > 1e-6f) bevel_edges(mm, s, r.amount);
            }, "modal");
            return;
        }
        const int k = kind == ModalKind::Grab ? 0 : kind == ModalKind::Rotate ? 1 : 2;
        apply_transform_(modal_mesh_, k, r.translate, r.rotate_axis, r.rotate_deg, r.scale, r.scale_basis);
    }

    void extrude_interactive_(const ViewProj& vp, glm::vec2 m) {
        auto& md = mesh_;
        if (md.selection.affected_vertices(md.mesh).empty()) return;
        if (md.selection.mode == SelectMode::Edge) {
            md.edit("Extrude Edges", [](EditMesh& mm, MeshSelection& s) { extrude_edges(mm, s, glm::vec3(0.0f)); });
            start_modal_(ModalKind::Grab, vp, m, true);
            return;
        }
        // Region extrude, then move along the (world-space) average normal.
        glm::vec3 n(0.0f);
        for (uint32_t f : md.selection.affected_faces(md.mesh)) n += md.mesh.face_normal(f);
        md.edit("Extrude", [](EditMesh& mm, MeshSelection& s) { extrude_faces(mm, s, 0.0f); });
        std::optional<glm::vec3> axis;
        if (glm::length(n) > 1e-6f) axis = glm::normalize(glm::mat3(mesh_world_()) * glm::normalize(n));
        start_modal_(ModalKind::Grab, vp, m, true, axis);
    }

    void invert_mesh_selection_() {
        auto& md = mesh_;
        auto& s = md.selection;
        MeshSelection inv;
        inv.mode = s.mode;
        if (s.mode == SelectMode::Vertex) { for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) if (!s.verts.count(v)) inv.verts.insert(v); }
        else if (s.mode == SelectMode::Edge) { for (const auto& e : md.mesh.edges()) if (!s.edges.count(e)) inv.edges.insert(e); }
        else { for (uint32_t f = 0; f < md.mesh.faces.size(); ++f) if (!s.faces.count(f)) inv.faces.insert(f); }
        s = inv;
    }

    // --- object operations ---

    std::vector<ObjectId> visible_ids_() const {
        std::vector<ObjectId> out;
        for (ObjectId id : doc_.all_ids()) if (!hidden_.count(id)) out.push_back(id);
        return out;
    }

    void hide_(const std::vector<ObjectId>& ids) {
        for (ObjectId id : ids) hidden_.insert(id);
        for (ObjectId id : ids) if (doc_.is_selected(id)) doc_.select(id, true);
        apply_hidden_();
    }
    void unhide_all_() {
        for (ObjectId id : hidden_) {
            if (auto* live = sync_.live(id)) {
                const Node* n = doc_.find(id);
                live->set_active(n ? get_bool(*n, "active", true) : true);
            }
            doc_.select(id, true);
        }
        hidden_.clear();
    }
    void apply_hidden_() {
        for (ObjectId id : hidden_) if (auto* live = sync_.live(id)) live->set_active(false);
        for (ObjectId id : isolated_) if (auto* live = sync_.live(id)) live->set_active(false);
    }

    /** @brief World matrix of an object (live), or identity. */
    glm::mat4 world_of_(ObjectId id) {
        if (auto* live = sync_.live(id); live && live->get_transform()) return live->get_transform()->transform().get_world_matrix();
        return glm::mat4(1.0f);
    }

    /** @brief Splits a matrix into the Transform's (position, Euler degrees, scale). */
    static void decompose_(const glm::mat4& m, glm::vec3& pos, glm::vec3& rot, glm::vec3& scl) {
        pos = glm::vec3(m[3]);
        scl = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
        glm::mat4 r(1.0f);
        for (int i = 0; i < 3; ++i) r[i] = glm::vec4(glm::vec3(m[i]) / std::max(scl[i], 1e-8f), 0.0f);
        rot = matrix_to_euler(r);
    }

    /** @brief Writes `id`'s transform so that it ends up at world matrix `world` under `parent`. */
    void set_world_transform_(ObjectId id, const glm::mat4& world, const glm::mat4& parent_world, const std::string& label) {
        glm::vec3 p, r, s;
        decompose_(glm::inverse(parent_world) * world, p, r, s);
        apply_(doc_.set_transform(id, p, r, s, label));   // structural (reparenting): always the document
    }

    void parent_selection_to_active_() {
        const ObjectId parent = doc_.primary();
        if (!parent || doc_.selection().size() < 2) { log_warn("Select the children, then the parent last (active)"); return; }
        const glm::mat4 pw = world_of_(parent);
        for (ObjectId id : std::vector<ObjectId>(doc_.selection())) {
            if (id == parent || doc_.is_ancestor(id, parent)) continue;
            const glm::mat4 w = world_of_(id);
            if (doc_.reparent(id, parent).scope != ChangeScope::None) set_world_transform_(id, w, pw, "Keep Transform");
        }
        queue_rebuild_();
    }

    void clear_parent_keep_transform_() {
        for (ObjectId id : std::vector<ObjectId>(doc_.selection())) {
            const glm::mat4 w = world_of_(id);
            if (doc_.reparent(id, 0).scope != ChangeScope::None) set_world_transform_(id, w, glm::mat4(1.0f), "Keep Transform");
        }
        queue_rebuild_();
    }

    void clear_transform_(int what) {
        for (ObjectId id : doc_.selection()) {
            glm::vec3 p, r, s;
            get_object_transform_(id, p, r, s);
            if (what == 0) p = glm::vec3(0.0f);
            if (what == 1) r = glm::vec3(0.0f);
            if (what == 2) s = glm::vec3(1.0f);
            set_object_transform_(id, p, r, s, what == 0 ? "Clear Location" : what == 1 ? "Clear Rotation" : "Clear Scale");
        }
    }

    void selection_to_cursor_() {
        for (ObjectId id : doc_.selection()) {
            glm::mat4 w = world_of_(id);
            w[3] = glm::vec4(cursor3d_, 1.0f);
            const auto parent = doc_.parent_of(id);
            set_world_transform_(id, w, parent && *parent ? world_of_(*parent) : glm::mat4(1.0f), "Selection to Cursor");
        }
    }

    /** @brief Numpad 0: matches the editor view to the scene's main camera. */
    void view_through_scene_camera_() {
        ObjectId cam_id = 0;
        for (ObjectId id : doc_.all_ids()) {
            const int ci = doc_.find_component(id, "Camera");
            if (ci < 0) continue;
            if (!cam_id || get_bool(doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)], "main", false)) cam_id = id;
        }
        if (!cam_id) { log_warn("The scene has no camera"); return; }
        const glm::mat4 w = world_of_(cam_id);
        const glm::vec3 pos(w[3]);
        const glm::vec3 fwd = -glm::normalize(glm::vec3(w[2]));
        const glm::vec3 od = -fwd;
        camera_.pitch_deg = glm::degrees(std::asin(std::clamp(od.z, -1.0f, 1.0f)));
        camera_.yaw_deg = glm::degrees(std::atan2(od.x, -od.y));
        camera_.focus = pos + fwd * camera_.distance;
        const Node& c = doc_.find(cam_id)->at("components").as_seq()[static_cast<size_t>(doc_.find_component(cam_id, "Camera"))];
        camera_.fov = get_float(c, "fov", camera_.fov);
        camera_.apply();
    }

    /** @brief Ctrl+Alt+Numpad 0: moves the scene camera to the current view. */
    void align_scene_camera_to_view_() {
        for (ObjectId id : doc_.all_ids()) {
            if (doc_.find_component(id, "Camera") < 0) continue;
            glm::mat4 w = glm::translate(glm::mat4(1.0f), camera_.position()) *
                          euler_to_matrix(glm::vec3(90.0f - camera_.pitch_deg, 0.0f, camera_.yaw_deg));
            const auto parent = doc_.parent_of(id);
            set_world_transform_(id, w, parent && *parent ? world_of_(*parent) : glm::mat4(1.0f), "Align Camera to View");
            log_info("Camera aligned to view");
            return;
        }
    }

    // --- edit mode in the scene ---

    /** @brief Tab: Object <-> Edit Mode on the active mesh (from Sculpt Mode: back to Object). */
    void toggle_edit_mode_() {
        if (playing()) return;
        if (active_type_ == AssetType::Mesh) {   // a mesh asset: Object <-> Edit on the asset itself
            set_interaction_mode(mode_ == InteractionMode::Object ? InteractionMode::Edit : InteractionMode::Object);
            return;
        }
        if (asset_view_()) return;
        if (edit_object_) { exit_mesh_mode_(); return; }
        enter_mesh_mode_(InteractionMode::Edit);
    }

    /**
     * @brief The mesh key an object's geometry comes from: its MeshRenderer's `mesh_path`, or --
     *        when that is empty -- its WaterBody's. A water body's MeshRenderer names no mesh on
     *        purpose: toy::water::WaterSystem bakes the WaterBody's mesh and publishes it there at
     *        runtime, so for editing (and outlines, bounds) the WaterBody's key is the real one.
     */
    static std::string object_mesh_key_(const Node& obj) {
        if (!obj.contains("components")) return {};
        std::string water;
        for (const auto& c : obj.at("components").as_seq()) {
            const std::string type = component_type(c);
            if (type == "MeshRenderer" || type == "SkinnedMeshRenderer") {
                // A skinned mesh's MeshRenderer has no mesh of its own: the SkinnedMeshRenderer's is it.
                std::string key = get_string(c, "mesh_path");
                if (!key.empty()) return key;
            } else if (type == "WaterBody") {
                water = get_string(c, "mesh_path");
            }
        }
        return water;
    }

    /** @brief A mesh key's file, resolved like the engine does (scene folder, then asset roots). */
    fs::path resolve_mesh_key_(const std::string& key) {
        const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
        return coopa::yaml::resolve_variant(engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
    }

    /**
     * @brief A WaterBody with no `mesh_path` draws a procedural grid (`size` x `resolution`). To edit
     *        it, write that exact grid out as a mesh file -- next to the scene, in its meshes/
     *        folder -- and point the WaterBody at it (one undo step). Returns the new key, or empty.
     */
    std::string water_grid_to_mesh_(ObjectId id, int water_ci) {
        const Node* obj = doc_.find(id);
        if (!obj) return {};
        Node comp = obj->at("components").as_seq()[static_cast<size_t>(water_ci)];
        glm::vec2 size(20.0f);   // WaterBody's defaults
        int res = 48;
        if (comp.contains("size")) {
            const Node& s = comp.at("size");
            if (s.contains("x")) size.x = s.at("x").get_value<float>();
            if (s.contains("y")) size.y = s.at("y").get_value<float>();
        }
        if (comp.contains("resolution")) res = static_cast<int>(comp.at("resolution").get_value<int64_t>());
        res = std::clamp(res, 1, 512);

        // The same grid WaterSystem generates (local XY, centred, one UV tile across).
        EditMesh m;
        for (int y = 0; y <= res; ++y)
            for (int x = 0; x <= res; ++x)
                m.positions.push_back({(x / float(res) - 0.5f) * size.x, (y / float(res) - 0.5f) * size.y, 0.0f});
        auto idx = [&](int x, int y) { return static_cast<uint32_t>(y * (res + 1) + x); };
        auto uv = [&](int x, int y) { return glm::vec2(x / float(res), y / float(res)); };
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x) {
                Face f;
                f.corners = {{idx(x, y), uv(x, y)}, {idx(x + 1, y), uv(x + 1, y)},
                             {idx(x + 1, y + 1), uv(x + 1, y + 1)}, {idx(x, y + 1), uv(x, y + 1)}};
                m.faces.push_back(std::move(f));
            }

        std::string base = get_string(*obj, "name");
        if (base.empty()) base = "water";
        for (char& c : base) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
        }
        base += "_surface";
        const fs::path dir = doc_.path().empty() ? project_.assets() / "meshes" : doc_.path().parent_path() / "meshes";
        std::string key = base;
        for (int n = 2; coopa::yaml::document_exists(dir / (key + ".yaml")); ++n) key = base + "_" + std::to_string(n);
        try {
            fs::create_directories(dir);
            coopa::yaml::save_document(dir / (key + ".yaml"), mesh_to_node(m));
            project_.refresh();
        } catch (const std::exception& e) {
            log_error(std::string("Could not write the water mesh: ") + e.what());
            return {};
        }
        comp["mesh_path"] = Node(key);
        apply_(doc_.set_component(id, water_ci, comp, "Water grid to mesh", "water_mesh"));
        mesh_cache_.clear();
        log_info("Water grid written to meshes/" + key + ".yaml -- the WaterBody now uses it");
        return key;
    }

    /** @brief The active object's mesh file, or empty (logging why). */
    fs::path active_mesh_path_(ObjectId id) {
        const Node* obj = doc_.find(id);
        std::string key = obj ? object_mesh_key_(effective_(*obj)) : std::string();
        if (key.empty()) {
            const int water_ci = doc_.find_component(id, "WaterBody");
            if (water_ci >= 0) {
                key = water_grid_to_mesh_(id, water_ci);
            } else if (doc_.find_component(id, "MeshRenderer") < 0) {
                log_warn("This mode needs a mesh object");
            }
            if (key.empty()) return {};
        }
        const fs::path path = resolve_mesh_key_(key);
        if (!coopa::yaml::document_exists(path)) { log_error("Mesh file not found for '" + key + "'"); return {}; }
        if (refuse_engine_asset_(path)) return {};   // toyengine's mesh: Copy to Project to edit it
        return path;
    }

    /**
     * @brief Enters Edit or Sculpt Mode on the active mesh object -- the one way in. Switching
     *        between the two on the same object keeps the isolation and view.
     */
    bool enter_mesh_mode_(InteractionMode mode) {
        if (playing() || mode == InteractionMode::Object) return false;
        if (active_type_ == AssetType::Mesh) {
            // The mesh asset is already the document: just switch mode.
            if (mode_ == mode) return true;
            mode_end_(mode_);
            mode_ = mode;
            mode_begin_(mode_);
            return true;
        }
        if (asset_view_()) return false;
        const ObjectId id = edit_object_ ? edit_object_ : doc_.primary();
        if (!id) return false;
        if (edit_object_ == id) {
            if (mode_ == mode) return true;
            mode_end_(mode_);
            mode_ = mode;
            mode_begin_(mode_);
            return true;
        }
        if (edit_object_) exit_mesh_mode_();
        const fs::path path = active_mesh_path_(id);
        if (path.empty()) return false;
        if (mesh_.path != path || !mesh_.open()) {
            try { switch_mesh_doc_(path); } catch (const std::exception& e) { log_error(e.what()); return false; }
        }
        mesh_.scene_owned = true;
        edit_object_ = id;
        mode_ = mode;
        scene_uploaded_revision_ = 0;
        if (isolate_in_edit_) isolate_(id);
        mode_begin_(mode_);
        static const char* kModeNames[] = {"Object", "Edit", "Sculpt", "Vertex Paint", "Weight Paint"};
        log_info(std::string(kModeNames[static_cast<int>(mode)]) + " mode: " + project_.relative(path) +
                 " (Tab to leave, Ctrl+S saves it)");
        return true;
    }

    /** @brief Back to Object Mode -- the one way out (restores isolation and view). */
    void exit_mesh_mode_() {
        if (active_type_ == AssetType::Mesh && !edit_object_) {
            if (modal_.active()) { apply_modal_(modal_.kind(), ModalTransform::Result{}); modal_.cancel(); mesh_.undo.end_merge(); }
            mode_end_(mode_);
            loopcut_.active = false;
            mode_ = InteractionMode::Object;
            return;
        }
        if (!edit_object_) return;
        if (modal_.active()) { apply_modal_(modal_.kind(), ModalTransform::Result{}); modal_.cancel(); mesh_.undo.end_merge(); }
        mode_end_(mode_);
        loopcut_.active = false;
        unisolate_();
        edit_object_ = 0;
        mode_ = InteractionMode::Object;
        tool_settings_restore_();
        // A mesh edited from a scene object is saved on the way out (as Blender's Edit Mode
        // commits to the object): the scene, its colliders and a Play all see the file.
        if (mesh_.scene_owned && mesh_.dirty()) save_mesh();
    }

    // --- Local-View-style isolation while editing a mesh ---

    /** @brief Does this object draw geometry (and is not a light / probe that lights the mesh)? */
    static bool isolate_candidate_(const Node& obj) {
        if (!obj.contains("components")) return false;
        bool renders = false, lights = false;
        for (const auto& c : obj.at("components").as_seq()) {
            const std::string t = component_type(c);
            if (t == "MeshRenderer" || t == "SkinnedMeshRenderer" || t == "Terrain" || t == "SdfRenderer" || t == "Cloth") renders = true;
            if (t.find("Light") != std::string::npos || t == "ReflectionProbe" || t == "GiProbeVolume" || t == "Volume") lights = true;
        }
        return renders && !lights;
    }

    /** @brief Hides every other renderable object (lights stay) and frames the mesh. */
    void isolate_(ObjectId keep) {
        isolated_.clear();
        for (const auto& [id, live] : sync_.live_objects()) {
            if (id == keep || !live || hidden_.count(id)) continue;
            const Node* n = doc_.find(id);
            if (n && isolate_candidate_(*n)) isolated_.insert(id);
        }
        apply_hidden_();
        pre_isolate_pose_ = {camera_.focus, camera_.yaw_deg, camera_.pitch_deg, camera_.distance, camera_.ortho, true};
        if (!mesh_.mesh.positions.empty()) {
            glm::vec3 lo, hi;
            mesh_.mesh.bounds(lo, hi);
            const glm::mat4 w = mesh_world_();
            glm::vec3 wlo(1e30f), whi(-1e30f);
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
                const glm::vec3 p = glm::vec3(w * glm::vec4(c, 1.0f));
                wlo = glm::min(wlo, p);
                whi = glm::max(whi, p);
            }
            camera_.frame(wlo, whi);
        }
    }

    /** @brief Undoes isolate_(): objects come back (unless the user hid them) and the view returns. */
    void unisolate_() {
        for (ObjectId id : isolated_) {
            if (hidden_.count(id)) continue;
            if (auto* live = sync_.live(id)) {
                const Node* n = doc_.find(id);
                live->set_active(n ? get_bool(*n, "active", true) : true);
            }
        }
        const bool had = !isolated_.empty() || pre_isolate_pose_.valid;
        isolated_.clear();
        if (had && pre_isolate_pose_.valid) {
            camera_.focus = pre_isolate_pose_.focus;
            camera_.yaw_deg = pre_isolate_pose_.yaw;
            camera_.pitch_deg = pre_isolate_pose_.pitch;
            camera_.distance = pre_isolate_pose_.distance;
            camera_.ortho = pre_isolate_pose_.ortho;
            camera_.auto_ortho = false;
            camera_.apply();
        }
        pre_isolate_pose_.valid = false;
    }

public:
    /** @brief The viewport header's "Isolate in Edit Mode" toggle (saved in prefs). */
    void set_isolate_in_edit(bool on) {
        isolate_in_edit_ = on;
        Node prefs = Project::load_prefs();
        prefs["isolate_edit_mode"] = Node(on);
        Project::save_prefs(prefs);
        if (!edit_object_) return;
        if (on && isolated_.empty()) isolate_(edit_object_);
        else if (!on) unisolate_();
    }
    bool isolate_in_edit() const { return isolate_in_edit_; }
    bool is_isolated(ObjectId id) const { return isolated_.count(id) > 0; }
    InteractionMode interaction_mode() const {
        return (edit_object_ || active_type_ == AssetType::Mesh) ? mode_ : InteractionMode::Object;
    }
    bool set_interaction_mode(InteractionMode m) {
        if (m == InteractionMode::Object) { exit_mesh_mode_(); return true; }
        return enter_mesh_mode_(m);
    }

private:
    void tool_settings_restore_() {}

    /** @brief Shows the edited (unsaved) mesh on every live object that uses its file. */
    void push_mesh_to_scene_() {
        if (sculpt_active_ || paint_active_) return;   // Sculpt / paint modes show their own dynamic mesh
        if (!mesh_.open() || mesh_.path.empty() || scene_uploaded_revision_ == mesh_.geometry_revision) return;
        if (!mesh_.dirty() && !edit_object_ && !force_scene_push_) return;
        force_scene_push_ = false;
        scene_uploaded_revision_ = mesh_.geometry_revision;
        std::shared_ptr<coopa::gfx::engine::data::Mesh> payload;
        try {
            auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(mesh_to_node(mesh_.mesh));
            payload = std::make_shared<coopa::gfx::engine::data::Mesh>(
                coopa::gfx::engine::data::Mesh::from_cpu(engine_.device(), engine_.allocator(), std::move(cpu)));
        } catch (const std::exception& e) {
            log_error(std::string("Mesh upload failed: ") + e.what());
            return;
        }
        auto handle = engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/edit_mesh", payload);
        // Triangle soup of the edited mesh for water bodies, built only if one uses this file.
        std::vector<glm::vec3> water_pos;
        std::vector<glm::vec2> water_uv;
        std::vector<uint32_t>  water_idx;
        for (const auto& [id, live] : sync_.live_objects()) {
            const Node* obj = doc_.find(id);
            if (!obj || !live) continue;
            const std::string key = object_mesh_key_(effective_(*obj));
            if (key.empty() || resolve_mesh_key_(key) != mesh_.path) continue;
            bool via_renderer = false;
            const Node& eff = effective_(*obj);
            if (eff.contains("components")) {
                for (const auto& c : eff.at("components").as_seq()) {
                    if (component_type(c) == "MeshRenderer" && !get_string(c, "mesh_path").empty()) via_renderer = true;
                }
            }
            if (via_renderer) {
                if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) mr->set_mesh(handle);
                continue;
            }
            // A skinned mesh: hand its SkinnedMeshRenderer the edited bind pose; it rebuilds and keeps
            // deforming it with the rig (bound against the rest pose it captured at start).
            if (auto* smr = live->get_component<toy::scene::SkinnedMeshRenderer>()) {
                try {
                    auto src = std::make_shared<coopa::gfx::engine::data::SkinnedMeshSource>(
                        coopa::gfx::engine::data::SkinnedMeshSource::from_node(Node::deserialize(coopa::yaml::emit(mesh_to_node(mesh_.mesh)))));
                    smr->set_source(engine_.assets().create<coopa::gfx::engine::data::SkinnedMeshSource>(
                        "editor/edit_skin@" + std::to_string(id), src));
                    smr->rebuild();
                } catch (const std::exception& e) {
                    log_error(std::string("Skinned mesh update failed: ") + e.what());
                }
                continue;
            }
            // A water body: its MeshRenderer shows WaterSystem's bake, not the raw mesh. Hand the
            // edited geometry to the WaterBody instead; WaterSystem re-bakes it (it runs in edit
            // mode) with waves, foam and flow intact.
            auto* water = live->get_component<toy::water::WaterBody>();
            if (!water) continue;
            if (water_idx.empty()) {
                for (const auto& f : mesh_.mesh.faces) {
                    for (size_t k = 1; k + 1 < f.corners.size(); ++k) {
                        for (const Corner* c : {&f.corners[0], &f.corners[k], &f.corners[k + 1]}) {
                            water_idx.push_back(static_cast<uint32_t>(water_pos.size()));
                            water_pos.push_back(mesh_.mesh.positions[c->v]);
                            water_uv.push_back(c->uv);
                        }
                    }
                }
            }
            if (!water_idx.empty()) water->set_geometry(water_pos, water_uv, water_idx);
        }
        mesh_cache_.clear();
    }

    /**
     * @brief World-space axes (columns) for a transform orientation: identity for Global,
     *        the object's (or edited mesh's) axes for Local, the selection's Normal basis in
     *        edit mode (Local outside it).
     */
    glm::mat3 orientation_basis_(bool mesh_edit, Orientation o) {
        if (o == Orientation::Global) return glm::mat3(1.0f);
        auto axes = [](const glm::mat4& w) {
            return glm::mat3(glm::normalize(glm::vec3(w[0])), glm::normalize(glm::vec3(w[1])), glm::normalize(glm::vec3(w[2])));
        };
        if (mesh_edit) {
            const glm::mat4 w = mesh_world_();
            if (o == Orientation::Normal) {
                const glm::mat3 nb = normal_basis(mesh_.mesh, mesh_.selection);
                const glm::vec3 z = glm::normalize(glm::inverse(glm::transpose(glm::mat3(w))) * nb[2]);
                glm::vec3 x = glm::mat3(w) * nb[0];
                x -= z * glm::dot(x, z);
                x = glm::length(x) > 1e-9f ? glm::normalize(x) : (std::abs(z.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
                return glm::mat3(x, glm::cross(z, x), z);
            }
            return axes(w);
        }
        if (auto* live = sync_.live(doc_.primary()); live && live->get_transform()) return axes(live->get_transform()->transform().get_world_matrix());
        return glm::mat3(1.0f);
    }

    bool gizmo_target_(bool mesh_edit, glm::vec3& pivot, glm::mat3& basis) {
        basis = orientation_basis_(mesh_edit, orient_);
        if (mesh_edit) {
            if (mesh_.selection.affected_vertices(mesh_.mesh).empty()) return false;
            const glm::mat4 w = mesh_world_();
            pivot = glm::vec3(w * glm::vec4(selection_center(mesh_.mesh, mesh_.selection), 1.0f));
            return true;
        }
        if (doc_.selection().empty()) return false;
        glm::vec3 sum(0.0f);
        int n = 0;
        for (ObjectId id : doc_.selection()) {
            auto* live = sync_.live(id);
            if (!live || !live->get_transform()) continue;
            const glm::mat4 w = live->get_transform()->transform().get_world_matrix();
            sum += glm::vec3(w[3]);
            ++n;
        }
        if (n == 0) return false;
        pivot = sum / static_cast<float>(n);
        return true;
    }

    struct DragStart {
        glm::vec3 pos, rot, scl;
        glm::mat4 parent_world{1.0f};
        glm::vec3 world_pos;
    };

    /** @brief The proportional radius in screen pixels at world point `at` (the circle's size). */
    float proportional_radius_px_(const ViewProj& vp, const glm::vec3& at) const {
        const glm::vec3 right = glm::normalize(glm::vec3(glm::inverse(vp.view)[0]));
        const auto a = vp.project(at), b = vp.project(at + right * proportional_.radius);
        return a && b ? glm::length(*b - *a) : 0.0f;
    }

    /**
     * @brief For the transform about to run (or after the radius changes mid-way): which
     *        unselected vertices proportional editing drags and how much, then the symmetry
     *        partners of everything that moves -- all from the positions before the move.
     */
    void update_proportional_() {
        const std::set<uint32_t> sel = mesh_.selection.affected_vertices(mesh_drag_base_);
        const glm::mat4 w = mesh_world_();
        prop_pivot_ = glm::vec3(w * glm::vec4(selection_center(mesh_drag_base_, mesh_.selection), 1.0f));
        prop_weights_.clear();
        prop_radius_px_ = 0.0f;
        if (proportional_.enabled) {
            const auto vp = view_proj_();
            std::function<std::optional<glm::vec2>(const glm::vec3&)> project;
            if (vp) {
                prop_radius_px_ = proportional_radius_px_(*vp, prop_pivot_);
                project = [v = *vp](const glm::vec3& p) { return v.project(p); };
            }
            prop_weights_ = proportional_weights(mesh_drag_base_, sel, proportional_, w, project, prop_radius_px_);
        }
        std::set<uint32_t> moved = sel;
        for (const auto& [v, wt] : prop_weights_) moved.insert(v);
        // Symmetry: who mirrors whom, from the positions before anything moves.
        mirror_map_ = build_mirror_map(mesh_drag_base_, moved, edit_symmetry_, w);
    }

    /** @brief Captures the values a gizmo drag / modal operator applies its deltas to. */
    void begin_transform_(bool mesh_edit) {
        if (mesh_edit) {
            mesh_drag_base_ = mesh_.mesh;
            update_proportional_();
            return;
        }
        drag_starts_.clear();
        for (ObjectId id : doc_.selection()) {
            DragStart s;
            get_object_transform_(id, s.pos, s.rot, s.scl);
            auto* live = sync_.live(id);
            if (live && live->parent() && live->parent()->get_transform()) {
                s.parent_world = live->parent()->get_transform()->transform().get_world_matrix();
            }
            s.world_pos = glm::vec3(s.parent_world * glm::vec4(s.pos, 1.0f));
            drag_starts_[id] = s;
        }
    }

    /**
     * @brief Applies a cumulative world-space delta to what begin_transform_() captured.
     * @param kind 0 translate, 1 rotate, 2 scale.
     */
    void apply_transform_(bool mesh_edit, int kind, const glm::vec3& translate, const glm::vec3& rot_axis, float rot_deg,
                          const glm::vec3& scale, const glm::mat3& scale_basis = glm::mat3(1.0f)) {
        if (mesh_edit) {
            const glm::mat4 w = mesh_world_();
            const glm::vec3 pivot = glm::vec3(w * glm::vec4(selection_center(mesh_drag_base_, mesh_.selection), 1.0f));
            const glm::mat4 xf_world = delta_matrix(kind == 0 ? translate : glm::vec3(0.0f), rot_axis, kind == 1 ? rot_deg : 0.0f,
                                                    kind == 2 ? scale : glm::vec3(1.0f), pivot, scale_basis);
            const glm::mat4 local = glm::inverse(w) * xf_world * w;
            const glm::mat4 inv_w = glm::inverse(w);
            const EditMesh base = mesh_drag_base_;
            mesh_.edit(kind == 0 ? "Move" : kind == 1 ? "Rotate" : "Scale", [&](EditMesh& mm, MeshSelection& sel) {
                mm = base;
                transform_selection(mm, sel, local);
                // Proportional editing: the same transform, every part scaled by the weight.
                for (const auto& [v, wt] : prop_weights_) {
                    const glm::mat4 xw = delta_matrix(kind == 0 ? translate * wt : glm::vec3(0.0f), rot_axis, kind == 1 ? rot_deg * wt : 0.0f,
                                                      kind == 2 ? glm::vec3(1.0f) + (scale - glm::vec3(1.0f)) * wt : glm::vec3(1.0f),
                                                      pivot, scale_basis);
                    mm.positions[v] = glm::vec3(inv_w * xw * w * glm::vec4(base.positions[v], 1.0f));
                }
                apply_mirror_map(mm, mirror_map_);
            }, "transform");
            return;
        }
        if (playing()) return;
        glm::vec3 pivot(0.0f);
        for (const auto& [id, s] : drag_starts_) pivot += s.world_pos;
        if (!drag_starts_.empty()) pivot /= static_cast<float>(drag_starts_.size());
        const bool multi = drag_starts_.size() > 1;
        for (const auto& [id, s] : drag_starts_) {
            const glm::mat4 inv_parent = glm::inverse(s.parent_world);
            glm::vec3 pos = s.pos, rot = s.rot, scl = s.scl;
            if (kind == 0) {
                pos = glm::vec3(inv_parent * glm::vec4(s.world_pos + translate, 1.0f));
            } else if (kind == 1) {
                const glm::mat4 R = glm::rotate(glm::mat4(1.0f), glm::radians(rot_deg), glm::normalize(rot_axis));
                const glm::mat3 pr(s.parent_world);
                const glm::mat4 parent_rot(glm::mat3(glm::normalize(pr[0]), glm::normalize(pr[1]), glm::normalize(pr[2])));
                rot = matrix_to_euler(glm::inverse(parent_rot) * R * parent_rot * euler_to_matrix(s.rot));
                if (multi) pos = glm::vec3(inv_parent * glm::vec4(pivot + glm::vec3(R * glm::vec4(s.world_pos - pivot, 0.0f)), 1.0f));
            } else {
                scl = s.scl * scale;
                if (multi) pos = glm::vec3(inv_parent * glm::vec4(pivot + (s.world_pos - pivot) * scale, 1.0f));
            }
            set_object_transform_(id, pos, rot, scl, kind == 0 ? "Move" : kind == 1 ? "Rotate" : "Scale", "transform");
        }
    }

    void box_select_(bool mesh_edit, glm::vec2 a, glm::vec2 b, bool additive, bool subtract) {
        auto vp = view_proj_();
        if (!vp) return;
        const imm::Box r{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
        auto inside = [&](const glm::vec3& p) { auto q = vp->project(p); return q && r.contains(*q); };
        if (mesh_edit) {
            auto& mm = mesh_.mesh;
            auto& sel = mesh_.selection;
            const glm::mat4 w = mesh_world_();
            auto wp = [&](const glm::vec3& p) { return glm::vec3(w * glm::vec4(p, 1.0f)); };
            if (!additive && !subtract) { sel.verts.clear(); sel.edges.clear(); sel.faces.clear(); }
            auto apply = [&](auto& set, const auto& v) { if (subtract) set.erase(v); else set.insert(v); };
            // As in Blender: only what is in view, unless X-Ray (or Wireframe) sees through.
            const EditVisibility& vis = edit_visibility_(*vp);
            if (sel.mode == SelectMode::Vertex) {
                for (uint32_t v = 0; v < mm.positions.size(); ++v) if (vis.verts[v] && inside(wp(mm.positions[v]))) apply(sel.verts, v);
            } else if (sel.mode == SelectMode::Edge) {
                for (const auto& e : mm.edges()) {
                    auto it = vis.edges.find(e);
                    if (it != vis.edges.end() && it->second && inside(wp((mm.positions[e.first] + mm.positions[e.second]) * 0.5f))) apply(sel.edges, e);
                }
            } else {
                for (uint32_t f = 0; f < mm.faces.size(); ++f) if (vis.faces[f] && inside(wp(mm.face_center(f)))) apply(sel.faces, f);
            }
            return;
        }
        if (!additive && !subtract) doc_.clear_selection();
        std::set<ObjectId> hits;
        for (const auto& [lid, live] : sync_.live_objects()) {
            if (!live || !live->get_transform() || hidden_.count(lid)) continue;
            if (!inside(glm::vec3(live->get_transform()->transform().get_world_matrix()[3]))) continue;
            hits.insert(box_target_(lid));   // a part of an instance selects the instance
        }
        for (ObjectId id : hits) {
            if (subtract) { if (doc_.is_selected(id)) doc_.select(id, true); }
            else if (!doc_.is_selected(id)) doc_.select(id, true);
        }
    }

    void draw_viewport_overlay_(imm::Context& ctx, bool mesh_edit) {
        auto vp = view_proj_();
        if (!vp) return;
        ctx.push_clip(vp->rect);   // the scene image, not the letterbox bars around it
        draw_viewport_overlay_body_(ctx, mesh_edit, *vp);
        if (anim_record_ && anim_posed_) {
            // Recording (the Timeline's red dot): Blender's red frame -- moves become keys.
            const imm::Box r = vp->rect;
            ctx.outline({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, glm::vec4(0.9f, 0.18f, 0.18f, 0.9f), 3.0f);
            shadow_text_(ctx, glm::vec2(r.x + 10, r.bottom() - 24), "Recording: moves become keys at frame " +
                         std::to_string(static_cast<int>(std::round(anim_time_ * kAnimFps))), glm::vec4(1.0f, 0.45f, 0.45f, 1));
        }
        ctx.pop_clip();
    }

    void draw_viewport_overlay_body_(imm::Context& ctx, bool mesh_edit, const ViewProj& vp_ref) {
        const ViewProj* vp = &vp_ref;
        grid_wanted_ = show_overlays_ && show_grid_ && !(playing() && !mesh_edit) &&
                       active_type_ != AssetType::Material && active_type_ != AssetType::Texture;
        const glm::vec4 accent = ctx.style.object_selected;
        const glm::vec4 active_col = ctx.style.object_active;
        auto draw_mesh_edges = [&](const EditMesh& mm, const std::vector<Edge>& edges, const glm::mat4& world, glm::vec4 col, float t) {
            size_t drawn = 0;
            for (const auto& e : edges) {
                if (++drawn > 30000) break;
                auto a = vp->project(glm::vec3(world * glm::vec4(mm.positions[e.first], 1.0f)));
                auto b = vp->project(glm::vec3(world * glm::vec4(mm.positions[e.second], 1.0f)));
                if (a && b) ctx.line(*a, *b, col, t);
            }
        };
        if (in_brush_mode_()) {
            // No selection outline or wire while sculpting or painting, as in Blender.
            if (in_sculpt_mode_()) draw_sculpt_overlay_(ctx, *vp);
            else draw_paint_overlay_(ctx, *vp);
        } else if (asset_view_() && !mesh_edit) {
            // Asset previews (mesh in Object Mode, material lookdev, texture): no scene overlays.
        } else if (!mesh_edit) {
            // Wireframe shading: every mesh object's edges. (Not X-Ray: as in Blender, it makes
            // surfaces translucent and draws no wires -- they read as outlines on everything.)
            if (shading_ == Shading::Wireframe) {
                for (const auto& [id, live] : sync_.live_objects()) {
                    const Node* node = doc_.find(id);
                    if (!node || !live || !live->active() || !live->get_transform() || doc_.is_selected(id)) continue;
                    if (const CachedMesh* cm = mesh_for_object_(*node)) {
                        draw_mesh_edges(cm->mesh, cm->edges, live->get_transform()->transform().get_world_matrix(),
                                        et_.viewport.object_wire, 1.0f);
                    }
                }
            }
            // Non-mesh objects: Blender's glyphs (camera frustum, light symbols, empty axes).
            if (show_overlays_ && show_glyphs_) {
                for (const auto& [id, live] : sync_.live_objects()) {
                    const Node* node = doc_.find(id);
                    if (!node || !live || !live->active() || !live->get_transform() || mesh_for_object_(*node)) continue;
                    glm::vec4 col(0.02f, 0.02f, 0.02f, 0.9f);
                    if (shading_ == Shading::Wireframe) col = et_.viewport.wire;
                    if (doc_.is_selected(id)) col = doc_.primary() == id ? active_col : accent;
                    draw_object_glyph_(ctx, *vp, *node, live->get_transform()->transform().get_world_matrix(), col);
                }
            }
            // Selection outlines (the active object brighter, as in Blender). A selected instance
            // is one thing: its inherited parts are outlined with it.
            for (ObjectId id : doc_.selection()) {
                auto* live = sync_.live(id);
                const Node* node = doc_.find(id);
                if (!live || !node || !live->get_transform() || !live->active()) continue;
                const glm::vec4 col = doc_.primary() == id ? active_col : accent;
                std::function<void(ObjectId, const Node&)> outline = [&](ObjectId oid, const Node& n) {
                    auto* l = sync_.live(oid);
                    if (!l || !l->get_transform() || !l->active()) return;
                    if (const CachedMesh* cm = mesh_for_object_(n)) draw_mesh_edges(cm->mesh, cm->edges, l->get_transform()->transform().get_world_matrix(), col, 1.5f);
                    if (!doc_.is_instance(id) || !n.contains("children")) return;
                    for (const auto& c : n.at("children").as_seq()) if (c.contains(kInheritedKey)) outline(SceneDocument::id_of(c), c);
                };
                outline(id, *node);
                // Origin dot.
                if (show_overlays_ && show_origins_) {
                    if (auto o = vp->project(glm::vec3(live->get_transform()->transform().get_world_matrix()[3]))) {
                        ctx.circle(*o, 3.0f, et_.viewport.origin_outline);
                        ctx.circle(*o, 2.0f, doc_.primary() == id ? active_col : accent);
                    }
                }
            }
        } else if (mesh_.open()) {
            const EditMesh& mm = mesh_.mesh;
            const auto& sel = mesh_.selection;
            const glm::mat4 w = mesh_world_();
            auto wp = [&](const glm::vec3& p) { return glm::vec3(w * glm::vec4(p, 1.0f)); };
            // Only what the camera can see (Solid shading), as in Blender; X-ray shows all, what
            // is behind the surface dimmed (EditVisibility::kBehind) so the depth still reads.
            const EditVisibility& vis = edit_visibility_(*vp);
            auto dim = [&](char state, glm::vec4 c) { return state == EditVisibility::kBehind ? imm::with_alpha(c, c.a * 0.4f) : c; };
            std::vector<Edge> front, behind;
            front.reserve(vis.edges.size());
            for (const auto& [e, state] : vis.edges) {
                if (state == EditVisibility::kBehind) behind.push_back(e);
                else if (state) front.push_back(e);
            }
            draw_mesh_edges(mm, behind, w, dim(EditVisibility::kBehind, et_.viewport.edit_wire), 1.0f);
            draw_mesh_edges(mm, front, w, et_.viewport.edit_wire, 1.0f);
            auto edge_state = [&](uint32_t x, uint32_t y) -> char {
                auto it = vis.edges.find(make_edge(x, y));
                return it != vis.edges.end() ? it->second : 0;
            };
            auto edge_shown = [&](uint32_t x, uint32_t y) { return edge_state(x, y) != 0; };
            if (sel.mode == SelectMode::Face) {
                for (uint32_t f : sel.faces) {
                    if (f >= mm.faces.size()) continue;
                    const auto& c = mm.faces[f].corners;
                    for (size_t i = 0; i < c.size(); ++i) {
                        const uint32_t x = c[i].v, y = c[(i + 1) % c.size()].v;
                        if (!edge_shown(x, y)) continue;
                        auto a = vp->project(wp(mm.positions[x])), b = vp->project(wp(mm.positions[y]));
                        if (a && b) ctx.line(*a, *b, dim(edge_state(x, y), accent), 2.0f);
                    }
                }
                for (uint32_t f = 0; f < mm.faces.size(); ++f) {
                    if (!vis.faces[f]) continue;
                    if (auto p = vp->project(wp(mm.face_center(f)))) {
                        ctx.fill({p->x - 2, p->y - 2, 4, 4}, dim(vis.faces[f], sel.faces.count(f) ? accent : et_.viewport.face_dot));
                    }
                }
            } else if (sel.mode == SelectMode::Edge) {
                for (const auto& e : sel.edges) {
                    if (!edge_shown(e.first, e.second)) continue;
                    auto a = vp->project(wp(mm.positions[e.first])), b = vp->project(wp(mm.positions[e.second]));
                    if (a && b) ctx.line(*a, *b, dim(edge_state(e.first, e.second), accent), 2.5f);
                }
            } else {
                for (uint32_t v = 0; v < mm.positions.size(); ++v) {
                    if (!vis.verts[v]) continue;
                    auto p = vp->project(wp(mm.positions[v]));
                    if (!p) continue;
                    ctx.fill({p->x - 3, p->y - 3, 6, 6}, dim(vis.verts[v], sel.verts.count(v) ? accent : et_.viewport.vertex));
                }
            }
            draw_mesh_tool_overlay_(ctx, *vp);
        }
        // 3D cursor (scene).
        if (!asset_view_() && show_overlays_ && show_cursor3d_) {
            if (auto c = vp->project(cursor3d_)) {
                for (int i = 0; i < 8; ++i) {
                    const float a0 = 6.2831853f * i / 8.0f, a1 = 6.2831853f * (i + 1) / 8.0f;
                    ctx.line(*c + glm::vec2(std::cos(a0), std::sin(a0)) * 9.0f, *c + glm::vec2(std::cos(a1), std::sin(a1)) * 9.0f,
                             i % 2 ? et_.viewport.cursor_ring_a : et_.viewport.cursor_ring_b, 1.5f);
                }
                ctx.line(*c - glm::vec2(14, 0), *c - glm::vec2(5, 0), et_.viewport.cursor_cross, 1.0f);
                ctx.line(*c + glm::vec2(5, 0), *c + glm::vec2(14, 0), et_.viewport.cursor_cross, 1.0f);
                ctx.line(*c - glm::vec2(0, 14), *c - glm::vec2(0, 5), et_.viewport.cursor_cross, 1.0f);
                ctx.line(*c + glm::vec2(0, 5), *c + glm::vec2(0, 14), et_.viewport.cursor_cross, 1.0f);
            }
        }
        // Gizmo (transform tools only, hidden while a modal operator runs).
        if (tool_ != Tool::Select && show_gizmo_ && !modal_.active() && !(playing() && !mesh_edit)) {
            glm::vec3 pivot;
            glm::mat3 basis;
            gizmo_.free_color = et_.viewport.gizmo_free;
            if (gizmo_target_(mesh_edit, pivot, basis)) gizmo_.draw(ctx, *vp, pivot, basis);
        }
        draw_overlay_text_(ctx, *vp);
        // Mode banner.
        if (mesh_edit && !asset_view_()) {
            const std::string t = "Edit Mode  -  " + mesh_.name + (mesh_.dirty() ? " *" : "");
            ctx.text_in({vp->rect.x + 8, vp->rect.bottom() - 26, 400, 20}, t, ctx.style.text_dim);
        }
    }

    // =================================================================================
    // Modals and dialogs
    // =================================================================================

    void draw_modals_(imm::Context& ctx) {
        file_dialog_.draw(ctx);
        draw_build_modal_(ctx);
        draw_build_settings_modal_(ctx);

        if (ctx.begin_modal("Unsaved Changes", {370, 0})) {   // height fits the content
            ctx.paragraph("There are unsaved changes. Save them first?");
            ctx.spacing(4);
            if (ctx.button("Save All", 110)) {
                save_all_();
                ctx.close_modal();
                if (auto fn = std::move(pending_after_confirm_)) { pending_after_confirm_ = {}; fn(); }
            }
            ctx.same_line();
            if (ctx.button("Discard", 110)) {
                ctx.close_modal();
                discard_all_changes_();
                if (auto fn = std::move(pending_after_confirm_)) { pending_after_confirm_ = {}; fn(); }
            }
            ctx.same_line();
            if (ctx.button("Cancel", 110)) { ctx.close_modal(); pending_after_confirm_ = {}; }
            ctx.end_modal();
        }

        if (ctx.begin_modal("New Project", {520, 190})) {
            ctx.paragraph("Creates a project folder with an assets/ tree, a config.yaml and a starter scene.");
            ctx.input_text("Folder", &new_project_path_);
            if (ctx.button("Browse...", 100)) {
                ctx.close_modal();
                file_dialog_.open(ctx, FileDialog::Mode::PickFolder, "Choose Parent Folder", fs::path(new_project_path_).parent_path(), {},
                                  [this](const fs::path& p) { new_project_path_ = (p / "my_project").string(); pending_modal_ = "New Project"; });
            }
            ctx.same_line();
            if (ctx.button("Create", 100)) {
                ctx.close_modal();
                try {
                    Project::create(new_project_path_);
                    const fs::path p = new_project_path_;
                    guarded_([this, p] { switch_project_ = p; });
                } catch (const std::exception& e) {
                    log_error(std::string("Create project failed: ") + e.what());
                }
            }
            ctx.same_line();
            if (ctx.button("Cancel", 100)) ctx.close_modal();
            ctx.end_modal();
        }

        if (ctx.begin_modal("Package Project", {520, 210})) {
            ctx.paragraph("Copies assets/ to the output folder with every YAML file encoded as .caml. The packaged game "
                          "loads them unchanged (set TOY_CAML_KEY if you use a custom key).");
            ctx.input_text("Output folder", &package_dir_);
            ctx.property_bool("Keep .yaml copies", &package_keep_yaml_);
            if (ctx.button("Package", 110)) {
                ctx.close_modal();
                package(package_dir_, package_keep_yaml_);
            }
            ctx.same_line();
            if (ctx.button("Cancel", 100)) ctx.close_modal();
            ctx.end_modal();
        }

        if (ctx.begin_modal("Controls", {720, 0})) {   // height fits the content
            static const char* lines[] = {
                "Navigate:   MMB orbit, Shift+MMB pan, Ctrl+MMB / wheel zoom; Alt+LMB = MMB (no middle button)",
                "            Shift+wheel / Ctrl+wheel pan vertically / horizontally, sideways scroll orbits",
                "Trackpad:   two-finger swipe orbits, Shift+swipe pans, pinch / Ctrl+swipe zooms",
                "Views:      numpad 1/3/7 front/right/top (Ctrl opposite), 5 ortho, 0 camera, 2/4/6/8 orbit,",
                "            F or . frame selected (F: Object Mode), Home frame all, ` view menu, Ctrl+Alt+Num0 camera to view",
                "Select:     LMB, Shift+LMB toggle, drag box (Shift add, Ctrl subtract), A all, Alt+A none, Ctrl+I invert",
                "Transform:  G move, R rotate, S scale -- then X/Y/Z axis (twice: local), Shift+X plane,",
                "            type a value, Ctrl snap, Shift precise, LMB/Enter confirm, RMB/Esc cancel",
                "Objects:    Shift+A add, Shift+D duplicate, X / Del delete, H hide, Alt+H unhide, Shift+H isolate,",
                "            Ctrl+P parent, Alt+P clear parent, Alt+G/R/S clear, Tab edit mode, RMB context menu",
                "Edit mode:  1/2/3 vert/edge/face, E extrude, I inset, Ctrl+B bevel, F fill, M merge, X delete,",
                "            L / Ctrl+L linked, Shift+D duplicate, U UV menu, Alt+N normals",
                "Viewport:   Z shading menu, Shift+Z wireframe, Shift+S snap, Shift+RMB 3D cursor, N / T panels,",
                "            Ctrl+Space maximize, Shift+Space tools",
                "General:    Ctrl+S save all, Ctrl+Z / Ctrl+Shift+Z undo / redo, F5 play / stop, F2 rename",
            };
            for (const char* l : lines) ctx.label(l);
            ctx.spacing();
            if (ctx.button("Close", 100)) ctx.close_modal();
            ctx.end_modal();
        }
        draw_about_modal_(ctx);
        if (!pending_modal_.empty()) { ctx.open_modal(pending_modal_); pending_modal_.clear(); }
    }

    void confirm_unsaved_(std::function<void()> then) {
        pending_after_confirm_ = std::move(then);
        pending_modal_ = "Unsaved Changes";
    }

    /** @brief Runs `fn` now if nothing is unsaved, else after the user saves or discards. */
    void guarded_(std::function<void()> fn) {
        if (!has_unsaved()) { deferred_.push_back(std::move(fn)); return; }
        confirm_unsaved_([this, fn = std::move(fn)] { deferred_.push_back(fn); });
    }

    void discard_all_changes_() {
        // Mark everything clean; the action that follows replaces or reloads what it needs.
        if (doc_.dirty()) { doc_.undo_stack().clear(); }
        mesh_.saved_revision = mesh_.undo.revision();
        material_.saved_revision = material_.undo.revision();
        game_theme_.saved_revision = game_theme_.undo.revision();
        config_.saved_revision = config_.undo.revision();
        force_quit_ = true;
    }

    void save_active_() { save_all_(); }

    void save_all_() {
        if (doc_.dirty()) save_scene();
        if (mesh_.open() && mesh_.dirty()) save_mesh();
        if (material_.open() && material_.dirty()) save_material();
        if (game_theme_.open() && game_theme_.dirty()) save_theme();
        if (config_.dirty()) save_config();
    }

    /** @brief Picks a sound file and imports it into assets/audio/<tags>/. */
    void import_audio_dialog_(std::vector<std::string> tags = {}) {
        const char* home = std::getenv("HOME");
        file_dialog_.open(ui(), FileDialog::Mode::OpenFile, "Import Sound", home ? fs::path(home) : project_.root(), {".wav", ".mp3"},
                          [this, tags](const fs::path& p) {
                              deferred_.push_back([this, p, tags] {
                                  fs::path dir;
                                  int n = 0;
                                  with_tags_(tags, [&] { dir = project_.assets() / new_asset_dir_("audio"); n = import_audio({p}); });
                                  if (n > 0) open_asset(AssetType::Audio, project_.relative(dir / p.filename()));
                              });
                          });
    }

    void open_scene_dialog_() {
        file_dialog_.open(ui(), FileDialog::Mode::OpenFile, "Open Scene", project_.assets() / "scenes", {".yaml", ".caml"},
                          [this](const fs::path& p) { deferred_.push_back([this, p] { open_scene(p); }); });
    }

    void save_scene_as_dialog_() {
        std::string name = doc_.scene_name();
        for (char& ch : name) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        const fs::path start = doc_.path().empty() ? project_.assets() / "scenes" : doc_.path().parent_path();
        file_dialog_.open(ui(), FileDialog::Mode::SaveFile, "Save Scene As", start, {".yaml"},
                          [this](const fs::path& p) { save_scene_as(p); }, doc_.path().empty() ? name + ".yaml" : doc_.path().filename().string());
    }

    void open_project_dialog_() {
        file_dialog_.open(ui(), FileDialog::Mode::PickFolder, "Open Project (folder with a .toy or assets/)", project_.root().parent_path(), {},
                          [this](const fs::path& p) {
                              Project candidate(p);
                              if (!candidate.valid()) { log_error(p.string() + " has no .toy file or assets/ folder"); return; }
                              guarded_([this, p] { switch_project_ = p; });
                          });
    }

    void new_project_dialog_() {
        if (new_project_path_.empty()) new_project_path_ = (project_.root().parent_path() / "my_project").string();
        pending_modal_ = "New Project";
    }

    void open_package_dialog_() {
        if (package_dir_.empty()) package_dir_ = (project_.root() / "build" / "package").string();
        pending_modal_ = "Package Project";
    }

    std::string current_undo_label_(bool undo) {
        auto pick = [&](const std::string& u, const std::string& r) { return undo ? u : r; };
        switch (undo_target_()) {
            case UndoTarget::Scene: return pick(doc_.undo_stack().undo_label(), doc_.undo_stack().redo_label());
            case UndoTarget::Mesh: return pick(mesh_.undo.undo_label(), mesh_.undo.redo_label());
            case UndoTarget::Material: return pick(material_.undo.undo_label(), material_.undo.redo_label());
            case UndoTarget::Theme: return pick(game_theme_.undo.undo_label(), game_theme_.undo.redo_label());
            case UndoTarget::Config: return pick(config_.undo.undo_label(), config_.undo.redo_label());
        }
        return {};
    }

    // =================================================================================
    // Shortcuts
    // =================================================================================

    /** @brief Window-wide keys; the viewport and outliner keymaps live with those areas. */
    void handle_shortcuts_(imm::Context& ctx) {
        const Mods cmd = Mods::Super;   // shortcut() also accepts Control for Super
        if (ctx.shortcut(Key::S, cmd)) save_all_();
        if (ctx.shortcut(Key::S, cmd | Mods::Shift)) save_scene_as_dialog_();
        if (ctx.shortcut(Key::Z, cmd)) undo();
        if (ctx.shortcut(Key::Z, cmd | Mods::Shift) || ctx.shortcut(Key::Y, cmd)) redo();
        if (ctx.shortcut(Key::N, cmd)) guarded_([this] { new_scene(); });
        if (ctx.shortcut(Key::O, cmd)) guarded_([this] { open_scene_dialog_(); });
        if (ctx.shortcut(Key::B, cmd | Mods::Shift)) build_refresh();
        if (ctx.shortcut(Key::Q, cmd)) { if (request_close()) quit_ = true; }
        if (ctx.shortcut(Key::F5)) {
            if (active_type_ == AssetType::UI) ui_set_interact_(!ui_interact_);
            else playing() ? stop() : play();
        }
        // Esc gives the mouse back to the editor; the game keeps running (F5 / Stop ends it).
        if (game_focused_ && ctx.shortcut(Key::Escape)) set_game_focus_(false);
        if (ctx.any_popup_open()) return;
        // Outliner keymap (mouse over the outliner).
        if (hierarchy_hovered_ && !asset_view_() && !playing()) {
            if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) delete_selected();
            if (ctx.shortcut(Key::F2) && doc_.primary() && !doc_.is_inherited(doc_.primary())) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
            if (ctx.shortcut(Key::A)) { doc_.clear_selection(); for (ObjectId id : visible_ids_()) doc_.select(id, true); }
            if (ctx.shortcut(Key::A, Mods::Alt)) doc_.clear_selection();
            if (ctx.shortcut(Key::H)) hide_(doc_.selection());
            if (ctx.shortcut(Key::H, Mods::Alt)) unhide_all_();
            if (ctx.shortcut(Key::KpDecimal) || ctx.shortcut(Key::Period) || ctx.shortcut(Key::F)) frame_selected();
        }
        if (ctx.shortcut(Key::F2) && !asset_view_() && doc_.primary() && !playing() && !doc_.is_inherited(doc_.primary())) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
    }

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
    ClipDocument anim_clip_;
    float anim_time_ = 0.0f;
    bool anim_playing_ = false, anim_record_ = false, anim_show_rest_ = false, anim_posed_ = false, anim_scrubbing_ = false;
    std::shared_ptr<coopa::anim::AnimationClip> anim_runtime_;
    uint64_t anim_runtime_rev_ = 0, anim_pose_rev_ = 0;
    float anim_pose_time_ = -1.0f;
    std::set<ClipModel::KeyRef> anim_sel_keys_;
    KeyDrag anim_drag_;
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

    void set_game_focus_(bool focused) {
        game_focused_ = focused;
        engine_.set_game_input_focus(focused);
    }
    std::unordered_map<ObjectId, imm::Box> eye_rects_;   // Outliner eye toggles (tests)
    std::map<std::string, imm::Box> test_rects_;           // Where named widgets were last drawn (tests)
    bool open_add_menu_ = false;
    float outliner_h_ = 260;
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

    // Hierarchy / browser.
    ObjectId context_target_ = 0;
    ObjectId rename_id_ = 0;
    int rename_frames_ = 0;
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
    std::optional<fs::path> switch_project_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_EDITOR_APP_H
