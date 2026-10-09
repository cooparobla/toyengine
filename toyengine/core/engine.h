/**
 * @file engine.h
 * @brief Owns the engine lifetime: window, Vulkan objects, assets, scene, and the render loop.
 *
 * Constructs a shared coopa::job::JobEngine first, so it outlives every subsystem that
 * submits to it, then a gfx::app::Context (which owns the Window -> Instance -> Surface ->
 * Device -> Allocator -> Swapchain -> CommandPool -> RenderPass -> Renderer bring-up chain,
 * plus frame timing and resize handling), then layers PixelRenderPipeline, AssetManager and
 * SceneManager on top -- all three handed the JobEngine so asset decode, transform resolution
 * and the render-list gathers can dispatch to it.
 */

#ifndef TOYENGINE_CORE_ENGINE_H
#define TOYENGINE_CORE_ENGINE_H

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/util/image_readback.h>
#include <gfxcoopa/engine/loaders/mesh_loader.h>
#include <gfxcoopa/engine/loaders/skinned_mesh_source_loader.h>
#include <gfxcoopa/engine/loaders/texture_loader.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/components/camera_component.h>

#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/ik_system.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/input/input_map.h>
#include <coopa/job/engine.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_manager.h>
#include <coopa/scene/systems/transform_system.h>

#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/system/nav_system.h>
#include <physxcoopa/debug/debug_draw.h>

#include <toyengine/core/branding.h>
#include <toyengine/core/config.h>
#include <toyengine/core/runtime_paths.h>
#include <toyengine/core/scene_loading.h>
#include <toyengine/core/user_settings.h>
#include <toyengine/audio/audio_components.h>
#include <toyengine/audio/audio_system.h>
#include <sfxcoopa/sfx_yaml.h>
#include <uicoopa/audio/audio_yaml.h>
#include <uicoopa/audio/sound_library.h>
#include <toyengine/render/pixel_render_config.h>
#include <toyengine/render/pixel_render_pipeline.h>
#include <uicoopa/layout/canvas.h>
#include <uicoopa/ui_yaml.h>

#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/scene_link.h>
#include <toyengine/world/snow_system.h>
#include <toyengine/world/terrain_system.h>
#include <toyengine/water/water_system.h>
#include <toyengine/particles/particle_system_runner.h>
#include <toyengine/particles/particle_yaml.h>
#include <toyengine/weather/weather_reactor.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/weather/atmosphere_model.h>
#include <toyengine/render/visibility.h>
#include <toyengine/core/module.h>
#include <toyengine/debug/debug_overlay.h>
#include <toyengine/save/register.h>
#include <toyengine/save/save_system.h>

#include <root_directory.h>

namespace toy {
namespace core {

/**
 * @struct EngineOptions
 * @brief How an Engine is embedded: by the game (the defaults) or by a tool like the editor.
 */
struct EngineOptions {
    /// Directory holding the project's assets/ folder. Relative scene/palette/LUT paths in
    /// the config resolve against it, and <project_root>/assets is the first asset search
    /// root (the engine checkout's assets/ is the fallback under it). Empty means the
    /// TOY_PROJECT_DIR env var, else the project this binary was built for (TOY_PROJECT_ROOT,
    /// which is ROOT_DIR when building toyengine itself).
    std::filesystem::path project_root;
    /// Load config.scene.default_scene (or SCENE) during construction. A tool that picks its
    /// scene later turns this off and calls load_scene()/set_scene() itself.
    bool load_default_scene = true;
    /// Give the window / taskbar / Dock the toyengine logo (branding.h) at startup.
    bool set_app_icon = true;
        /// Start in edit mode: loaded scenes do not simulate (see set_edit_mode()).
    bool edit_mode = false;
    /// Escape closes the window (the game's default). An editor turns this off: Escape there
    /// cancels things.
    bool escape_quits = true;
};

/**
 * @struct FrameHooks
 * @brief Optional callbacks a host (the editor) runs at fixed points inside tick().
 */
struct FrameHooks {
    /// After input polling and asset updates, before the scene's update(). Input state for
    /// this frame is current; the host updates its own tools here.
    std::function<void(float dt)> pre_scene_update;
    /// After every scene's late_update() (UI events dispatched, commands flushed): the safe
    /// point to rebuild UI or restructure the scene.
    std::function<void(float dt)> post_late_update;
    /// Immediately before the pipeline records the frame: last chance to push debug lines or
    /// change the display region.
    std::function<void(float dt)> pre_render;
};

/**
 * @class Engine
 * @brief Top-level owner of the window, Vulkan device, assets, scene, and
 *        the render loop.
 *
 * Not copyable or movable -- every gfxcoopa RAII object it owns holds
 * references into sibling members, so relocation would invalidate them.
 */
class Engine {
public:
    /**
     * @brief Constructs the window and every core Vulkan/asset/scene object.
     * @param config Application configuration (window, render, scene, output).
     */
    explicit Engine(AppConfig config) : Engine(std::move(config), EngineOptions{}) {}

    /**
     * @brief Constructs an Engine embedded per `options` (project root, edit mode, ...).
     */
    Engine(AppConfig config, EngineOptions options)
        : options_(normalize_options_(std::move(options))),
          config_(std::move(config)),
          jobs_(config_.jobs.worker_threads ? config_.jobs.worker_threads
                                             : std::thread::hardware_concurrency()),
          ctx_(make_context_config_(config_)),
          pipeline_(std::make_unique<render::PixelRenderPipeline>(
              ctx_.device(), ctx_.allocator(), ctx_.swapchain(), ctx_.render_pass(),
              ctx_.command_pool(), make_render_config_(config_, options_.project_root))),
          assets_(&jobs_)
    {
        // Read once here rather than in the initializer list -- these are declared after
        // assets_/scene_mgr_/input_, and initializing them there regardless of list order
        // (member init always follows DECLARATION order) would trip -Wreorder for no benefit,
        // since neither env read depends on any other member.
        fixed_dt_       = fixed_dt_from_env_();
        capture_frames_ = capture_frames_from_env_();
        capture_ring_   = capture_ring_from_env_();
        no_input_       = no_input_from_env_();

        bind_default_input_();

        scene_mgr_.set_job_engine(&jobs_);
        pipeline_->set_job_engine(&jobs_);
        pipeline_->set_parallel_threshold(config_.jobs.parallel_threshold);

        // Profiling mode (PROFILE env var): per-frame CPU/GPU timings to a CSV + an exit summary.
        if (const std::string path = profile_path_from_env_(); !path.empty()) {
            profile_ = std::make_unique<render::FrameProfile>(path);
            if (profile_->ok()) {
                pipeline_->set_profiler(profile_.get());
                std::cout << "[profile] Profiling mode: per-frame timings -> " << path << "\n";
            } else {
                std::cerr << "[profile] Could not open '" << path << "' for writing; profiling off\n";
                profile_.reset();
            }
        }

        // The stats overlay's startup mode (config.yaml `debug.overlay`); F3 cycles it later.
        if (const auto mode = debug::parse_overlay_mode(config_.debug.overlay)) {
            debug_overlay_.set_mode(*mode);
        } else {
            std::cerr << "[toyengine] Unknown debug.overlay '" << config_.debug.overlay
                      << "' (expected off, fps or full); the overlay starts off\n";
        }

        edit_mode_ = options_.edit_mode;
        if (options_.set_app_icon) apply_app_icon_();
        // The project's assets first, then the engine checkout's as the fallback layer (shared
        // meshes, materials, fonts, UI themes); the same directory when building toyengine itself.
        for (const std::string& root : asset_roots()) assets_.add_search_root(root);
        // `prefab: objects/crate` (object assets) resolves against the same roots.
        coopa::scene::SceneLoader::set_search_roots(asset_roots());
        assets_.register_loader<coopa::gfx::engine::data::Mesh>(
            std::make_unique<coopa::gfx::engine::loaders::MeshLoader>(ctx_.device(), ctx_.allocator(), ctx_.command_pool()));
        // Pure-CPU bind-pose data for SkinnedMeshRenderer (toy::scene) -- no GPU handles, unlike
        // the Mesh loader above; see skinned_mesh_source.h's file doc for why toyengine CPU-skins
        // instead of uploading bone matrices to a shader.
        assets_.register_loader<coopa::gfx::engine::data::SkinnedMeshSource>(
            std::make_unique<coopa::gfx::engine::loaders::SkinnedMeshSourceLoader>());
        // Pixel-art texture sampling. texel_aa (the default): LINEAR + clamp-to-edge
        // (SamplerDesc::pixel_art_smooth()), paired with gfx_texel_aa_uv() in gbuffer.frag --
        // texel interiors render flat and hard-edged exactly like point sampling, but texel
        // boundaries blend over one screen pixel, so a moving camera glides them sub-pixel
        // instead of snapping them to the pixel grid (the snap reads as full-surface shimmer
        // on magnified texels). texel_aa: false restores raw NEAREST point sampling for A/B.
        assets_.register_loader<coopa::gfx::engine::data::Texture>(
            std::make_unique<coopa::gfx::engine::loaders::TextureLoader>(
                ctx_.device(), ctx_.allocator(), ctx_.command_pool(),
                config_.render.texel_aa ? coopa::gfx::SamplerDesc::pixel_art_smooth()
                                        : coopa::gfx::SamplerDesc::pixel_art()));
        coopa::gfx::engine::components::register_render_components(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), assets_);
        // The GPU-aware overload (a strict superset of the argument-free one): ClothRenderer
        // allocates its own dynamic vertex buffers and publishes the result as a runtime asset,
        // so it needs the same device/allocator/assets capture register_render_components() takes.
        scene::register_scene_components(ctx_.device(), ctx_.allocator(), assets_,
                                          ctx_.frames_in_flight());
        coopa::physx::register_physics_components(assets_, config_.physics);
        // "ParticleSystem" + "LightFlicker" (toyengine/particles/): emitter meshes load CPU-side,
        // mesh-mode render meshes and sprite textures through the same loaders as MeshRenderer.
        particles::register_particle_components(assets_);
        // "WeatherReactor" (toyengine/weather/): objects switching with the time of day / weather.
        weather::register_weather_components();
        // "SnowDeformer" (toyengine/world/snow_system.h): objects pressing trails into deep snow.
        world::register_snow_components();
        // "SaveId" (a stable identity in saves) and the demo "SaveDemo" (toyengine/save/).
        save::register_save_components();
        // The GPU overload, so font/sprite paths in scene YAML load on demand and
        // FontDefaults::resolve_font is wired for themes. Must precede load_scene(), like
        // every other parser registration above. Its captured device/allocator references
        // are released by the SceneLoader::clear_component_parsers() already in ~Engine().
        coopa::ui::register_ui_components(ctx_.device(), ctx_.allocator(), ctx_.command_pool());
        // Registers the AnimationClip asset loader and the "Animator" component parser --
        // blendy exports skeletal/object animation as baked Transform keyframe tracks (there is
        // no vertex skinning in this engine), referenced from a scene's `Animator` component.
        // Must precede load_scene(), like every other parser registration above.
        coopa::anim::register_animation_components(assets_);
        init_audio_();
        init_saves_();
        // Project modules (TOY_MODULE in a project's src/, see module.h) last, so they can
        // build on -- or replace -- any parser registered above.
        for (const Module& m : modules()) {
            if (m.on_engine_init) m.on_engine_init(*this);
        }

        if (options_.load_default_scene) {
            load_scene(scene_path_from_env_(config_.scene.default_scene));
        }
        read_cursor_pos_override_();
    }

    /**
     * @brief Replaces every managed scene with the one at `path`, fully set up: per-scene
     *        systems installed, every asset load drained, material shaders validated.
     *
     * Safe to call on a running Engine (the editor opens scenes this way). In edit mode the
     * new scene starts non-simulating.
     *
     * @param path Scene file (.yaml/.caml); relative paths resolve against the project root.
     * @return The new active scene.
     */
    coopa::scene::Scene& load_scene(const std::string& path) {
        ctx_.wait_idle();
        cancel_scene_load_("superseded by load_scene()");
        const std::string resolved = resolve_scene_path_(path);
        scene_mgr_.load_scene(resolved);
        scene_settings_[&scene_mgr_.get_active_scene()] = read_scene_settings_(resolved);
        scene_paths_[&scene_mgr_.get_active_scene()] = resolved;
        prepare_scene_(scene_mgr_.get_active_scene());
        return scene_mgr_.get_active_scene();
    }

    /**
     * @brief Loads the scene at `path` in the background while the current one keeps running,
     *        then swaps it in behind `options.transition` (see scene_loading.h).
     *
     * The document is read on a worker; objects are built a batch per frame
     * (options.build_budget_ms) and asset uploads finalized within options.finalize_budget_ms
     * per frame, so frames keep presenting throughout. The current scene is untouched until
     * activation: one GPU wait, the old scenes are destroyed (unless options.additive), the new
     * one is started, its systems installed, and it begins simulating (subject to edit mode).
     * Unlike load_scene() there is no blocking asset drain -- anything a system starts loading
     * at activation streams in over the next frames.
     *
     * One load at a time: while one is in flight a second request returns the in-flight
     * handle unchanged. load_scene() / set_scene() cancel it.
     *
     * @param path    Scene file (.yaml/.caml), resolved like load_scene()'s.
     * @param options Transition, additive, min_display_time, spawn_point, budgets.
     * @return A handle reporting progress(), is_ready() and the on_complete hook.
     */
    SceneLoadHandle load_scene_async(const std::string& path, SceneLoadOptions options = {}) {
        if (scene_load_ && scene_load_->pending()) {
            std::cerr << "[toyengine] load_scene_async('" << path << "'): '" << scene_load_->path
                      << "' is still loading; ignoring the new request\n";
            return SceneLoadHandle(scene_load_);
        }
        if (scene_load_ && !scene_load_->finished()) finish_transition_();   // a previous fade-in still running
        auto st = std::make_shared<detail::SceneLoadState>();
        st->path = resolve_scene_path_(path);
        st->options = std::move(options);
        st->peak_pending = assets_.pending_load_count();
        st->read = coopa::scene::SceneLoader::read_document_async(st->path, &jobs_);
        scene_load_ = st;
        if (st->options.transition.kind != SceneTransition::Kind::None) {
            transition_.set_fade(st->options.transition.color, 0.0f);
            // Over the game and the host's UI inside the display rect; under the stats HUD (100).
            add_overlay_layer(&transition_.scene(), 50, /*in_display_rect=*/true);
        }
        return SceneLoadHandle(st);
    }

    /** @brief The current (or last) async load's handle; empty if there never was one. */
    SceneLoadHandle scene_load() const { return SceneLoadHandle(scene_load_); }
    /** @brief True while an async load has not yet activated its scene. */
    bool scene_loading() const { return scene_load_ && scene_load_->pending(); }

    /**
     * @brief Replaces every managed scene with an already-built one (e.g. from
     *        SceneLoader::load_from_node()) and sets it up exactly as load_scene() does.
     * @param settings The scene document's `scene.settings` (its config overrides -- see
     *                 AppConfig::with_scene_settings()); null for none.
     */
    coopa::scene::Scene& set_scene(coopa::scene::Scene&& scene, const fkyaml::node& settings = fkyaml::node()) {
        ctx_.wait_idle();
        cancel_scene_load_("superseded by set_scene()");
        clear_scenes_();
        coopa::scene::Scene* raw = scene_mgr_.add_scene(std::make_unique<coopa::scene::Scene>(std::move(scene)));
        scene_settings_[raw] = settings;
        prepare_scene_(*raw);
        return *raw;
    }

    /**
     * @brief Adds an already-built scene alongside the current ones and makes it active,
     *        set up like load_scene(). The previously active scene keeps its state but stops
     *        updating until reactivated with activate_scene().
     *
     * The editor's play mode and material preview use this: the edit scene stays intact
     * underneath and comes back untouched when the added scene is removed.
     */
    coopa::scene::Scene& push_scene(coopa::scene::Scene&& scene, bool simulating,
                                    const fkyaml::node& settings = fkyaml::node()) {
        ctx_.wait_idle();
        if (scene_mgr_.has_scene()) scene_mgr_.set_scene_active(&scene_mgr_.get_active_scene(), false);
        coopa::scene::Scene* raw = scene_mgr_.add_scene(std::make_unique<coopa::scene::Scene>(std::move(scene)));
        scene_settings_[raw] = settings;
        scene_mgr_.set_active_scene(raw);
        prepare_scene_(*raw);
        raw->set_simulating(simulating);
        return *raw;
    }

    /** @brief Destroys a scene added by push_scene() (or any managed scene). */
    void remove_scene(coopa::scene::Scene* scene) {
        ctx_.wait_idle();
        scene_mgr_.remove_scene(scene);
        scene_settings_.erase(scene);
        scene_paths_.erase(scene);
        if (scene_mgr_.has_scene()) apply_scene_settings_(scene_mgr_.get_active_scene());
    }

    /** @brief Makes a managed scene the active, updating one; every other scene stops updating. */
    void activate_scene(coopa::scene::Scene* scene) {
        for (coopa::scene::Scene* s : scene_mgr_.scenes()) scene_mgr_.set_scene_active(s, s == scene);
        scene_mgr_.set_active_scene(scene);
        apply_scene_settings_(*scene);
    }

    // --- Scene settings (per-scene config overrides) -----------------------------------
    //
    // A scene file may override config.yaml's render and physics keys under `scene.settings`
    // (see AppConfig::with_scene_settings()). Whichever scene is active runs with config.yaml
    // plus its own overrides: render settings are applied live whenever the active scene
    // changes, physics settings when the scene's physics system is installed. Startup-fixed
    // render keys (see PixelRenderPipeline::apply_live_config()) cannot vary per scene.

    /**
     * @brief Replaces the config document scene overrides are layered on -- the editor hands
     *        over its edited (possibly unsaved) config.yaml. From then on render and physics are
     *        always re-derived from this document, overrides or not. Re-applies to the active
     *        scene now.
     * @param adjust Runs on every config derived from the document, e.g. the editor's own
     *               render overrides, so a re-derivation never undoes them.
     */
    void set_config_source(const fkyaml::node& config_yaml, std::function<void(AppConfig&)> adjust = {}) {
        config_.source = config_yaml.is_mapping() ? config_yaml : fkyaml::node::mapping();
        config_adjust_ = std::move(adjust);
        source_driven_ = true;
        if (scene_mgr_.has_scene()) apply_scene_settings_(scene_mgr_.get_active_scene(), kLiveRebuildDebounceFrames);
    }

    /**
     * @brief Sets a managed scene's overrides (its `scene.settings`), applying them if it is
     *        active. Its `weather` block goes to the scene's WeatherSystem live (the clock and
     *        the active condition keep running -- see WeatherSystem::set_settings()).
     */
    void set_scene_settings(coopa::scene::Scene& scene, const fkyaml::node& settings) {
        scene_settings_[&scene] = settings;
        if (scene_mgr_.has_scene() && &scene_mgr_.get_active_scene() == &scene)
            apply_scene_settings_(scene, kLiveRebuildDebounceFrames);
        if (weather::WeatherSystem* w = weather::find(scene)) w->set_settings(weather::parse_settings(weather_node_(settings)));
    }

