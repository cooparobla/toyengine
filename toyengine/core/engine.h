/**
 * @file engine.h
 * @brief Owns the engine lifetime: window, Vulkan objects, assets, scene, and the render loop.
 *
 * Constructs a shared coopa::job::JobEngine first, so it outlives every subsystem that
 * submits to it, then a gfx::app::Context (which owns the Window -> Instance -> Surface ->
 * Device -> Allocator -> Swapchain -> CommandPool -> RenderPass -> Renderer bring-up chain,
 * plus frame timing and resize handling), then layers ToyRenderPipeline, AssetManager and
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
#include <sfxcoopa/sfx_yaml.h>
#include <uicoopa/audio/audio_yaml.h>
#include <uicoopa/audio/sound_library.h>
#include <toyengine/render/toy_render_config.h>
#include <toyengine/render/frame_profile.h>
#include <toyengine/render/particle_types.h>
#include <toyengine/render/toy_render_math.h>
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
namespace render { class ToyRenderPipeline; }
namespace audio { class AudioSystem; }

namespace core {

/**
 * @struct EngineOptions
 * @brief How an Engine is embedded: by the game (the defaults) or by a tool like the editor.
 */
struct EngineOptions {
    /// Directory holding the project's assets/ folder. Relative scene/palette/LUT paths in
    /// the config resolve against it, and <project_root>/assets is the first asset search
    /// root (the engine checkout's assets/ is the fallback under it). Empty means the
    /// TOY_PROJECT_DIR env var, else the project this binary was built for (core::build_project_root(),
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
    Engine(AppConfig config, EngineOptions options);

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
    coopa::scene::Scene& load_scene(const std::string& path);

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
    SceneLoadHandle load_scene_async(const std::string& path, SceneLoadOptions options = {});

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
    coopa::scene::Scene& set_scene(coopa::scene::Scene&& scene, const fkyaml::node& settings = fkyaml::node());

    /**
     * @brief Adds an already-built scene alongside the current ones and makes it active,
     *        set up like load_scene(). The previously active scene keeps its state but stops
     *        updating until reactivated with activate_scene().
     *
     * The editor's play mode and material preview use this: the edit scene stays intact
     * underneath and comes back untouched when the added scene is removed.
     */
    coopa::scene::Scene& push_scene(coopa::scene::Scene&& scene, bool simulating,
                                    const fkyaml::node& settings = fkyaml::node());

    /** @brief Destroys a scene added by push_scene() (or any managed scene). */
    void remove_scene(coopa::scene::Scene* scene);

    /** @brief Makes a managed scene the active, updating one; every other scene stops updating. */
    void activate_scene(coopa::scene::Scene* scene);

    // --- Scene settings (per-scene config overrides) -----------------------------------
    //
    // A scene file may override config.yaml's render and physics keys under `scene.settings`
    // (see AppConfig::with_scene_settings()). Whichever scene is active runs with config.yaml
    // plus its own overrides: render settings are applied live whenever the active scene
    // changes, physics settings when the scene's physics system is installed. Startup-fixed
    // render keys (see ToyRenderPipeline::apply_live_config()) cannot vary per scene.

    /**
     * @brief Replaces the config document scene overrides are layered on -- the editor hands
     *        over its edited (possibly unsaved) config.yaml. From then on render and physics are
     *        always re-derived from this document, overrides or not. Re-applies to the active
     *        scene now.
     * @param adjust Runs on every config derived from the document, e.g. the editor's own
     *               render overrides, so a re-derivation never undoes them.
     */
    void set_config_source(const fkyaml::node& config_yaml, std::function<void(AppConfig&)> adjust = {});

    /**
     * @brief Sets a managed scene's overrides (its `scene.settings`), applying them if it is
     *        active. Its `weather` block goes to the scene's WeatherSystem live (the clock and
     *        the active condition keep running -- see WeatherSystem::set_settings()).
     */
    void set_scene_settings(coopa::scene::Scene& scene, const fkyaml::node& settings);

    /// The active scene's weather and time of day (toyengine/weather/), or null without a scene.
    weather::WeatherSystem* weather() { return scene_mgr_.has_scene() ? weather::find(scene_mgr_.get_active_scene()) : nullptr; }

