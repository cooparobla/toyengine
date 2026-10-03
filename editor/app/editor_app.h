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
#include "file_dialog.h"
#include "project.h"
#include "scene_sync.h"

#include "../build/packager.h"
#include "../core/scene_document.h"
#include "../mesh/mesh_ops.h"
#include "../mesh/primitives.h"
#include "../schema/component_schema.h"
#include "../schema/inspector.h"
#include "../schema/settings_schema.h"
#include "../viewport/editor_camera.h"
#include "../viewport/gizmo.h"
#include "../viewport/modal_transform.h"

#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>
#include <toyengine/render/passes/debug_line_pass.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/data/mesh.h>

#include <uicoopa/immediate/imm_canvas.h>
#include <uicoopa/ui_yaml.h>

#include <coopa/scene/scene_loader.h>

#include <root_directory.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

namespace fs = std::filesystem;
using coopa::input::Key;
using coopa::input::Mods;

enum class Tab { Scene = 0, Asset = 1, Render = 2, Project = 3 };
enum class Shading { Wireframe = 0, Solid = 1, Full = 2 };
enum class AssetKind { None, Mesh, Material };

/** @brief State carried across a renderer restart (the Engine is rebuilt underneath). */
struct EditorState {
    std::optional<SceneDocument> scene;
    Tab tab = Tab::Scene;
    Shading shading = Shading::Solid;
    glm::vec3 cam_focus{0.0f};
    float cam_yaw = 35.0f, cam_pitch = 25.0f, cam_distance = 12.0f;
    std::optional<ConfigDocument> config;
};

class EditorApp {
public:
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
        const std::string font_path = std::string(PROJ_DIR) + "/uicoopa/assets/fonts/Inter-Regular.ttf";
        canvas_->context().text.set_font(coopa::ui::UIResourceCache::instance().font_for_path(font_path));
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
        tab_ = state.tab;
        shading_ = state.shading;
        if (state.config) config_ = std::move(*state.config);
        else config_.load(project_.config_path());