    /// The active scene's weather and time of day (toyengine/weather/), or null without a scene.
    weather::WeatherSystem* weather() { return scene_mgr_.has_scene() ? weather::find(scene_mgr_.get_active_scene()) : nullptr; }

    /// Save slots and the hooks that fill them (toyengine/save/save_system.h).
    save::SaveSystem& saves() { return saves_; }

    /**
     * @brief The file a managed scene was loaded from (resolved, as load_scene() /
     *        load_scene_async() opened it); "" for one built in memory (set_scene, push_scene).
     */
    std::string scene_path(const coopa::scene::Scene& scene) const {
        auto it = scene_paths_.find(&scene);
        return it != scene_paths_.end() ? it->second : std::string();
    }

    /** @brief The config `scene` runs with: config.yaml plus its overrides. */
    AppConfig scene_config(const coopa::scene::Scene& scene) const {
        auto it = scene_settings_.find(&scene);
        const fkyaml::node none;
        const fkyaml::node& settings = it != scene_settings_.end() ? it->second : none;
        if (!source_driven_) return config_.with_scene_settings(settings);
        AppConfig derived = AppConfig::from_node(config_.source).with_scene_settings(settings);
        derived.window = config_.window;   // what the adjust hook sizes against
        if (config_adjust_) config_adjust_(derived);
        AppConfig out = config_;
        out.render = derived.render;
        out.physics = derived.physics;
        out.navigation = derived.navigation;
        return out;
    }

    /**
     * @brief Edit mode on/off. In edit mode the active scene does not simulate (only systems
     *        with ISceneSystem::runs_in_edit_mode() run), gameplay input drivers stand down,
     *        and the cursor is never captured.
     */
    void set_edit_mode(bool edit) {
        edit_mode_ = edit;
        if (scene_mgr_.has_scene()) scene_mgr_.get_active_scene().set_simulating(!edit);
        // Edit mode never holds the pointer: hand back a cursor a game may have captured.
        if (edit && ctx_.input().cursor_mode() != coopa::input::CursorMode::Normal) {
            ctx_.input().set_cursor_mode(coopa::input::CursorMode::Normal);
        }
    }
    bool edit_mode() const { return edit_mode_; }

    /**
     * @brief Gives the game the keyboard and mouse, or takes them away, while it keeps running.
     *
     * A standalone run always has focus (the default), so nothing changes there. A host that
     * embeds the game -- the editor's play mode -- starts it unfocused and focuses it on a
     * click in its viewer: unfocused, the gameplay input drivers push zero input (exactly as
     * NO_INPUT=1 does) and the cursor is released; focused, the cursor is captured when the
     * scene's CameraController asks for it (subject to the same NO_INPUT / invisible-window
     * stand-downs as the startup capture).
     */
    void set_game_input_focus(bool focused) {
        game_focused_ = focused;
        if (focused) {
            // Edit mode never captures (see set_edit_mode()): focus is only recorded for when
            // the game runs again.
            if (edit_mode_ || no_input_ || !config_.window.visible || !scene_mgr_.has_scene()) return;
            auto* cc = scene_mgr_.get_active_scene().find_first_component<scene::CameraController>();
            if (cc && cc->capture_cursor) ctx_.input().set_cursor_mode(coopa::input::CursorMode::Disabled);
        } else if (ctx_.input().cursor_mode() != coopa::input::CursorMode::Normal) {
            ctx_.input().set_cursor_mode(coopa::input::CursorMode::Normal);
        }
    }
    bool game_input_focus() const { return game_focused_; }

    /** @brief Installs the host callbacks tick() runs; see FrameHooks. */
    void set_frame_hooks(FrameHooks hooks) { hooks_ = std::move(hooks); }

    /**
     * @brief A second scene ticked and drawn on top of the active one -- the editor's UI.
     *
     * It is updated after the active scene, simulates regardless of edit mode, has its
     * canvases driven like the active scene's, and its screen-space canvases are drawn after
     * (over) the active scene's. Never saved, never replaced by load_scene(). Null removes it.
     */
    void set_overlay_scene(coopa::scene::Scene* scene) {
        overlay_scene_ = scene;
        pipeline_->set_overlay_scene(scene);
    }

    /**
     * @brief Adds an engine overlay layer: a scene ticked like the overlay scene and drawn
     *        over everything else -- the game, its HUD and the overlay scene (the editor UI).
     *
     * For the engine's own screens (the debug stats HUD, scene transitions). Layers draw in
     * ascending `order`, equal orders in the order they were added. `in_display_rect` places
     * the layer's screen canvases inside the scene's display rect (display_rect(): the
     * letterboxed game image, or the editor's viewport) at the display's scale, without input;
     * otherwise they cover the window like the overlay scene's. Re-adding a scene updates it.
     * Non-owning: remove_overlay_layer() before destroying the scene.
     */
    /// One add_overlay_layer() entry.
    struct OverlayLayer {
        coopa::scene::Scene* scene = nullptr;
        int  order = 0;
        bool in_display_rect = false;
    };
    void add_overlay_layer(coopa::scene::Scene* scene, int order = 0, bool in_display_rect = false) {
        if (!scene) return;
        std::erase_if(overlay_layers_, [scene](const OverlayLayer& l) { return l.scene == scene; });
        auto at = std::find_if(overlay_layers_.begin(), overlay_layers_.end(),
                               [order](const OverlayLayer& l) { return l.order > order; });
        overlay_layers_.insert(at, OverlayLayer{scene, order, in_display_rect});
        sync_overlay_layers_();
    }
    void remove_overlay_layer(coopa::scene::Scene* scene) {
        if (std::erase_if(overlay_layers_, [scene](const OverlayLayer& l) { return l.scene == scene; })) {
            sync_overlay_layers_();
        }
    }
    /** @brief The current overlay layers' scenes, in draw order. */
    std::vector<coopa::scene::Scene*> overlay_layers() const {
        std::vector<coopa::scene::Scene*> out;
        for (const OverlayLayer& l : overlay_layers_) out.push_back(l.scene);
        return out;
    }

    /**
     * @brief The on-screen stats HUD (toyengine/debug/debug_overlay.h): set_mode() / cycle()
     *        switch it (F3 does in a running game), and toy::debug::watch()/text() feed its
     *        Game block. Off by default (config.yaml `debug.overlay`).
     */
    debug::DebugOverlay& debug_overlay() { return debug_overlay_; }

    /**
     * @brief The profile the frame is being timed into: PROFILE mode's, or the live one the
     *        overlay's full mode runs (FrameProfile::latest() holds the last complete frame).
     *        Null when neither is on.
     */
    const render::FrameProfile* frame_profile() const { return active_profile_(); }

    /**
     * @brief Confines the rendered scene to a window-pixel rect (an editor viewport panel),
     *        letterboxed inside it. nullopt restores the whole window.
     */
    void set_display_region(std::optional<render::LetterboxRect> region) {
        pipeline_->set_display_region(region);
    }

    /**
     * @brief Where the ACTIVE scene's screen-space canvases sit, in window (framebuffer)
     *        pixels: inside `rect` instead of across the whole window. nullopt (the default)
     *        is a game's full-window HUD.
     *
     * For a host that shows the game in part of its window -- the editor's viewport, or its
     * UI designer's preview frame. Each screen canvas is sized to the rect, offset to it, its
     * draws clipped to it and its cursor mapped into it; with `input` false it draws but
     * never reacts. The overlay scene (the editor's own UI) is never placed.
     */
    struct ScreenUiPlacement {
        render::LetterboxRect rect;
        bool input = true;
        /// Magnification: the canvas is laid out for rect / zoom pixels and drawn scaled into
        /// rect -- a 1920x1080 HUD previewed in a 960x540 frame is zoom 0.5. See
        /// CanvasComponent::set_display_zoom().
        float zoom = 1.0f;
    };
    void set_scene_ui_placement(std::optional<ScreenUiPlacement> placement) { scene_ui_placement_ = placement; }
    const std::optional<ScreenUiPlacement>& scene_ui_placement() const { return scene_ui_placement_; }

    /** @brief The window-pixel rect the low-res scene image currently lands in. */
    render::LetterboxRect display_rect() const {
        return pipeline_->display_rect_for(const_cast<coopa::gfx::app::Context&>(ctx_).swapchain().extent().width,
                                         const_cast<coopa::gfx::app::Context&>(ctx_).swapchain().extent().height);
    }

    /**
     * @brief World-space ray through a window-pixel position, through the current display
     *        rect and the main camera. Direction is normalized.
     * @return False when there is no camera or the position is outside the display rect.
     */
    bool viewport_ray(const glm::vec2& window_px, glm::vec3& out_origin, glm::vec3& out_dir) const {
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        if (!cam) return false;
        const uint32_t rw = pipeline_->render_width();
        const uint32_t rh = pipeline_->render_height();
        if (rw == 0 || rh == 0) return false;
        const render::LetterboxRect box = display_rect();
        if (box.w == 0 || box.h == 0) return false;
        const glm::vec2 local = window_px - glm::vec2(box.x, box.y);
        if (local.x < 0 || local.y < 0 || local.x > box.w || local.y > box.h) return false;
        const float aspect = static_cast<float>(rw) / static_cast<float>(rh);
        const glm::mat4 inv_vp = glm::inverse(cam->get_projection_matrix(aspect) * cam->get_view_matrix());
        const glm::vec2 ndc(2.0f * local.x / static_cast<float>(box.w) - 1.0f,
                            1.0f - 2.0f * local.y / static_cast<float>(box.h));
        glm::vec4 near_h = inv_vp * glm::vec4(ndc, 0.0f, 1.0f);
        glm::vec4 far_h  = inv_vp * glm::vec4(ndc, 1.0f, 1.0f);
        if (std::abs(near_h.w) < 1e-9f || std::abs(far_h.w) < 1e-9f) return false;
        out_origin = glm::vec3(near_h) / near_h.w;
        out_dir = glm::vec3(far_h) / far_h.w - out_origin;
        const float len = glm::length(out_dir);
        if (len < 1e-12f) return false;
        out_dir /= len;
        return true;
    }

    /**
     * @brief Projects a world point to window pixels through the main camera and display rect.
     * @return False if there is no camera or the point is behind it.
     */
    bool world_to_window(const glm::vec3& world, glm::vec2& out_px) const {
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        if (!cam) return false;
        const uint32_t rw = pipeline_->render_width();
        const uint32_t rh = pipeline_->render_height();
        const render::LetterboxRect box = display_rect();
        if (rw == 0 || rh == 0 || box.w == 0) return false;
        const float aspect = static_cast<float>(rw) / static_cast<float>(rh);
        const glm::vec4 clip = cam->get_projection_matrix(aspect) * cam->get_view_matrix() * glm::vec4(world, 1.0f);
        if (clip.w <= 1e-6f) return false;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out_px = glm::vec2(box.x + (ndc.x * 0.5f + 0.5f) * box.w,
                           box.y + (0.5f - ndc.y * 0.5f) * box.h);
        return true;
    }

    // --- Subsystem access, for embedding hosts (the editor) and tests ---
    render::PixelRenderPipeline&  pipeline()      { return *pipeline_; }
    /// @brief The frame-in-flight slot this tick records into -- for a host re-uploading a
    /// dynamic mesh (Mesh::update_vertices) the way ClothRenderer does.
    uint32_t                      frame_slot()    { return ctx_.current_frame(); }
    coopa::asset::AssetManager&   assets()        { return assets_; }
    coopa::scene::SceneManager&   scene_manager() { return scene_mgr_; }
    const AppConfig&              config() const  { return config_; }
    /// The engine's audio: buses (Master/Music/SFX/UI), one-shots, pause. See audio_system.h.
    audio::AudioSystem&           audio()          { return *audio_; }
    const std::filesystem::path&  project_root() const { return options_.project_root; }
    /// Asset search roots, highest priority first: <project_root>/assets, then the engine
    /// checkout's assets/ (omitted when they are the same directory, and in a packaged build,
    /// whose assets/ already holds everything -- see RuntimeLayout).
    std::vector<std::string> asset_roots() const {
        std::vector<std::string> roots{(options_.project_root / "assets").string()};
        const std::filesystem::path& engine_assets = RuntimeLayout::current().engine_assets;
        std::error_code ec;
        if (!engine_assets.empty() &&
            !std::filesystem::equivalent(options_.project_root / "assets", engine_assets, ec)) {
            roots.push_back(engine_assets.string());
        }
        return roots;
    }
    bool has_scene() const { return scene_mgr_.has_scene(); }
    /// Window size in swapchain pixels.
    glm::uvec2 window_extent() {
        return {ctx_.swapchain().extent().width, ctx_.swapchain().extent().height};
    }
    /// Framebuffer pixels per Input cursor unit. Always 1: gfxcoopa's Window already converts
    /// GLFW's screen-point cursor positions to framebuffer pixels at the boundary (see
    /// presentation/window.h's to_framebuffer_coords_()), so Input speaks swapchain pixels on
    /// every platform. Kept as the one place that would change if that ever stopped being true.
    float cursor_scale() { return 1.0f; }

    /**
     * @brief Queues `fn` to run against the live Input right after the next poll -- synthetic
     *        clicks, keys and scrolls for tests and scripted runs, injected exactly where real
     *        events land (so per-frame pressed/released edges behave like real input).
     */
    void queue_input(std::function<void(coopa::input::Input&)> fn) { input_queue_.push_back(std::move(fn)); }
    /// Framebuffer pixels per screen point for this display, whether or not the window is
    /// visible -- what a tool UI scales by so its text keeps a physical size.
    float display_scale() { return ctx_.window().content_scale(); }
    /// The cursor in swapchain pixels (honours set_cursor_override()).
    glm::vec2 cursor_pixels() { return ctx_.input().cursor_position() * cursor_scale(); }
    /// Blocks until the GPU is idle (before destroying a resource a frame may reference).
    void wait_idle() { ctx_.wait_idle(); }

private:
    /**
     * @brief Fills in EngineOptions defaults (project_root -> default_project_root()). Runs first
     *        in the constructor, before the Vulkan context exists, so it is also where a packaged
     *        build points the loader at its bundled driver (prepare_runtime_environment()).
     */
    static EngineOptions normalize_options_(EngineOptions o) {
        prepare_runtime_environment();
        if (o.project_root.empty()) o.project_root = default_project_root();
        return o;
    }

public:
    /**
     * @brief The project this process runs when nothing says otherwise: a packaged build's own
     *        resources; else TOY_PROJECT_DIR if set (a relocated run), else the project it was
     *        compiled for. See RuntimeLayout::project_root().
     */
    static std::filesystem::path default_project_root() {
        return RuntimeLayout::current().project_root();
    }

private:

    /**
     * @brief Brings up audio: the AudioSystem (null device when headless or config says so),
     *        sfxcoopa's AudioSource / AudioListener parsers and uicoopa's UI sound components on
     *        the same engine, the UI sound library, and the player's saved bus volumes.
     */
    void init_audio_() {
        audio::AudioSystemOptions ao;
        ao.sample_rate = config_.audio.sample_rate;
        ao.open_device = config_.audio.enabled;
        // A run with no visible window (tests, headless captures, the editor's offscreen
        // engines) never takes over the sound card.
        ao.null_backend = config_.audio.device == "null" || !config_.window.visible;
        audio_ = std::make_unique<audio::AudioSystem>(ao);
        audio::AudioSystem::set_active(audio_.get());

        auto& sfx = coopa::sfx::SfxResources::instance();
        sfx.set_engine(&audio_->engine());
        sfx.set_search_roots(asset_roots());
        coopa::sfx::register_sfx_components();
        audio::register_audio_components();
#ifdef UICOOPA_HAS_AUDIO
        coopa::ui::UiAudio::set_active(audio_->ui());
        coopa::ui::register_ui_audio_components();
        // UI sounds: the first asset root with a sounds/ manifest (a packaged build carries
        // uicoopa's defaults there), else uicoopa's own from source.
        std::string sounds_dir;
        std::error_code ec;
        for (const std::string& r : asset_roots()) {
            if (coopa::yaml::document_exists(std::filesystem::path(r) / "sounds" / "sounds.yaml")) { sounds_dir = r + "/sounds"; break; }
        }
        if (sounds_dir.empty() && !RuntimeLayout::current().packaged()) sounds_dir = std::string(PROJ_DIR) + "/uicoopa/assets/sounds";
        if (!sounds_dir.empty() && coopa::yaml::document_exists(std::filesystem::path(sounds_dir) / "sounds.yaml")) {
            coopa::ui::SoundLibrary::instance().set_search_dir(sounds_dir);
            coopa::ui::SoundLibrary::instance().load_manifest("sounds.yaml");
        }
#endif
        // Bus volumes: config.yaml's defaults, then whatever this player chose last time.
        UserSettings& us = UserSettings::instance();
        us.load();
        const std::pair<const char*, float> buses[] = {
            {audio::k_bus_master, config_.audio.master}, {audio::k_bus_music, config_.audio.music},
            {audio::k_bus_sfx, config_.audio.sfx}, {audio::k_bus_ui, config_.audio.ui}};
        for (const auto& [bus, def] : buses) audio_->set_bus_volume(bus, us.get_float(audio::volume_setting_key(bus), def));
    }

    /**
     * @brief The save system: config.yaml's `save:` keys, and the host hooks it takes the
     *        scene from and loads saved scenes through (load_scene_async(), never in an engine
     *        embedded by a tool -- the editor's play mode applies saves in place).
     */
    void init_saves_() {
        if (const auto enc = save::parse_save_encoding(config_.save.encode)) {
            saves_.set_encoding(*enc);
        } else {
            std::cerr << "[toyengine] Unknown save.encode '" << config_.save.encode << "' (expected auto, yaml or caml)\n";
        }
        save::SaveSystem::Host host;
        host.scene = [this]() -> coopa::scene::Scene* { return scene_mgr_.has_scene() ? &scene_mgr_.get_active_scene() : nullptr; };
        host.scene_path = [this]() { return scene_mgr_.has_scene() ? save_scene_ref_(scene_path(scene_mgr_.get_active_scene())) : std::string(); };
        host.load_scene = [this](const std::string& scene, std::function<void(coopa::scene::Scene&)> ready,
                                 std::function<void(const std::string&)> failed) {
            if (options_.edit_mode || scene_loading()) return false;
            SceneLoadHandle h = load_scene_async(scene);
            h.on_complete(std::move(ready));
            h.on_failed(std::move(failed));
            return true;
        };
        saves_.set_host(std::move(host));
        save::SaveSystem::set_active(&saves_);
    }

    /** @brief A scene file as saves store it: relative to the project root when inside it. */
    std::string save_scene_ref_(const std::string& path) const {
        if (path.empty()) return path;
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(path, options_.project_root, ec);
        if (ec || rel.empty() || *rel.begin() == "..") return path;
        return rel.generic_string();
    }