    /// Save slots and the hooks that fill them (toyengine/save/save_system.h).
    save::SaveSystem& saves() { return saves_; }

    /**
     * @brief The file a managed scene was loaded from (resolved, as load_scene() /
     *        load_scene_async() opened it); "" for one built in memory (set_scene, push_scene).
     */
    std::string scene_path(const coopa::scene::Scene& scene) const;

    /** @brief The config `scene` runs with: config.yaml plus its overrides. */
    AppConfig scene_config(const coopa::scene::Scene& scene) const;

    /**
     * @brief Edit mode on/off. In edit mode the active scene does not simulate (only systems
     *        with ISceneSystem::runs_in_edit_mode() run), gameplay input drivers stand down,
     *        and the cursor is never captured.
     */
    void set_edit_mode(bool edit);
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
    void set_game_input_focus(bool focused);
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
    void set_overlay_scene(coopa::scene::Scene* scene);

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
    void add_overlay_layer(coopa::scene::Scene* scene, int order = 0, bool in_display_rect = false);
    void remove_overlay_layer(coopa::scene::Scene* scene);
    /** @brief The current overlay layers' scenes, in draw order. */
    std::vector<coopa::scene::Scene*> overlay_layers() const;

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
    void set_display_region(std::optional<render::LetterboxRect> region);

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
    render::LetterboxRect display_rect() const;

    /**
     * @brief World-space ray through a window-pixel position, through the current display
     *        rect and the main camera. Direction is normalized.
     * @return False when there is no camera or the position is outside the display rect.
     */
    bool viewport_ray(const glm::vec2& window_px, glm::vec3& out_origin, glm::vec3& out_dir) const;

    /**
     * @brief Projects a world point to window pixels through the main camera and display rect.
     * @return False if there is no camera or the point is behind it.
     */
    bool world_to_window(const glm::vec3& world, glm::vec2& out_px) const;

    // --- Subsystem access, for embedding hosts (the editor) and tests ---
    render::ToyRenderPipeline&  pipeline()      { return *pipeline_; }
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
    std::vector<std::string> asset_roots() const;
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
    static EngineOptions normalize_options_(EngineOptions o);

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
    void init_audio_();

    /**
     * @brief The save system: config.yaml's `save:` keys, and the host hooks it takes the
     *        scene from and loads saved scenes through (load_scene_async(), never in an engine
     *        embedded by a tool -- the editor's play mode applies saves in place).
     */
    void init_saves_();

    /** @brief A scene file as saves store it: relative to the project root when inside it. */
    std::string save_scene_ref_(const std::string& path) const;

    /** @brief Per frame: play time while the game runs, and the quick-save / quick-load keys. */
    void update_saves_(float dt);

    void shutdown_audio_();

    /** @brief Per frame: the listener follows an AudioListener, else the main camera. */
    void update_audio_(float dt);

    /** @brief Destroys every managed scene. */
    void clear_scenes_();

    /** @brief A scene file's `scene.settings` block (null if it has none or can't be read). */
    static fkyaml::node read_scene_settings_(const std::string& path);

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
    AppConfig apply_scene_settings_(const coopa::scene::Scene& scene, int rebuild_delay = 0);

    /**
     * @brief Everything a freshly loaded scene needs before its first frame: per-scene
     *        systems, drained asset loads, shader validation, edit-mode and cursor state.
     */
    void prepare_scene_(coopa::scene::Scene& scene, bool drain_assets = true);

public:

    ~Engine();

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
    void run();

    /**
     * @brief MUTABLE access to the live render config, for changing parameters at runtime.
     *
     * Forwards to ToyRenderPipeline::render_config_mut() -- see that method for which
     * fields take effect immediately (most of them, including every fog parameter) and
     * which are startup-fixed and will not.
     *
     * @code
     * engine.render_config().fog_density = 0.12f;   // visible next frame
     * @endcode
     */
    render::ToyRenderConfig& render_config();

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
    void set_cursor_override(const glm::vec2& window_pixels);

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
    coopa::gfx::util::ImageData capture_image(bool low_res = true);