        if (state.scene) {
            doc_ = std::move(*state.scene);
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
        s.tab = tab_;
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
    Tab tab() const { return tab_; }
    Shading shading() const { return shading_; }
    bool playing() const { return play_scene_ != nullptr; }
    /** @brief True while a scene object's mesh is in edit mode (Tab). */
    bool edit_mode_active() const { return in_edit_mode_(); }
    bool quit_requested() const { return quit_; }
    bool restart_requested() const { return restart_; }
    /** @brief A project switch was requested: the main loop rebuilds everything for it. */
    const std::optional<fs::path>& switch_project_requested() const { return switch_project_; }
    const std::deque<std::pair<int, std::string>>& log() const { return log_; }
    imm::Box viewport_box() const { return viewport_box_; }
    coopa::ui::imm::Context& ui() { return canvas_->context(); }

    /** @brief True if anything is unsaved (scene, mesh, material, config). */
    bool has_unsaved() const {
        return doc_.dirty() || (mesh_.open() && mesh_.dirty()) || (material_.open() && material_.dirty()) || config_.dirty();
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
        doc_.reset("Untitled");
        Node starter = Project::default_scene_node("Untitled");
        for (const auto& o : starter.at("scene").at("root_objects").as_seq()) {
            Node copy = o;
            doc_.add_object(copy, 0, -1, "New Scene");
        }
        doc_.undo_stack().clear();
        rebuild_scene_();
        frame_all();
    }

    bool open_scene(const fs::path& path) {
        stop();
        try {
            doc_.load(path);
        } catch (const std::exception& e) {
            log_error(std::string("Open failed: ") + e.what());
            return false;
        }
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
        try {
            doc_.save(path);
            sync_.fallback_path = path;
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
        Node obj = doc_.make_object(doc_.unique_name(primitive, parent));
        set_object_position_(obj, spawn_point_());
        Node mr = default_component("MeshRenderer");
        mr["mesh_path"] = Node(key);
        obj["components"].as_seq().push_back(mr);
        const ObjectId id = doc_.add_object(obj, parent, -1, "Create " + primitive);
        after_structure_change_(id);
        return id;
    }

    ObjectId create_empty(ObjectId parent = 0) {
        Node obj = doc_.make_object(doc_.unique_name("Empty", parent));
        set_object_position_(obj, parent ? glm::vec3(0.0f) : spawn_point_());
        const ObjectId id = doc_.add_object(obj, parent, -1, "Create Empty");
        after_structure_change_(id);
        return id;
    }

    /** @brief Creates an object carrying one component of `type` (lights, cameras, ...). */
    ObjectId create_with_component(const std::string& type, const std::string& name, ObjectId parent = 0) {
        Node obj = doc_.make_object(doc_.unique_name(name, parent));
        set_object_position_(obj, spawn_point_() + glm::vec3(0, 0, type == "Camera" ? 2.0f : 3.0f));
        obj["components"].as_seq().push_back(default_component(type));
        const ObjectId id = doc_.add_object(obj, parent, -1, "Create " + name);
        after_structure_change_(id);
        return id;
    }

    void delete_selected() {
        if (doc_.selection().empty()) return;
        apply_(doc_.delete_objects(doc_.selection()));
    }

    void duplicate_selected() {
        if (doc_.selection().empty()) return;
        auto ids = doc_.duplicate_objects(doc_.selection());
        doc_.clear_selection();
        for (ObjectId id : ids) doc_.select(id, true);
        queue_rebuild_();
    }

    void undo() {
        switch (tab_) {
            case Tab::Scene:
                if (in_edit_mode_()) mesh_.do_undo();
                else if (!playing()) { doc_.undo(); queue_rebuild_(); }
                break;
            case Tab::Asset:
                if (asset_kind_ == AssetKind::Mesh) mesh_.do_undo();
                else if (asset_kind_ == AssetKind::Material) { material_.do_undo(); refresh_material_preview_(); }
                break;
            default: config_.do_undo(); apply_config_live(); break;
        }
    }
    void redo() {
        switch (tab_) {
            case Tab::Scene:
                if (in_edit_mode_()) mesh_.do_redo();
                else if (!playing()) { doc_.redo(); queue_rebuild_(); }
                break;
            case Tab::Asset:
                if (asset_kind_ == AssetKind::Mesh) mesh_.do_redo();
                else if (asset_kind_ == AssetKind::Material) { material_.do_redo(); refresh_material_preview_(); }
                break;
            default: config_.do_redo(); apply_config_live(); break;
        }
    }

    /** @brief Frames the selection (or everything) in the viewport. */
    void frame_selected() {
        glm::vec3 lo(1e30f), hi(-1e30f);
        bool any = false;
        if ((tab_ == Tab::Asset && asset_kind_ == AssetKind::Mesh && mesh_.open()) || in_edit_mode_()) {
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
        glm::vec3 lo(1e30f), hi(-1e30f);
        bool any = false;
        if (tab_ == Tab::Asset && asset_kind_ != AssetKind::None) {
            if (asset_kind_ == AssetKind::Mesh) mesh_.mesh.bounds(lo, hi);
            else { lo = glm::vec3(-0.6f); hi = glm::vec3(0.6f); }
            any = true;
        } else {
            for (ObjectId id : doc_.all_ids()) any |= object_bounds_(id, lo, hi, /*meshes_only=*/true);
        }
        if (any && glm::length(hi - lo) < 1e5f) camera_.frame(lo, hi);
    }

    // --- play mode ---

    void play() {
        if (playing()) return;
        deferred_.push_back([this] {
            const std::string anchor = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).string();
            try {
                coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc_.node(), anchor);
                play_scene_ = &engine_.push_scene(std::move(scene), true);
                engine_.set_edit_mode(false);
                play_scene_->set_simulating(true);
                if (auto* cam = play_scene_->find_first_component<coopa::gfx::engine::components::CameraComponent>()) cam->make_main();
                log_info("Play");
            } catch (const std::exception& e) {
                play_scene_ = nullptr;
                log_error(std::string("Play failed: ") + e.what());
            }
        });
    }

    void stop() {
        if (!play_scene_) return;
        engine_.remove_scene(play_scene_);
        play_scene_ = nullptr;
        engine_.set_edit_mode(true);
        if (sync_.scene()) engine_.activate_scene(sync_.scene());
        camera_.make_main();
        log_info("Stop");
    }

    // --- shading / tabs ---

    void set_shading(Shading s) {
        shading_ = s;
        apply_shading_();
    }

    void set_tab(Tab t) {
        if (t == tab_) return;
        if ((tab_ == Tab::Asset) != (t == Tab::Asset)) {
            // The asset preview and the scene each keep their own view.
            gizmo_ = Gizmo{};
            CameraPose& out = tab_ == Tab::Asset ? asset_pose_ : scene_pose_;
            const CameraPose& in = t == Tab::Asset ? asset_pose_ : scene_pose_;
            out = {camera_.focus, camera_.yaw_deg, camera_.pitch_deg, camera_.distance, camera_.ortho, true};
            if (in.valid) {
                camera_.focus = in.focus; camera_.yaw_deg = in.yaw; camera_.pitch_deg = in.pitch;
                camera_.distance = in.distance; camera_.ortho = in.ortho;
                camera_.apply();
            }
        }
        tab_ = t;
        deferred_.push_back([this] { ensure_active_scene_(); });
    }

    // =================================================================================
    // Asset actions
    // =================================================================================

    bool open_mesh(const fs::path& path) {
        try {
            mesh_.load(path);
        } catch (const std::exception& e) {
            log_error(std::string("Open mesh failed: ") + e.what());
            return false;
        }
        asset_kind_ = AssetKind::Mesh;
        set_tab(Tab::Asset);
        mesh_edit_ = true;
        uploaded_revision_ = 0;
        deferred_.push_back([this] { ensure_preview_(); frame_all(); });
        log_info("Editing mesh " + project_.relative(path));
        return true;
    }

    void new_mesh(const std::string& primitive) {
        std::string key = primitive;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        for (char& c : key) if (c == ' ') c = '_';
        mesh_.reset(make_primitive(primitive), unique_asset_name_("meshes", key));
        asset_kind_ = AssetKind::Mesh;
        mesh_edit_ = true;
        uploaded_revision_ = 0;
        set_tab(Tab::Asset);
        deferred_.push_back([this] { ensure_preview_(); frame_all(); });
    }

    bool save_mesh() {
        if (!mesh_.open()) return false;
        if (mesh_.path.empty()) mesh_.path = project_.assets() / "meshes" / (mesh_.name + ".yaml");
        try {
            mesh_.save();
            project_.refresh();
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
        std::string ref = project_.relative(path);
        if (ref.size() > 5 && (ref.compare(ref.size() - 5, 5, ".yaml") == 0 || ref.compare(ref.size() - 5, 5, ".caml") == 0)) {
            ref = ref.substr(0, ref.size() - 5);
        }
        try {
            material_.load(path, ref);
        } catch (const std::exception& e) {
            log_error(std::string("Open material failed: ") + e.what());
            return false;
        }
        asset_kind_ = AssetKind::Material;
        set_tab(Tab::Asset);
        deferred_.push_back([this] { ensure_preview_(); refresh_material_preview_(); frame_all(); });
        log_info("Editing material " + ref);
        return true;
    }

    /** @brief Creates materials/<name>.yaml (from `from` if given) and opens it. */
    bool create_material(const std::string& name, const Node& from = Node()) {
        const std::string n = unique_asset_name_("materials", name);
        const fs::path path = project_.assets() / "materials" / (n + ".yaml");
        Node m = from.is_mapping() ? from : Node::mapping();
        erase_key(m, "base");
        if (m.size() == 0) {
            m["albedo"] = make_color(glm::vec3(0.8f));
            m["metallic"] = make_float(0.0);
            m["roughness"] = make_float(0.5);
        }
        try {
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
    void apply_config_live() {
        core::AppConfig parsed = core::AppConfig::from_node(config_.node);
        render::PixelRenderConfig next = parsed.render;
        const render::PixelRenderConfig& live = engine_.render_config();
        next.shader_dir = live.shader_dir;
        next.shaders = live.shaders;
        next.surface_shaders = live.surface_shaders;
        next.palette_path = live.palette_path;
        next.grading_lut_path = live.grading_lut_path;
        // The viewport shading mode owns debug_view while the editor runs.
        next.debug_view = live.debug_view;
        engine_.pipeline().apply_live_config(next);
        config_base_debug_view_ = parsed.render.debug_view;
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
        const fs::path game = fs::path(ROOT_DIR) / "build" / "toyengine";
        if (fs::exists(game)) opt.game_binary = game;
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
        float best_t = 1e30f, best_marker_px = 1e30f;
        for (const auto& [id, live] : sync_.live_objects()) {
            const Node* node = doc_.find(id);
            if (!node || !live || !live->get_transform()) continue;
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
                }
            }
        }
        // A marker clicked dead-on wins; otherwise surfaces take priority over markers.
        if (best_marker && (best == 0 || best_marker_px < 4.0f)) return best_marker;
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

    std::string unique_asset_name_(const std::string& dir, const std::string& base) {
        auto exists = [&](const std::string& n) { return coopa::yaml::document_exists(project_.assets() / dir / (n + ".yaml")); };
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
        return env;
    }

    // =================================================================================
    // Document -> live scene
    // =================================================================================

    void rebuild_scene_() {
        stop();   // set_scene() below replaces every engine scene, a playing one included
        mesh_cache_.clear();
        sync_.rebuild(engine_, doc_);
        if (!sync_.last_error.empty()) { log_error("Scene: " + sync_.last_error); sync_.last_error.clear(); }
        preview_scene_ = nullptr;   // set_scene() replaced every engine scene
        preview_object_ = nullptr;
        uploaded_revision_ = 0;
        scene_uploaded_revision_ = 0;   // re-show an unsaved edited mesh on the new live objects
        if (edit_object_ && !doc_.find(edit_object_)) edit_object_ = 0;
        for (auto it = hidden_.begin(); it != hidden_.end();) it = doc_.find(*it) ? std::next(it) : hidden_.erase(it);
        ensure_active_scene_();
        apply_hidden_();
    }

    void queue_rebuild_() {
        if (rebuild_queued_) return;
        rebuild_queued_ = true;
        deferred_.push_back([this] { rebuild_queued_ = false; rebuild_scene_(); });
    }

    /** @brief Applies a document change to the live scene (deferred to a safe point). */
    void apply_(const Change& c) {
        if (c.scope == ChangeScope::None) return;
        if (c.scope == ChangeScope::Structure) { queue_rebuild_(); return; }
        if (c.scope == ChangeScope::Transform) {   // cheap and safe immediately
            sync_.apply(engine_, doc_, c);
            return;
        }
        deferred_.push_back([this, c] { if (!rebuild_queued_) sync_.apply(engine_, doc_, c); });
    }

    void after_structure_change_(ObjectId select) {
        doc_.select(select);
        queue_rebuild_();
    }

    /** @brief Makes the scene the current tab wants the engine's active scene. */
    void ensure_active_scene_() {
        coopa::scene::Scene* want = nullptr;
        if (tab_ == Tab::Asset && asset_kind_ != AssetKind::None) {
            ensure_preview_();
            want = preview_scene_;
        } else {
            want = play_scene_ ? play_scene_ : sync_.scene();
        }
        if (want && engine_.has_scene() && &engine_.scene() != want) engine_.activate_scene(want);
        if (!play_scene_ || tab_ == Tab::Asset) camera_.make_main();
        apply_shading_();
    }

    // =================================================================================
    // Asset preview scene
    // =================================================================================

    void ensure_preview_() {
        if (preview_scene_) return;
        Node doc = Node::mapping();
        Node scene = Node::mapping();
        scene["scene_name"] = Node(std::string("EditorPreview"));
        Node roots = Node::sequence();
        auto object = [](const std::string& name) {
            Node o = Node::mapping();
            o["name"] = Node(name);
            Node comps = Node::sequence();
            Node t = Node::mapping();
            t["type"] = Node(std::string("Transform"));
            comps.as_seq().push_back(t);
            o["components"] = comps;
            return o;
        };
        Node light = object("PreviewLight");
        Node l = Node::mapping();
        l["type"] = Node(std::string("DirectionalLight"));
        l["direction"] = make_vec3({-0.45f, -0.55f, -0.7f});
        l["intensity"] = make_float(1.3);
        l["cast_shadows"] = Node(true);
        light["components"].as_seq().push_back(l);
        roots.as_seq().push_back(light);
        Node fill = object("PreviewFill");
        Node pl = Node::mapping();
        pl["type"] = Node(std::string("PointLight"));
        pl["intensity"] = make_float(40.0);
        pl["range"] = make_float(30.0);
        fill["components"].as_seq()[0]["position"] = make_vec3({3.0f, 4.0f, 3.0f});
        fill["components"].as_seq().push_back(pl);
        roots.as_seq().push_back(fill);
        Node obj = object("PreviewObject");
        Node mr = Node::mapping();
        mr["type"] = Node(std::string("MeshRenderer"));
        Node mat = Node::mapping();
        mat["albedo"] = make_color(glm::vec3(0.72f));
        mat["roughness"] = make_float(0.55);
        mr["material"] = mat;
        obj["components"].as_seq().push_back(mr);
        roots.as_seq().push_back(obj);
        scene["root_objects"] = roots;
        doc["scene"] = scene;
        try {
            coopa::scene::Scene s = coopa::scene::SceneLoader::load_from_node(doc, (project_.assets() / "__preview__.yaml").string());
            coopa::scene::Scene* active = engine_.has_scene() ? &engine_.scene() : nullptr;
            preview_scene_ = &engine_.push_scene(std::move(s), false);
            (void)active;
            preview_object_ = preview_scene_->find_object("PreviewObject");
            uploaded_revision_ = 0;
        } catch (const std::exception& e) {
            log_error(std::string("Preview scene failed: ") + e.what());
        }
    }

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
            if (asset_kind_ == AssetKind::Material) {
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

    void post_late_update_(float) {
        // Run queued actions; actions may queue more (run those next frame).
        auto pending = std::move(deferred_);
        deferred_.clear();
        for (auto& fn : pending) fn();
        // Keep live objects showing an edited, unsaved mesh; keep hidden objects hidden.
        if (tab_ == Tab::Scene && !playing()) { push_mesh_to_scene_(); apply_hidden_(); }
        // Keep the asset preview's mesh current.
        if (tab_ == Tab::Asset && preview_scene_) {
            if (asset_kind_ == AssetKind::Mesh && uploaded_revision_ != mesh_.geometry_revision) {
                upload_preview_mesh_(mesh_.mesh, "editor/preview_mesh");
                refresh_material_preview_();
                uploaded_revision_ = mesh_.geometry_revision;
            } else if (asset_kind_ == AssetKind::Material && uploaded_revision_ == 0) {
                upload_preview_mesh_(make_uv_sphere(0.5f, 48, 24), "editor/preview_sphere");
                refresh_material_preview_();
                uploaded_revision_ = 1;
            }
        }
    }

    void pre_render_(float) {
        // Scale the UI to the display's points (2x framebuffer pixels on Retina).
        ui_scale_ = std::max(1.0f, engine_.display_scale());
        if (auto* canvas = canvas_canvas_()) canvas->scaler.scale_factor = ui_scale_;
        if (!viewport_box_.empty()) {
            render::LetterboxRect r;
            r.x = static_cast<int32_t>(viewport_box_.x * ui_scale_);
            r.y = static_cast<int32_t>(viewport_box_.y * ui_scale_);
            r.w = static_cast<uint32_t>(std::max(1.0f, viewport_box_.w * ui_scale_));
            r.h = static_cast<uint32_t>(std::max(1.0f, viewport_box_.h * ui_scale_));
            engine_.set_display_region(r);
        }
        if (!playing() || tab_ == Tab::Asset) camera_.make_main();

    }

    coopa::ui::CanvasComponent* canvas_canvas_() {
        coopa::scene::SceneObject* o = canvas_->owner ? canvas_->owner->parent() : nullptr;
        return o ? o->get_component<coopa::ui::CanvasComponent>() : nullptr;
    }

    /**
     * @brief The ground grid, drawn in the full-resolution UI layer: faint, adaptive spacing
     *        (lines stay ~25+ px apart), fading out with distance from the focus point.
     */
    void draw_grid_(imm::Context& ctx, const ViewProj& vp) {
        const float d = camera_.distance;
        float step = 0.1f;
        while (step * 40.0f < d * 2.0f) step *= (std::fmod(std::log10(step) + 10.0f, 1.0f) < 0.1f) ? 5.0f : 2.0f;
        const float half = step * 30.0f;
        const glm::vec2 c(std::round(camera_.focus.x / step) * step, std::round(camera_.focus.y / step) * step);
        const int n = 30;
        auto seg_line = [&](glm::vec3 a, glm::vec3 b, glm::vec4 col, float t) {
            // Segmented so lines that cross behind the camera still draw their visible part.
            const int k = 24;
            std::optional<glm::vec2> prev = vp.project(a);
            glm::vec3 pa = a;
            for (int i = 1; i <= k; ++i) {
                const glm::vec3 pb = glm::mix(a, b, i / float(k));
                const auto q = vp.project(pb);
                const float fade = 1.0f - std::min(1.0f, glm::length(glm::vec2((pa + pb) * 0.5f) - glm::vec2(camera_.focus)) / half);
                if (prev && q && fade > 0.0f) ctx.line(*prev, *q, imm::with_alpha(col, col.a * fade), t);
                prev = q;
                pa = pb;
            }
        };
        for (int i = -n; i <= n; ++i) {
            const float x = c.x + i * step, y = c.y + i * step;
            const bool major_x = std::abs(std::remainder(x, step * 10.0f)) < step * 0.01f;
            const bool major_y = std::abs(std::remainder(y, step * 10.0f)) < step * 0.01f;
            glm::vec4 cx = major_x ? glm::vec4(1, 1, 1, 0.14f) : glm::vec4(1, 1, 1, 0.055f);
            glm::vec4 cy = major_y ? glm::vec4(1, 1, 1, 0.14f) : glm::vec4(1, 1, 1, 0.055f);
            if (std::abs(x) < step * 0.01f) cx = imm::with_alpha(ctx.style.axis_y, 0.45f);
            if (std::abs(y) < step * 0.01f) cy = imm::with_alpha(ctx.style.axis_x, 0.45f);
            seg_line({x, c.y - half, 0.0f}, {x, c.y + half, 0.0f}, cx, 1.0f);
            seg_line({c.x - half, y, 0.0f}, {c.x + half, y, 0.0f}, cy, 1.0f);
        }
    }

    void apply_shading_() {
        std::string view;
        switch (shading_) {
            case Shading::Wireframe: view = "wireframe"; break;
            case Shading::Solid: view = "solid"; break;
            case Shading::Full: view = full_debug_view_.empty() ? std::string("off") : full_debug_view_; break;
        }
        engine_.render_config().debug_view = view;
    }

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

    /** @brief CPU geometry for an object's MeshRenderer (cached by resolved path). */
    const CachedMesh* mesh_for_object_(const Node& obj) {
        if (!obj.contains("components")) return nullptr;
        for (const auto& c : obj.at("components").as_seq()) {
            if (component_type(c) != "MeshRenderer") continue;
            const std::string key = get_string(c, "mesh_path");
            if (key.empty()) return nullptr;
            const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
            const std::string resolved = engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string());
            auto it = mesh_cache_.find(resolved);
            if (it != mesh_cache_.end()) return it->second.mesh.faces.empty() ? nullptr : &it->second;
            CachedMesh cm;
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
    // UI: top level
    // =================================================================================

    void draw_(imm::Context& ctx) {
        const glm::vec2 sz = ctx.canvas_size();
        const float menubar_h = 24, toolbar_h = 30, status_h = 22;
        draw_menubar_(ctx, {0, 0, sz.x, menubar_h});
        draw_toolbar_(ctx, {0, menubar_h, sz.x, toolbar_h});
        const imm::Box content{0, menubar_h + toolbar_h, sz.x, std::max(0.0f, sz.y - menubar_h - toolbar_h - status_h)};
        switch (tab_) {
            case Tab::Scene:   draw_scene_tab_(ctx, content); break;
            case Tab::Asset:   draw_asset_tab_(ctx, content); break;
            case Tab::Render:  draw_render_tab_(ctx, content); break;
            case Tab::Project: draw_project_tab_(ctx, content); break;
        }
        draw_status_(ctx, {0, sz.y - status_h, sz.x, status_h});
        draw_modals_(ctx);
        handle_shortcuts_(ctx);
    }

    // --- menubar ---

    void draw_menubar_(imm::Context& ctx, const imm::Box& b) {
        ctx.begin_menubar(b);
        if (ctx.begin_menu("File")) {
            if (ctx.menu_item("New Scene", "Ctrl+N")) guarded_([this] { new_scene(); });
            if (ctx.menu_item("Open Scene...", "Ctrl+O")) guarded_([this] { open_scene_dialog_(); });
            if (ctx.begin_menu("Open Recent Scene")) {
                for (const auto& s : project_.scenes()) {
                    if (ctx.menu_item(s)) { const fs::path p = project_.absolute(s); guarded_([this, p] { open_scene(p); }); }
                }
                ctx.end_menu();
            }
            ctx.menu_separator();
            if (ctx.menu_item("Save", "Ctrl+S")) save_active_();
            if (ctx.menu_item("Save Scene As...", "Ctrl+Shift+S")) save_scene_as_dialog_();
            if (ctx.menu_item("Save All")) save_all_();
            ctx.menu_separator();
            if (ctx.menu_item("New Project...")) new_project_dialog_();
            if (ctx.menu_item("Open Project...")) open_project_dialog_();
            if (ctx.begin_menu("Recent Projects")) {
                for (const auto& r : Project::recent_projects()) {
                    if (ctx.menu_item(r)) { const fs::path p = r; guarded_([this, p] { switch_project_ = p; }); }
                }
                ctx.end_menu();
            }
            ctx.menu_separator();
            if (ctx.menu_item("Quit", "Ctrl+Q")) { if (request_close()) quit_ = true; }
            ctx.end_menu();
        }
        if (ctx.begin_menu("Edit")) {
            const std::string ul = "Undo " + current_undo_label_(true);
            const std::string rl = "Redo " + current_undo_label_(false);
            if (ctx.menu_item(ul, "Ctrl+Z")) undo();
            if (ctx.menu_item(rl, "Ctrl+Shift+Z")) redo();
            ctx.menu_separator();
            const bool scene_tab = tab_ == Tab::Scene && !playing();
            if (ctx.menu_item("Duplicate", "Shift D", nullptr, scene_tab)) { duplicate_selected(); pending_modal_kind_ = ModalKind::Grab; }
            if (ctx.menu_item("Delete", "X", nullptr, scene_tab)) delete_selected();
            if (ctx.menu_item("Select All", "A", nullptr, scene_tab)) { doc_.clear_selection(); for (ObjectId id : visible_ids_()) doc_.select(id, true); }
            if (ctx.menu_item("Select None", "Alt A", nullptr, scene_tab)) doc_.clear_selection();
            ctx.menu_separator();
            if (ctx.menu_item(in_edit_mode_() ? "Object Mode" : "Edit Mode", "Tab", nullptr, tab_ == Tab::Scene && !playing())) toggle_edit_mode_();
            if (ctx.menu_item("Parent to Active", "Ctrl P", nullptr, scene_tab)) parent_selection_to_active_();
            if (ctx.menu_item("Clear Parent (keep transform)", "Alt P", nullptr, scene_tab)) clear_parent_keep_transform_();
            if (ctx.menu_item("Hide Selected", "H", nullptr, scene_tab)) hide_(doc_.selection());
            if (ctx.menu_item("Unhide All", "Alt H", nullptr, scene_tab)) unhide_all_();
            ctx.end_menu();
        }
        if (ctx.begin_menu("View")) {
            if (ctx.begin_menu("Shading")) {
                bool w = shading_ == Shading::Wireframe, s = shading_ == Shading::Solid, f = shading_ == Shading::Full;
                if (ctx.menu_item("Wireframe", "Z", &w)) set_shading(Shading::Wireframe);
                if (ctx.menu_item("Solid", "Z", &s)) set_shading(Shading::Solid);
                if (ctx.menu_item("Full Render", "Z", &f)) set_shading(Shading::Full);
                ctx.end_menu();
            }
            if (ctx.begin_menu("Debug View (Full Render)")) {
                static const std::vector<std::string> views = {"off", "albedo", "normals", "roughness", "metallic", "emissive",
                    "material_ao", "world_pos", "depth", "direct", "indirect", "shadows", "contact_shadows", "ssao", "ssr",
                    "ssr_confidence", "ssgi", "dof", "volumetrics", "lines"};
                for (const auto& v : views) {
                    bool on = (full_debug_view_.empty() ? std::string("off") : full_debug_view_) == v;
                    if (ctx.menu_item(v, {}, &on)) { full_debug_view_ = v == "off" ? std::string() : v; set_shading(Shading::Full); }
                }
                ctx.end_menu();
            }
            if (ctx.menu_item("Grid", "", &show_grid_)) show_grid_ = !show_grid_;
            if (ctx.menu_item("Gizmos", "", &show_gizmo_)) show_gizmo_ = !show_gizmo_;
            ctx.menu_separator();
            if (ctx.menu_item("Frame Selected", "Num.")) frame_selected();
            if (ctx.menu_item("Frame All", "Home")) frame_all();
            if (ctx.menu_item(camera_.ortho ? "Perspective" : "Orthographic", "Num5")) { camera_.ortho = !camera_.ortho; camera_.apply(); }
            ctx.end_menu();
        }
        if (ctx.begin_menu("Create")) {
            const bool ok = tab_ == Tab::Scene && !playing();
            if (ctx.menu_item("Empty", "", nullptr, ok)) create_empty();
            if (ctx.begin_menu("Mesh", ok)) {
                for (const auto& p : primitive_names()) if (ctx.menu_item(p)) create_primitive(p);
                ctx.end_menu();
            }
            if (ctx.begin_menu("Light", ok)) {
                if (ctx.menu_item("Directional Light")) create_with_component("DirectionalLight", "Sun");
                if (ctx.menu_item("Point Light")) create_with_component("PointLight", "Point Light");
                if (ctx.menu_item("Spot Light")) create_with_component("SpotLight", "Spot Light");
                if (ctx.menu_item("Environment Light")) create_with_component("EnvironmentLight", "Environment");
                if (ctx.menu_item("Reflection Probe")) create_with_component("ReflectionProbe", "Reflection Probe");
                ctx.end_menu();
            }
            if (ctx.menu_item("Camera", "", nullptr, ok)) create_with_component("Camera", "Camera");
            if (ctx.menu_item("Terrain", "", nullptr, ok)) create_with_component("Terrain", "Terrain");
            ctx.menu_separator();
            if (ctx.begin_menu("New Mesh Asset")) {
                for (const auto& p : primitive_names()) if (ctx.menu_item(p)) new_mesh(p);
                ctx.end_menu();
            }
            if (ctx.menu_item("New Material Asset")) create_material("material");
            ctx.end_menu();
        }
        if (ctx.begin_menu("Build")) {
            if (ctx.menu_item(playing() ? "Stop" : "Play", "F5")) playing() ? stop() : play();
            ctx.menu_separator();
            if (ctx.menu_item("Package Project (.caml)...")) open_package_dialog_();
            if (ctx.menu_item("Restart Renderer")) restart_ = true;
            ctx.end_menu();
        }
        if (ctx.begin_menu("Help")) {
            if (ctx.menu_item("Controls...")) ctx.open_modal("Controls");
            if (ctx.menu_item("About toyengine editor")) ctx.open_modal("About");
            ctx.end_menu();
        }
        ctx.end_menubar();
    }

    // --- toolbar: tabs, play, shading ---

    void draw_toolbar_(imm::Context& ctx, const imm::Box& b) {
        static const std::vector<std::string> tabs = {"Scene", "Asset", "Render Settings", "Project Settings"};
        int t = static_cast<int>(tab_);
        if (ctx.tab_bar("main_tabs", b, tabs, &t)) set_tab(static_cast<Tab>(t));
        // Right side: play/stop + shading.
        float x = b.right() - 8;
        auto right_button = [&](const char* label, bool on, float w) {
            x -= w;
            const imm::Box r{x, b.y + 4, w, b.h - 8};
            x -= 4;
            bool hov = false, held = false;
            const bool clicked = ctx.invisible_button(label, r, &hov, &held);
            ctx.fill(r, on ? imm::with_alpha(ctx.style.accent, 0.85f) : held ? ctx.style.button_active : hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(r, imm::label_text(label), on ? glm::vec4(0.08f, 0.08f, 0.09f, 1) : ctx.style.text, 0, true);
            return clicked;
        };
        if (right_button("Full##sh", shading_ == Shading::Full, 46)) set_shading(Shading::Full);
        if (right_button("Solid##sh", shading_ == Shading::Solid, 50)) set_shading(Shading::Solid);
        if (right_button("Wire##sh", shading_ == Shading::Wireframe, 46)) set_shading(Shading::Wireframe);
        x -= 14;
        if (right_button(playing() ? "Stop##play" : "Play##play", playing(), 60)) playing() ? stop() : play();
    }

    void draw_status_(imm::Context& ctx, const imm::Box& b) {
        ctx.fill(b, ctx.style.panel_alt);
        ctx.fill({b.x, b.y, b.w, 1}, ctx.style.border);
        std::string left = project_.name() + "  |  " + (doc_.path().empty() ? std::string("unsaved scene") : project_.relative(doc_.path())) +
                           (doc_.dirty() ? " *" : "");
        if (playing()) left += "  |  PLAYING";
        ctx.text_in({b.x, b.y, b.w * 0.5f, b.h}, left, ctx.style.text_dim);
        const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - status_time_).count();
        if (!status_.empty() && age < 8.0) {
            const glm::vec4 c = status_level_ == 2 ? ctx.style.error : status_level_ == 1 ? ctx.style.warning : ctx.style.text_dim;
            const float w = ctx.text_width(status_) + 16;
            ctx.text_in({b.right() - w, b.y, w, b.h}, status_, c);
        }
    }

    // =================================================================================
    // UI: Scene tab
    // =================================================================================

    void draw_scene_tab_(imm::Context& ctx, const imm::Box& c) {
        if (maximized_) { draw_viewport_(ctx, c, in_edit_mode_()); return; }
        if (!show_left_ || !show_right_) { draw_scene_tab_partial_(ctx, c); return; }
        const float split = 4;
        left_w_ = std::clamp(left_w_, 150.0f, c.w * 0.4f);
        right_w_ = std::clamp(right_w_, 220.0f, c.w * 0.45f);
        bottom_h_ = std::clamp(bottom_h_, 90.0f, c.h * 0.6f);
        const imm::Box left{c.x, c.y, left_w_, c.h - bottom_h_ - split};
        const imm::Box right{c.right() - right_w_, c.y, right_w_, c.h};
        const imm::Box bottom{c.x, c.bottom() - bottom_h_, c.w - right_w_ - split, bottom_h_};
        const imm::Box center{left.right() + split, c.y, right.x - split - (left.right() + split), c.h - bottom_h_ - split};

        // Splitters drag their bar's position; the panel sizes follow from it.
        float lx = left_w_;
        if (ctx.splitter("split_left", {left.right(), c.y, split, left.h}, true, &lx, 150, c.w * 0.4f).changed) left_w_ = lx;
        float rx = right.x - split;
        if (ctx.splitter("split_right", {right.x - split, c.y, split, c.h}, true, &rx, c.right() - c.w * 0.45f, c.right() - 220).changed) {
            right_w_ = c.right() - rx - split;
        }
        float by = bottom.y - split;
        if (ctx.splitter("split_bottom", {c.x, bottom.y - split, c.w - right_w_ - split, split}, false, &by, c.bottom() - c.h * 0.6f,
                         c.bottom() - 90).changed) {
            bottom_h_ = c.bottom() - by - split;
        }

        draw_hierarchy_(ctx, left);
        draw_viewport_(ctx, center, in_edit_mode_());
        draw_inspector_(ctx, right);
        draw_asset_browser_(ctx, bottom);
    }

    /** @brief The Scene tab with the left (T) and/or right (N) panels hidden. */
    void draw_scene_tab_partial_(imm::Context& ctx, const imm::Box& c) {
        const float split = 4;
        const float lw = show_left_ ? std::clamp(left_w_, 150.0f, c.w * 0.4f) : 0.0f;
        const float rw = show_right_ ? std::clamp(right_w_, 220.0f, c.w * 0.45f) : 0.0f;
        const imm::Box left{c.x, c.y, lw, c.h};
        const imm::Box right{c.right() - rw, c.y, rw, c.h};
        const imm::Box center{c.x + lw + (lw > 0 ? split : 0), c.y, c.w - lw - rw - (lw > 0 ? split : 0) - (rw > 0 ? split : 0), c.h};
        if (show_left_) draw_hierarchy_(ctx, left);
        draw_viewport_(ctx, center, in_edit_mode_());
        if (show_right_) draw_inspector_(ctx, right);
    }

    void draw_hierarchy_(imm::Context& ctx, const imm::Box& b) {
        hierarchy_hovered_ = ctx.is_hovered(b);
        ctx.begin_panel("hierarchy", b, playing() ? "Outliner (playing: edits disabled)" : "Outliner");
        // Scene name row.
        std::string scene_name = doc_.scene_name();
        ctx.push_id("scene_name_row");
        if (!playing() && ctx.input_text("Scene", &scene_name)) apply_(doc_.set_scene_key("scene_name", Node(scene_name)));
        ctx.pop_id();
        ctx.separator();
        if (rename_id_ && !doc_.find(rename_id_)) rename_id_ = 0;
        draw_tree_(ctx, doc_.root_objects(), 0);
        // Drop on empty space: move to root.
        if (!playing()) {
            if (auto dropped = ctx.drop_target("object", imm::Box{b.x, ctx.cursor().y, b.w, std::max(20.0f, b.bottom() - ctx.cursor().y)})) {
                const ObjectId id = std::stoll(*dropped);
                apply_(doc_.reparent(id, 0));
            }
            if (ctx.open_context_popup_in("hier_ctx", b)) context_target_ = 0;
        }
        if (ctx.begin_popup("hier_ctx")) {
            draw_object_context_menu_(ctx, context_target_);
            ctx.end_popup();
        }
        ctx.end_panel();
    }

    void draw_tree_(imm::Context& ctx, const Node& list, int depth) {
        if (!list.is_sequence()) return;
        // Copy ids first: an action in this loop may restructure the document.
        std::vector<ObjectId> ids;
        for (const auto& o : list.as_seq()) ids.push_back(SceneDocument::id_of(o));
        for (ObjectId id : ids) {
            const Node* o = doc_.find(id);
            if (!o) continue;
            const std::string name = get_string(*o, "name", "Object");
            const bool has_children = o->contains("children") && o->at("children").is_sequence() && o->at("children").size() > 0;
            const bool active = get_bool(*o, "active", true);
            ctx.push_id(static_cast<int64_t>(id));
            if (rename_id_ == id) {
                imm::Box row = ctx.next_box(ctx.style.row_height);
                std::string n = name;
                if (rename_frames_++ == 0) ctx.begin_text_edit("rename", n);
                if (ctx.input_text_box("rename", {row.x + depth, row.y, row.w - depth, row.h}, &n) && !n.empty()) {
                    apply_(doc_.set_object_key(id, "name", Node(n), "Rename"));
                }
                if (!ctx.wants_keyboard() && rename_frames_ > 1) rename_id_ = 0;
                ctx.pop_id();
                continue;
            }
            const glm::vec4 dim = ctx.style.text_disabled;
            const bool inherited = o->contains("inherit_from");
            const std::string label = inherited ? name + "  (prefab)" : name;
            auto r = ctx.tree_node(ctx.get_id("node"), label, !has_children, doc_.is_selected(id), true, active ? nullptr : &dim);
            if (r.clicked) {
                const bool add = has(ctx.input().mods, Mods::Shift) || has(ctx.input().mods, imm::Context::command_mod());
                doc_.select(id, add);
            }
            if (r.double_clicked && !playing()) { rename_id_ = id; rename_frames_ = 0; }   // Blender: double-click renames
            if (r.right_clicked) { context_target_ = id; if (!doc_.is_selected(id)) doc_.select(id); ctx.open_popup("hier_ctx_obj"); }
            if (!playing()) {
                ctx.drag_source("object", std::to_string(id), name);
                if (auto dropped = ctx.drop_target("object", r.rect)) {
                    const ObjectId src = std::stoll(*dropped);
                    if (src != id) apply_(doc_.reparent(src, id));
                }
            }
            if (ctx.begin_popup("hier_ctx_obj")) {
                draw_object_context_menu_(ctx, context_target_);
                ctx.end_popup();
            }
            if (r.open) {
                draw_tree_(ctx, o->at("children"), depth + 1);
                ctx.tree_pop();
            }
            ctx.pop_id();
        }
    }

    void draw_object_context_menu_(imm::Context& ctx, ObjectId target) {
        const bool ok = !playing();
        if (ctx.begin_menu("Create Child", ok && target != 0)) {
            if (ctx.menu_item("Empty")) create_empty(target);
            for (const auto& p : primitive_names()) if (ctx.menu_item(p)) create_primitive(p, target);
            if (ctx.menu_item("Point Light")) create_with_component("PointLight", "Point Light", target);
            ctx.end_menu();
        }
        if (ctx.begin_menu("Create", ok)) {
            if (ctx.menu_item("Empty")) create_empty();
            for (const auto& p : primitive_names()) if (ctx.menu_item(p)) create_primitive(p);
            ctx.end_menu();
        }
        ctx.menu_separator();
        if (ctx.menu_item("Rename", "F2", nullptr, ok && target != 0)) { rename_id_ = target; rename_frames_ = 0; }
        if (ctx.menu_item("Duplicate", "Ctrl+D", nullptr, ok && target != 0)) duplicate_selected();
        if (ctx.menu_item("Delete", "Del", nullptr, ok && target != 0)) delete_selected();
        if (ctx.menu_item("Unparent", "", nullptr, ok && target != 0)) apply_(doc_.reparent(target, 0));
        ctx.menu_separator();
        if (ctx.menu_item("Frame", "Num.", nullptr, target != 0)) frame_selected();
    }

    void draw_inspector_(imm::Context& ctx, const imm::Box& b) {
        ctx.begin_panel("inspector", b, "Inspector");
        const ObjectId id = doc_.primary();
        Node* obj = id ? doc_.find(id) : nullptr;
        if (!obj) {
            ctx.label_dim("Nothing selected.");
            ctx.spacing();
            ctx.heading("Scene");
            Node scene = doc_.node().at("scene");
            bool auto_t = get_bool(scene, "auto_transform", true);
            if (!playing() && ctx.property_bool("Auto transform", &auto_t)) apply_(doc_.set_scene_key("auto_transform", Node(auto_t)));
            if (doc_.node().at("scene").contains("inherit_from")) {
                ctx.label_dim("Inherits: " + get_string(doc_.node().at("scene"), "inherit_from"));
            }
            ctx.end_panel();
            return;
        }
        const bool editable = !playing();
        // Header: active + name.
        bool active = get_bool(*obj, "active", true);
        if (editable && ctx.checkbox("##active", &active)) apply_(doc_.set_object_key(id, "active", Node(active), "Toggle Active"));
        ctx.same_line();
        std::string name = get_string(*obj, "name", "Object");
        imm::Box nb = ctx.next_box(ctx.style.row_height);
        if (editable && ctx.input_text_box("objname", nb, &name) && !name.empty()) apply_(doc_.set_object_key(id, "name", Node(name), "Rename"));
        if (obj->contains("inherit_from")) ctx.label_dim("Prefab: " + get_string(*obj, "inherit_from") + " (edits override)");
        if (doc_.selection().size() > 1) ctx.label_dim(std::to_string(doc_.selection().size()) + " objects selected (editing the last)");
        ctx.spacing(2);

        obj = doc_.find(id);
        if (!obj || !obj->contains("components")) { ctx.end_panel(); return; }
        const size_t count = obj->at("components").size();
        InspectorEnv env = inspector_env_();
        for (size_t i = 0; i < count; ++i) {
            obj = doc_.find(id);
            if (!obj || i >= obj->at("components").size()) break;
            Node comp = obj->at("components").as_seq()[i];
            const std::string type = component_type(comp);
            const ComponentSchema* schema = find_schema(type);
            ctx.push_id(static_cast<int64_t>(i));
            bool remove = false;
            const bool removable = editable && (!schema || schema->removable);
            const bool open = ctx.collapsing_header(type.empty() ? std::string("(untyped)") : type, true, removable ? &remove : nullptr);
            if (editable && ctx.open_context_popup_on_last("comp_ctx")) {}
            if (ctx.begin_popup("comp_ctx")) {
                if (ctx.menu_item("Move Up", "", nullptr, i > 0)) apply_(doc_.move_component(id, static_cast<int>(i), -1));
                if (ctx.menu_item("Move Down", "", nullptr, i + 1 < count)) apply_(doc_.move_component(id, static_cast<int>(i), +1));
                if (ctx.menu_item("Reset to Defaults", "", nullptr, schema != nullptr)) {
                    apply_(doc_.set_component(id, static_cast<int>(i), default_component(type), "Reset " + type));
                }
                if (ctx.menu_item("Copy as YAML")) {
                    Node c = comp;
                    strip_private_keys(c);
                    if (ctx.input().set_clipboard) ctx.input().set_clipboard(coopa::yaml::emit(c));
                }
                if (type == "MeshRenderer" && comp.contains("material") && comp.at("material").is_mapping() &&
                    ctx.menu_item("Extract Material to Asset")) {
                    extract_material_(id, static_cast<int>(i), comp);
                }
                if (ctx.menu_item("Remove", "", nullptr, removable)) remove = true;
                ctx.end_popup();
            }
            if (remove) {
                apply_(doc_.remove_component(id, static_cast<int>(i)));
                ctx.pop_id();
                break;
            }
            if (open) {
                ctx.indent(4);
                const Node before = comp;
                EditResult r = draw_component(ctx, comp, env, [this](const std::string& rel) {
                    open_material(project_.absolute(rel));
                });
                ctx.unindent(4);
                if (r.changed && editable && !(comp == before)) {
                    const std::string key = "c" + std::to_string(id) + ":" + std::to_string(i) + ":" + r.key;
                    apply_(doc_.set_component(id, static_cast<int>(i), comp, "Edit " + type + "." + r.key,
                                              r.active ? key : std::string()));
                }
                if (r.finished) doc_.end_merge();
            }
            ctx.pop_id();
            ctx.spacing(4);
        }
        if (editable) {
            ctx.spacing();
            std::vector<std::string> addable = {"Add Component..."};
            std::map<std::string, std::vector<std::string>> by_cat;
            for (const auto& [t, s] : schemas()) {
                if (s.unique && doc_.find_component(id, t) >= 0) continue;
                by_cat[s.category].push_back(t);
            }
            for (const auto& [cat, types] : by_cat) for (const auto& t : types) addable.push_back(cat + " / " + t);
            int pick = 0;
            if (ctx.combo("##addcomp", &pick, addable) && pick > 0) {
                const std::string entry = addable[static_cast<size_t>(pick)];
                const std::string type = entry.substr(entry.find(" / ") + 3);
                apply_(doc_.add_component(id, default_component(type)));
            }
        }
        ctx.end_panel();
    }

    void extract_material_(ObjectId id, int index, const Node& comp) {
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
        apply_(doc_.set_component(id, index, c, "Extract Material"));
        log_info("Extracted material to materials/" + n + ".yaml");
    }

    // --- asset browser ---

    void draw_asset_browser_(imm::Context& ctx, const imm::Box& b) {
        static const std::vector<std::string> cats = {"Scenes", "Meshes", "Materials", "Textures", "Physics", "Log"};
        ctx.fill(b, ctx.style.panel_bg);
        const imm::Box tabs{b.x, b.y, b.w, 26};
        ctx.tab_bar("asset_cats", tabs, cats, &asset_cat_);
        // Refresh + search on the right of the tab strip.
        const imm::Box search{b.right() - 200, b.y + 3, 150, 20};
        ctx.input_text_box("asset_search", search, &asset_filter_, "filter...");
        if (ctx.invisible_button("refresh", {b.right() - 44, b.y + 3, 40, 20})) project_.refresh();
        ctx.text_in(ctx.last_rect(), "Scan", ctx.last_hovered() ? ctx.style.text : ctx.style.text_dim, 0, true);
        const imm::Box list{b.x, tabs.bottom(), b.w, b.h - tabs.h};
        ctx.begin_region("asset_list", list, true);
        if (asset_cat_ == 5) {
            for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
                const glm::vec4 c = it->first == 2 ? ctx.style.error : it->first == 1 ? ctx.style.warning : ctx.style.text_dim;
                ctx.paragraph(it->second, &c);
            }
            ctx.end_region();
            return;
        }
        std::vector<std::string> items;
        std::string kind;
        switch (asset_cat_) {
            case 0: items = project_.scenes(); kind = "scene"; break;
            case 1: items = project_.list("meshes", ".yaml"); kind = "mesh";
                    if (!doc_.path().empty()) {
                        const auto env = inspector_env_();
                        for (const auto& s : env.list_assets("meshes", ".yaml"))
                            if (std::find(items.begin(), items.end(), s) == items.end()) items.push_back(s);
                    }
                    break;
            case 2: items = project_.list("materials", ".yaml"); kind = "material"; break;
            case 3: items = project_.list("textures", ".png"); kind = "texture"; break;
            case 4: items = project_.list("physics_materials", ".yaml"); kind = "physics"; break;
        }
        const float cell_w = 170;
        const int cols = std::max(1, static_cast<int>(ctx.available_width() / cell_w));
        int col = 0;
        for (const auto& item : items) {
            if (!asset_filter_.empty() && item.find(asset_filter_) == std::string::npos) continue;
            if (col > 0) ctx.same_line();
            ctx.push_id(item);
            const std::string label = fs::path(item).filename().string();
            const bool sel = selected_asset_ == item;
            if (ctx.selectable(label, sel, cell_w - 6)) {
                if (sel && ctx.time() - asset_click_time_ < 0.4) open_asset_(kind, item);
                selected_asset_ = item;
                asset_click_time_ = ctx.time();
            }
            ctx.tooltip(item);
            ctx.drag_source("asset", item, label);
            if (ctx.open_context_popup_on_last("asset_ctx")) selected_asset_ = item;
            if (ctx.begin_popup("asset_ctx")) {
                if (ctx.menu_item("Open")) open_asset_(kind, item);
                if (kind == "mesh" && ctx.menu_item("Add to Scene")) add_mesh_to_scene_(item);
                if (kind == "material" && ctx.menu_item("Assign to Selection")) assign_material_(item);
                if (ctx.menu_item("Copy Path")) { if (ctx.input().set_clipboard) ctx.input().set_clipboard(item); }
                ctx.end_popup();
            }
            ctx.pop_id();
            col = (col + 1) % cols;
        }
        if (items.empty()) ctx.label_dim("(empty)");
        ctx.end_region();
    }

    void open_asset_(const std::string& kind, const std::string& item) {
        // Scene-local paths (from a scene directory listing) resolve against that directory.
        fs::path abs = project_.absolute(item);
        if (!coopa::yaml::document_exists(abs) && !doc_.path().empty()) abs = doc_.path().parent_path() / item;
        if (kind == "scene") guarded_([this, abs] { open_scene(abs); });
        else if (kind == "mesh") open_mesh(abs);
        else if (kind == "material") open_material(abs);
        else log_info(item);
    }

    void add_mesh_to_scene_(const std::string& item, std::optional<glm::vec3> at = std::nullopt) {
        if (playing()) return;
        std::string key = fs::path(item).stem().string();
        Node obj = doc_.make_object(doc_.unique_name(key));
        set_object_position_(obj, at.value_or(spawn_point_()));
        Node mr = default_component("MeshRenderer");
        mr["mesh_path"] = Node(key);
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

    // =================================================================================
    // UI: viewport (shared by the Scene and Asset tabs)
    // =================================================================================

    // =================================================================================
    // Viewport: Blender-style interaction
    //
    //   Navigation  MMB orbit, Shift+MMB pan, Ctrl+MMB / wheel zoom; Alt+LMB emulates MMB;
    //               Shift+wheel / Ctrl+wheel pan, horizontal scroll orbits
    //   Views       numpad 1/3/7 (+Ctrl opposite), 5 ortho, 0 camera, 2/4/6/8 orbit,
    //               +/- zoom, numpad . frame selected, Home frame all, ` view menu
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

    enum class Tool { Select = 0, Move, Rotate, Scale };

    bool in_edit_mode_() const { return tab_ == Tab::Scene && edit_object_ != 0; }

    /** @brief The edited mesh's object-to-world matrix (identity in the Asset tab). */
    glm::mat4 mesh_world_() {
        if (tab_ == Tab::Scene && edit_object_) {
            if (auto* live = sync_.live(edit_object_); live && live->get_transform()) return live->get_transform()->transform().get_world_matrix();
        }
        return glm::mat4(1.0f);
    }

    void draw_viewport_(imm::Context& ctx, const imm::Box& area, bool mesh_edit) {
        // Header strip: mode, tools, orientation, overlays.
        const imm::Box bar{area.x, area.y, area.w, 26};
        ctx.fill(bar, ctx.style.panel_alt);
        float x = bar.x + 4;
        auto tool = [&](const char* label, bool on, float w) {
            const imm::Box r{x, bar.y + 3, w, bar.h - 6};
            x += w + 3;
            bool hov = false, held = false;
            const bool clicked = ctx.invisible_button(label, r, &hov, &held);
            ctx.fill(r, on ? imm::with_alpha(ctx.style.accent, 0.8f) : hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(r, imm::label_text(label), on ? glm::vec4(0.08f, 0.08f, 0.09f, 1) : ctx.style.text, 0, true);
            return clicked;
        };
        if (tab_ == Tab::Scene) {
            if (tool(in_edit_mode_() ? "Edit Mode##mode" : "Object Mode##mode", in_edit_mode_(), 96)) toggle_edit_mode_();
            ctx.tooltip("Tab");
            x += 6;
        }
        if (tool("Select##t", tool_ == Tool::Select, 52)) tool_ = Tool::Select;
        if (tool("Move##t", tool_ == Tool::Move, 46)) tool_ = Tool::Move;
        if (tool("Rotate##t", tool_ == Tool::Rotate, 54)) tool_ = Tool::Rotate;
        if (tool("Scale##t", tool_ == Tool::Scale, 46)) tool_ = Tool::Scale;
        x += 6;
        if (tool(gizmo_.local ? "Local##sp" : "Global##sp", gizmo_.local, 54)) gizmo_.local = !gizmo_.local;
        if (tool("Grid##gr", show_grid_, 40)) show_grid_ = !show_grid_;
        if (tool(camera_.ortho ? "Ortho##pr" : "Persp##pr", camera_.ortho, 50)) { camera_.ortho = !camera_.ortho; camera_.apply(); }
        if (mesh_edit) {
            x += 6;
            auto& sel = mesh_.selection;
            if (tool("Vert##m", sel.mode == SelectMode::Vertex, 40)) convert_selection(mesh_.mesh, sel, SelectMode::Vertex);
            if (tool("Edge##m", sel.mode == SelectMode::Edge, 40)) convert_selection(mesh_.mesh, sel, SelectMode::Edge);
            if (tool("Face##m", sel.mode == SelectMode::Face, 40)) convert_selection(mesh_.mesh, sel, SelectMode::Face);
        }
        const char* shading_names[] = {"Wireframe", "Solid", "Full Render"};
        ctx.text_in({bar.right() - 110, bar.y, 106, bar.h}, shading_names[static_cast<int>(shading_)], ctx.style.text_dim);

        viewport_box_ = imm::Box{area.x, bar.bottom(), area.w, std::max(1.0f, area.h - bar.h)};
        ctx.push_clip(viewport_box_);
        handle_viewport_input_(ctx, mesh_edit);
        draw_viewport_overlay_(ctx, mesh_edit);
        if (modal_.active()) {
            const std::string h = modal_.header();
            const imm::Box hb{viewport_box_.x + 8, viewport_box_.y + 8, ctx.text_width(h) + 16, 24};
            ctx.fill(hb, glm::vec4(0, 0, 0, 0.55f));
            ctx.text_in(hb, h, ctx.style.text);
        }
        ctx.pop_clip();
        draw_viewport_popups_(ctx, mesh_edit);
        // Asset drops into the viewport.
        if (!mesh_edit && tab_ == Tab::Scene) {
            if (auto dropped = ctx.drop_target("asset", viewport_box_)) {
                const std::string& item = *dropped;
                if (item.rfind("meshes/", 0) == 0 || item.find("/meshes/") != std::string::npos) {
                    add_mesh_to_scene_(item, ground_point_(ctx.mouse()));
                } else if (item.rfind("materials/", 0) == 0) {
                    const ObjectId hit = pick_object(ctx.mouse());
                    if (hit) assign_material_(item, hit);
                } else if (item.rfind("scenes/", 0) == 0) {
                    const fs::path p = project_.absolute(item);
                    guarded_([this, p] { open_scene(p); });
                }
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
        const bool hovered = ctx.is_hovered(viewport_box_);
        viewport_hovered_ = hovered;
        const auto vp = view_proj_();
        if (!vp) return;

        // --- a running modal operator owns all input ---
        if (modal_.active()) {
            run_modal_(ctx, *vp, ctrl, shift);
            return;
        }
        if (pending_modal_kind_ != ModalKind::None && hovered) {
            const ModalKind k = pending_modal_kind_;
            pending_modal_kind_ = ModalKind::None;
            start_modal_(k, *vp, m, mesh_edit, pending_modal_axis_);
            pending_modal_axis_.reset();
            return;
        }

        // --- navigation ---
        const bool nav_press = hovered && (in.pressed[2] || (in.pressed[0] && alt));
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
        if (hovered && (in.scroll.x != 0.0f || in.scroll.y != 0.0f) && !ctx.popup_hovered()) {
            // Blender's wheel: zoom; Shift+wheel pans vertically, Ctrl+wheel horizontally.
            // Deltas are used as-is, so macOS smooth scrolling (fractional notches) zooms
            // smoothly instead of being mistaken for a gesture. Horizontal scroll (tilt
            // wheels, trackpad sideways swipes) orbits around the view.
            if (in.scroll.y != 0.0f) {
                if (shift) camera_.pan(glm::vec2(0.0f, in.scroll.y * -20.0f), viewport_box_.h);
                else if (ctrl) camera_.pan(glm::vec2(in.scroll.y * -20.0f, 0.0f), viewport_box_.h);
                else camera_.dolly(in.scroll.y);
            }
            if (in.scroll.x != 0.0f && !shift && !ctrl) camera_.orbit(glm::vec2(in.scroll.x * -6.0f, 0.0f));
        }

        // --- gizmo (the Move / Rotate / Scale tools) ---
        bool gizmo_took_mouse = false;
        const bool can_edit = !(playing() && tab_ == Tab::Scene);
        if (tool_ != Tool::Select && show_gizmo_ && can_edit) {
            gizmo_.mode = tool_ == Tool::Move ? GizmoMode::Translate : tool_ == Tool::Rotate ? GizmoMode::Rotate : GizmoMode::Scale;
            glm::vec3 pivot;
            glm::mat3 basis(1.0f);
            if (gizmo_target_(mesh_edit, pivot, basis)) {
                const GizmoDelta g = gizmo_.update(*vp, pivot, basis, m, in.pressed[0] && !alt, in.down[0], in.released[0], ctrl,
                                                   hovered && !ctx.popup_hovered());
                gizmo_took_mouse = g.active || gizmo_.hot_axis() >= 0;
                if (g.started) begin_transform_(mesh_edit);
                if (g.active) {
                    const int kind = gizmo_.mode == GizmoMode::Translate ? 0 : gizmo_.mode == GizmoMode::Rotate ? 1 : 2;
                    apply_transform_(mesh_edit, kind, g.translate, g.rotate_axis, g.rotate_deg, g.scale);
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
        if (!gizmo_took_mouse && hovered && in.pressed[0] && !alt) {
            select_press_ = m;
            select_pending_ = true;
        }
        if (select_pending_ && in.down[0] && glm::distance(m, select_press_) > 5.0f) box_selecting_ = true;
        if (select_pending_ && in.released[0]) {
            if (box_selecting_) box_select_(mesh_edit, select_press_, m, shift, ctrl);
            else if (mesh_edit) pick_mesh_element_(m, shift);
            else {
                const ObjectId id = pick_object(m);
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
            ctx.fill(r, glm::vec4(1, 1, 1, 0.06f));
            ctx.outline(r, glm::vec4(1, 1, 1, 0.6f));
        }

        if (hovered && !ctx.wants_keyboard() && !ctx.any_popup_open()) viewport_keymap_(ctx, *vp, mesh_edit, can_edit);
    }

    /** @brief Keys that act while the mouse is over the viewport (Blender's 3D View keymap). */
    void viewport_keymap_(imm::Context& ctx, const ViewProj& vp, bool mesh_edit, bool can_edit) {
        const glm::vec2 m = ctx.mouse();
        // --- views (any mode) ---
        if (ctx.shortcut(Key::Kp1)) camera_.axis_view('f', false);
        if (ctx.shortcut(Key::Kp1, Mods::Control)) camera_.axis_view('f', true);
        if (ctx.shortcut(Key::Kp3)) camera_.axis_view('r', false);
        if (ctx.shortcut(Key::Kp3, Mods::Control)) camera_.axis_view('r', true);
        if (ctx.shortcut(Key::Kp7)) camera_.axis_view('t', false);
        if (ctx.shortcut(Key::Kp7, Mods::Control)) camera_.axis_view('t', true);
        if (ctx.shortcut(Key::Kp5)) { camera_.ortho = !camera_.ortho; camera_.apply(); }
        if (ctx.shortcut(Key::Kp0)) view_through_scene_camera_();
        if (ctx.shortcut(Key::Kp0, Mods::Control | Mods::Alt)) align_scene_camera_to_view_();
        if (ctx.shortcut(Key::Kp4)) camera_.orbit(glm::vec2(15.0f / 0.35f, 0));
        if (ctx.shortcut(Key::Kp6)) camera_.orbit(glm::vec2(-15.0f / 0.35f, 0));
        if (ctx.shortcut(Key::Kp8)) camera_.orbit(glm::vec2(0, -15.0f / 0.35f));
        if (ctx.shortcut(Key::Kp2)) camera_.orbit(glm::vec2(0, 15.0f / 0.35f));
        if (ctx.shortcut(Key::KpAdd) || ctx.shortcut(Key::Equal)) camera_.dolly(1.0f);
        if (ctx.shortcut(Key::KpSubtract) || ctx.shortcut(Key::Minus)) camera_.dolly(-1.0f);
        if (ctx.shortcut(Key::KpDecimal)) frame_selected();
        if (ctx.shortcut(Key::Home)) frame_all();
        if (ctx.shortcut(Key::GraveAccent)) ctx.open_popup("vp_view_menu", m);
        if (ctx.shortcut(Key::Z)) ctx.open_popup("vp_shading_menu", m);
        if (ctx.shortcut(Key::Z, Mods::Shift)) set_shading(shading_ == Shading::Wireframe ? Shading::Solid : Shading::Wireframe);
        if (ctx.shortcut(Key::N)) show_right_ = !show_right_;
        if (ctx.shortcut(Key::T)) show_left_ = !show_left_;
        if (ctx.shortcut(Key::Space, Mods::Control)) maximized_ = !maximized_;
        if (ctx.shortcut(Key::S, Mods::Shift)) ctx.open_popup("vp_snap_menu", m);
        if (ctx.shortcut(Key::C, Mods::Shift)) { cursor3d_ = glm::vec3(0.0f); frame_all(); }
        if (ctx.shortcut(Key::Space, Mods::Shift)) ctx.open_popup("vp_tool_menu", m);
        if (!can_edit) return;

        if (mesh_edit) {
            auto& md = mesh_;
            if (ctx.shortcut(Key::Num1)) convert_selection(md.mesh, md.selection, SelectMode::Vertex);
            if (ctx.shortcut(Key::Num2)) convert_selection(md.mesh, md.selection, SelectMode::Edge);
            if (ctx.shortcut(Key::Num3)) convert_selection(md.mesh, md.selection, SelectMode::Face);
            if (ctx.shortcut(Key::A)) md.selection.select_all(md.mesh);
            if (ctx.shortcut(Key::A, Mods::Alt)) md.selection.clear();
            if (ctx.shortcut(Key::I, Mods::Control)) invert_mesh_selection_();
            if (ctx.shortcut(Key::L, Mods::Control)) select_linked(md.mesh, md.selection);
            if (ctx.shortcut(Key::L)) { pick_mesh_element_(m, true); select_linked(md.mesh, md.selection); }
            if (ctx.shortcut(Key::G)) start_modal_(ModalKind::Grab, vp, m, true);
            if (ctx.shortcut(Key::R)) start_modal_(ModalKind::Rotate, vp, m, true);
            if (ctx.shortcut(Key::S)) start_modal_(ModalKind::Scale, vp, m, true);
            if (ctx.shortcut(Key::E)) extrude_interactive_(vp, m);
            if (ctx.shortcut(Key::I)) start_modal_(ModalKind::Inset, vp, m, true);
            if (ctx.shortcut(Key::B, Mods::Control)) start_modal_(ModalKind::Bevel, vp, m, true);
            if (ctx.shortcut(Key::F)) { bool ok = false; md.edit("Make Face", [&](EditMesh& mm, MeshSelection& s) { ok = fill_face(mm, s); }); if (!ok) log_warn("Make Face needs 3+ selected vertices"); }
            if (ctx.shortcut(Key::D, Mods::Shift)) {
                md.edit("Duplicate", [](EditMesh& mm, MeshSelection& s) { duplicate_faces(mm, s); });
                start_modal_(ModalKind::Grab, vp, m, true);
            }
            if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) ctx.open_popup("vp_mesh_delete", m);
            if (ctx.shortcut(Key::M)) ctx.open_popup("vp_mesh_merge", m);
            if (ctx.shortcut(Key::U)) ctx.open_popup("vp_mesh_uv", m);
            if (ctx.shortcut(Key::N, Mods::Alt)) ctx.open_popup("vp_mesh_normals", m);
            if (ctx.shortcut(Key::Tab) && tab_ == Tab::Scene) toggle_edit_mode_();
            return;
        }
        if (tab_ != Tab::Scene) return;
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
            if (ctx.menu_item("Frame Selected", "Num.")) frame_selected();
            if (ctx.menu_item("Frame All", "Home")) frame_all();
            if (ctx.menu_item(camera_.ortho ? "Perspective" : "Orthographic", "Num5")) { camera_.ortho = !camera_.ortho; camera_.apply(); }
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_shading_menu", 150)) {
            bool w = shading_ == Shading::Wireframe, s = shading_ == Shading::Solid, f = shading_ == Shading::Full;
            if (ctx.menu_item("Wireframe", "", &w)) set_shading(Shading::Wireframe);
            if (ctx.menu_item("Solid", "", &s)) set_shading(Shading::Solid);
            if (ctx.menu_item("Full Render", "", &f)) set_shading(Shading::Full);
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
            if (ctx.menu_item("Cursor to Grid")) cursor3d_ = glm::round(cursor3d_);
            if (ctx.menu_item("Selection to Cursor", "", nullptr, !mesh_edit && tab_ == Tab::Scene)) selection_to_cursor_();
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
        if (ctx.begin_popup("vp_obj_ctx", 200)) {
            const bool any = !doc_.selection().empty();
            if (ctx.menu_item("Edit Mode", "Tab", nullptr, any)) toggle_edit_mode_();
            ctx.menu_separator();
            if (ctx.menu_item("Duplicate", "Shift D", nullptr, any)) { duplicate_selected(); pending_modal_kind_ = ModalKind::Grab; }
            if (ctx.menu_item("Delete", "X", nullptr, any)) delete_selected();
            if (ctx.menu_item("Rename", "F2", nullptr, any)) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
            ctx.menu_separator();
            if (ctx.menu_item("Parent to Active", "Ctrl P", nullptr, doc_.selection().size() > 1)) parent_selection_to_active_();
            if (ctx.menu_item("Clear Parent (keep transform)", "Alt P", nullptr, any)) clear_parent_keep_transform_();
            ctx.menu_separator();
            if (ctx.menu_item("Hide", "H", nullptr, any)) hide_(doc_.selection());
            if (ctx.menu_item("Unhide All", "Alt H")) unhide_all_();
            if (ctx.menu_item("Frame Selected", "Num.", nullptr, any)) frame_selected();
            if (ctx.menu_item("Set 3D Cursor Here", "Shift RMB")) cursor3d_ = surface_point_(ctx_popup_origin_(m));
            ctx.end_popup();
        }
        auto mesh_op = [&](const char* label, auto&& fn) { mesh_.edit(label, fn); };
        if (ctx.begin_popup("vp_mesh_ctx", 200)) {
            if (ctx.menu_item("Extrude", "E") && vp) extrude_interactive_(*vp, m);
            if (ctx.menu_item("Inset", "I")) pending_modal_kind_ = ModalKind::Inset;
            if (ctx.menu_item("Bevel", "Ctrl B")) pending_modal_kind_ = ModalKind::Bevel;
            if (ctx.menu_item("Make Face", "F")) mesh_op("Make Face", [](EditMesh& mm, MeshSelection& s) { fill_face(mm, s); });
            if (ctx.menu_item("Merge at Center", "M")) mesh_op("Merge", [](EditMesh& mm, MeshSelection& s) { merge_at_center(mm, s); });
            if (ctx.menu_item("Flip Normals", "Alt N")) mesh_op("Flip Normals", [](EditMesh& mm, MeshSelection& s) { flip_normals(mm, s); });
            if (ctx.menu_item("Shade Smooth")) mesh_op("Shade Smooth", [](EditMesh& mm, MeshSelection& s) { set_smooth(mm, s, true); });
            if (ctx.menu_item("Shade Flat")) mesh_op("Shade Flat", [](EditMesh& mm, MeshSelection& s) { set_smooth(mm, s, false); });
            ctx.menu_separator();
            if (ctx.menu_item("Delete Faces", "X")) mesh_op("Delete", [](EditMesh& mm, MeshSelection& s) { delete_selection(mm, s); });
            ctx.end_popup();
        }
        if (ctx.begin_popup("vp_mesh_delete", 170)) {
            auto del = [&](SelectMode as) {
                mesh_op("Delete", [as](EditMesh& mm, MeshSelection& s) { convert_selection(mm, s, as); delete_selection(mm, s); });
            };
            if (ctx.menu_item("Vertices")) del(SelectMode::Vertex);
            if (ctx.menu_item("Edges")) del(SelectMode::Edge);
            if (ctx.menu_item("Faces")) del(SelectMode::Face);
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
        if (playing() && tab_ == Tab::Scene) return;
        glm::vec3 pivot;
        glm::mat3 basis(1.0f);
        const bool saved_local = gizmo_.local;
        gizmo_.local = true;   // the local basis feeds double-pressed axis constraints
        const bool ok = gizmo_target_(mesh, pivot, basis);
        gizmo_.local = saved_local;
        if (!ok) return;
        if (kind == ModalKind::Inset || kind == ModalKind::Bevel) {
            if (!mesh) return;
        }
        begin_transform_(mesh);
        modal_mesh_ = mesh;
        modal_base_sel_ = mesh_.selection;
        modal_.begin(kind, vp, pivot, basis, mouse, axis);
    }

    void run_modal_(imm::Context& ctx, const ViewProj& vp, bool ctrl, bool shift) {
        const auto& in = ctx.input();
        const ModalKind kind = modal_.kind();
        const auto outcome = modal_.update(vp, ctx.mouse(), in.keys, in.pressed[0], in.pressed[1], ctrl, shift);
        ctx.consume_keyboard();
        select_pending_ = box_selecting_ = false;
        if (outcome == ModalTransform::Outcome::Cancelled) {
            apply_modal_(kind, ModalTransform::Result{});
            if (modal_mesh_) mesh_.undo.end_merge(); else doc_.end_merge();
            return;
        }
        apply_modal_(kind, modal_.result());
        if (outcome == ModalTransform::Outcome::Confirmed) {
            if (modal_mesh_) mesh_.undo.end_merge(); else doc_.end_merge();
        }
        // Guide line along the constraint axis.
        if (auto axis = modal_.constraint_axis()) {
            const glm::vec3 p = modal_.pivot();
            auto a = vp.project(p - *axis * 1000.0f), b = vp.project(p + *axis * 1000.0f);
            auto c = vp.project(p);
            if (c) {
                if (!a) a = c;
                if (!b) b = c;
                const glm::vec4 col = std::abs(axis->x) > 0.9f ? ctx.style.axis_x : std::abs(axis->y) > 0.9f ? ctx.style.axis_y
                                    : std::abs(axis->z) > 0.9f ? ctx.style.axis_z : ctx.style.accent;
                ctx.line(*a, *b, col, 1.5f);
            }
        }
    }

    void apply_modal_(ModalKind kind, const ModalTransform::Result& r) {
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
        apply_transform_(modal_mesh_, k, r.translate, r.rotate_axis, r.rotate_deg, r.scale);
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
        apply_(doc_.set_transform(id, p, r, s, label));
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
            doc_.get_transform(id, p, r, s);
            if (what == 0) p = glm::vec3(0.0f);
            if (what == 1) r = glm::vec3(0.0f);
            if (what == 2) s = glm::vec3(1.0f);
            apply_(doc_.set_transform(id, p, r, s, what == 0 ? "Clear Location" : what == 1 ? "Clear Rotation" : "Clear Scale"));
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

    void toggle_edit_mode_() {
        if (tab_ != Tab::Scene || playing()) return;
        if (edit_object_) { edit_object_ = 0; tool_settings_restore_(); return; }
        const ObjectId id = doc_.primary();
        if (!id) return;
        const int ci = doc_.find_component(id, "MeshRenderer");
        if (ci < 0) { log_warn("Edit mode needs a mesh object"); return; }
        const std::string key = get_string(doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)], "mesh_path");
        if (key.empty()) return;
        const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
        const fs::path path = coopa::yaml::resolve_variant(engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
        if (!coopa::yaml::document_exists(path)) { log_error("Mesh file not found for '" + key + "'"); return; }
        if (mesh_.open() && mesh_.dirty() && mesh_.path != path) save_mesh();
        if (mesh_.path != path || !mesh_.open()) {
            try { mesh_.load(path); } catch (const std::exception& e) { log_error(e.what()); return; }
        }
        edit_object_ = id;
        scene_uploaded_revision_ = 0;
        log_info("Edit mode: " + project_.relative(path) + " (Tab to leave, Ctrl+S saves it)");
    }
    void tool_settings_restore_() {}

    /** @brief Shows the edited (unsaved) mesh on every live object that uses its file. */
    void push_mesh_to_scene_() {
        if (!mesh_.open() || mesh_.path.empty() || scene_uploaded_revision_ == mesh_.geometry_revision) return;
        if (!mesh_.dirty() && !edit_object_) return;
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
        const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
        for (const auto& [id, live] : sync_.live_objects()) {
            const int ci = doc_.find_component(id, "MeshRenderer");
            if (ci < 0 || !live) continue;
            const std::string key = get_string(doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)], "mesh_path");
            const fs::path p = coopa::yaml::resolve_variant(engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
            if (p != mesh_.path) continue;
            if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) mr->set_mesh(handle);
        }
        mesh_cache_.clear();
    }

    /** @brief The gizmo / modal pivot and axes for the current selection (world space). */
    bool gizmo_target_(bool mesh_edit, glm::vec3& pivot, glm::mat3& basis) {
        basis = glm::mat3(1.0f);
        if (mesh_edit) {
            if (mesh_.selection.affected_vertices(mesh_.mesh).empty()) return false;
            const glm::mat4 w = mesh_world_();
            pivot = glm::vec3(w * glm::vec4(selection_center(mesh_.mesh, mesh_.selection), 1.0f));
            if (gizmo_.local) basis = glm::mat3(glm::normalize(glm::vec3(w[0])), glm::normalize(glm::vec3(w[1])), glm::normalize(glm::vec3(w[2])));
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
            if (gizmo_.local && id == doc_.primary()) {
                basis = glm::mat3(glm::normalize(glm::vec3(w[0])), glm::normalize(glm::vec3(w[1])), glm::normalize(glm::vec3(w[2])));
            }
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

    /** @brief Captures the values a gizmo drag / modal operator applies its deltas to. */
    void begin_transform_(bool mesh_edit) {
        if (mesh_edit) { mesh_drag_base_ = mesh_.mesh; return; }
        drag_starts_.clear();
        for (ObjectId id : doc_.selection()) {
            DragStart s;
            doc_.get_transform(id, s.pos, s.rot, s.scl);
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
                          const glm::vec3& scale) {
        if (mesh_edit) {
            const glm::mat4 w = mesh_world_();
            const glm::vec3 pivot = glm::vec3(w * glm::vec4(selection_center(mesh_drag_base_, mesh_.selection), 1.0f));
            const glm::mat4 xf_world = delta_matrix(kind == 0 ? translate : glm::vec3(0.0f), rot_axis, kind == 1 ? rot_deg : 0.0f,
                                                    kind == 2 ? scale : glm::vec3(1.0f), pivot);
            const glm::mat4 local = glm::inverse(w) * xf_world * w;
            const EditMesh base = mesh_drag_base_;
            mesh_.edit(kind == 0 ? "Move" : kind == 1 ? "Rotate" : "Scale", [&](EditMesh& mm, MeshSelection& sel) {
                mm = base;
                transform_selection(mm, sel, local);
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
            apply_(doc_.set_transform(id, pos, rot, scl, kind == 0 ? "Move" : kind == 1 ? "Rotate" : "Scale", "transform"));
        }
    }

    void pick_mesh_element_(glm::vec2 px, bool additive) {
        auto vp = view_proj_();
        if (!vp) return;
        auto& mm = mesh_.mesh;
        auto& sel = mesh_.selection;
        const glm::mat4 w = mesh_world_();
        auto wp = [&](uint32_t v) { return glm::vec3(w * glm::vec4(mm.positions[v], 1.0f)); };
        if (!additive) { sel.verts.clear(); sel.edges.clear(); sel.faces.clear(); }
        auto toggle = [&](auto& set, const auto& v) { if (!set.insert(v).second && additive) set.erase(v); };
        if (sel.mode == SelectMode::Vertex) {
            int best = -1;
            float bd = 12.0f;
            for (uint32_t v = 0; v < mm.positions.size(); ++v) {
                auto p = vp->project(wp(v));
                if (p && glm::distance(*p, px) < bd) { bd = glm::distance(*p, px); best = static_cast<int>(v); }
            }
            if (best >= 0) toggle(sel.verts, static_cast<uint32_t>(best));
        } else if (sel.mode == SelectMode::Edge) {
            std::optional<Edge> best;
            float bd = 10.0f;
            for (const auto& e : mm.edges()) {
                auto a = vp->project(wp(e.first)), b = vp->project(wp(e.second));
                if (!a || !b) continue;
                const float d = point_segment_distance(px, *a, *b);
                if (d < bd) { bd = d; best = e; }
            }
            if (best) toggle(sel.edges, *best);
        } else {
            glm::vec3 o, d;
            vp->ray(px, o, d);
            int best = -1;
            float bt = 1e30f;
            for (uint32_t f = 0; f < mm.faces.size(); ++f) {
                const auto& c = mm.faces[f].corners;
                for (size_t k = 1; k + 1 < c.size(); ++k) {
                    auto t = ray_triangle(o, d, wp(c[0].v), wp(c[k].v), wp(c[k + 1].v));
                    if (t && *t < bt) { bt = *t; best = static_cast<int>(f); }
                }
            }
            if (best >= 0) toggle(sel.faces, static_cast<uint32_t>(best));
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
            if (sel.mode == SelectMode::Vertex) {
                for (uint32_t v = 0; v < mm.positions.size(); ++v) if (inside(wp(mm.positions[v]))) apply(sel.verts, v);
            } else if (sel.mode == SelectMode::Edge) {
                for (const auto& e : mm.edges()) if (inside(wp((mm.positions[e.first] + mm.positions[e.second]) * 0.5f))) apply(sel.edges, e);
            } else {
                for (uint32_t f = 0; f < mm.faces.size(); ++f) if (inside(wp(mm.face_center(f)))) apply(sel.faces, f);
            }
            return;
        }
        if (!additive && !subtract) doc_.clear_selection();
        for (const auto& [id, live] : sync_.live_objects()) {
            if (!live || !live->get_transform() || hidden_.count(id)) continue;
            if (!inside(glm::vec3(live->get_transform()->transform().get_world_matrix()[3]))) continue;
            if (subtract) { if (doc_.is_selected(id)) doc_.select(id, true); }
            else if (!doc_.is_selected(id)) doc_.select(id, true);
        }
    }

    void draw_viewport_overlay_(imm::Context& ctx, bool mesh_edit) {
        auto vp = view_proj_();
        if (!vp) return;
        ctx.push_clip(vp->rect);   // the scene image, not the letterbox bars around it
        draw_viewport_overlay_body_(ctx, mesh_edit, *vp);
        ctx.pop_clip();
    }

    void draw_viewport_overlay_body_(imm::Context& ctx, bool mesh_edit, const ViewProj& vp_ref) {
        const ViewProj* vp = &vp_ref;
        if (show_grid_ && !(playing() && !mesh_edit)) draw_grid_(ctx, *vp);
        const glm::vec4 accent = ctx.style.accent;
        const glm::vec4 active_col{1.0f, 0.78f, 0.35f, 1.0f};
        auto draw_mesh_edges = [&](const EditMesh& mm, const std::vector<Edge>& edges, const glm::mat4& world, glm::vec4 col, float t) {
            size_t drawn = 0;
            for (const auto& e : edges) {
                if (++drawn > 30000) break;
                auto a = vp->project(glm::vec3(world * glm::vec4(mm.positions[e.first], 1.0f)));
                auto b = vp->project(glm::vec3(world * glm::vec4(mm.positions[e.second], 1.0f)));
                if (a && b) ctx.line(*a, *b, col, t);
            }
        };
        if (!mesh_edit) {
            // Wireframe mode: every mesh object's edges.
            if (shading_ == Shading::Wireframe) {
                for (const auto& [id, live] : sync_.live_objects()) {
                    const Node* node = doc_.find(id);
                    if (!node || !live || !live->active() || !live->get_transform() || doc_.is_selected(id)) continue;
                    if (const CachedMesh* cm = mesh_for_object_(*node)) {
                        draw_mesh_edges(cm->mesh, cm->edges, live->get_transform()->transform().get_world_matrix(),
                                        glm::vec4(0.75f, 0.78f, 0.82f, 0.85f), 1.0f);
                    }
                }
            }
            // Non-mesh objects: an origin marker.
            for (const auto& [id, live] : sync_.live_objects()) {
                const Node* node = doc_.find(id);
                if (!node || !live || !live->active() || !live->get_transform() || mesh_for_object_(*node)) continue;
                auto p = vp->project(glm::vec3(live->get_transform()->transform().get_world_matrix()[3]));
                if (!p) continue;
                glm::vec4 col(0.8f, 0.8f, 0.85f, 0.9f);
                std::string tag;
                for (const auto& c : node->at("components").as_seq()) {
                    const std::string t = component_type(c);
                    if (t.find("Light") != std::string::npos) { col = glm::vec4(1.0f, 0.85f, 0.3f, 1.0f); tag = "L"; }
                    else if (t == "Camera") { col = glm::vec4(0.5f, 0.75f, 1.0f, 1.0f); tag = "C"; }
                }
                if (doc_.is_selected(id)) col = doc_.primary() == id ? active_col : accent;
                ctx.triangle({p->x, p->y - 7}, {p->x + 7, p->y}, {p->x, p->y + 7}, col);
                ctx.triangle({p->x, p->y - 7}, {p->x - 7, p->y}, {p->x, p->y + 7}, col);
                if (!tag.empty()) ctx.draw_text({p->x + 9, p->y - 8}, tag, col);
            }
            // Selection outlines (the active object brighter, as in Blender).
            for (ObjectId id : doc_.selection()) {
                auto* live = sync_.live(id);
                const Node* node = doc_.find(id);
                if (!live || !node || !live->get_transform() || !live->active()) continue;
                if (const CachedMesh* cm = mesh_for_object_(*node)) {
                    draw_mesh_edges(cm->mesh, cm->edges, live->get_transform()->transform().get_world_matrix(),
                                    doc_.primary() == id ? active_col : accent, 1.5f);
                }
            }
        } else if (mesh_.open()) {
            const EditMesh& mm = mesh_.mesh;
            const auto& sel = mesh_.selection;
            const glm::mat4 w = mesh_world_();
            auto wp = [&](const glm::vec3& p) { return glm::vec3(w * glm::vec4(p, 1.0f)); };
            draw_mesh_edges(mm, mm.edges(), w, glm::vec4(0.04f, 0.04f, 0.05f, 0.9f), 1.0f);
            if (sel.mode == SelectMode::Face) {
                for (uint32_t f : sel.faces) {
                    if (f >= mm.faces.size()) continue;
                    const auto& c = mm.faces[f].corners;
                    for (size_t i = 0; i < c.size(); ++i) {
                        auto a = vp->project(wp(mm.positions[c[i].v])), b = vp->project(wp(mm.positions[c[(i + 1) % c.size()].v]));
                        if (a && b) ctx.line(*a, *b, accent, 2.0f);
                    }
                }
                for (uint32_t f = 0; f < mm.faces.size(); ++f) {
                    if (auto p = vp->project(wp(mm.face_center(f)))) {
                        ctx.fill({p->x - 2, p->y - 2, 4, 4}, sel.faces.count(f) ? accent : glm::vec4(0.1f, 0.1f, 0.12f, 0.9f));
                    }
                }
            } else if (sel.mode == SelectMode::Edge) {
                for (const auto& e : sel.edges) {
                    auto a = vp->project(wp(mm.positions[e.first])), b = vp->project(wp(mm.positions[e.second]));
                    if (a && b) ctx.line(*a, *b, accent, 2.5f);
                }
            } else {
                for (uint32_t v = 0; v < mm.positions.size(); ++v) {
                    auto p = vp->project(wp(mm.positions[v]));
                    if (!p) continue;
                    ctx.fill({p->x - 3, p->y - 3, 6, 6}, sel.verts.count(v) ? accent : glm::vec4(0.06f, 0.06f, 0.07f, 1.0f));
                }
            }
        }
        // 3D cursor (scene).
        if (tab_ == Tab::Scene) {
            if (auto c = vp->project(cursor3d_)) {
                for (int i = 0; i < 8; ++i) {
                    const float a0 = 6.2831853f * i / 8.0f, a1 = 6.2831853f * (i + 1) / 8.0f;
                    ctx.line(*c + glm::vec2(std::cos(a0), std::sin(a0)) * 9.0f, *c + glm::vec2(std::cos(a1), std::sin(a1)) * 9.0f,
                             i % 2 ? glm::vec4(1, 1, 1, 0.9f) : glm::vec4(0.9f, 0.2f, 0.2f, 0.9f), 1.5f);
                }
                ctx.line(*c - glm::vec2(14, 0), *c - glm::vec2(5, 0), glm::vec4(0, 0, 0, 0.8f), 1.0f);
                ctx.line(*c + glm::vec2(5, 0), *c + glm::vec2(14, 0), glm::vec4(0, 0, 0, 0.8f), 1.0f);
                ctx.line(*c - glm::vec2(0, 14), *c - glm::vec2(0, 5), glm::vec4(0, 0, 0, 0.8f), 1.0f);
                ctx.line(*c + glm::vec2(0, 5), *c + glm::vec2(0, 14), glm::vec4(0, 0, 0, 0.8f), 1.0f);
            }
        }
        // Gizmo (transform tools only, hidden while a modal operator runs).
        if (tool_ != Tool::Select && show_gizmo_ && !modal_.active() && !(playing() && !mesh_edit)) {
            glm::vec3 pivot;
            glm::mat3 basis;
            if (gizmo_target_(mesh_edit, pivot, basis)) gizmo_.draw(ctx, *vp, pivot, basis);
        }
        // Orientation hint (bottom-left).
        const glm::vec2 o{vp->rect.x + 34, vp->rect.bottom() - 34};
        const glm::mat3 vr(vp->view);
        const glm::vec4 cols[3] = {ctx.style.axis_x, ctx.style.axis_y, ctx.style.axis_z};
        const char* names[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; ++i) {
            glm::vec3 a(0.0f);
            a[i] = 1.0f;
            const glm::vec3 v = vr * a;
            const glm::vec2 tip = o + glm::vec2(v.x, -v.y) * 22.0f;
            ctx.line(o, tip, cols[i], 2.0f);
            ctx.draw_text(tip + glm::vec2(-3, -8), names[i], cols[i]);
        }
        // Mode banner.
        if (mesh_edit && tab_ == Tab::Scene) {
            const std::string t = "Edit Mode  -  " + mesh_.name + (mesh_.dirty() ? " *" : "");
            ctx.text_in({vp->rect.x + 8, vp->rect.bottom() - 26, 400, 20}, t, ctx.style.text_dim);
        }
    }

    // =================================================================================
    // UI: Asset tab
    // =================================================================================

    void draw_asset_tab_(imm::Context& ctx, const imm::Box& c) {
        const float split = 4;
        asset_left_w_ = std::clamp(asset_left_w_, 160.0f, c.w * 0.35f);
        const float right_w = std::clamp(right_w_, 240.0f, c.w * 0.45f);
        const imm::Box left{c.x, c.y, asset_left_w_, c.h};
        const imm::Box right{c.right() - right_w, c.y, right_w, c.h};
        const imm::Box center{left.right() + split, c.y, right.x - split - left.right() - split, c.h};
        float lw = asset_left_w_;
        if (ctx.splitter("asset_split", {left.right(), c.y, split, c.h}, true, &lw, 160, c.w * 0.35f).changed) asset_left_w_ = lw;

        // Left: asset lists.
        ctx.begin_panel("asset_lists", left, "Assets");
        if (ctx.collapsing_header("Meshes")) {
            if (ctx.button("New...##mesh", -1)) ctx.open_popup("new_mesh_popup");
            if (ctx.begin_popup("new_mesh_popup")) {
                for (const auto& p : primitive_names()) if (ctx.menu_item(p)) new_mesh(p);
                ctx.end_popup();
            }
            for (const auto& m : project_.list("meshes", ".yaml")) {
                ctx.push_id(m);
                const bool sel = asset_kind_ == AssetKind::Mesh && project_.relative(mesh_.path) == m;
                if (ctx.selectable(fs::path(m).stem().string(), sel)) open_mesh(project_.absolute(m));
                ctx.drag_source("asset", m, fs::path(m).filename().string());
                ctx.pop_id();
            }
        }
        if (ctx.collapsing_header("Materials")) {
            if (ctx.button("New##mat", -1)) create_material("material");
            for (const auto& m : project_.list("materials", ".yaml")) {
                ctx.push_id(m);
                const bool sel = asset_kind_ == AssetKind::Material && project_.relative(material_.path) == m;
                if (ctx.selectable(fs::path(m).stem().string(), sel)) open_material(project_.absolute(m));
                ctx.drag_source("asset", m, fs::path(m).filename().string());
                ctx.pop_id();
            }
        }
        ctx.end_panel();

        // Center: viewport over the preview scene.
        if (asset_kind_ == AssetKind::None) {
            viewport_box_ = imm::Box{center.x, center.y, center.w, center.h};
            ctx.fill(center, ctx.style.window_bg);
            ctx.text_in(center, "Open or create a mesh or material on the left.", ctx.style.text_dim, 0, true);
        } else {
            draw_viewport_(ctx, center, asset_kind_ == AssetKind::Mesh && mesh_edit_);
        }

        // Right: tools / properties.
        ctx.begin_panel("asset_props", right, asset_kind_ == AssetKind::Mesh ? "Mesh" : asset_kind_ == AssetKind::Material ? "Material" : "Properties");
        if (asset_kind_ == AssetKind::Mesh) draw_mesh_tools_(ctx);
        else if (asset_kind_ == AssetKind::Material) draw_material_editor_(ctx);
        ctx.end_panel();
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
        if (ctx.button("Add to Scene", 120)) {
            if (md.path.empty() || md.dirty()) save_mesh();
            if (!md.path.empty()) { set_tab(Tab::Scene); add_mesh_to_scene_(project_.relative(md.path)); }
        }
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
        if (ctx.button("Assign to Selection", 150)) { set_tab(Tab::Scene); assign_material_(md.ref + ".yaml"); }
        ctx.spacing();
        Node before = md.node;
        InspectorEnv env = inspector_env_();
        EditResult r = draw_fields(ctx, material_fields(), md.node, env, true, {"base"});
        if (r.changed) {
            md.commit("Edit " + r.key, before, r.active ? "m:" + r.key : std::string());
            refresh_material_preview_();
        }
        if (r.finished) md.undo.end_merge();
    }

    // =================================================================================
    // UI: Render settings tab
    // =================================================================================

    void draw_render_tab_(imm::Context& ctx, const imm::Box& c) {
        const float split = 4;
        settings_w_ = std::clamp(settings_w_, 300.0f, c.w * 0.7f);
        const imm::Box left{c.x, c.y, settings_w_, c.h};
        const imm::Box right{left.right() + split, c.y, c.w - settings_w_ - split, c.h};
        float sw = settings_w_;
        if (ctx.splitter("render_split", {left.right(), c.y, split, c.h}, true, &sw, 300, c.w * 0.7f).changed) settings_w_ = sw;

        ctx.begin_panel("render_settings", left, "Render Settings (config.yaml)");
        if (ctx.button(config_.dirty() ? "Save config.yaml *" : "Save config.yaml", 160)) save_config();
        ctx.same_line();
        if (ctx.button("Restart Renderer", 150)) restart_ = true;
        ctx.tooltip("Recreates the renderer with these settings (applies startup-only fields marked *).");
        ctx.label_dim("* = applies after a renderer restart.  Reset hands a key back to its quality preset.");
        ctx.spacing();
        Node& render = config_.section("render");
        InspectorEnv env = inspector_env_();
        for (const auto& g : render_settings_groups()) {
            if (!ctx.collapsing_header(g.title, g.title == "Features" || g.title == "Viewport & Resolution")) continue;
            for (const auto& f0 : g.fields) {
                FieldDesc f = f0;
                if (f.startup_only) f.label = f.display() + " *";
                draw_setting_row_(ctx, f, render, env, "render");
            }
        }
        if (ctx.collapsing_header("Other Keys", false)) {
            const auto known = render_settings_keys();
            const std::set<std::string> skip(known.begin(), known.end());
            Node before = config_.node;
            EditResult r = draw_fields(ctx, {}, render, env, true, skip);
            if (r.changed) { config_.commit("Edit render." + r.key, before, r.active ? "cfg:" + r.key : std::string()); apply_config_live(); }
            if (r.finished) config_.undo.end_merge();
        }
        ctx.end_panel();

        draw_viewport_(ctx, right, false);
    }

    /** @brief One config field with a reset button when the key is explicitly set. */
    void draw_setting_row_(imm::Context& ctx, const FieldDesc& f, Node& section, const InspectorEnv& env, const std::string& section_name) {
        ctx.push_id(f.key);
        const bool present = section.contains(f.key);
        const Node before = config_.node;
        EditResult r = draw_field(ctx, f, section, env);
        const imm::Box row = ctx.last_rect();
        if (present) {
            // Reset sits over the right end of the label column.
            const imm::Box label_end{row.x - 22, row.y + 3, 18, ctx.style.row_height - 6};
            if (ctx.invisible_button("reset", label_end)) {
                erase_key(section, f.key);
                r.changed = true;
                r.finished = true;
            }
            ctx.text_in(label_end, "x", ctx.last_hovered() ? ctx.style.error : ctx.style.text_disabled, 0, true);
            ctx.tooltip("Reset (remove the key: the quality preset / default applies)");
        }
        if (r.changed) {
            config_.commit("Edit " + section_name + "." + f.key, before, r.active ? "cfg:" + f.key : std::string());
            if (section_name == "render") apply_config_live();
            if (f.startup_only) log_info(f.key + " changes on renderer restart");
        }
        if (r.finished) config_.undo.end_merge();
        ctx.pop_id();
    }

    // =================================================================================
    // UI: Project settings tab
    // =================================================================================

    void draw_project_tab_(imm::Context& ctx, const imm::Box& c) {
        viewport_box_ = c;   // the scene keeps rendering underneath, fully covered
        ctx.begin_panel("project_settings", c, "Project Settings  -  " + project_.root().string());
        if (ctx.button(config_.dirty() ? "Save config.yaml *" : "Save config.yaml", 160)) save_config();
        ctx.same_line();
        if (ctx.button("Package Project...", 150)) open_package_dialog_();
        ctx.spacing();
        // Default scene.
        if (ctx.collapsing_header("Startup")) {
            Node& sc = config_.section("scene");
            std::vector<std::string> scenes;
            for (const auto& s : project_.scenes()) scenes.push_back("assets/" + s);
            const std::string cur = get_string(sc, "default_scene");
            int idx = -1;
            for (size_t i = 0; i < scenes.size(); ++i) if (scenes[i] == cur) idx = static_cast<int>(i);
            if (idx < 0 && !cur.empty()) { scenes.push_back(cur); idx = static_cast<int>(scenes.size()) - 1; }
            const Node before = config_.node;
            if (ctx.combo("Default scene", &idx, scenes) && idx >= 0) {
                sc["default_scene"] = Node(scenes[static_cast<size_t>(idx)]);
                config_.commit("Default Scene", before, {});
            }
            if (!doc_.path().empty() && ctx.button("Use Current Scene", 160)) {
                sc["default_scene"] = Node("assets/" + project_.relative(doc_.path()));
                config_.commit("Default Scene", before, {});
            }
        }
        InspectorEnv env = inspector_env_();
        for (const auto& g : project_settings_groups()) {
            if (!ctx.collapsing_header(g.title)) continue;
            Node& section = config_.section(project_section_key(g.title));
            for (const auto& f : g.fields) draw_setting_row_(ctx, f, section, env, project_section_key(g.title));
            const std::set<std::string> skip = [&] { std::set<std::string> s; for (const auto& f : g.fields) s.insert(f.key); return s; }();
            const Node before = config_.node;
            EditResult r = draw_fields(ctx, {}, section, env, true, skip);
            if (r.changed) config_.commit("Edit " + g.title, before, r.active ? "cfg:" + g.title + r.key : std::string());
            if (r.finished) config_.undo.end_merge();
        }
        ctx.spacing();
        ctx.label_dim("Window, physics and jobs settings take effect the next time the game (or editor) starts.");
        ctx.end_panel();
    }

    // =================================================================================
    // Modals and dialogs
    // =================================================================================

    void draw_modals_(imm::Context& ctx) {
        file_dialog_.draw(ctx);

        if (ctx.begin_modal("Unsaved Changes", {440, 170})) {
            ctx.paragraph("There are unsaved changes. Save them first?");
            ctx.spacing();
            if (ctx.button("Save All", 120)) {
                save_all_();
                ctx.close_modal();
                if (auto fn = std::move(pending_after_confirm_)) { pending_after_confirm_ = {}; fn(); }
            }
            ctx.same_line();
            if (ctx.button("Discard", 120)) {
                ctx.close_modal();
                discard_all_changes_();
                if (auto fn = std::move(pending_after_confirm_)) { pending_after_confirm_ = {}; fn(); }
            }
            ctx.same_line();
            if (ctx.button("Cancel", 100)) { ctx.close_modal(); pending_after_confirm_ = {}; }
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

        if (ctx.begin_modal("Controls", {720, 420})) {
            static const char* lines[] = {
                "Navigate:   MMB orbit, Shift+MMB pan, Ctrl+MMB / wheel zoom; Alt+LMB = MMB (no middle button)",
                "            Shift+wheel / Ctrl+wheel pan vertically / horizontally, sideways scroll orbits",
                "Views:      numpad 1/3/7 front/right/top (Ctrl opposite), 5 ortho, 0 camera, 2/4/6/8 orbit,",
                "            numpad . frame selected, Home frame all, ` view menu, Ctrl+Alt+Num0 camera to view",
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
        if (ctx.begin_modal("About", {420, 170})) {
            ctx.heading("toyengine editor");
            ctx.paragraph("Scenes, meshes, materials and render settings for toyengine, written as the same YAML / "
                          ".caml files the game loads. UI built with uicoopa's immediate-mode layer.");
            if (ctx.button("Close", 100)) ctx.close_modal();
            ctx.end_modal();
        }
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
        config_.saved_revision = config_.undo.revision();
        force_quit_ = true;
    }

    void save_active_() {
        switch (tab_) {
            case Tab::Scene: save_scene(); break;
            case Tab::Asset:
                if (asset_kind_ == AssetKind::Mesh) save_mesh();
                else if (asset_kind_ == AssetKind::Material) save_material();
                break;
            default: save_config(); break;
        }
    }

    void save_all_() {
        if (doc_.dirty()) save_scene();
        if (mesh_.open() && mesh_.dirty()) save_mesh();
        if (material_.open() && material_.dirty()) save_material();
        if (config_.dirty()) save_config();
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
        file_dialog_.open(ui(), FileDialog::Mode::PickFolder, "Open Project (folder containing assets/)", project_.root().parent_path(), {},
                          [this](const fs::path& p) {
                              Project candidate(p);
                              if (!candidate.valid()) { log_error(p.string() + " has no assets/ folder"); return; }
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
        switch (tab_) {
            case Tab::Scene: return pick(doc_.undo_stack().undo_label(), doc_.undo_stack().redo_label());
            case Tab::Asset:
                if (asset_kind_ == AssetKind::Mesh) return pick(mesh_.undo.undo_label(), mesh_.undo.redo_label());
                if (asset_kind_ == AssetKind::Material) return pick(material_.undo.undo_label(), material_.undo.redo_label());
                return {};
            default: return pick(config_.undo.undo_label(), config_.undo.redo_label());
        }
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
        if (ctx.shortcut(Key::Q, cmd)) { if (request_close()) quit_ = true; }
        if (ctx.shortcut(Key::F5)) playing() ? stop() : play();
        if (ctx.shortcut(Key::Escape) && playing()) stop();
        if (ctx.any_popup_open()) return;
        // Outliner keymap (mouse over the outliner).
        if (hierarchy_hovered_ && tab_ == Tab::Scene && !playing()) {
            if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) delete_selected();
            if (ctx.shortcut(Key::F2) && doc_.primary()) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
            if (ctx.shortcut(Key::A)) { doc_.clear_selection(); for (ObjectId id : visible_ids_()) doc_.select(id, true); }
            if (ctx.shortcut(Key::A, Mods::Alt)) doc_.clear_selection();
            if (ctx.shortcut(Key::H)) hide_(doc_.selection());
            if (ctx.shortcut(Key::H, Mods::Alt)) unhide_all_();
            if (ctx.shortcut(Key::KpDecimal)) frame_selected();
        }
        if (ctx.shortcut(Key::F2) && tab_ == Tab::Scene && doc_.primary() && !playing()) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
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
    ConfigDocument config_;

    Tab tab_ = Tab::Scene;
    Shading shading_ = Shading::Solid;
    std::string full_debug_view_;
    std::string config_base_debug_view_;
    AssetKind asset_kind_ = AssetKind::None;
    bool mesh_edit_ = true;

    coopa::scene::Scene* play_scene_ = nullptr;
    coopa::scene::Scene* preview_scene_ = nullptr;
    coopa::scene::SceneObject* preview_object_ = nullptr;
    uint64_t uploaded_revision_ = 0;

    std::vector<std::function<void()>> deferred_;
    bool rebuild_queued_ = false;

    // Layout.
    float left_w_ = 250, right_w_ = 330, bottom_h_ = 180, asset_left_w_ = 220, settings_w_ = 460;
    imm::Box viewport_box_;
    bool show_grid_ = true;
    bool show_gizmo_ = true;

    // Viewport interaction (Blender-style; see the viewport section).
    Tool tool_ = Tool::Select;
    ModalTransform modal_;
    bool modal_mesh_ = false;
    MeshSelection modal_base_sel_;
    ModalKind pending_modal_kind_ = ModalKind::None;
    std::optional<glm::vec3> pending_modal_axis_;
    int nav_mode_ = 0;   // 0 orbit, 1 pan, 2 zoom
    ObjectId edit_object_ = 0;
    uint64_t scene_uploaded_revision_ = 0;
    glm::vec3 cursor3d_{0.0f};
    std::set<ObjectId> hidden_;
    bool show_left_ = true, show_right_ = true, maximized_ = false;
    bool viewport_hovered_ = false, hierarchy_hovered_ = false;
    struct CameraPose { glm::vec3 focus{0.0f}; float yaw = 35, pitch = 25, distance = 12; bool ortho = false; bool valid = false; };
    CameraPose scene_pose_, asset_pose_;
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