    /** @brief Per frame: play time while the game runs, and the quick-save / quick-load keys. */
    void update_saves_(float dt) {
        if (edit_mode_ || !scene_mgr_.has_scene()) return;
        saves_.tick(dt);
        const std::string& slot = config_.save.quick_slot;
        if (slot.empty() || options_.edit_mode || input_blocked_() || scene_loading()) return;
        if (input_.is_pressed("quick_save", ctx_.input())) {
            if (saves_.save(slot)) std::cout << "[toyengine] Saved '" << slot << "' (" << saves_.slot_path(slot).string() << ")\n";
        } else if (input_.is_pressed("quick_load", ctx_.input()) && saves_.has_slot(slot)) {
            if (saves_.load(slot)) std::cout << "[toyengine] Loading '" << slot << "'\n";
        }
    }

    void shutdown_audio_() {
        if (!audio_) return;
        UserSettings::instance().flush();
        audio_->stop_all();
        if (audio::AudioSystem::active() == audio_.get()) audio::AudioSystem::set_active(nullptr);
        auto& sfx = coopa::sfx::SfxResources::instance();
        if (sfx.engine() == &audio_->engine()) sfx.set_engine(nullptr);
#ifdef UICOOPA_HAS_AUDIO
        if (coopa::ui::UiAudio::active() == audio_->ui()) coopa::ui::UiAudio::set_active(nullptr);
#endif
    }

    /** @brief Per frame: the listener follows an AudioListener, else the main camera. */
    void update_audio_(float dt) {
        if (!audio_) return;
        glm::mat4 cam_world;
        const glm::mat4* cam_ptr = nullptr;
        if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) {
            cam_world = glm::inverse(cam->get_view_matrix());
            cam_ptr = &cam_world;
        }
        audio_->update(dt, cam_ptr);
        // Persist a changed volume promptly (a player who quits by killing the app keeps it).
        if (UserSettings::instance().dirty() && (++settings_flush_frames_ % 120) == 0) UserSettings::instance().flush();
    }

    /** @brief Destroys every managed scene. */
    void clear_scenes_() {
        if (audio_) audio_->stop_all();
        for (coopa::scene::Scene* s : scene_mgr_.scenes()) scene_mgr_.remove_scene(s);
        scene_settings_.clear();
        scene_paths_.clear();
    }

    /** @brief A scene file's `scene.settings` block (null if it has none or can't be read). */
    static fkyaml::node read_scene_settings_(const std::string& path) {
        try {
            const fkyaml::node doc = coopa::yaml::load_document(coopa::yaml::resolve_variant(path));
            if (doc.contains("scene") && doc.at("scene").contains("settings")) return doc.at("scene").at("settings");
        } catch (const std::exception&) {}
        return fkyaml::node();
    }

    /**
     * @brief Applies `scene`'s effective render config live (see scene_config()) and returns
     *        its full effective config, for the physics install in prepare_scene_().
     *
     * A scene without overrides leaves the live config alone -- unless the previous one had
     * overrides to undo, or the editor drives the config document -- so code that tweaks
     * render_config() at runtime keeps its changes across plain scene loads. The live
     * debug_view and the resolved shader/palette state are always kept.
     *
     * A change to a field fixed at pipeline construction (TOY_STARTUP_FIXED_FIELDS: most
     * feature switches, render size, shadow map sizes...) queues an in-place pipeline rebuild
     * (queue_rebuild_()), run at the top of a tick after `rebuild_delay` frames -- so a scene
     * can turn SSR or bloom on or off, and a dragged editor slider rebuilds once, not per frame.
     */
    AppConfig apply_scene_settings_(const coopa::scene::Scene& scene, int rebuild_delay = 0) {
        // The weather's writes are not config: put the config's own values back first, so they
        // are what gets layered or kept (the weather re-captures and re-applies next frame).
        restore_weather_atmosphere_();
        restore_sky_colours_();
        const AppConfig eff = scene_config(scene);
        auto it = scene_settings_.find(&scene);
        const bool overrides = it != scene_settings_.end() && AppConfig::has_scene_overrides(it->second);
        if (overrides || overrides_applied_ || source_driven_) {
            render::PixelRenderConfig next = make_render_config_(eff, options_.project_root);
            const render::PixelRenderConfig& live = pipeline_->render_config();
            next.shaders = live.shaders;
            next.surface_shaders = live.surface_shaders;
            next.debug_view = live.debug_view;
            next.fill_aspect = live.fill_aspect;   // the engine's, from the display region -- see update_fill_extent_()
            // Fields fixed at construction (a feature switch, a target size) need a new
            // pipeline: queue one for the top of the next tick. Everything else applies now.
            // Compared with what the config asked for last time, not with the live pipeline:
            // only fields the config / scene actually changed rebuild, and a runtime tweak of
            // render_config() (a test, game code) is never undone.
            if (!derived_render_) derived_render_ = source_render_config_();
            if (render::PixelRenderPipeline::needs_rebuild(*derived_render_, next)) queue_rebuild_(*derived_render_, next, rebuild_delay);
            derived_render_ = next;
            pipeline_->apply_live_config(next, /*warn_ignored=*/false);
        }
        overrides_applied_ = overrides;
        return eff;
    }

    /**
     * @brief Everything a freshly loaded scene needs before its first frame: per-scene
     *        systems, drained asset loads, shader validation, edit-mode and cursor state.
     */
    void prepare_scene_(coopa::scene::Scene& scene, bool drain_assets = true) {
        // The scene's own settings first: render (live) before the water system reads
        // water_quality below, physics for install_physics_system().
        const AppConfig scene_cfg = apply_scene_settings_(scene);
        // BEFORE the physics system in numeric order (50 vs 100), which is the whole point: a
        // component driving a kinematic body's Transform has to write it before PhysicsSystem reads
        // it, or physics spends the frame solving against the previous pose while the renderer draws
        // the new one. See kinematic_control_system.h's file doc.
        scene::install_kinematic_control_system(scene);
        // Order 60, so it too sits ahead of Physics (100) -- and, more to the point, ahead of the
        // Behaviour walk (200) and TransformResolve (350), so a terrain chunk that appears this
        // frame already has its world matrix resolved when the render gather reads it. A scene
        // with no Terrain component pays one empty get_components<>() sweep per frame.
        world::install_terrain_system(scene, ctx_.device(), ctx_.allocator(), assets_);
        // Order 90: bakes water surfaces and refreshes the buoyant-body list right before
        // Physics (100) steps, whose substep callback it drives -- see water_system.h's file doc.
        water::install_water_system(scene, &ctx_.device(), &ctx_.allocator(), &assets_)
            ->set_settings(water_settings_for_(pipeline_->render_config().water_quality));
        coopa::physx::system::install_physics_system(scene, scene_cfg.physics);
        // Order 150: reads the physics world (100) for its colliders, and moves NavAgents before
        // the Behaviour walk (200) -- see physxcoopa/system/nav_system.h.
        if (scene_cfg.navigation.enabled) coopa::physx::system::install_nav_system(scene, scene_cfg.navigation);
        // Order 360: after TransformResolve (350), so every emitter's world matrix is current;
        // runs in edit mode too, so effects preview live in the editor -- see
        // particle_system_runner.h.
        // `simulation: gpu` systems run in compute when particles.gpu_enabled and the device has
        // it; their alive counts come back from the renderer's readback.
        particles::install_particle_system(scene)->set_gpu(
            config_.particles.gpu_enabled && ctx_.device().supports_compute(),
            [this](uint64_t id, uint32_t& count, uint32_t& generation) {
                return pipeline_ && pipeline_->gpu_particle_alive(id, count, generation);
            });
        // Order 40: the clock, the active weather condition, sky / fog / sun and the runtime
        // weather effects -- ahead of the Behaviour walk, so gameplay reads this frame's weather.
        // Runs in edit mode too (clock held), so the editor previews it.
        {
            auto it = scene_settings_.find(&scene);
            weather::install_weather_system(scene, weather_node_(it != scene_settings_.end() ? it->second : fkyaml::node()));
        }
        // Order 370: after TransformResolve, so deformers stamp where they are drawn; reads the
        // weather (40) for the cover and the precipitation map -- see world/snow_system.h.
        world::install_snow_system(scene);

        // Activate TransformSystem before the first drain or render, so the world_matrix()
        // reads below are never asked to resolve a still-dirty transform; then block until
        // every load issued above has finished, so frame 0 sees a fully populated scene.
        coopa::scene::install_transform_system(scene);
        // Runs at UpdatePhase::Animation (300), before TransformResolve (350) -- Scene::update()
        // orders installed systems by phase regardless of install call order, so this only needs
        // to exist before the scene starts ticking, same as install_transform_system() above.
        coopa::anim::install_animation_system(scene);
        // Order 320: TwoBoneIK / LookAtIK / FootIK over the animated pose, before TransformResolve
        // (350) -- see coopa/animation/ik_system.h. A scene with no IK pays one component sweep.
        coopa::anim::install_ik_system(scene);
        // Orders 90 / 290 / 330: Ragdoll mode changes before Physics, and the recovery blend
        // around the Animator and IK -- see toyengine/scene/ragdoll.h.
        scene::install_ragdoll_system(scene);
        // An async load skips the drain: its build already waited for the scene's own assets,
        // and whatever a system starts loading here streams in over the next frames.
        if (drain_assets) drain_pending_assets_();

        // Fail fast on a typo'd/unregistered PBRMaterial::shader -- see
        // PixelRenderPipeline::validate_material_shaders()'s doc for why this can't happen
        // during YAML parsing itself.
        pipeline_->validate_material_shaders(scene);

        scene.set_simulating(!edit_mode_);
        apply_cursor_capture_();
    }

public:

    ~Engine() {
        ctx_.wait_idle();
        shutdown_audio_();
        if (save::SaveSystem::active() == &saves_) save::SaveSystem::set_active(nullptr);
        // Scenes before assets_.shutdown() below: every MeshRenderer (and texture, font, clip...)
        // holds an AssetHandle whose destructor releases its slot in the AssetManager. Left to
        // member destruction, the scenes went AFTER shutdown() had already freed those slots --
        // a use-after-free on every Engine teardown (AddressSanitizer: ~MeshRenderer writing a
        // slot freed by AssetManager::shutdown()), which corrupted the heap for whatever a
        // process did next (the editor tests' "random" segfaults).
        clear_scenes_();
        // An unfinished async load holds a built scene (asset handles) and the transition layer
        // a loading-screen UI (UIResourceCache fonts): both go before the caches below.
        cancel_scene_load_("engine shutting down");
        finish_transition_();
        transition_.release();
        // The debug overlay's scene draws with a UIResourceCache font, cleared below.
        remove_overlay_layer(debug_overlay_.has_scene() ? &debug_overlay_.scene() : nullptr);
        debug_overlay_.release_scene();
        debug::GameLines::instance().set_accepting(false);
        // register_render_components()'s parser lambdas capture ctx_'s device/allocator/
        // cmd_pool by reference in SceneLoader's function-local static registry, which
        // would otherwise only be destroyed at program exit -- after ctx_ goes out of
        // scope. Clear it now, while they're still alive. Then shut down the asset
        // manager (which holds every loaded Mesh/Texture's GPU allocation) before ctx_
        // (and the device/allocator it owns) destructs.
        coopa::scene::SceneLoader::clear_component_parsers();

        // UIResourceCache holds GPU-resident Fonts/Textures in function-local static storage,
        // so without this they would only be released at program exit -- after ctx_'s Device
        // and Allocator are gone, which trips VMA's "allocations not freed" assertion. Same
        // contract as clear_component_parsers() above; see UIResourceCache::clear()'s doc.
        coopa::ui::UIResourceCache::instance().clear();
        // font_for_path() publishes the first font it loads as the static FontDefaults::font,
        // and clear() just destroyed it without resetting that. Left dangling, the NEXT Engine
        // in this process (every editor test) hands it to its first unthemed Text as the
        // fallback font -- a use-after-free AddressSanitizer caught in mark_text_atlases().
        coopa::ui::FontDefaults::font = nullptr;
        assets_.shutdown();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /**
     * @brief Runs the main loop until the window closes or a frame limit is hit.
     *
     * Honors two headless-verification env vars (matching blendy's
     * ONESHOT/MAX_FRAMES convention): ONESHOT=1 renders exactly one frame,
     * MAX_FRAMES=N renders N frames, both then exit cleanly instead of
     * waiting for the window to close.
     *
     * Two further env vars make multi-frame captures reproducible, which plain ONESHOT/
     * MAX_FRAMES cannot: MAX_FRAMES alone still advances the scene on wall-clock dt (so a
     * wall-clock-driven orbit camera lands at a different angle every run), and ONESHOT's
     * single frame has no SSR temporal history yet -- exactly the one frame that cannot show
     * a temporal artifact like reflection flicker.
     *   FIXED_DT=<seconds>   overrides the per-tick delta time fed to assets/scene updates
     *                        (see frame_dt_()), so e.g. an auto-rotating orbit camera advances
     *                        by an exact, repeatable angle every frame instead of whatever the
     *                        wall clock produced.
     *   CAPTURE_FRAMES=<N>   dumps one PNG per tick to output/seq/frame_%04d.png (via
     *                        capture_sequence_frame_()) for N frames, then stops the loop --
     *                        independent of MAX_FRAMES, which still applies if also set.
     *   CAPTURE_RING=<N>     keeps the LAST N frames in RAM while playing normally, and
     *                        writes them all to output/seq/ only on exit. Unlike
     *                        CAPTURE_FRAMES there is no per-frame PNG encode, so gameplay
     *                        stays close to full speed -- the cost is one GPU readback per
     *                        frame plus N * width * height * 4 bytes of memory (~2.5 GB for
     *                        300 frames at 1080p). Play until the artifact happens, then
     *                        quit: the ring holds the final N frames losslessly.
     *   NO_INPUT=1           zeroes all camera-controller input every frame (see
     *                        drive_camera_controller_()), so a capture running on a live
     *                        desktop isn't perturbed by real mouse/keyboard activity.
     *
     * One further env var is read by the CONSTRUCTOR, not this loop:
     *   SCENE=<name|path>    loads a different scene than assets/config.yaml's
     *                        scene.default_scene -- see scene_path_from_env_().
     * (HEADLESS and CONFIG are applied by main.cpp, before the Engine exists.)
     *
     * Profiling mode, read by the constructor:
     *   PROFILE=1 | <path>.csv  per-frame CPU phase + per-feature GPU timings to a CSV
     *                        (output/profile.csv for "1"), plus an averaged per-feature
     *                        breakdown printed on exit -- see render/frame_profile.h and
     *                        render/gpu_profiler.h. Graph it with tools/plot_profile.py.
     *
     * On exit, prints the mean wall-clock frame time over every frame after the first
     * kWarmupFrames -- startup, pipeline creation and first-use costs excluded -- so a
     * MAX_FRAMES run doubles as a benchmark.
     */
    void run() {
        constexpr uint64_t kWarmupFrames = 30;
        uint32_t captured = 0;
        std::deque<coopa::gfx::util::ImageData> ring;
        uint64_t frames_run = 0;
        std::chrono::steady_clock::time_point timed_start{};
        while (!ctx_.should_close()) {
            if (!tick()) break;
            if (++frames_run == kWarmupFrames) timed_start = std::chrono::steady_clock::now();

            if (capture_frames_ > 0) {
                capture_sequence_frame_(captured);
                ++captured;
                if (captured >= capture_frames_) break;
            }
            if (capture_ring_ > 0) {
                ctx_.wait_idle();
                ring.push_back(capture_image(config_.output.save_low_res));
                if (ring.size() > capture_ring_) ring.pop_front();
            }

            if (ctx_.max_frames() > 0 && ctx_.frame_index() >= ctx_.max_frames()) break;
        }

        if (frames_run > kWarmupFrames) {
            ctx_.wait_idle();   // count the GPU work of the frames already submitted
            const double secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - timed_start).count();
            const double ms = secs * 1000.0 / static_cast<double>(frames_run - kWarmupFrames);
            std::printf("[toyengine] Frame time: %.2f ms (%.1f fps), mean of %llu frames after %llu warm-up\n",
                        ms, 1000.0 / ms, static_cast<unsigned long long>(frames_run - kWarmupFrames),
                        static_cast<unsigned long long>(kWarmupFrames));
            const std::string mesh_stats = pipeline_->mesh_draw_stats_summary();
            if (!mesh_stats.empty()) std::printf("[toyengine] Mesh draws/frame: %s\n", mesh_stats.c_str());
        }
        if (profile_) {
            ctx_.wait_idle();
            profile_->print_summary();
        }

        if (!ring.empty()) {
            std::filesystem::create_directories("output/seq");
            uint32_t index = 0;
            for (const auto& frame : ring) {
                char path[64];
                std::snprintf(path, sizeof(path), "output/seq/frame_%04u.png", index++);
                coopa::gfx::util::save_image_png(frame, path);
            }
            std::cout << "[toyengine] Wrote " << ring.size() << " ring-captured frames to output/seq/\n";
        }

        // Never in a shipping build: a player's game must not write screenshots on quit.
        if (!k_shipping && config_.output.save_on_exit) {
            ctx_.wait_idle();
            const std::string out = output_path_(config_.output.filepath);
            std::error_code dir_ec;
            if (const auto dir = std::filesystem::path(out).parent_path(); !dir.empty()) {
                std::filesystem::create_directories(dir, dir_ec);
            }
            save_screenshot(out, config_.output.save_low_res);
        }
    }

    /**
     * @brief MUTABLE access to the live render config, for changing parameters at runtime.
     *
     * Forwards to PixelRenderPipeline::render_config_mut() -- see that method for which
     * fields take effect immediately (most of them, including every fog parameter) and
     * which are startup-fixed and will not.
     *
     * @code
     * engine.render_config().fog_density = 0.12f;   // visible next frame
     * @endcode
     */
    render::PixelRenderConfig& render_config() { return pipeline_->render_config_mut(); }

    /**
     * @brief Pins the pointer to a fixed window-pixel position from now on, exactly as
     *        CURSOR_POS="<x>,<y>" does -- but settable on a running Engine.
     *
     * Re-applied every tick (see apply_cursor_pos_override_()), so it wins over real pointer
     * motion. The env var is only read once, at construction, which is enough for a scripted
     * capture but not for a test that wants to park the pointer on a world-space Button, look
     * at the frame, then move it away and look again: doing that through the env var costs a
     * whole second Engine -- window, device, pipelines and scene included -- to change two
     * numbers.
     *
     * @param window_pixels Position in window pixels, the same space CURSOR_POS uses.
     */
    void set_cursor_override(const glm::vec2& window_pixels) {
        cursor_pos_          = window_pixels;
        cursor_pos_override_ = true;
    }

    /** @brief Releases set_cursor_override(), handing the pointer back to the real mouse. */
    void clear_cursor_override() { cursor_pos_override_ = false; }