    /**
     * @brief Writes the current offscreen buffer to a PNG.
     * @param path    Destination file path.
     * @param low_res True writes the internal low-resolution buffer 1:1 (pixel-perfect, but
     *                BEFORE any display-resolution effect such as tilt shift, and containing
     *                NO UI -- neither canvas layer is part of it, since both composite later
     *                and at window resolution; see ToyRenderPipeline::low_res_color_image()).
     *                False writes the final image actually shown in the window, UI included
     *                (ToyRenderPipeline::final_color_image()), at DISPLAY resolution.
     */
    void save_screenshot(const std::string& path, bool low_res = true);

    /**
     * @brief Writes the just-rendered frame to output/seq/frame_%04d.png, for CAPTURE_FRAMES.
     * @param index 0-based sequence index; formatted into the filename.
     */
    void capture_sequence_frame_(uint32_t index);

    /**
     * @brief Advances and renders exactly one frame.
     * @return False when the loop should stop (window close requested).
     */
    bool tick();

    /**
     * @brief Advances the async load one frame: polls the document, builds a batch of objects,
     *        tracks the asset loads, drives the transition and activates when everything is in.
     */
    void update_scene_load_(float dt);

    /**
     * @brief The swap: one GPU wait, the old scenes out (unless additive), the built scene in,
     *        started, systems installed, simulating -- then on_complete.
     */
    void activate_scene_load_(detail::SceneLoadState& st);

    /** @brief Moves the new scene's player (first CharacterController) onto its `spawn_point` object. */
    static void apply_spawn_point_(coopa::scene::Scene& scene, const std::string& spawn_point);

    /** @brief Drops an unactivated async load (its built objects too) and its transition. */
    void cancel_scene_load_(const std::string& why);

    /** @brief Takes the transition layer down (fade fully gone, loading screen destroyed). */
    void finish_transition_();

    /**
     * @brief Turns this frame's SceneLink requests into load_scene_async() calls -- for scenes
     *        this engine runs, and never in an engine embedded by a tool (the editor's play mode
     *        stays in the scene being edited).
     */
    void consume_scene_link_requests_();

    /** @brief Hands the pipeline overlay_layers_' scenes, in draw order. */
    void sync_overlay_layers_();

    /** @brief Where a layer's screen canvases go: the display rect, or (nullopt) the window. */
    std::optional<ScreenUiPlacement> overlay_layer_placement_(const OverlayLayer& layer);

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
    void sync_live_profile_();

    /**
     * @brief The overlay's per-frame upkeep, after input polling: F3, its layer (registered
     *        exactly while it is visible, so "off" ticks and draws nothing), and the frame time.
     *
     * F3 belongs to the running game: an editor in edit mode, or a game without input focus
     * (NO_INPUT, the editor's unfocused play mode), leaves it to the host.
     */
    void update_debug_overlay_();

    /**
     * @brief The overlay font, loaded on first use: JetBrains Mono from the checkout (even
     *        digit widths), else the shipped Inter. Never left as uicoopa's default font --
     *        font_for_path() would otherwise make the first font it loads every unthemed
     *        game Text's fallback.
     */
    coopa::ui::Font* debug_overlay_font_();

    /**
     * @brief Fills the overlay's full-mode numbers. Runs only in full mode, every
     *        DebugOverlay::kRefreshFrames frames; the object walk is linear in the scene.
     */
    void collect_debug_stats_();

    /**
     * @brief Hands the renderer this frame's particle batches: every ParticleSystem whose
     *        bounds the main camera can see gets its instances built (sorted back to front,
     *        on the job workers) and its batch queued. The bridge between toyengine/particles/
     *        and render/, which only share the plain data in render/particle_types.h.
     */
    void sync_particle_render_state_(coopa::scene::Scene& scene);

    /** @brief A scene settings block's `weather` mapping (null if it has none). */
    static fkyaml::node weather_node_(const fkyaml::node& settings);

    /** @brief The render-config values the weather overwrites (weather::controlled_render_keys()). */
    struct WeatherRenderBase {
        glm::vec3 zenith{0.0f}, horizon{0.0f}, ground{0.0f};
        float ambient = 1.0f, sky = 1.0f, exposure = 1.0f;
        int fog_mode = 1;
        float fog_density = 0.0f, fog_linear_start = 0.0f, fog_linear_end = 0.0f;
        glm::vec3 fog_color{0.0f};
        float fog_sky_blend = 0.0f, fog_max_opacity = 1.0f, fog_height_falloff = 0.0f, fog_sun_amount = 0.0f;
        float cloud_coverage = 0.0f;

