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
#include <coopa/asset/asset_manager.h>
#include <coopa/input/input_map.h>
#include <coopa/job/engine.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_manager.h>
#include <coopa/scene/systems/transform_system.h>

#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/debug/debug_draw.h>

#include <toyengine/core/branding.h>
#include <toyengine/core/config.h>
#include <toyengine/core/runtime_paths.h>
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
#include <toyengine/world/terrain_system.h>
#include <toyengine/water/water_system.h>
#include <toyengine/core/module.h>

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
        // Project modules (TOY_MODULE in a project's src/, see module.h) last, so they can
        // build on -- or replace -- any parser registered above.
        for (const Module& m : modules()) {
            if (m.on_engine_init) m.on_engine_init(*this);
        }

        if (options_.load_default_scene) {
            load_scene(resolve_path_(scene_path_from_env_(config_.scene.default_scene)));
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
        const std::string resolved = resolve_path_(path);
        scene_mgr_.load_scene(resolved);
        scene_settings_[&scene_mgr_.get_active_scene()] = read_scene_settings_(resolved);
        prepare_scene_(scene_mgr_.get_active_scene());
        return scene_mgr_.get_active_scene();
    }

    /**
     * @brief Replaces every managed scene with an already-built one (e.g. from
     *        SceneLoader::load_from_node()) and sets it up exactly as load_scene() does.
     * @param settings The scene document's `scene.settings` (its config overrides -- see
     *                 AppConfig::with_scene_settings()); null for none.
     */
    coopa::scene::Scene& set_scene(coopa::scene::Scene&& scene, const fkyaml::node& settings = fkyaml::node()) {
        ctx_.wait_idle();
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
        if (scene_mgr_.has_scene()) apply_scene_settings_(scene_mgr_.get_active_scene());
    }

    /** @brief Sets a managed scene's overrides (its `scene.settings`), applying them if it is active. */
    void set_scene_settings(coopa::scene::Scene& scene, const fkyaml::node& settings) {
        scene_settings_[&scene] = settings;
        if (scene_mgr_.has_scene() && &scene_mgr_.get_active_scene() == &scene) apply_scene_settings_(scene);
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
     */
    AppConfig apply_scene_settings_(const coopa::scene::Scene& scene) {
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
            pipeline_->apply_live_config(next);
        }
        overrides_applied_ = overrides;
        return eff;
    }

    /**
     * @brief Everything a freshly loaded scene needs before its first frame: per-scene
     *        systems, drained asset loads, shader validation, edit-mode and cursor state.
     */
    void prepare_scene_(coopa::scene::Scene& scene) {
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

        // Activate TransformSystem before the first drain or render, so the world_matrix()
        // reads below are never asked to resolve a still-dirty transform; then block until
        // every load issued above has finished, so frame 0 sees a fully populated scene.
        coopa::scene::install_transform_system(scene);
        // Runs at UpdatePhase::Animation (300), before TransformResolve (350) -- Scene::update()
        // orders installed systems by phase regardless of install call order, so this only needs
        // to exist before the scene starts ticking, same as install_transform_system() above.
        coopa::anim::install_animation_system(scene);
        drain_pending_assets_();

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
     * The in-memory half of save_screenshot() (which is now this plus a PNG write), for a
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
        if (profile_) {
            profile_->begin_frame(profile_frame_++);
            profile_->prune();
        }
        render::FrameProfile* prof = profile_.get();

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
        }
        {
            CpuTimer t(prof, CpuScope::Assets);
            assets_.update(dt);
        }
        if (hooks_.pre_scene_update) hooks_.pre_scene_update(dt);

        {
            CpuTimer t(prof, CpuScope::SceneUpdate);
            if (scene_mgr_.has_scene() && !edit_mode_) {
                drive_camera_controller_(scene_mgr_.get_active_scene());
                drive_kinematic_controllers_(scene_mgr_.get_active_scene());
                drive_free_movers_(scene_mgr_.get_active_scene());
            }
            scene_mgr_.update(dt);
            if (overlay_scene_) overlay_scene_->update(dt);
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
            // late_update() runs LateBehaviourSystem, flushes each worker's deferred
            // SceneCommandBuffer and advances Scene::frame_index(). Must precede render() so a
            // same-frame deferred spawn or destroy is reflected in what is drawn, matching
            // Unity's Update -> LateUpdate -> render order.
            scene_mgr_.late_update(dt);
            if (overlay_scene_) overlay_scene_->late_update(dt);
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
            update_fill_extent_();
            sync_water_render_state_(scene_mgr_.get_active_scene());
            CpuTimer t(prof, CpuScope::Render);
            pipeline_->render(ctx_.renderer(), scene_mgr_.get_active_scene(), dt);
        }

        return !ctx_.should_close();
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
        if (profile_) pipeline_->set_profiler(profile_.get());
        pipeline_->set_overlay_scene(overlay_scene_);
        pipeline_->set_display_region(region);
        if (scene_mgr_.has_scene()) pipeline_->validate_material_shaders(scene_mgr_.get_active_scene());
    }

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
        // Same per-frame-in-flight re-upload contract as ClothRenderer above, driving a
        // CPU skin instead of a cloth solver -- see skinned_mesh_renderer.h's file doc.
        for (scene::SkinnedMeshRenderer* smr : scene.get_components<scene::SkinnedMeshRenderer>()) {
            smr->upload(ctx_.current_frame());
        }
    }

    /**
     * @brief Fills pipeline_->debug_lines() from the active scene's PhysicsSystem, when
     *        config_.render.debug_view == "lines" -- the physxcoopa <-> toy::render
     *        bridge debug_line_pass.h's file doc describes: the render layer's DebugLine and
     *        pack_gpu_color() know nothing about physics, so this is the one place a
     *        coopa::physx::debug::DebugLine gets translated into one.
     *
     * No-op (and clears any stale lines) when a different debug_view is active or when no
     * "Physics" system is installed, so switching debug_view at runtime never leaves last
     * frame's overlay stuck.
     */
    void gather_debug_lines_(coopa::scene::Scene& scene) {
        std::vector<render::DebugLine>& out = pipeline_->debug_lines();
        out.clear();
        // An embedding host (the editor's grid, gizmos, wireframe) appends after this, from
        // FrameHooks::pre_render -- which runs after this clear, so it always wins the frame.
        if (render::parse_debug_view(config_.render.debug_view) != render::DebugView::Lines) return;

        auto* sys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
        if (!sys) return;

        coopa::physx::debug::DebugDraw draw;
        sys->world().debug_draw(draw, config_.physics.debug_draw);
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
        // so a project shader shadows the engine's of the same name -- see cmake/ToyProject.cmake),
        // then the engine's, gfxcoopa's shared base library, and uicoopa's UI shaders LAST so a
        // uicoopa file can never shadow a gfxcoopa base shader -- the runtime mirror of the glslc
        // -I search order (assets/shaders/.glslc_flags). Packaged: the one directory the
        // packager merged those layers into, in that same precedence. See RuntimeLayout.
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
            /* capture_frag */ "", // Opaque domain -- unused
            /* cull */ coopa::gfx::CullMode::None, // two-sided card, not a closed opaque solid
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
            /* capture_frag */ "", // Opaque domain -- unused
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
            /* capture_frag */ "", // Opaque domain -- unused
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
            /* capture_frag */ "", // Opaque domain -- unused
            /* cull */ coopa::gfx::CullMode::Back,
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
            /* capture_frag */ "", // Opaque domain -- unused
            /* cull */ coopa::gfx::CullMode::Back,
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
            /* capture_frag */ "water_capture.frag", // same hook, over the capture backbone --
                                                      // see TransparentCapturePass::add_variant()
            /* cull */ coopa::gfx::CullMode::None, // two-sided: a camera under the surface
                                                   // sees its underside (Snell's window) --
                                                   // see water_surface.glsl. The SSR capture
                                                   // keeps back-face culling regardless.
        });

        return rc;
    }

    EngineOptions options_;
    AppConfig     config_;
    /// Each managed scene's `scene.settings` overrides (null: none) -- see scene_config().
    std::unordered_map<const coopa::scene::Scene*, fkyaml::node> scene_settings_;
    bool overrides_applied_ = false;   ///< The live render config carries some scene's overrides.
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
    uint64_t profile_frame_ = 0;

    bool                  edit_mode_     = false;
    std::optional<ScreenUiPlacement> scene_ui_placement_;   ///< See set_scene_ui_placement().
    FrameHooks            hooks_;
    std::vector<std::function<void(coopa::input::Input&)>> input_queue_;
    coopa::scene::Scene*  overlay_scene_ = nullptr;
};


} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_ENGINE_H