    /**
     * @brief Reads the current offscreen buffer back into host memory, as tightly-packed
     *        row-major RGBA8.
     *
     * The in-memory half of save_screenshot() (which is this plus a PNG write), for a
     * caller that wants to look at the pixels rather than keep them: a headless test
     * comparing two frames, or measuring how many of them carry a given colour, pays neither
     * a PNG encode nor a decode nor a temporary file for the privilege.
     *
     * Waits for the device to go idle first, so the returned pixels are the frame the last
     * tick() finished rather than one still being recorded.
     *
     * @param low_res Selects the same image save_screenshot() would write -- see its @p
     *                low_res parameter for what each one contains.
     * @return The image's pixel bytes, dimensions and bytes-per-texel.
     */
    coopa::gfx::util::ImageData capture_image(bool low_res = true) {
        ctx_.wait_idle();
        coopa::gfx::memory::Image& src = low_res ? pipeline_->low_res_color_image()
                                                 : pipeline_->final_color_image();
        return coopa::gfx::util::read_image(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), src);
    }

    /**
     * @brief Writes the current offscreen buffer to a PNG.
     * @param path    Destination file path.
     * @param low_res True writes the internal low-resolution buffer 1:1 (pixel-perfect, but
     *                BEFORE any display-resolution effect such as tilt shift, and containing
     *                NO UI -- neither canvas layer is part of it, since both composite later
     *                and at window resolution; see PixelRenderPipeline::low_res_color_image()).
     *                False writes the final image actually shown in the window, UI included
     *                (PixelRenderPipeline::final_color_image()), at DISPLAY resolution.
     */
    void save_screenshot(const std::string& path, bool low_res = true) {
        coopa::gfx::util::save_image_png(capture_image(low_res), path);
        if (low_res) {
            std::cout << "[toyengine] Saved " << path << " (" << pipeline_->render_width()
                      << "x" << pipeline_->render_height() << ")\n";
        } else {
            std::cout << "[toyengine] Saved " << path << " (display resolution)\n";
        }
    }

    /**
     * @brief Writes the just-rendered frame to output/seq/frame_%04d.png, for CAPTURE_FRAMES.
     * @param index 0-based sequence index; formatted into the filename.
     */
    void capture_sequence_frame_(uint32_t index) {
        ctx_.wait_idle();
        // Relative, like config_.output.filepath -- resolves against the process CWD, not
        // ROOT_DIR (see save_screenshot()'s own doc / AppConfig::output.filepath's default).
        std::filesystem::create_directories("output/seq");
        char path[64];
        std::snprintf(path, sizeof(path), "output/seq/frame_%04u.png", index);
        save_screenshot(path, config_.output.save_low_res);
    }

    /**
     * @brief Advances and renders exactly one frame.
     * @return False when the loop should stop (window close requested).
     */
    bool tick() {
        using render::CpuScope;
        using render::CpuTimer;
        // Before `prof` is taken: this may create or destroy the live profile.
        sync_live_profile_();
        render::FrameProfile* prof = active_profile_();
        if (prof) {
            prof->begin_frame(profile_frame_++);
            prof->prune();
        }

        float dt = 0.0f;
        {
            CpuTimer t(prof, CpuScope::Input);
            ctx_.poll(); // window_.new_frame() + poll_events() + frame timer update.
            for (auto& fn : input_queue_) fn(ctx_.input());
            input_queue_.clear();

            if (options_.escape_quits && input_.is_down("quit", ctx_.input())) {
                ctx_.window().set_should_close(true);
            }

            dt = frame_dt_();
            apply_cursor_pos_override_();
            update_debug_overlay_();
        }
        // Before anything emits UI: drive_ui_canvases_() seeds every canvas's DrawList with the
        // pipeline's white texture, and a rebuild later in the frame would free it under that
        // frame's UI (drawn black). Uses the display region last frame's pre_render set.
        run_pending_rebuild_();
        if (scene_mgr_.has_scene()) update_fill_extent_();
        {
            CpuTimer t(prof, CpuScope::Assets);
            // While a scene loads in the background its uploads are spread over frames.
            assets_.update(dt, scene_loading() ? scene_load_->options.finalize_budget_ms : -1.0f);
            consume_scene_link_requests_();
            // Also after a failure, while the cover fades back off the running scene.
            if (scene_load_ && (!scene_load_->finished() || scene_load_->alpha > 0.0f)) update_scene_load_(dt);
        }
        update_saves_(dt);
        if (hooks_.pre_scene_update) hooks_.pre_scene_update(dt);

        {
            CpuTimer t(prof, CpuScope::SceneUpdate);
            if (scene_mgr_.has_scene() && !edit_mode_) {
                drive_camera_controller_(scene_mgr_.get_active_scene());
                drive_kinematic_controllers_(scene_mgr_.get_active_scene());
                drive_free_movers_(scene_mgr_.get_active_scene());
                drive_character_controllers_(scene_mgr_.get_active_scene());
                drive_ragdolls_(scene_mgr_.get_active_scene());
            }
            scene_mgr_.update(dt);
            if (overlay_scene_) overlay_scene_->update(dt);
            for (const OverlayLayer& l : overlay_layers_) l.scene->update(dt);
        }
        {
            CpuTimer t(prof, CpuScope::LateUpdate);
            if (scene_mgr_.has_scene()) {
                // Between update() and late_update(), and that window is the only correct slot:
                // world matrices are current only after UpdatePhase::TransformResolve (350) has
                // run inside update(), and a canvas needs its pointer position before
                // EventSystem::process() is dispatched from inside late_update() (400).
                drive_ui_canvases_(scene_mgr_.get_active_scene(), scene_ui_placement_);
            }
            if (overlay_scene_) drive_ui_canvases_(*overlay_scene_, std::nullopt);
            for (const OverlayLayer& l : overlay_layers_) drive_ui_canvases_(*l.scene, overlay_layer_placement_(l));
            // late_update() runs LateBehaviourSystem, flushes each worker's deferred
            // SceneCommandBuffer and advances Scene::frame_index(). Must precede render() so a
            // same-frame deferred spawn or destroy is reflected in what is drawn, matching
            // Unity's Update -> LateUpdate -> render order.
            scene_mgr_.late_update(dt);
            if (overlay_scene_) overlay_scene_->late_update(dt);
            // After every other scene's late_update(): the stats and the game's watch() lines
            // for this frame are all in.
            if (debug_overlay_.full() && debug_overlay_.refresh_due()) collect_debug_stats_();
            for (const OverlayLayer& l : overlay_layers_) l.scene->late_update(dt);
        }
        update_audio_(dt);
        if (hooks_.post_late_update) hooks_.post_late_update(dt);

        if (scene_mgr_.has_scene()) {
            {
                CpuTimer t(prof, CpuScope::DynamicMeshes);
                upload_dynamic_meshes_(scene_mgr_.get_active_scene());
                gather_debug_lines_(scene_mgr_.get_active_scene());
            }
            if (hooks_.pre_render) hooks_.pre_render(dt);
            sync_water_render_state_(scene_mgr_.get_active_scene());
            sync_weather_render_state_(scene_mgr_.get_active_scene());
            sync_sky_render_state_(scene_mgr_.get_active_scene(), dt);
            sync_surface_state_(scene_mgr_.get_active_scene());
            {
                // After the host's pre_render hook, like the water sync: the batches point into
                // the systems' buffers and go to THIS pipeline.
                CpuTimer t(prof, CpuScope::DynamicMeshes);
                sync_particle_render_state_(scene_mgr_.get_active_scene());
            }
            CpuTimer t(prof, CpuScope::Render);
            pipeline_->render(ctx_.renderer(), scene_mgr_.get_active_scene(), dt);
        }
        // toy::debug::watch()/text() lines last one frame.
        debug::GameLines::instance().clear();

        return !ctx_.should_close();
    }

    /**
     * @brief Advances the async load one frame: polls the document, builds a batch of objects,
     *        tracks the asset loads, drives the transition and activates when everything is in.
     */
    void update_scene_load_(float dt) {
        detail::SceneLoadState& st = *scene_load_;
        ++st.frames;
        st.elapsed += dt;
        const SceneTransition& tr = st.options.transition;
        const bool covered_kind = tr.kind != SceneTransition::Kind::None;
        st.peak_pending = std::max(st.peak_pending, assets_.pending_load_count());

        try {
            if (st.stage == SceneLoadStage::ReadingDocument && st.read && st.read->ready()) {
                if (st.read->failed()) throw std::runtime_error(st.read->error());
                const fkyaml::node& doc = st.read->document();
                if (doc.contains("scene") && doc.at("scene").contains("settings")) st.settings = doc.at("scene").at("settings");
                st.builder = std::make_unique<coopa::scene::SceneLoader::Builder>(doc, st.path, coopa::scene::SceneLoader::LoadOptions{.start = false});
                st.read.reset();
                st.stage = SceneLoadStage::Building;
            } else if (st.stage == SceneLoadStage::Building) {
                if (st.builder->step(st.options.build_budget_ms)) st.stage = SceneLoadStage::LoadingAssets;
            }
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] Async load of '" << st.path << "' failed: " << e.what() << "\n";
            st.fail(e.what());
            transition_.hide_loading_screen();   // fade back in over the old scene
        }
        if (st.stage == SceneLoadStage::LoadingAssets && assets_.pending_load_count() == 0) {
            st.stage = SceneLoadStage::WaitingToSwap;
        }

        // Progress: the document 5%, the build 65%, the assets 30%.
        switch (st.stage) {
            case SceneLoadStage::ReadingDocument: break;
            case SceneLoadStage::Building:
                st.raise_progress(0.05f + 0.65f * st.builder->progress());
                break;
            case SceneLoadStage::LoadingAssets: {
                const float peak = static_cast<float>(std::max<size_t>(st.peak_pending, 1));
                st.raise_progress(0.70f + 0.30f * (1.0f - static_cast<float>(assets_.pending_load_count()) / peak));
                break;
            }
            default: st.raise_progress(st.stage == SceneLoadStage::Failed ? st.progress : 1.0f); break;
        }

        // The cover: up over fade_out before activation, down over fade_in after (or after a failure).
        if (st.stage == SceneLoadStage::FadingIn || st.stage == SceneLoadStage::Failed) {
            st.alpha = tr.fade_in > 0.0f ? std::max(0.0f, st.alpha - dt / tr.fade_in) : 0.0f;
        } else if (covered_kind) {
            st.alpha = tr.fade_out > 0.0f ? std::min(1.0f, st.elapsed / tr.fade_out) : 1.0f;
        }
        const bool covered = !covered_kind || st.alpha >= 1.0f;

        if (tr.kind == SceneTransition::Kind::LoadingScreen && st.pending()) {
            if (covered && !st.loading_screen_shown) {
                st.loading_screen_shown = true;
                if (!tr.loading_screen.empty() && !transition_.show_loading_screen(tr.loading_screen)) {
                    std::cerr << "[toyengine] Loading screen '" << tr.loading_screen << "' could not be shown\n";
                }
            }
            transition_.update_loading_screen(st.progress, scene_load_status_text(st.stage));
        }

        if (st.stage == SceneLoadStage::WaitingToSwap && covered && st.elapsed >= st.options.min_display_time) {
            activate_scene_load_(st);
        }

        if (covered_kind) transition_.set_fade(tr.color, st.alpha);
        if (st.stage == SceneLoadStage::FadingIn && st.alpha <= 0.0f) st.stage = SceneLoadStage::Done;
        if (st.finished() && st.alpha <= 0.0f) finish_transition_();
    }

    /**
     * @brief The swap: one GPU wait, the old scenes out (unless additive), the built scene in,
     *        started, systems installed, simulating -- then on_complete.
     */
    void activate_scene_load_(detail::SceneLoadState& st) {
        ctx_.wait_idle();
        transition_.hide_loading_screen();
        std::unique_ptr<coopa::scene::Scene> built = st.builder->take();   // unstarted
        st.builder.reset();
        apply_spawn_point_(*built, st.options.spawn_point);
        coopa::scene::Scene* previous_active = scene_mgr_.has_scene() ? &scene_mgr_.get_active_scene() : nullptr;
        if (!st.options.additive) {
            clear_scenes_();
            previous_active = nullptr;
        }
        coopa::scene::Scene* raw = scene_mgr_.add_scene(std::move(built));
        scene_settings_[raw] = st.settings;
        scene_paths_[raw] = st.path;
        // The same order as load_scene(): start (SceneLoader::load() starts while building),
        // then the per-scene systems, so components that add bodies in start() are seen by them.
        raw->start();
        prepare_scene_(*raw, /*drain_assets=*/false);
        // Additive: the previously active scene stays the active one, with its own settings.
        if (previous_active) {
            scene_mgr_.set_active_scene(previous_active);
            apply_scene_settings_(*previous_active);
        }
        st.stage = st.options.transition.kind == SceneTransition::Kind::None ? SceneLoadStage::Done : SceneLoadStage::FadingIn;
        st.complete(*raw);
    }

    /** @brief Moves the new scene's player (first CharacterController) onto its `spawn_point` object. */
    static void apply_spawn_point_(coopa::scene::Scene& scene, const std::string& spawn_point) {
        if (spawn_point.empty()) return;
        coopa::scene::SceneObject* spawn = scene.find_object(spawn_point);
        auto* player = scene.find_first_component<scene::CharacterController>();
        if (!spawn || !player || !player->owner) {
            std::cerr << "[toyengine] spawn_point '" << spawn_point << "': "
                      << (spawn ? "no CharacterController in the scene" : "no such object") << "\n";
            return;
        }
        auto* stc = spawn->get_transform();
        auto* ptc = player->owner->get_transform();
        if (!stc || !ptc) return;
        const glm::mat4 world = stc->get_world_matrix();
        coopa::util::Transform& t = ptc->transform();
        glm::mat4 parent_inv(1.0f);
        if (player->owner->parent() && player->owner->parent()->get_transform()) {
            parent_inv = glm::inverse(player->owner->parent()->get_transform()->get_world_matrix());
        }
        t.set_position(glm::vec3(parent_inv * world[3]));
        // Facing: the spawn point's yaw (the controller turns about +Z).
        const glm::vec3 fwd = glm::vec3(world[1]);
        if (glm::length(glm::vec2(fwd)) > 1e-4f) {
            glm::vec3 euler = t.rotation_degrees();
            euler.z = glm::degrees(std::atan2(-fwd.x, fwd.y));
            t.set_rotation(euler);
        }
    }

    /** @brief Drops an unactivated async load (its built objects too) and its transition. */
    void cancel_scene_load_(const std::string& why) {
        if (!scene_load_ || !scene_load_->pending()) return;
        scene_load_->fail("cancelled: " + why);
        finish_transition_();
    }

    /** @brief Takes the transition layer down (fade fully gone, loading screen destroyed). */
    void finish_transition_() {
        if (scene_load_ && !scene_load_->finished()) scene_load_->stage = SceneLoadStage::Done;
        if (transition_.has_scene()) {
            transition_.hide_loading_screen();
            transition_.set_fade(glm::vec3(0.0f), 0.0f);
            remove_overlay_layer(&transition_.scene());
        }
        if (scene_load_) scene_load_->alpha = 0.0f;
    }

    /**
     * @brief Turns this frame's SceneLink requests into load_scene_async() calls -- for scenes
     *        this engine runs, and never in an engine embedded by a tool (the editor's play mode
     *        stays in the scene being edited).
     */
    void consume_scene_link_requests_() {
        std::vector<scene::SceneLinkRequest> requests = scene::SceneLinkRequests::instance().take();
        for (const scene::SceneLinkRequest& r : requests) {
            const auto managed = scene_mgr_.scenes();
            if (std::find(managed.begin(), managed.end(), r.origin) == managed.end()) continue;
            if (options_.edit_mode) {
                std::cout << "[toyengine] SceneLink to '" << r.target_scene << "' ignored in an embedded engine (editor play mode)\n";
                continue;
            }
            SceneLoadOptions o;
            const auto kind = SceneTransition::parse_kind(r.transition);
            if (!kind) std::cerr << "[toyengine] SceneLink: unknown transition '" << r.transition << "' (fade, loading_screen, none); fading\n";
            switch (kind.value_or(SceneTransition::Kind::Fade)) {
                case SceneTransition::Kind::None: o.transition = SceneTransition::none(); break;
                case SceneTransition::Kind::Fade: o.transition = SceneTransition::fade(r.fade_time, r.color); break;
                case SceneTransition::Kind::LoadingScreen:
                    o.transition = SceneTransition::loading_screen_ui(r.loading_screen, r.fade_time, r.color);
                    break;
            }
            o.min_display_time = r.min_display_time;
            o.spawn_point = r.spawn_point;
            load_scene_async(r.target_scene, std::move(o));
        }
    }

    /** @brief Hands the pipeline overlay_layers_' scenes, in draw order. */
    void sync_overlay_layers_() {
        std::vector<coopa::scene::Scene*> scenes;
        scenes.reserve(overlay_layers_.size());
        for (const OverlayLayer& l : overlay_layers_) scenes.push_back(l.scene);
        pipeline_->set_overlay_layers(std::move(scenes));
    }

    /** @brief Where a layer's screen canvases go: the display rect, or (nullopt) the window. */
    std::optional<ScreenUiPlacement> overlay_layer_placement_(const OverlayLayer& layer) {
        if (!layer.in_display_rect) return std::nullopt;
        const render::LetterboxRect rect = display_rect();
        if (rect.w == 0 || rect.h == 0) return std::nullopt;
        // At the display's scale, so the HUD keeps a physical size (and crisp text) on HiDPI.
        return ScreenUiPlacement{rect, /*input=*/false, std::max(1.0f, display_scale())};
    }

    /** @brief PROFILE mode's profile, else the overlay's live one, else null. */
    render::FrameProfile* active_profile_() const {
        return profile_ ? profile_.get() : live_profile_.get();
    }

    /**
     * @brief Runs a live FrameProfile (and with it the pipeline's GPU timestamps) exactly
     *        while the overlay is in full mode -- unless PROFILE already runs one, which the
     *        overlay then reads instead. Top of tick() only: CPU timers hold the pointer for
     *        the rest of the frame.
     */
    void sync_live_profile_() {
        const bool want = debug_overlay_.full() && !profile_;
        if (want == (live_profile_ != nullptr)) return;
        if (want) {
            live_profile_ = std::make_unique<render::FrameProfile>();
            pipeline_->set_profiler(live_profile_.get());
        } else {
            pipeline_->set_profiler(nullptr);   // waits for the GPU, then drops its queries
            live_profile_.reset();
        }
    }

    /**
     * @brief The overlay's per-frame upkeep, after input polling: F3, its layer (registered
     *        exactly while it is visible, so "off" ticks and draws nothing), and the frame time.
     *
     * F3 belongs to the running game: an editor in edit mode, or a game without input focus
     * (NO_INPUT, the editor's unfocused play mode), leaves it to the host.
     */
    void update_debug_overlay_() {
        if constexpr (!debug::k_overlay_compiled) return;
        if (!edit_mode_ && !input_blocked_() && input_.is_pressed("debug_overlay", ctx_.input())) {
            debug_overlay_.cycle();
        }
        if (!debug_overlay_.visible()) {
            if (debug_overlay_.has_scene() && !overlay_layers_.empty()) remove_overlay_layer(&debug_overlay_.scene());
            return;
        }
        coopa::scene::Scene& layer = debug_overlay_.scene(debug_overlay_font_());
        const bool registered = std::any_of(overlay_layers_.begin(), overlay_layers_.end(),
                                            [&layer](const OverlayLayer& l) { return l.scene == &layer; });
        // Order 100: over any later engine layer of lower order (a scene transition fades
        // beneath the stats).
        if (!registered) add_overlay_layer(&layer, 100, /*in_display_rect=*/true);
        // Wall-clock, not FIXED_DT: the HUD reports how fast frames really are.
        debug_overlay_.record_frame(ctx_.delta_time() * 1000.0f);
    }

    /**
     * @brief The overlay font, loaded on first use: JetBrains Mono from the checkout (even
     *        digit widths), else the shipped Inter. Never left as uicoopa's default font --
     *        font_for_path() would otherwise make the first font it loads every unthemed
     *        game Text's fallback.
     */
    coopa::ui::Font* debug_overlay_font_() {
        if (debug_overlay_font_path_.empty()) {
            const std::string mono = std::string(PROJ_DIR) + "/uicoopa/assets/fonts/JetBrainsMono-Regular.ttf";
            if (std::filesystem::exists(mono)) debug_overlay_font_path_ = mono;
            for (const std::string& root : asset_roots()) {
                if (!debug_overlay_font_path_.empty()) break;
                const std::filesystem::path p = std::filesystem::path(root) / "fonts" / "inter_regular.ttf";
                if (std::filesystem::exists(p)) debug_overlay_font_path_ = p.string();
            }
            if (debug_overlay_font_path_.empty()) return nullptr;
        }
        coopa::ui::Font* const before = coopa::ui::FontDefaults::font;
        coopa::ui::Font* font = coopa::ui::UIResourceCache::instance().font_for_path(debug_overlay_font_path_);
        if (!before && coopa::ui::FontDefaults::font == font) coopa::ui::FontDefaults::font = nullptr;
        return font;
    }

    /**
     * @brief Fills the overlay's full-mode numbers. Runs only in full mode, every
     *        DebugOverlay::kRefreshFrames frames; the object walk is linear in the scene.
     */
    void collect_debug_stats_() {
        debug::OverlayStats& st = debug_overlay_.stats();
        st = debug::OverlayStats{};
        if (const render::FrameProfile* prof = active_profile_()) {
            if (const render::FrameProfile::Row* row = prof->latest()) st.profile = *row;
        }
        st.render_width  = pipeline_->render_width();
        st.render_height = pipeline_->render_height();
        const auto& draws = pipeline_->last_frame_stats();
        st.draws             = draws.camera_draws;
        st.instances         = draws.camera_instances;
        st.triangles         = draws.camera_triangles;
        st.shadow_draws      = draws.shadow_draws;
        st.shadow_triangles  = draws.shadow_triangles;
        st.renderers         = draws.renderers;
        st.renderers_visible = draws.camera_visible;
        st.pending_assets    = assets_.pending_load_count();
        for (const auto& heap : ctx_.allocator().heap_budgets()) {
            st.heaps.push_back(debug::OverlayStats::Heap{heap.usage, heap.budget, heap.device_local});
        }
        if (!scene_mgr_.has_scene()) return;
        coopa::scene::Scene& scene = scene_mgr_.get_active_scene();

        std::function<void(const coopa::scene::SceneObject&)> count = [&](const coopa::scene::SceneObject& o) {
            ++st.objects;
            st.components += o.components().size();
            for (const auto& c : o.children()) count(*c);
        };
        for (const auto& root : scene.root_objects()) count(*root);
        if (auto* ps = dynamic_cast<particles::ParticleSimulationSystem*>(scene.find_system("Particles"))) {
            st.particles = ps->total_particles();
        }
        if (auto* phys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) {
            st.has_physics = true;
            const auto& world = phys->world();
            world.for_each_body([&](coopa::physx::dynamics::BodyId, const coopa::physx::dynamics::Body& b) {
                ++st.bodies;
                if (b.awake && b.type == coopa::physx::dynamics::BodyType::Dynamic) ++st.bodies_awake;
            });
            st.contacts   = world.manifolds().size();
            st.physics_ms = phys->last_step_ms();
        }
        if (auto* nav = coopa::physx::system::find_nav_system(scene)) {
            st.has_nav    = true;
            st.nav_agents = nav->agent_count();
            st.nav_ms     = nav->stats().total_ms;
            st.nav_paths  = nav->stats().paths_planned;
        }
    }

    /**
     * @brief Hands the renderer this frame's particle batches: every ParticleSystem whose
     *        bounds the main camera can see gets its instances built (sorted back to front,
     *        on the job workers) and its batch queued. The bridge between toyengine/particles/
     *        and render/, which only share the plain data in render/particle_types.h.
     */
    void sync_particle_render_state_(coopa::scene::Scene& scene) {
        particle_frame_.quads.clear();
        particle_frame_.meshes.clear();
        if (auto* sys = dynamic_cast<particles::ParticleSimulationSystem*>(scene.find_system("Particles"))) {
            using coopa::gfx::engine::components::CameraComponent;
            const CameraComponent* cam = CameraComponent::main();
            particles::ParticleView view;
            render::Frustum frustum{};
            if (cam) {
                view.camera_pos = cam->get_world_position();
                const float aspect = static_cast<float>(pipeline_->render_width()) /
                                     static_cast<float>(std::max(1u, pipeline_->render_height()));
                frustum = render::Frustum::from_matrix(cam->get_projection_matrix(aspect) * cam->get_view_matrix());
            }
            sys->collect_render(scene, view, [&](const glm::vec3& lo, const glm::vec3& hi) {
                if (!cam) return true;
                render::WorldBounds b;
                b.center = 0.5f * (lo + hi);
                b.extent = 0.5f * (hi - lo);
                return frustum.intersects(b);
            }, particle_frame_);
        }
        pipeline_->set_particle_state(particle_frame_);
    }

    /** @brief A scene settings block's `weather` mapping (null if it has none). */
    static fkyaml::node weather_node_(const fkyaml::node& settings) {
        if (settings.is_mapping() && settings.contains("weather")) return settings.at("weather");
        return fkyaml::node();
    }

    /** @brief The render-config values the weather overwrites (weather::controlled_render_keys()). */
    struct WeatherRenderBase {
        glm::vec3 zenith{0.0f}, horizon{0.0f}, ground{0.0f};
        float ambient = 1.0f, sky = 1.0f, exposure = 1.0f;
        int fog_mode = 1;
        float fog_density = 0.0f, fog_linear_start = 0.0f, fog_linear_end = 0.0f;
        glm::vec3 fog_color{0.0f};
        float fog_sky_blend = 0.0f, fog_max_opacity = 1.0f, fog_height_falloff = 0.0f, fog_sun_amount = 0.0f;
        float cloud_coverage = 0.0f;

        void capture(const render::PixelRenderConfig& c) {
            cloud_coverage = c.cloud_coverage;
            zenith = c.indirect.sky_zenith; horizon = c.indirect.sky_horizon; ground = c.indirect.sky_ground;
            ambient = c.indirect.ambient_intensity; sky = c.indirect.sky_intensity; exposure = c.exposure;
            fog_mode = c.fog_mode; fog_density = c.fog_density; fog_linear_start = c.fog_linear_start;
            fog_linear_end = c.fog_linear_end; fog_color = c.fog_color; fog_sky_blend = c.fog_sky_blend;
            fog_max_opacity = c.fog_max_opacity; fog_height_falloff = c.fog_height_falloff; fog_sun_amount = c.fog_sun_amount;
        }
        void restore(render::PixelRenderConfig& c) const {
            c.cloud_coverage = cloud_coverage;
            c.indirect.sky_zenith = zenith; c.indirect.sky_horizon = horizon; c.indirect.sky_ground = ground;
            c.indirect.ambient_intensity = ambient; c.indirect.sky_intensity = sky; c.exposure = exposure;
            c.fog_mode = fog_mode; c.fog_density = fog_density; c.fog_linear_start = fog_linear_start;
            c.fog_linear_end = fog_linear_end; c.fog_color = fog_color; c.fog_sky_blend = fog_sky_blend;
            c.fog_max_opacity = fog_max_opacity; c.fog_height_falloff = fog_height_falloff; c.fog_sun_amount = fog_sun_amount;
        }
    };

    /**
     * @brief Writes the active scene's weather atmosphere (sky, ambient, fog) into the live
     *        render config. The first frame it drives, the config's own values are saved; when
     *        the weather stops (disabled, or a scene without it) they are put back.
     */
    void sync_weather_render_state_(coopa::scene::Scene& scene) {
        weather::WeatherSystem* w = weather::find(scene);
        if (!w || !w->enabled() || !w->state().enabled) { restore_weather_atmosphere_(); return; }
        render::PixelRenderConfig& c = pipeline_->render_config_mut();
        if (!weather_applied_) { weather_base_.capture(c); weather_applied_ = true; }
        const weather::Atmosphere& a = w->atmosphere();
        c.indirect.sky_zenith = a.sky_zenith;
        c.indirect.sky_horizon = a.sky_horizon;
        c.indirect.sky_ground = a.sky_ground;
        c.indirect.ambient_intensity = a.ambient_intensity;
        c.indirect.sky_intensity = a.sky_intensity;
        c.fog_mode = 1;   // exponential height fog (the condition's falloff thins it upward)
        c.fog_density = a.fog_density;
        c.fog_color = a.fog_color;
        c.fog_sky_blend = a.fog_sky_blend;
        c.fog_max_opacity = a.fog_max_opacity;
        c.fog_height_falloff = a.fog_height_falloff;
        c.fog_sun_amount = a.fog_sun_amount;
        c.exposure = weather_base_.exposure * a.exposure_scale;   // scaled, not owned: the row stays editable
        c.cloud_coverage = std::clamp(w->state().cloud_cover, 0.0f, 1.0f);
    }

    /**
     * @brief The physical sky (render sky_model: physical): evaluates the atmosphere on the CPU
     *        (weather/atmosphere_model.h) for this frame's sun and hands the renderer its
     *        SkyFrameState. Runs after the weather sync and writes over the gradient colours
     *        (sky_zenith / horizon / ground) with the physical sky's own, so every consumer of the
     *        gradient agrees with the sky drawn. The sun is the weather's while it runs, else the
     *        scene's directional light (the sky follows wherever it points).
     *
     * The colours are not config: without the weather they are captured once (sky_base_) and put
     * back when the physical sky stops; with the weather, its own base (weather_base_) holds them.
     */
    void sync_sky_render_state_(coopa::scene::Scene& scene, float dt) {
        using coopa::gfx::engine::components::DirectionalLightComponent;
        render::PixelRenderConfig& c = pipeline_->render_config_mut();
        weather::WeatherSystem* w = weather::find(scene);
        const bool physical = c.sky_model == "physical";
        if (w) w->set_physical_sky(physical);
        render::SkyFrameState st;
        if (!physical) { restore_sky_colours_(); pipeline_->set_sky_state(st); return; }
        const bool weather_live = w && w->enabled() && w->state().enabled;
        if (weather_live && sky_applied_) {
            // The weather started over our colours and captured them as "the config's": hand it
            // the real ones, and let its base own them from here on.
            weather_base_.zenith = sky_base_[0]; weather_base_.horizon = sky_base_[1]; weather_base_.ground = sky_base_[2];
            sky_applied_ = false;
        } else if (!weather_live && !sky_applied_) {
            sky_base_ = {c.indirect.sky_zenith, c.indirect.sky_horizon, c.indirect.sky_ground};
            sky_applied_ = true;
        }

        // The scene's sun: the same light the renderer picks (the first active one).
        DirectionalLightComponent* light = nullptr;
        for (DirectionalLightComponent* l : scene.get_components<DirectionalLightComponent>()) {
            if (l->owner && l->owner->active()) { light = l; break; }
        }
        glm::vec3 sun_to = glm::normalize(glm::vec3(0.4f, 0.3f, 0.85f));
        if (weather_live) sun_to = w->state().sun_direction;
        else if (light && glm::length(light->direction) > 1e-6f) sun_to = -glm::normalize(light->direction);
        const float coverage = std::clamp(c.cloud_coverage, 0.0f, 1.0f);
        // Sun illuminance the sky is scaled by: the clear-sky sun (the weather's, else the light's own).
        const float sun_ref = weather_live ? w->settings().sun_intensity : (light ? std::max(light->intensity, 0.0f) : 1.2f);

        sky_atmosphere_.set_media(render::SkyAtmosphereMedia::earth(c.atmosphere_density, c.ozone));
        glm::vec3 cam_pos(0.0f);
        if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) cam_pos = cam->get_world_position();
        const float view_r = sky_atmosphere_.view_radius(cam_pos.z);
        const float K = k_sky_gain * sun_ref;
        const weather::SkyGradient g = sky_atmosphere_.gradient(sun_to, k_moon_ratio, view_r);
        const glm::vec3 floor_col = glm::vec3(0.55f, 0.75f, 1.3f) * 1e-3f * K * 0.3f;
        glm::vec3 zenith = g.zenith * K + floor_col, horizon = g.horizon * K + floor_col, ground = g.ground * K + floor_col * 0.5f;
        if (c.clouds && coverage > 0.0f) {
            // Cloud cover greys the dome toward the overcast's flat light, a little darker.
            auto overcast = [&](glm::vec3 col) {
                const float lum = glm::dot(col, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                return glm::mix(col, glm::vec3(lum * (1.0f - 0.45f * coverage)), coverage * 0.85f);
            };
            const glm::vec3 mean = (zenith + horizon) * 0.5f;
            zenith = overcast(glm::mix(zenith, mean, coverage * 0.6f));
            horizon = overcast(glm::mix(horizon, mean, coverage * 0.6f));
            ground = overcast(ground);
        }
        c.indirect.sky_zenith = zenith;
        c.indirect.sky_horizon = horizon;
        c.indirect.sky_ground = ground;

        // The light's colour through the air: transmittance toward it, relative to straight up
        // (so a high sun stays as authored and a low one reddens and fades).
        const glm::vec3 light_to = light && glm::length(light->direction) > 1e-6f ? -glm::normalize(light->direction) : sun_to;
        const glm::vec3 t_up = glm::max(sky_atmosphere_.transmittance(view_r, 1.0f), glm::vec3(1e-4f));
        glm::vec3 tint = sky_atmosphere_.transmittance_toward(view_r, light_to) / t_up;
        if (c.clouds && !weather_live) tint *= glm::mix(1.0f, 0.3f, coverage * coverage);   // the weather's condition dims its own
        st.light_tint = glm::min(tint, glm::vec3(1.0f));

        st.active = true;
        st.clouds = c.clouds;
        st.media = sky_atmosphere_.media();
        st.sun_to = sun_to;
        st.moon_to = -sun_to;
        st.sky_illuminance = K;
        st.moon_ratio = k_moon_ratio;
        st.sun_disc_radiance = c.sun_disc_size > 0.0f ? K * 40.0f : 0.0f;
        st.moon_disc_radiance = c.moon_disc_size > 0.0f ? K * k_moon_ratio * 60.0f : 0.0f;
        st.sun_disc_cos = std::cos(glm::radians(std::max(c.sun_disc_size, 0.0f) * 0.5f));
        st.moon_disc_cos = std::cos(glm::radians(std::max(c.moon_disc_size, 0.0f) * 0.5f));
        const float dark = std::clamp((-0.02f - sun_to.z) / 0.16f, 0.0f, 1.0f);
        st.star_visibility = c.sky_stars ? dark * dark * (3.0f - 2.0f * dark) * (c.clouds ? 1.0f - 0.5f * coverage : 1.0f) : 0.0f;
        st.night_floor = K * 0.3f;

        st.cloud_coverage = coverage;
        st.cloud_altitude = c.cloud_altitude;
        st.cloud_thickness = c.cloud_thickness;
        st.cloud_density = c.cloud_density;
        glm::vec2 wind_dir(1.0f, 0.0f);
        if (weather_live && glm::length(glm::vec2(w->state().wind)) > 0.05f) wind_dir = glm::normalize(glm::vec2(w->state().wind));
        sky_cloud_offset_ += wind_dir * c.cloud_wind_speed * std::max(dt, 0.0f);
        sky_cloud_offset_ = glm::mod(sky_cloud_offset_, glm::vec2(13000.0f * 53.0f));   // whole periods of both shape-map reads (sky_clouds.frag)
        st.cloud_offset = sky_cloud_offset_;
        sky_time_ += std::max(dt, 0.0f);
        st.time = std::fmod(sky_time_, 3600.0f);
        // Clouds are lit by the sun until it is well below the horizon, then by the moon.
        if (sun_to.z > -0.12f) {
            st.cloud_light_to = sun_to;
            // A little softer while the sun is low, so a sunset's lit cloud deck does not drive
            // the exposure meter so hard that the ground (lit at a grazing angle) goes black.
            const float low = std::clamp(sun_to.z / 0.3f, 0.0f, 1.0f);
            st.cloud_light_color = glm::vec3(K * (0.6f + 0.4f * low));
        } else {
            st.cloud_light_to = -sun_to;
            st.cloud_light_color = glm::vec3(0.75f, 0.85f, 1.0f) * K * k_moon_ratio * 0.5f;
        }
        pipeline_->set_sky_state(st);
    }

    /**
     * @brief Hands the renderer this frame's surface world (render/surface_world.h): the lying
     *        snow, wetness and wind from the weather (or render snow_cover_override), the
     *        weather's precipitation map as the "open to the sky" test, and the snow trench
     *        field objects press into.
     */
    void sync_surface_state_(coopa::scene::Scene& scene) {
        render::SurfaceFrameState st;
        const render::PixelRenderConfig& c = pipeline_->render_config();
        weather::WeatherSystem* w = weather::find(scene);
        const bool live = w && w->enabled() && w->state().enabled;
        if (live) {
            const weather::WeatherState& ws = w->state();
            st.snow_cover = ws.snow_cover;
            st.snow_depth = w->settings().snow_max_depth;
            st.snow_patch_hard = w->settings().snow_patch_hard;
            st.snow_patch_size = w->settings().snow_patch_size;
            st.wetness = ws.wetness;
            st.wind = ws.wind;
            if (const auto& field = w->ground_probe().field()) {
                // Aliasing pointer: the heights live as long as the field the renderer holds.
                // The sky layer (moving bodies looked through) where the probe made one.
                st.occl_heights = std::shared_ptr<const std::vector<float>>(
                    field, field->sky_heights.size() == field->heights.size() ? &field->sky_heights : &field->heights);
                st.occl_origin = field->origin;
                st.occl_cell = field->cell;
                st.occl_nx = field->nx;
                st.occl_ny = field->ny;
                st.occl_fallback = field->fallback;
            }
        }
        if (c.snow_cover_override >= 0.0f) st.snow_cover = std::min(c.snow_cover_override, 1.0f);
        if (const world::SnowSystem* snow = world::find_snow(scene); snow && st.snow_cover > 0.0f) {
            const world::SnowField& f = snow->field();
            if (f.focused()) {
                st.trench_words = f.words().data();
                st.trench_n = f.n();
                st.trench_cell = f.cell();
                st.trench_window = f.window();
                st.trench_scale = f.scale();
                st.trench_version = f.version();
            }
        }
        pipeline_->set_surface_state(std::move(st));
    }

    /** @brief Puts back the gradient colours the physical sky overwrote without the weather. */
    void restore_sky_colours_() {
        if (!sky_applied_) return;
        render::PixelRenderConfig& c = pipeline_->render_config_mut();
        c.indirect.sky_zenith = sky_base_[0]; c.indirect.sky_horizon = sky_base_[1]; c.indirect.sky_ground = sky_base_[2];
        sky_applied_ = false;
    }

    void restore_weather_atmosphere_() {
        if (!weather_applied_) return;
        weather_base_.restore(pipeline_->render_config_mut());
        weather_applied_ = false;
    }

    /** @brief render::RenderQuality (config.yaml's water_quality) as the water module's tier. */
    static water::WaterSettings water_settings_for_(render::RenderQuality q) {
        switch (q) {
            case render::RenderQuality::Low:    return water::WaterSettings::from_quality(water::WaterQuality::Low);
            case render::RenderQuality::Medium: return water::WaterSettings::from_quality(water::WaterQuality::Medium);
            case render::RenderQuality::Ultra:  return water::WaterSettings::from_quality(water::WaterQuality::Ultra);
            case render::RenderQuality::High:   break;
        }
        return water::WaterSettings::from_quality(water::WaterQuality::High);
    }

    /**
     * @brief Hands the renderer this frame's water state: the live ripple rings (aged) and, if
     *        the main camera is below a water surface, that body's underwater look. The bridge
     *        between toyengine/water/ and render/, which deliberately don't know each other.
     */
    void sync_water_render_state_(coopa::scene::Scene& scene) {
        render::WaterFrameState state;
        auto* water = dynamic_cast<water::WaterSystem*>(scene.find_system("Water"));
        if (water) {
            // water_quality is live: a changed tier reaches the system for its next update.
            const render::RenderQuality q = pipeline_->render_config().water_quality;
            if (water->settings().quality != water_settings_for_(q).quality) water->set_settings(water_settings_for_(q));
            const water::WaterSettings& ws = water->settings();
            state.ripple_layers   = ws.ripple_layers;
            state.detail_distance = ws.detail_distance;
            state.ripple_range    = ws.ripple_range;

            // Hooks between Scene::update() and here (the editor applying its edits) may have
            // destroyed or rebuilt water objects since the system cached its list.
            water->refresh_bodies(scene);
            const float now = water->time();
            state.time = water->render_time();
            auto* cam = coopa::gfx::engine::components::CameraComponent::main();
            const bool has_eye = cam && cam->owner;
            const glm::vec3 eye = has_eye ? glm::vec3(cam->owner->get_transform()->transform().get_world_matrix()[3])
                                          : glm::vec3(0.0f);
            // Rings within the draw range, nearest first, at most the tier's cap -- then handed
            // over oldest first, the order the pipeline expects.
            std::vector<std::pair<float, std::size_t>> near;
            near.reserve(water->ripples().size());
            for (std::size_t i = 0; i < water->ripples().size(); ++i) {
                const float d = has_eye ? glm::distance(glm::vec2(eye), water->ripples()[i].position) : 0.0f;
                if (d <= ws.ripple_range) near.push_back({d, i});
            }
            if (near.size() > ws.max_ripples) {
                std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(ws.max_ripples), near.end());
                near.resize(ws.max_ripples);
            }
            std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            state.ripples.reserve(near.size());
            for (const auto& [d, i] : near) {
                const auto& r = water->ripples()[i];
                state.ripples.push_back({r.position, now - r.birth, r.strength, r.radius});
            }
            if (has_eye) {
                const water::WaterSystem::UnderwaterInfo info = water->underwater_at(eye);
                // Also just ABOVE the surface (within half a metre): the bottom of the near plane
                // can already be under water, and the pass decides per pixel which side it is on
                // -- that is what splits the image at the waterline.
                const bool near_surface = info.body && info.depth > -0.5f && info.depth <= 0.0f;
                if ((info.underwater || near_surface) && info.body) {
                    state.underwater    = true;
                    state.surface_level = info.surface_height;
                    state.fog_color     = info.body->underwater_color;
                    state.visibility    = info.body->underwater_visibility;
                    state.absorption    = info.body->underwater_absorption;
                    state.caustics      = info.body->caustics;
                }
            }
        }
        pipeline_->set_water_state(std::move(state));
    }

    /**
     * @brief `resolution_mode: fill`: keeps the render target's aspect equal to the display
     *        region's (the window, or an editor viewport panel), so the image fills it instead
     *        of letterboxing. The pipeline's targets are sized at construction, so a new aspect
     *        means rebuilding it -- done once the wanted extent has held for a few frames (a
     *        splitter drag doesn't rebuild every frame; until then the old image is fitted).
     *        Runs at the top of tick(), before any UI is emitted -- see the call site.
     */
    void update_fill_extent_() {
        const render::PixelRenderConfig& cfg = pipeline_->render_config();
        if (cfg.resolution_mode != "fill") return;
        const VkExtent2D sc = ctx_.swapchain().extent();
        if (sc.width == 0 || sc.height == 0) return;
        const auto& region = pipeline_->display_region();
        const float rw = region ? static_cast<float>(region->w) : static_cast<float>(sc.width);
        const float rh = region ? static_cast<float>(region->h) : static_cast<float>(sc.height);
        if (rw < 2 || rh < 2) return;
        render::PixelRenderConfig want = cfg;
        want.fill_aspect = rw / rh;
        const render::RenderExtent e = render::compute_render_extent(want, sc.width, sc.height);
        if (e.width == pipeline_->render_width() && e.height == pipeline_->render_height()) {
            fill_stable_frames_ = 0;
            return;
        }
        if (e.width != fill_pending_.width || e.height != fill_pending_.height) {
            fill_pending_ = e;
            fill_stable_frames_ = 0;
        }
        if (++fill_stable_frames_ < kFillDebounceFrames) return;
        fill_stable_frames_ = 0;
        rebuild_pipeline(want);
    }

    /// Frames a new fill-mode extent must hold before the pipeline is rebuilt for it.
    static constexpr int kFillDebounceFrames = 8;

    /**
     * @brief The render config the config's source document alone asks for -- parsed the way
     *        scene_config() parses a scene's overrides, so the first comparison is like for
     *        like: render fields set on the AppConfig in code (not in its document) never look
     *        like a change to rebuild for.
     */
    render::PixelRenderConfig source_render_config_() const {
        AppConfig d = config_;
        d.render = AppConfig::from_node(config_.source).render;
        if (source_driven_ && config_adjust_) config_adjust_(d);
        return make_render_config_(d, options_.project_root);
    }

    /// Frames a live (editor) change to a construction-fixed render field waits before the
    /// pipeline is rebuilt for it, so a dragged slider rebuilds once it settles.
    static constexpr int kLiveRebuildDebounceFrames = 6;

    /**
     * @brief Queues a pipeline rebuild taking `cfg`'s construction-fixed fields, run by
     *        run_pending_rebuild_() once `delay` more ticks have passed (a newer request
     *        replaces it and restarts the wait). Never rebuilt in place here: this can be
     *        called mid-frame (a script switching scenes, an editor button) and the frame's
     *        UI already holds the pipeline's textures.
     */
    void queue_rebuild_(const render::PixelRenderConfig& from, const render::PixelRenderConfig& to, int delay, bool force = false) {
        if (!pending_rebuild_) rebuild_from_ = from;   // a newer request keeps the first baseline
        pending_rebuild_ = to;
        rebuild_wait_ = delay;
        rebuild_forced_ = rebuild_forced_ || force;
    }

    /**
     * @brief Runs a queued rebuild once its wait is over. The new pipeline takes the live
     *        config (so runtime tweaks and the carried shader/debug state survive) with the
     *        construction-fixed fields the request changed on top -- all of them when forced
     *        (restart_renderer()). A request that ended up changing nothing (switched back
     *        within the debounce) doesn't rebuild.
     */
    void run_pending_rebuild_() {
        if (!pending_rebuild_) return;
        if (rebuild_wait_-- > 0) return;
        render::PixelRenderConfig cfg = pipeline_->render_config();
        const float fill_aspect = cfg.fill_aspect;
        bool changed = rebuild_forced_;
#define TOY_TAKE_QUEUED(field)                                                              \
        if (rebuild_forced_ || rebuild_from_.field != pending_rebuild_->field) {            \
            changed = changed || cfg.field != pending_rebuild_->field;                       \
            cfg.field = pending_rebuild_->field;                                             \
        }
        TOY_STARTUP_FIXED_FIELDS(TOY_TAKE_QUEUED)
#undef TOY_TAKE_QUEUED
        cfg.fill_aspect = fill_aspect;   // the engine's, from the display region
        pending_rebuild_.reset();
        rebuild_forced_ = false;
        if (!changed) return;
        rebuild_pipeline(cfg);
        ++pipeline_rebuilds_;
    }

    /**
     * @brief Rebuilds the render pipeline with `cfg` (e.g. a new render size), carrying over
     *        everything the Engine configured on the old one. Temporal history restarts.
     */
    void rebuild_pipeline(const render::PixelRenderConfig& cfg) {
        ctx_.wait_idle();
        const std::optional<render::LetterboxRect> region = pipeline_->display_region();
        pipeline_.reset();   // free the old targets before allocating the new ones
        pipeline_ = std::make_unique<render::PixelRenderPipeline>(ctx_.device(), ctx_.allocator(), ctx_.swapchain(),
                                                                  ctx_.render_pass(), ctx_.command_pool(), cfg);
        pipeline_->set_job_engine(&jobs_);
        pipeline_->set_parallel_threshold(config_.jobs.parallel_threshold);
        if (render::FrameProfile* prof = active_profile_()) pipeline_->set_profiler(prof);
        pipeline_->set_overlay_scene(overlay_scene_);
        sync_overlay_layers_();
        pipeline_->set_display_region(region);
        if (scene_mgr_.has_scene()) pipeline_->validate_material_shaders(scene_mgr_.get_active_scene());
    }

    /**
     * @brief Rebuilds the renderer in place from the active scene's effective config
     *        (config.yaml plus its overrides) -- every construction-fixed setting takes effect
     *        without closing the window or touching the device, scenes or assets. Runs at the
     *        top of the next tick; safe to call from UI code mid-frame.
     */
    void restart_renderer() {
        const AppConfig eff = scene_mgr_.has_scene() ? scene_config(scene_mgr_.get_active_scene()) : config_;
        render::PixelRenderConfig to = make_render_config_(eff, options_.project_root);
        queue_rebuild_(derived_render_ ? *derived_render_ : to, to, 0, /*force=*/true);
        derived_render_ = to;
    }

    /// True while a renderer rebuild is queued (see restart_renderer(), apply_scene_settings_()).
    bool renderer_rebuild_pending() const { return pending_rebuild_.has_value(); }

    /// How many queued renderer rebuilds have run (tests and diagnostics).
    int pipeline_rebuild_count() const { return pipeline_rebuilds_; }

    /**
     * @brief Delta time for this tick's asset/scene updates: FIXED_DT override if set, else
     *        the real wall-clock ctx_.delta_time(). See run()'s own doc for why this exists.
     */
    float frame_dt_() const {
        return fixed_dt_ >= 0.0f ? fixed_dt_ : ctx_.delta_time();
    }

    coopa::gfx::presentation::Window& window() { return ctx_.window(); }
    coopa::gfx::core::Device&         device() { return ctx_.device(); }
    coopa::gfx::memory::Allocator&    allocator() { return ctx_.allocator(); }
    coopa::gfx::command::CommandPool& command_pool() { return ctx_.command_pool(); }
    coopa::input::InputMap&           input()  { return input_; }
    /**
     * @brief Spawns an object asset (`objects/crate`) into the active scene at `position`,
     *        started and ready -- the runtime half of object assets (see scene_inherit.h).
     * @return The new object, or null if the asset could not be loaded (logged).
     */
    coopa::scene::SceneObject* spawn(const std::string& object_asset, const glm::vec3& position = glm::vec3(0.0f),
                                     coopa::scene::SceneObject* parent = nullptr) {
        if (!scene_mgr_.has_scene()) return nullptr;
        try {
            fkyaml::node overrides = fkyaml::node::mapping();
            fkyaml::node comps = fkyaml::node::sequence();
            fkyaml::node t = fkyaml::node::mapping();
            t["type"] = fkyaml::node(std::string("Transform"));
            fkyaml::node pos = fkyaml::node::mapping();
            pos["x"] = fkyaml::node(static_cast<double>(position.x));
            pos["y"] = fkyaml::node(static_cast<double>(position.y));
            pos["z"] = fkyaml::node(static_cast<double>(position.z));
            t["position"] = pos;
            comps.as_seq().push_back(t);
            overrides["components"] = comps;
            return coopa::scene::SceneLoader::spawn(scene_mgr_.get_active_scene(), object_asset, parent, &overrides);
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] spawn('" << object_asset << "') failed: " << e.what() << "\n";
            return nullptr;
        }
    }

    /// @brief The active scene -- for physics-focused headless tests and gameplay code that
    /// needs to reach a system (e.g. `scene().find_system("Physics")`) or spawn objects.
    coopa::scene::Scene&              scene()  { return scene_mgr_.get_active_scene(); }
    /// @brief The raw per-frame keyboard/mouse state -- edges, deltas, held
    /// time -- for game code that wants more than input()'s named actions.
    coopa::input::Input&              input_state() { return ctx_.input(); }
    float    delta_time() const  { return ctx_.delta_time(); }
    float    elapsed() const     { return static_cast<float>(ctx_.elapsed()); }
    uint64_t frame_count() const { return ctx_.frame_index(); }