        void capture(const render::ToyRenderConfig& c);
        void restore(render::ToyRenderConfig& c) const;
    };

    /**
     * @brief Writes the active scene's weather atmosphere (sky, ambient, fog) into the live
     *        render config. The first frame it drives, the config's own values are saved; when
     *        the weather stops (disabled, or a scene without it) they are put back.
     */
    void sync_weather_render_state_(coopa::scene::Scene& scene);

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
    void sync_sky_render_state_(coopa::scene::Scene& scene, float dt);

    /**
     * @brief The cloud layer (render clouds) for this frame, with either sky model: the config's
     *        look, the weather-driven coverage, the wind drift and the light the clouds are lit by.
     *        Under the physical sky (`sky` set) that is the sun above the atmosphere, coloured on
     *        the GPU by the transmittance table, handing over to the moon at night. Under the
     *        gradient it is the scene's directional light as the weather (or the scene) set it --
     *        already the moon's at night and warm at sunset -- and the gradient's colours light
     *        the clouds' ambient in the shader.
     */
    void sync_cloud_state_(coopa::scene::Scene& scene, const render::SkyFrameState* sky);

    /**
     * @brief Hands the renderer this frame's surface world (render/surface_world.h): the lying
     *        snow, wetness and wind from the weather (or render snow_cover_override), the
     *        weather's precipitation map as the "open to the sky" test, and the snow trench
     *        field objects press into.
     */
    void sync_surface_state_(coopa::scene::Scene& scene);

    /** @brief Puts back the gradient colours the physical sky overwrote without the weather. */
    void restore_sky_colours_();

    void restore_weather_atmosphere_();

    /** @brief render::RenderQuality (config.yaml's water_quality) as the water module's tier. */
    static water::WaterSettings water_settings_for_(render::RenderQuality q);

    /**
     * @brief Hands the renderer this frame's water state: the live ripple rings (aged) and, if
     *        the main camera is below a water surface, that body's underwater look. The bridge
     *        between toyengine/water/ and render/, which deliberately don't know each other.
     */
    void sync_water_render_state_(coopa::scene::Scene& scene);

    /**
     * @brief `resolution_mode: fill`: keeps the render target's aspect equal to the display
     *        region's (the window, or an editor viewport panel), so the image fills it instead
     *        of letterboxing. The pipeline's targets are sized at construction, so a new aspect
     *        means rebuilding it -- done once the wanted extent has held for a few frames (a
     *        splitter drag doesn't rebuild every frame; until then the old image is fitted).
     *        Runs at the top of tick(), before any UI is emitted -- see the call site.
     */
    void update_fill_extent_();

    /// Frames a new fill-mode extent must hold before the pipeline is rebuilt for it.
    static constexpr int kFillDebounceFrames = 8;

    /**
     * @brief The render config the config's source document alone asks for -- parsed the way
     *        scene_config() parses a scene's overrides, so the first comparison is like for
     *        like: render fields set on the AppConfig in code (not in its document) never look
     *        like a change to rebuild for.
     */
    render::ToyRenderConfig source_render_config_() const;

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
    void queue_rebuild_(const render::ToyRenderConfig& from, const render::ToyRenderConfig& to, int delay, bool force = false);

    /**
     * @brief Runs a queued rebuild once its wait is over. The new pipeline takes the live
     *        config (so runtime tweaks and the carried shader/debug state survive) with the
     *        construction-fixed fields the request changed on top -- all of them when forced
     *        (restart_renderer()). A request that ended up changing nothing (switched back
     *        within the debounce) doesn't rebuild.
     */
    void run_pending_rebuild_();

    /**
     * @brief Rebuilds the render pipeline with `cfg` (e.g. a new render size), carrying over
     *        everything the Engine configured on the old one. Temporal history restarts.
     */
    void rebuild_pipeline(const render::ToyRenderConfig& cfg);