private:
    /** @brief Applies validation-layer-on-in-debug-builds to a base ContextConfig, plus ONESHOT/MAX_FRAMES. */
    static coopa::gfx::app::ContextConfig make_context_config_(const AppConfig& config) {
        coopa::gfx::app::ContextConfig cc;
        cc.title      = config.window.title;
        cc.width      = config.window.width;
        cc.height     = config.window.height;
        cc.resizable  = true;
        cc.vsync      = config.window.vsync;
        cc.visible    = config.window.visible;
#ifdef NDEBUG
        cc.validation = false;
#else
        cc.validation = true;
#endif
        // Shipping: no validation and none of gfxcoopa's scripted-run env hooks (MAX_FRAMES,
        // ONESHOT, ...) -- a player's environment must not reconfigure the game.
        if constexpr (k_shipping) {
            cc.validation = false;
            return cc;
        }
        // TOY_VALIDATION=1/0 forces the Khronos layer on/off regardless of build type.
        if (const char* v = std::getenv("TOY_VALIDATION"); v && *v) cc.validation = std::string(v) != "0";
        return coopa::gfx::app::ContextConfig::from_env(cc);
    }

    /**
     * @brief Binds the default action set: quit, fly move (as three axes),
     *        and look (as one vector).
     *
     * Orbit has no keyboard bindings: the mouse drives yaw/pitch and the scroll wheel
     * drives zoom, both read directly in drive_camera_controller_().
     */
    void bind_default_input_() {
        using coopa::input::Key;
        input_.bind("quit", Key::Escape);
        // Cycles the debug stats overlay off -> fps -> full (see update_debug_overlay_()).
        input_.bind("debug_overlay", Key::F3);
        input_.bind("quick_save", Key::F5);   // save.quick_slot (toyengine/save/)
        input_.bind("quick_load", Key::F9);

        input_.bind_axis("fly_x", Key::D, Key::A); // strafe: +right/-left
        input_.bind_axis("fly_y", Key::W, Key::S); // forward/back
        input_.bind_axis("fly_z", Key::E, Key::Q); // world up/down
        input_.bind_vector("look", Key::Right, Key::Left, Key::Up, Key::Down);

        // Object movement gets its OWN axes rather than reusing fly_x/fly_y: those belong to the
        // camera's Fly mode, and a scene with both a fly camera and a driveable object would
        // otherwise move them together on the same keypress.
        input_.bind_axis("move_x", Key::D, Key::A); // world +X / -X
        input_.bind_axis("move_y", Key::W, Key::S); // world +Y / -Y
        // Vertical, for FreeMover only -- KinematicController is planar by design and ignores it.
        // Tab/Shift rather than the more usual Space/Shift because Space is left free for a jump
        // action, which is the binding a character controller will want first.
        input_.bind_axis("move_z", Key::Tab, Key::LeftShift); // world +Z (up) / -Z (down)

        // CharacterController: WASD as one camera-relative vector, Space to jump, LeftShift to
        // sprint. `sprint` is its own action so move_z's binding above is untouched.
        input_.bind_vector("move", Key::D, Key::A, Key::W, Key::S);
        input_.bind("jump", Key::Space);
        input_.bind("sprint", Key::LeftShift);
        // Ragdoll (input_toggle): R goes limp / recovers.
        input_.bind("ragdoll", Key::R);
    }

    /**
     * @brief Pushes this frame's mouse/scroll/keyboard state into the active
     *        scene's CameraController (if any) before Scene::update() consumes it.
     *
     * Mouse motion and scroll drive Orbit mode; WASD/E/Q + arrow keys drive
     * Fly mode -- both read unconditionally since only one mode is ever
     * active on a given controller and the unused fields are simply ignored.
     *
     * NO_INPUT=1 (env var, read once at construction like FIXED_DT/CAPTURE_FRAMES)
     * zeroes every field instead, so headless captures aren't perturbed by
     * whatever the real mouse/keyboard happen to be doing during the run --
     * without it, FIXED_DT alone can't make an interactive-camera capture
     * reproducible.
     */
    /** @brief Gameplay drivers push zero input: NO_INPUT=1, or a host took focus away. */
    bool input_blocked_() const { return no_input_ || !game_focused_; }

    void drive_camera_controller_(coopa::scene::Scene& scene) {
        auto* cc = scene.find_first_component<scene::CameraController>();
        if (!cc) return;

        if (input_blocked_()) {
            cc->mouse_delta  = glm::vec2(0.0f);
            cc->scroll_input = 0.0f;
            cc->move_input   = glm::vec3(0.0f);
            cc->look_input   = glm::vec2(0.0f);
            return;
        }

        // coopa::input::Input already suppresses the spurious first-frame jump
        // GLFW's virtual cursor reports right when CursorMode::Disabled is
        // first applied -- see Input::push_cursor_position()'s doc.
        cc->mouse_delta = ctx_.input().cursor_delta();
        cc->scroll_input = ctx_.input().scroll_delta().y;

        cc->move_input = glm::vec3(
            input_.axis("fly_x", ctx_.input()),
            input_.axis("fly_y", ctx_.input()),
            input_.axis("fly_z", ctx_.input()));
        cc->look_input = input_.vector("look", ctx_.input());
    }

    /**
     * @brief Pushes this frame's movement keys into every KinematicController in the active scene,
     *        before Scene::update() consumes them.
     *
     * Same push-model contract as drive_camera_controller_() -- a component that never reads
     * coopa::input::Input stays testable with no live window, and NO_INPUT=1 zeroes the whole
     * thing so a headless capture is not perturbed by whatever the real keyboard is doing.
     *
     * Every controller in the scene gets the same vector, not just the first: multiple
     * simultaneously-driven objects is a legitimate (if unusual) authoring choice, and it costs
     * nothing to support.
     */
    void drive_kinematic_controllers_(coopa::scene::Scene& scene) {
        std::vector<scene::KinematicController*> controllers =
            scene.get_components<scene::KinematicController>();
        if (controllers.empty()) return;

        const glm::vec2 move = input_blocked_()
            ? glm::vec2(0.0f)
            : glm::vec2(input_.axis("move_x", ctx_.input()), input_.axis("move_y", ctx_.input()));
        for (scene::KinematicController* kc : controllers) kc->move_input = move;
    }

    /**
     * @brief Pushes this frame's movement keys into every FreeMover in the active scene, before
     *        Scene::update() consumes them.
     *
     * The three-axis counterpart to drive_kinematic_controllers_() above, sharing its move_x/
     * move_y axes and adding move_z -- so one keypress drives a planar kinematic body and a free
     * 3D marker the same way, which is what makes them feel like one control scheme rather than
     * two. Same push-model and NO_INPUT=1 contract as every other driver here.
     */
    void drive_free_movers_(coopa::scene::Scene& scene) {
        std::vector<scene::FreeMover*> movers = scene.get_components<scene::FreeMover>();
        if (movers.empty()) return;

        const glm::vec3 move = input_blocked_()
            ? glm::vec3(0.0f)
            : glm::vec3(input_.axis("move_x", ctx_.input()),
                        input_.axis("move_y", ctx_.input()),
                        input_.axis("move_z", ctx_.input()));
        for (scene::FreeMover* fm : movers) fm->move_input = move;
    }

    /**
     * @brief Pushes this frame's move/jump/sprint input into every CharacterController in the
     *        active scene, before Scene::update() consumes it.
     *
     * The move vector is made CAMERA-relative by handing the main camera's yaw over as the
     * controller's move basis: W walks the way the camera looks (flattened onto the ground).
     * `jump` is OR-ed in, not overwritten -- the controller consumes (clears) a press in its
     * next advance(), so a press is never lost to a frame that didn't advance. Same push-model and
     * NO_INPUT=1 contract as the other drivers.
     */
    void drive_character_controllers_(coopa::scene::Scene& scene) {
        std::vector<scene::CharacterController*> characters = scene.get_components<scene::CharacterController>();
        if (characters.empty()) return;

        float yaw = 0.0f;
        if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) {
            const glm::mat4 world = glm::inverse(cam->get_view_matrix());
            glm::vec3 fwd = -glm::vec3(world[2]);            // cameras look down local -Z
            if (std::abs(fwd.z) > 0.99f) fwd = glm::vec3(world[1]); // looking straight down: screen-up
            if (glm::length(glm::vec2(fwd.x, fwd.y)) > 1e-4f) yaw = glm::degrees(std::atan2(-fwd.x, fwd.y));
        }
        const bool blocked = input_blocked_();
        const glm::vec2 move = blocked ? glm::vec2(0.0f) : input_.vector("move", ctx_.input());
        const bool jump = !blocked && input_.is_pressed("jump", ctx_.input());
        const bool sprint = !blocked && input_.is_down("sprint", ctx_.input());
        for (scene::CharacterController* cc : characters) {
            cc->move_input = move;
            cc->move_basis_yaw_deg = yaw;
            cc->jump = cc->jump || jump;
            cc->sprint = sprint;
        }
    }

    /** @brief Pushes the "ragdoll" press (R) into every Ragdoll with `input_toggle` -- same
     *         push model and NO_INPUT=1 contract as the drivers above. */
    void drive_ragdolls_(coopa::scene::Scene& scene) {
        if (input_blocked_() || !input_.is_pressed("ragdoll", ctx_.input())) return;
        for (scene::Ragdoll* r : scene.get_components<scene::Ragdoll>()) {
            if (r->input_toggle) r->toggle();
        }
    }

    /**
     * @brief Refreshes and uploads every CPU-simulated mesh in the scene (today: cloth).
     *
     * Called between Scene::late_update() and PixelRenderPipeline::render(), which is the only
     * correct window and the reason this is an Engine step rather than a Component::late_update():
     *
     *   - It must come after UpdatePhase::Physics (100) and TransformResolve (350), so the cloth
     *     particles and the owner's world matrix are both current for this frame.
     *   - It must come before the frame's command buffer is recorded, since it writes the vertex
     *     buffer that recording will bind.
     *   - It needs ctx_.current_frame(), the in-flight slot -- which a Component has no way to
     *     know, and which is exactly what keeps the write off the buffer the GPU is still reading
     *     (this pipeline never waits per frame; see debug_line_pass.h's file doc).
     */
    void upload_dynamic_meshes_(coopa::scene::Scene& scene) {
        for (scene::ClothRenderer* cr : scene.get_components<scene::ClothRenderer>()) {
            cr->upload(ctx_.current_frame());
        }
        // Same per-frame-in-flight contract as ClothRenderer above. With GPU skinning each
        // upload() only builds its palette and queues a dispatch that the pipeline records at
        // the top of this frame's command buffer; otherwise it CPU-skins and uploads.
        render::passes::SkinningPass* gpu_skin = pipeline_ ? pipeline_->skinning_pass() : nullptr;
        if (gpu_skin) gpu_skin->begin_frame();
        for (scene::SkinnedMeshRenderer* smr : scene.get_components<scene::SkinnedMeshRenderer>()) {
            smr->upload(ctx_.current_frame(), gpu_skin);
        }
    }

    /**
     * @brief Fills pipeline_->debug_lines() from the active scene's PhysicsSystem, when
     *        config_.render.debug_view == "lines" -- the physxcoopa <-> toy::render
     *        bridge debug_line_pass.h's file doc describes: the render layer's DebugLine and
     *        pack_gpu_color() know nothing about physics, so this is the one place a
     *        coopa::physx::debug::DebugLine gets translated into one.
     *
     * Physics lines need debug_view "lines"; navigation lines (the scene's NavSystem, when its
     * `navigation.debug_draw` is set) show in every view. Always clears first, so switching
     * either off at runtime never leaves last frame's overlay stuck.
     */
    void gather_debug_lines_(coopa::scene::Scene& scene) {
        std::vector<render::DebugLine>& out = pipeline_->debug_lines();
        out.clear();
        // An embedding host (the editor's grid, gizmos, wireframe) appends after this, from
        // FrameHooks::pre_render -- which runs after this clear, so it always wins the frame.
        coopa::physx::debug::DebugDraw draw;
        // Navigation's overlay is opt-in per scene (`navigation.debug_draw`) and shows in every
        // debug_view: it is how a nav scene is read at all (walkable outline, paths, flow arrows).
        if (auto* nav = coopa::physx::system::find_nav_system(scene)) {
            nav->debug_draw(draw, nav->settings().debug_draw);
        }
        auto* sys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
        if (sys && render::parse_debug_view(config_.render.debug_view) == render::DebugView::Lines) {
            sys->world().debug_draw(draw, config_.physics.debug_draw);
        }
        out.reserve(draw.lines.size());
        for (const auto& line : draw.lines) {
            out.push_back(render::DebugLine{line.a, line.b, render::pack_gpu_color(line.color)});
        }
    }

    /**
     * @brief Feeds every CanvasComponent in the scene the per-frame state it cannot get for
     *        itself: the viewport (screen-space canvases) or the camera and pointer ray
     *        (world-space ones).
     *
     * Called between Scene::update() and Scene::late_update() -- see tick() for why that
     * window is the only correct one.
     *
     * The two kinds live in DIFFERENT spaces, and each is given the one its pass draws in.
     * World-space UI is projected through the camera, so its layout and its pointer picking both
     * happen in the internal render extent -- build_pointer_ray_() is what maps the window-pixel
     * cursor through the letterbox into that space. (Its layer is RASTERIZED at the letterbox
     * rect; see PixelRenderPipeline's ui_world_target_. That is a rendering resolution, not a
     * coordinate space, and does not enter here.) Screen-space UI renders last, into the
     * swapchain-sized overlay target at full window resolution, so it is sized to the window and
     * the raw window-pixel cursor is already in its space.
     *
     * Getting that split wrong is not cosmetic: CanvasComponent::set_input() divides the
     * cursor by the scale_factor set_viewport() derived, so sizing a screen canvas to the
     * render extent while feeding it window pixels puts every hit test off by the upscale
     * factor plus the letterbox offset.
     *
     * `view`/`proj` here are recomputed from the main camera rather than taken from the
     * pipeline, which has not run yet this frame. They are used for TWO different things
     * with different tolerances: the pointer ray (sub-pixel accuracy is irrelevant to
     * picking) and CameraFacing's billboard basis (a rotation, which TAA jitter and
     * camera_pixel_snap do not affect at all, since both perturb only the projection's
     * translation). The canvas-to-clip matrix the UI is actually RASTERIZED with is formed
     * inside the pipeline from its own authoritative jittered projection -- see
     * UiWorldPass::draw()'s view_proj parameter -- so the UI and the depth buffer it is
     * compared against never disagree.
     */
    void drive_ui_canvases_(coopa::scene::Scene& scene, const std::optional<ScreenUiPlacement>& placement) {
        std::vector<coopa::ui::CanvasComponent*> canvases = coopa::ui::collect_canvases(scene);
        if (canvases.empty()) return;
        // A scene that is drawn but not simulating (the editor's edit mode) never runs a
        // canvas's late_update(), which is where layout and emit happen -- so without this
        // every HUD and nameplate would be invisible while editing.
        const bool preview = !scene.is_simulating();

        const uint32_t rw = pipeline_->render_width();
        const uint32_t rh = pipeline_->render_height();
        const uint32_t ww = ctx_.swapchain().extent().width;
        const uint32_t wh = ctx_.swapchain().extent().height;
        const coopa::gfx::TextureView white        = pipeline_->world_ui_white_view();
        const coopa::gfx::TextureView screen_white = pipeline_->screen_ui_white_view();

        auto* cam = scene.find_first_component<coopa::gfx::engine::components::CameraComponent>();
        const float aspect = rh > 0 ? static_cast<float>(rw) / static_cast<float>(rh) : 1.0f;
        const glm::mat4 view = cam ? cam->get_view_matrix() : glm::mat4(1.0f);
        const glm::mat4 proj = cam ? cam->get_projection_matrix(aspect) : glm::mat4(1.0f);

        bool ray_valid = false;
        glm::vec3 ray_origin(0.0f);
        glm::vec3 ray_dir(0.0f);
        for (coopa::ui::CanvasComponent* canvas : canvases) {
            if (!canvas->is_world_space()) {
                // Same seeding as the world path below, and for the same reason: without it
                // a screen-space canvas emits every solid-colour quad against a null view.
                canvas->set_default_texture(screen_white);
                if (placement && placement->rect.w > 0 && placement->rect.h > 0) {
                    // Placed inside part of the window (see set_scene_ui_placement()).
                    const float zoom = placement->zoom > 0.0f ? placement->zoom : 1.0f;
                    canvas->set_viewport(std::max(1u, static_cast<uint32_t>(std::lround(placement->rect.w / zoom))),
                                         std::max(1u, static_cast<uint32_t>(std::lround(placement->rect.h / zoom))));
                    canvas->set_screen_origin(glm::vec2(placement->rect.x, placement->rect.y));
                    canvas->set_display_zoom(zoom);
                    canvas->set_input_enabled(placement->input);
                } else {
                    // The WINDOW extent, not the render extent: UiPass draws this canvas into the
                    // swapchain-sized overlay target, and ctx_.input()'s cursor is in window
                    // pixels, so this is the sizing that makes both agree. See the doc above.
                    canvas->set_viewport(ww, wh);
                    canvas->set_screen_origin(glm::vec2(0.0f));
                    canvas->set_display_zoom(1.0f);
                    canvas->set_input_enabled(true);
                }
                // Cursor positions arrive in screen points; the canvas is sized in framebuffer
                // pixels. They differ on HiDPI displays.
                canvas->set_input(ctx_.input(), cursor_scale());
                if (preview) canvas->preview_refresh();
                continue;
            }
            // Seed the DrawList's default texture BEFORE late_update() emits against it:
            // every solid-colour quad (panels, bar fills) is drawn with this view, and an
            // unseeded DrawList submits draws bound to a null image view. Idempotent and
            // cheap, and done per frame rather than once so a scene loaded later is covered.
            canvas->set_default_texture(white);
            canvas->update_world_transform(view);
            if (!ray_valid) {
                ray_valid = build_pointer_ray_(view, proj, rw, rh, ray_origin, ray_dir);
            }
            std::optional<glm::vec2> hit =
                ray_valid ? canvas->ray_to_canvas(ray_origin, ray_dir) : std::nullopt;
            // A miss must land far OUTSIDE the canvas. (0, 0) would be the canvas's own
            // bottom-left corner, which would leave whatever widget sits there permanently
            // hovered whenever the pointer is anywhere else.
            canvas->set_world_input(ctx_.input(), hit ? *hit : glm::vec2(-1.0e6f));
            if (preview) canvas->preview_refresh();
        }
    }

    /**
     * @brief Builds a world-space ray through the cursor, for world-space UI picking.
     *
     * Three coordinate hops, each of which has a way to be subtly wrong:
     *  1. Window pixels -> internal render-extent pixels. The low-res image is blitted into
     *     the window through a letterbox rect, so this must go through the SAME
     *     compute_display_rect() the upscale itself uses -- it dispatches on
     *     config.upscale_mode, which compute_fit()/compute_letterbox() alone would ignore.
     *  2. Render-extent pixels -> NDC. The world UI is rasterized through a NEGATIVE-height
     *     viewport, so framebuffer row 0 is ndc_y = +1: `ndc_y = 1 - 2*y/h`, the opposite
     *     sign of the usual Vulkan relation.
     *  3. NDC -> world, by unprojecting the near and far plane points. z = 0 is the near
     *     plane because GLM_FORCE_DEPTH_ZERO_TO_ONE is set (on coopa::lib, for every
     *     consumer), not because of anything this function does.
     *
     * @return False when the letterbox rect is degenerate (a zero-area window), in which
     *         case there is no meaningful ray and no canvas should report a hit.
     */
    bool build_pointer_ray_(const glm::mat4& view, const glm::mat4& proj,
                            uint32_t rw, uint32_t rh,
                            glm::vec3& out_origin, glm::vec3& out_dir) {
        if (rw == 0 || rh == 0) return false;
        render::LetterboxRect box = display_rect();
        if (box.w == 0 || box.h == 0) return false;

        glm::vec2 cursor = ctx_.input().cursor_position() * cursor_scale() - glm::vec2(box.x, box.y);
        // Divide by box.w/h rather than box.scale: under "fit" mode w/h are rounded to whole
        // pixels, so w/rw and scale differ slightly, and w/h is what the blit actually used.
        glm::vec2 px(cursor.x * static_cast<float>(rw) / static_cast<float>(box.w),
                     cursor.y * static_cast<float>(rh) / static_cast<float>(box.h));
        glm::vec2 ndc(2.0f * px.x / static_cast<float>(rw) - 1.0f,
                      1.0f - 2.0f * px.y / static_cast<float>(rh));

        glm::mat4 inv_vp = glm::inverse(proj * view);
        glm::vec4 near_h = inv_vp * glm::vec4(ndc, 0.0f, 1.0f);
        glm::vec4 far_h  = inv_vp * glm::vec4(ndc, 1.0f, 1.0f);
        if (std::abs(near_h.w) < 1e-9f || std::abs(far_h.w) < 1e-9f) return false;
        out_origin = glm::vec3(near_h) / near_h.w;
        // Left unnormalized on purpose: ray_to_canvas() only ever uses ratios of dot
        // products, so scale cancels -- and for an orthographic camera the near->far
        // difference IS the (constant) direction.
        out_dir = glm::vec3(far_h) / far_h.w - out_origin;
        return true;
    }

    /**
     * @brief Puts the OS cursor into disabled (hidden + unbounded) mode if the
     *        active scene's CameraController wants it -- called once after
     *        the initial scene load.
     *
     * There is no in-app control to release the cursor once captured; quitting
     * (Escape, still bound) is the only way out. A scene author can opt out
     * entirely via `capture_cursor: false` on the CameraController.
     *
     * Two non-interactive modes stand down regardless of what the scene asks for, because
     * capturing the pointer means GLFW_CURSOR_DISABLED -- a real, process-wide pointer grab
     * that hides and re-centres the cursor of whoever happens to be using the desktop:
     *   - NO_INPUT=1, which already means "this run must ignore the real keyboard and mouse"
     *     (see drive_camera_controller_()); grabbing input it then throws away is pure harm.
     *   - an invisible window (`window.visible: false`), which has no on-screen presence to
     *     justify owning the pointer in the first place.
     * Both are what lets the headless test suite render the same scenes a person plays,
     * without stealing their mouse for the duration.
     */
    void apply_cursor_capture_() {
        if (no_input_ || !config_.window.visible || edit_mode_) return;
        if (!scene_mgr_.has_scene()) return;
        auto* cc = scene_mgr_.get_active_scene().find_first_component<scene::CameraController>();
        if (cc && cc->capture_cursor) {
            ctx_.input().set_cursor_mode(coopa::input::CursorMode::Disabled);
        }
    }

    /**
     * @brief Sets the toyengine logo (rasterized from branding.h at the usual icon sizes) as the
     *        window's taskbar icon -- or, on macOS, the Dock icon.
     */
    void apply_app_icon_() {
        static const int kSizes[] = {16, 32, 48, 64, 128, 256, 512};
        std::vector<std::vector<uint8_t>> pixels;
        std::vector<coopa::gfx::presentation::Window::IconImage> images;
        for (int s : kSizes) pixels.push_back(rasterize_logo(s));
        for (size_t i = 0; i < pixels.size(); ++i) images.push_back({kSizes[i], kSizes[i], pixels[i].data()});
        ctx_.window().set_icon(images);
    }

    /** @brief Resolves a config-relative asset path against the project root, unless already absolute. */
    std::string resolve_path_(const std::string& path) const {
        return resolve_against_(options_.project_root, path);
    }

    /**
     * @brief resolve_path_() for a scene file, falling back to a lookup by name: a scene in tag
     *        folders (assets/scenes/tests/fog_test/scene.yaml) still loads from the shorthand
     *        assets/scenes/fog_test/scene.yaml (SCENE=fog_test, `toyengine fog_test`). See
     *        coopa::asset::AssetIndex.
     */
    std::string resolve_scene_path_(const std::string& path) const {
        const std::string resolved = resolve_path_(path);
        if (path.empty() || coopa::yaml::document_exists(resolved)) return resolved;
        std::string rel = std::filesystem::path(path).generic_string();
        if (rel.rfind("assets/", 0) == 0) rel = rel.substr(7);
        if (auto found = coopa::asset::AssetIndex::find_in(asset_roots(), rel)) return found->string();
        return resolved;
    }

    static std::string resolve_against_(const std::filesystem::path& root, const std::string& path) {
        if (path.empty() || std::filesystem::path(path).is_absolute()) return path;
        return (root / path).string();
    }

    /**
     * @brief Where a relative output file (output.filepath) goes: as given (cwd-relative) when
     *        running from source, under the per-user data directory when packaged -- an .app's
     *        cwd is "/" and its own folder may be read-only.
     */
    static std::string output_path_(const std::string& path) {
        if (path.empty() || std::filesystem::path(path).is_absolute() || !RuntimeLayout::current().packaged()) {
            return path;
        }
        return (user_data_dir() / path).string();
    }

    /**
     * @brief resolve_against_(), with the engine checkout as the fallback layer: a file the
     *        project doesn't have (a game project's config.yaml starts as a copy of the engine's,
     *        whose `palette:` names assets/palettes/...) resolves to the engine's copy if that
     *        exists. Same rule as the asset search roots (asset_roots()).
     */
    static std::string resolve_asset_file_(const std::filesystem::path& root, const std::string& path) {
        const std::string in_project = resolve_against_(root, path);
        if (in_project.empty() || std::filesystem::path(path).is_absolute()) return in_project;
        std::error_code ec;
        if (std::filesystem::exists(in_project, ec)) return in_project;
        // engine_assets is <checkout>/assets; `path` is project-relative (assets/palettes/...).
        const std::filesystem::path& engine_assets = RuntimeLayout::current().engine_assets;
        if (engine_assets.empty()) return in_project;   // packaged: nothing outside the package
        const std::filesystem::path in_engine = engine_assets.parent_path() / path;
        return std::filesystem::exists(in_engine, ec) ? in_engine.string() : in_project;
    }

    /**
     * @brief Pumps AssetManager until every load issued during scene load has been finalized.
     *
     * Mesh decode runs on jobs_'s worker threads, so a scene's mesh YAML parses overlap
     * instead of serializing at parse time -- but frame 0 must still see a fully loaded
     * scene, or an ONESHOT/CAPTURE_FRAMES capture would show an empty or partial image.
     * Reports any mesh that failed to decode, since MeshRenderer's parser cannot check
     * is_failed() synchronously while loading is async.
     */
    void drain_pending_assets_() {
        while (assets_.pending_load_count() > 0) {
            assets_.update(0.0f);
            std::this_thread::yield();
        }

        if (!scene_mgr_.has_scene()) return;
        for (auto* mr : scene_mgr_.get_active_scene()
                             .get_components<coopa::gfx::engine::components::MeshRenderer>()) {
            const auto& handle = mr->get_mesh();
            if (handle.is_failed()) {
                std::cerr << "[toyengine] Failed to load mesh '" << mr->mesh_path()
                          << "': " << handle.error() << std::endl;
            }
        }
    }

    /** @brief FIXED_DT env override for frame_dt_() -- unset (or unparsable) means -1, i.e. off. */
    static float fixed_dt_from_env_() {
        if (const char* v = debug_env("FIXED_DT")) return std::strtof(v, nullptr);
        return -1.0f;
    }

    /** @brief CAPTURE_FRAMES env override for run()'s sequence capture -- 0 means off. */
    static uint32_t capture_frames_from_env_() {
        if (const char* v = debug_env("CAPTURE_FRAMES")) return static_cast<uint32_t>(std::atoll(v));
        return 0;
    }

    /** @brief CAPTURE_RING env override for run()'s in-memory rolling capture -- 0 means off. */
    static uint32_t capture_ring_from_env_() {
        if (const char* v = debug_env("CAPTURE_RING")) return static_cast<uint32_t>(std::atoll(v));
        return 0;
    }

    /**
     * @brief CURSOR_POS="<x>,<y>" env override: pins the pointer to a fixed window-pixel
     *        position every frame.
     *
     * Mirrors uicoopa's own demo convention of the same name. Its reason to exist is
     * world-space UI: hit testing there runs a mouse ray through the letterbox rect and
     * against a canvas plane, and none of that is exercisable from a headless capture, where
     * the OS cursor never moves off (0, 0). With this, a scripted run can park the pointer on
     * a world-space Button and a screenshot shows its hover state.
     *
     * Re-applied every tick, before Input's per-frame edge detection is consumed, so it wins
     * over any real pointer motion on a live desktop.
     */
    void apply_cursor_pos_override_() {
        if (!cursor_pos_override_) return;
        ctx_.input().push_cursor_position(cursor_pos_.x, cursor_pos_.y);
    }

    /** @brief Parses CURSOR_POS once at construction; see apply_cursor_pos_override_(). */
    void read_cursor_pos_override_() {
        const char* v = debug_env("CURSOR_POS");
        if (!v || !*v) return;
        float x = 0.0f, y = 0.0f;
        if (std::sscanf(v, "%f,%f", &x, &y) == 2) {
            cursor_pos_ = glm::vec2(x, y);
            cursor_pos_override_ = true;
        }
    }

    /**
     * @brief SCENE env override for the scene loaded at startup.
     *
     * Accepts either a bare scene NAME under assets/scenes (`SCENE=world_canvas_test`, which
     * expands to assets/scenes/<name>/scene.yaml, the layout every scene in this repo uses)
     * or an explicit path to a .yaml/.caml. Matches the ONESHOT/MAX_FRAMES/FIXED_DT/CAPTURE_FRAMES/
     * NO_INPUT family: a scripted or one-off run should not have to edit assets/config.yaml,
     * which is version-controlled and describes the DEFAULT scene.
     *
     * @param configured The config file's own scene.default_scene, returned unchanged when
     *                   SCENE is unset or empty.
     */
    static std::string scene_path_from_env_(const std::string& configured) {
        const char* v = debug_env("SCENE");
        if (!v || !*v) return configured;
        std::string scene(v);
        if (coopa::yaml::is_document_ext(scene)) return scene;
        return "assets/scenes/" + scene + "/scene.yaml";
    }

    bool      cursor_pos_override_ = false;
    glm::vec2 cursor_pos_{0.0f};

    /** @brief NO_INPUT env override for drive_camera_controller_() -- any non-empty value that
     *  isn't "0" suppresses all camera input, for reproducible headless captures. */
    /**
     * @brief PROFILE env override: "1" (or any non-path value like "on"/"true") profiles to
     *        output/profile.csv; anything else ending in .csv is used as the path. Unset, empty
     *        or "0" = off.
     */
    static std::string profile_path_from_env_() {
        const char* v = debug_env("PROFILE");
        if (!v || !*v || std::string(v) == "0") return {};
        const std::string s(v);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".csv") == 0) return s;
        return "output/profile.csv";
    }

    static bool no_input_from_env_() {
        const char* v = debug_env("NO_INPUT");
        return v && *v && std::string(v) != "0";
    }

    /** @brief Copies AppConfig's render section into a PixelRenderConfig with shader_dir/palette_path resolved. */
    static render::PixelRenderConfig make_render_config_(const AppConfig& config,
                                                         const std::filesystem::path& project_root) {
        render::PixelRenderConfig rc = config.render;
        // From source: a project's own assets/shaders in front of everything (first-match-wins,
        // so a project shader replaces the engine's of the same name -- see
        // cmake/ToyProject.cmake), then the engine's, gfxcoopa's (SMAA + shared gfx/ headers)
        // and uicoopa's UI shaders -- the runtime mirror of the glslc -I search order
        // (assets/shaders/.glslc_flags). Those three library directories hold no same-named
        // files, so their relative order only matters for a project override. Packaged: the
        // one directory the packager merged those layers into, in that same precedence. See
        // RuntimeLayout.
        const std::vector<std::string> shader_roots = RuntimeLayout::current().shader_roots(project_root);
        rc.shader_dir = RuntimeLayout::current().packaged()
            ? shader_roots.front()
            : (RuntimeLayout::current().engine_assets / "shaders").string();
        rc.shaders = coopa::gfx::pipeline::ShaderLibrary(shader_roots);
        if (!rc.palette_path.empty()) rc.palette_path = resolve_asset_file_(project_root, rc.palette_path);
        if (!rc.grading_lut_path.empty()) rc.grading_lut_path = resolve_asset_file_(project_root, rc.grading_lut_path);

        // Derived surface shaders this app ships -- see gfx/surface/*.glsl. A scene material
        // opts in via `shader: <name>` (see PBRMaterial::shader); a material that never sets it
        // is completely unaffected by this registry existing.
        rc.surface_shaders.add({
            /* name  */ "foliage",
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
            /* vert  */ "foliage.vert",
            /* frag  */ "",  // reuses stock gbuffer.frag -- CUTOUT needs no fragment override
            /* shadow_vert */ "foliage_shadow.vert",
            /* shadow_frag */ "", // reuses stock shadow_depth.frag
            /* shadow_cube_vert */ "foliage_shadow_cube.vert",
            /* shadow_cube_frag */ "", // reuses stock shadow_cube.frag
            /* cull */ coopa::gfx::CullMode::None, // two-sided card, not a closed opaque solid
            /* tesc */ "",
            /* tese */ "foliage.tese",
            /* shadow_tese */ "foliage_shadow.tese",
            /* shadow_cube_tese */ "foliage_shadow_cube.tese",
        });
        rc.surface_shaders.add({
            /* name  */ "terrain",
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
            /* vert  */ "",  // stock gbuffer.vert -- only the UV decode differs
            /* frag  */ "terrain.frag", // tile-space UVs from the greedy chunk mesher
            /* shadow_vert */ "", // stock: an opaque caster never reads UVs in the shadow pass
            /* shadow_frag */ "",
            /* shadow_cube_vert */ "",
            /* shadow_cube_frag */ "",
            /* cull */ coopa::gfx::CullMode::Back, // closed columns, like any opaque solid
        });
        rc.surface_shaders.add({
            /* name  */ "terrain_styled",
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
            /* vert  */ "",  // stock gbuffer.vert
            /* frag  */ "terrain_styled.frag", // kind colour from the atlas, procedural world-space detail
            /* shadow_vert */ "",
            /* shadow_frag */ "",
            /* shadow_cube_vert */ "",
            /* shadow_cube_frag */ "",
            /* cull */ coopa::gfx::CullMode::Back,
        });
        rc.surface_shaders.add({
            /* name  */ "triplanar",
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
            /* vert  */ "triplanar.vert", // object-space position/normal for local mode
            /* frag  */ "triplanar.frag", // world/object-space triplanar maps -- see its doc
            /* shadow_vert */ "", // stock: no displacement, and an opaque caster reads no maps
            /* shadow_frag */ "",
            /* shadow_cube_vert */ "",
            /* shadow_cube_frag */ "",
            /* cull */ coopa::gfx::CullMode::Back,
            /* tesc */ "",
            /* tese */ "triplanar.tese",
        });
        rc.surface_shaders.add({
            /* name  */ "editor_paint",   // the toyeditor's Vertex / Weight Paint display -- see
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,   // editor_paint.vert
            /* vert  */ "editor_paint.vert",   // colour packed in the uv / tangent.w of an
            /* frag  */ "editor_paint.frag",   // editor-built preview mesh; nothing else uses it
            /* shadow_vert */ "",
            /* shadow_frag */ "",
            /* shadow_cube_vert */ "",
            /* shadow_cube_frag */ "",
            /* cull */ coopa::gfx::CullMode::Back,
        });
        rc.surface_shaders.add({
            /* name  */ "snow",   // deep snow: raised by the weather's cover, carved by SnowDeformers
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,   // -- see snow_surface.glsl
            /* vert  */ "snow.vert",
            /* frag  */ "snow.frag",
            /* shadow_vert */ "snow_shadow.vert",   // the raised snow casts the shadow it is drawn with
            /* shadow_frag */ "",
            /* shadow_cube_vert */ "snow_shadow_cube.vert",
            /* shadow_cube_frag */ "",
            /* cull */ coopa::gfx::CullMode::Back,
            /* tesc */ "",
            /* tese */ "snow.tese",
            /* shadow_tese */ "snow_shadow.tese",
            /* shadow_cube_tese */ "snow_shadow_cube.tese",
        });
        rc.surface_shaders.add({
            /* name  */ "water",
            /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Transparent,
            /* vert  */ "water.vert",
            /* frag  */ "water.frag",
            /* shadow_vert */ "", // Transparent domain -- unused (BLEND only shadows at alpha==1.0,
            /* shadow_frag */ "", //   and this demo's water is always translucent -- see water_surface.glsl)
            /* shadow_cube_vert */ "",
            /* shadow_cube_frag */ "",
            /* cull */ coopa::gfx::CullMode::None, // two-sided: a camera under the surface
                                                   // sees its underside (Snell's window) --
                                                   // see water_surface.glsl.
            /* tesc */ "water.tesc", // wider cull margin for the waves' reach
            /* tese */ "water.tese",
        });

        return rc;
    }

    EngineOptions options_;
    AppConfig     config_;
    /// Each managed scene's `scene.settings` overrides (null: none) -- see scene_config().
    std::unordered_map<const coopa::scene::Scene*, fkyaml::node> scene_settings_;
    bool overrides_applied_ = false;   ///< The live render config carries some scene's overrides.
    bool weather_applied_ = false;     ///< The live render config carries the weather's (or the physical sky's) atmosphere.
    weather::AtmosphereModel sky_atmosphere_;   ///< The physical sky on the CPU (sync_sky_render_state_()).
    bool sky_applied_ = false;                  ///< sky_base_ holds the gradient colours the physical sky overwrote.
    std::array<glm::vec3, 3> sky_base_{};       ///< The config's zenith / horizon / ground under the physical sky.
    glm::vec2 sky_cloud_offset_{0.0f};          ///< Accumulated cloud drift, metres.
    float sky_time_ = 0.0f;                     ///< Seconds the physical sky has run.
    /// Sky illuminance per unit of sun light intensity. Brighter than a real sky (about 1) to keep
    /// the engine's ambient-rich look; the sun disc, stars and the CPU gradient all scale with it.
    static constexpr float k_sky_gain = 3.0f;
    /// Moon illuminance / sun illuminance for the moonlit sky (artistic: real is ~1/400000).
    static constexpr float k_moon_ratio = 0.025f;
    WeatherRenderBase weather_base_;   ///< The config's own values under it (see sync_weather_render_state_()).
    bool source_driven_ = false;       ///< set_config_source() was called (the editor).
    std::function<void(AppConfig&)> config_adjust_;   ///< set_config_source()'s adjust hook.

    // jobs_ is declared (and constructed) before ctx_/pipeline_/assets_/scene_mgr_, and
    // destroyed after all of them, since every one of those may still be submitting to or
    // waiting on it up through their own destruction.
    coopa::job::JobEngine jobs_;

    // ctx_ is declared before pipeline_ (and constructed first, destroyed
    // last) since pipeline_ holds references into ctx_'s owned objects.
    coopa::gfx::app::Context    ctx_;
    /// Owned by pointer so `resolution_mode: fill` can rebuild it at a new render size (see
    /// update_fill_extent_()); its render targets and bind-once descriptors are sized at
    /// construction.
    std::unique_ptr<render::PixelRenderPipeline> pipeline_;
    render::RenderExtent fill_pending_{};      ///< fill mode: the extent waiting out the debounce
    int                  fill_stable_frames_ = 0;
    std::optional<render::PixelRenderConfig> pending_rebuild_;   ///< see queue_rebuild_()
    render::PixelRenderConfig rebuild_from_;    ///< the queued rebuild's baseline: only fields changed from it apply
    /// The render config the config document (+ scene overrides) last asked for; the baseline a
    /// change is measured against (unset until the first apply -- see source_render_config_()).
    std::optional<render::PixelRenderConfig> derived_render_;
    int                  rebuild_wait_ = 0;
    bool                 rebuild_forced_ = false;   ///< restart_renderer(): rebuild even if nothing differs
    int                  pipeline_rebuilds_ = 0;

    coopa::asset::AssetManager assets_;
    /// Declared before scene_mgr_ so it outlives every scene (an AudioSource stops its voice
    /// on destruction). Created in init_audio_().
    std::unique_ptr<audio::AudioSystem> audio_;
    uint32_t settings_flush_frames_ = 0;
    coopa::scene::SceneManager scene_mgr_;

    coopa::input::InputMap input_;

    // Deterministic sequence-capture support -- see run()'s own doc. Read once at construction
    // (env vars don't change mid-run); -1.0f / 0 are their respective "off" values.
    float    fixed_dt_       = -1.0f;
    uint32_t capture_frames_ = 0;
    uint32_t capture_ring_   = 0;
    bool     no_input_       = false;
    bool     game_focused_   = true;    ///< See set_game_input_focus().

    /// Profiling mode's sink (PROFILE env var); null when off. Declared last so it outlives
    /// nothing that records into it -- pipeline_'s GpuProfiler is reset in the destructor.
    std::unique_ptr<render::FrameProfile> profile_;
    /// The debug overlay's full mode times frames into this live (CSV-less) profile when
    /// PROFILE is not already on; null otherwise. See sync_live_profile_().
    std::unique_ptr<render::FrameProfile> live_profile_;
    uint64_t profile_frame_ = 0;

    std::vector<OverlayLayer> overlay_layers_;   ///< See add_overlay_layer(); in draw order.
    /// See debug_overlay(). Its scene is one of overlay_layers_ while it is visible.
    debug::DebugOverlay debug_overlay_;
    std::string         debug_overlay_font_path_;   ///< Resolved once, on first show.

    bool                  edit_mode_     = false;
    std::optional<ScreenUiPlacement> scene_ui_placement_;   ///< See set_scene_ui_placement().
    FrameHooks            hooks_;
    std::vector<std::function<void(coopa::input::Input&)>> input_queue_;
    coopa::scene::Scene*  overlay_scene_ = nullptr;
    /// The current (or last) load_scene_async() request; see update_scene_load_().
    std::shared_ptr<detail::SceneLoadState> scene_load_;
    /// The async load's fade / loading screen; an overlay layer while a transition shows.
    SceneTransitionLayer  transition_;
    /// Every managed scene's file (see scene_path()); kept beside scene_settings_.
    std::unordered_map<const coopa::scene::Scene*, std::string> scene_paths_;
    /// Save slots (see saves()); SaveSystem::active() while this engine lives.
    save::SaveSystem saves_;
    /// This frame's particle batches (see sync_particle_render_state_()); kept to reuse capacity.
    render::ParticleFrameState particle_frame_;
};


} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_ENGINE_H