    /**
     * @brief Rebuilds the renderer in place from the active scene's effective config
     *        (config.yaml plus its overrides) -- every construction-fixed setting takes effect
     *        without closing the window or touching the device, scenes or assets. Runs at the
     *        top of the next tick; safe to call from UI code mid-frame.
     */
    void restart_renderer();

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
                                     coopa::scene::SceneObject* parent = nullptr);

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
    static coopa::gfx::app::ContextConfig make_context_config_(const AppConfig& config);

    /**
     * @brief Binds the default action set: quit, fly move (as three axes),
     *        and look (as one vector).
     *
     * Orbit has no keyboard bindings: the mouse drives yaw/pitch and the scroll wheel
     * drives zoom, both read directly in drive_camera_controller_().
     */
    void bind_default_input_();

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

    void drive_camera_controller_(coopa::scene::Scene& scene);

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
    void drive_kinematic_controllers_(coopa::scene::Scene& scene);

    /**
     * @brief Pushes this frame's movement keys into every FreeMover in the active scene, before
     *        Scene::update() consumes them.
     *
     * The three-axis counterpart to drive_kinematic_controllers_() above, sharing its move_x/
     * move_y axes and adding move_z -- so one keypress drives a planar kinematic body and a free
     * 3D marker the same way, which is what makes them feel like one control scheme rather than
     * two. Same push-model and NO_INPUT=1 contract as every other driver here.
     */
    void drive_free_movers_(coopa::scene::Scene& scene);

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
    void drive_character_controllers_(coopa::scene::Scene& scene);

    /** @brief Pushes the "ragdoll" press (R) into every Ragdoll with `input_toggle` -- same
     *         push model and NO_INPUT=1 contract as the drivers above. */
    void drive_ragdolls_(coopa::scene::Scene& scene);

    /**
     * @brief Refreshes and uploads every CPU-simulated mesh in the scene (today: cloth).
     *
     * Called between Scene::late_update() and ToyRenderPipeline::render(), which is the only
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
    void upload_dynamic_meshes_(coopa::scene::Scene& scene);

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
    void gather_debug_lines_(coopa::scene::Scene& scene);

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
     * rect; see ToyRenderPipeline's ui_world_target_. That is a rendering resolution, not a
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
    void drive_ui_canvases_(coopa::scene::Scene& scene, const std::optional<ScreenUiPlacement>& placement);

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
                            glm::vec3& out_origin, glm::vec3& out_dir);

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
    void apply_cursor_capture_();

    /**
     * @brief Sets the toyengine logo (rasterized from branding.h at the usual icon sizes) as the
     *        window's taskbar icon -- or, on macOS, the Dock icon.
     */
    void apply_app_icon_();

    /** @brief Resolves a config-relative asset path against the project root, unless already absolute. */
    std::string resolve_path_(const std::string& path) const {
        return resolve_against_(options_.project_root, path);
    }

    /**
     * @brief resolve_path_() for a scene file, falling back to a lookup by name: a scene in tag
     *        folders (assets/scenes/rendering/fog_demo/scene.yaml) still loads from the shorthand
     *        assets/scenes/fog_demo/scene.yaml (SCENE=fog_demo, `toyengine fog_demo`). See
     *        coopa::asset::AssetIndex.
     */
    std::string resolve_scene_path_(const std::string& path) const;

    static std::string resolve_against_(const std::filesystem::path& root, const std::string& path);

    /**
     * @brief Where a relative output file (output.filepath) goes: as given (cwd-relative) when
     *        running from source, under the per-user data directory when packaged -- an .app's
     *        cwd is "/" and its own folder may be read-only.
     */
    static std::string output_path_(const std::string& path);

    /**
     * @brief resolve_against_(), with the engine checkout as the fallback layer: a file the
     *        project doesn't have (a game project's config.yaml starts as a copy of the engine's,
     *        whose `palette:` names assets/palettes/...) resolves to the engine's copy if that
     *        exists. Same rule as the asset search roots (asset_roots()).
     */
    static std::string resolve_asset_file_(const std::filesystem::path& root, const std::string& path);

    /**
     * @brief Pumps AssetManager until every load issued during scene load has been finalized.
     *
     * Mesh decode runs on jobs_'s worker threads, so a scene's mesh YAML parses overlap
     * instead of serializing at parse time -- but frame 0 must still see a fully loaded
     * scene, or an ONESHOT/CAPTURE_FRAMES capture would show an empty or partial image.
     * Reports any mesh that failed to decode, since MeshRenderer's parser cannot check
     * is_failed() synchronously while loading is async.
     */
    void drain_pending_assets_();

    /** @brief FIXED_DT env override for frame_dt_() -- unset (or unparsable) means -1, i.e. off. */
    static float fixed_dt_from_env_();

    /** @brief CAPTURE_FRAMES env override for run()'s sequence capture -- 0 means off. */
    static uint32_t capture_frames_from_env_();

    /** @brief CAPTURE_RING env override for run()'s in-memory rolling capture -- 0 means off. */
    static uint32_t capture_ring_from_env_();

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
    void apply_cursor_pos_override_();

    /** @brief Parses CURSOR_POS once at construction; see apply_cursor_pos_override_(). */
    void read_cursor_pos_override_();

    /**
     * @brief SCENE env override for the scene loaded at startup.
     *
     * Accepts either a bare scene NAME under assets/scenes (`SCENE=ui_demo`, which
     * is found by folder name anywhere under assets/scenes -- e.g. assets/scenes/ui/ui_demo/scene.yaml)
     * or an explicit path to a .yaml/.caml. Matches the ONESHOT/MAX_FRAMES/FIXED_DT/CAPTURE_FRAMES/
     * NO_INPUT family: a scripted or one-off run should not have to edit assets/config.yaml,
     * which is version-controlled and describes the DEFAULT scene.
     *
     * @param configured The config file's own scene.default_scene, returned unchanged when
     *                   SCENE is unset or empty.
     */
    static std::string scene_path_from_env_(const std::string& configured);

    bool      cursor_pos_override_ = false;
    glm::vec2 cursor_pos_{0.0f};

    /** @brief NO_INPUT env override for drive_camera_controller_() -- any non-empty value that
     *  isn't "0" suppresses all camera input, for reproducible headless captures. */
    /**
     * @brief PROFILE env override: "1" (or any non-path value like "on"/"true") profiles to
     *        output/profile.csv; anything else ending in .csv is used as the path. Unset, empty
     *        or "0" = off.
     */
    static std::string profile_path_from_env_();

    static bool no_input_from_env_();

    /** @brief Copies AppConfig's render section into a ToyRenderConfig with shader_dir/palette_path resolved. */
    static render::ToyRenderConfig make_render_config_(const AppConfig& config,
                                                         const std::filesystem::path& project_root);

    EngineOptions options_;
    AppConfig     config_;
    /// Each managed scene's `scene.settings` overrides (null: none) -- see scene_config().
    std::unordered_map<const coopa::scene::Scene*, fkyaml::node> scene_settings_;
    bool overrides_applied_ = false;   ///< The live render config carries some scene's overrides.
    bool weather_applied_ = false;     ///< The live render config carries the weather's (or the physical sky's) atmosphere.
    weather::AtmosphereModel sky_atmosphere_;   ///< The physical sky on the CPU (sync_sky_render_state_()).
    bool sky_applied_ = false;                  ///< sky_base_ holds the gradient colours the physical sky overwrote.
    std::array<glm::vec3, 3> sky_base_{};       ///< The config's zenith / horizon / ground under the physical sky.
    glm::dvec2 cloud_offset_{0.0};              ///< Accumulated cloud drift, metres (unwrapped: the renderer wraps it per use).
    double cloud_flat_phase_ = 0.0;             ///< The flat clouds' evolution phase, 0..1.
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
    std::unique_ptr<render::ToyRenderPipeline> pipeline_;
    render::RenderExtent fill_pending_{};      ///< fill mode: the extent waiting out the debounce
    int                  fill_stable_frames_ = 0;
    std::optional<render::ToyRenderConfig> pending_rebuild_;   ///< see queue_rebuild_()
    render::ToyRenderConfig rebuild_from_;    ///< the queued rebuild's baseline: only fields changed from it apply
    /// The render config the config document (+ scene overrides) last asked for; the baseline a
    /// change is measured against (unset until the first apply -- see source_render_config_()).
    std::optional<render::ToyRenderConfig> derived_render_;
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
