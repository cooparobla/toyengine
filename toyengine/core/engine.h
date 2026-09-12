/**
 * @file engine.h
 * @brief Owns the engine lifetime: window, Vulkan objects, assets, scene, and
 *        the render loop.
 *
 * Owns a shared coopa::job::JobEngine first (so it outlives every subsystem that
 * submits to it), then composes a gfx::app::Context (which owns the Window ->
 * Instance -> Surface -> Device -> Allocator -> Swapchain -> CommandPool ->
 * RenderPass -> Renderer bring-up chain, plus frame timing and resize
 * handling), then layers PixelRenderPipeline, AssetManager, and SceneManager
 * on top -- all three are handed the JobEngine so asset decode, transform
 * resolution, and the render-list gathers can dispatch to it. Engine no
 * longer orders or constructs any Vulkan/windowing object itself -- see
 * gfxcoopa/app/context.h.
 */

#ifndef TOYENGINE_CORE_ENGINE_H
#define TOYENGINE_CORE_ENGINE_H

#include <glm/glm.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/util/image_readback.h>
#include <gfxcoopa/engine/loaders/mesh_loader.h>
#include <gfxcoopa/engine/loaders/texture_loader.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/components/camera_component.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/input/input_map.h>
#include <coopa/job/engine.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_manager.h>
#include <coopa/scene/systems/transform_system.h>

#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/debug/debug_draw.h>

#include <toyengine/core/config.h>
#include <toyengine/render/pixel_render_config.h>
#include <toyengine/render/pixel_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/register.h>

#include <root_directory.h>

namespace toy {
namespace core {

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
    explicit Engine(AppConfig config)
        : config_(std::move(config)),
          jobs_(config_.jobs.worker_threads ? config_.jobs.worker_threads
                                             : std::thread::hardware_concurrency()),
          ctx_(make_context_config_(config_)),
          pipeline_(ctx_.device(), ctx_.allocator(), ctx_.swapchain(), ctx_.render_pass(),
                   ctx_.command_pool(), make_render_config_(config_)),
          assets_(&jobs_)
    {
        // Read once here rather than in the initializer list -- these are declared after
        // assets_/scene_mgr_/input_, and initializing them there regardless of list order
        // (member init always follows DECLARATION order) would trip -Wreorder for no benefit,
        // since neither env read depends on any other member.
        fixed_dt_       = fixed_dt_from_env_();
        capture_frames_ = capture_frames_from_env_();
        no_input_       = no_input_from_env_();

        bind_default_input_();

        scene_mgr_.set_job_engine(&jobs_);
        pipeline_.set_job_engine(&jobs_);
        pipeline_.set_parallel_threshold(config_.jobs.parallel_threshold);

        assets_.add_search_root(std::string(ROOT_DIR) + "/assets");
        assets_.register_loader<coopa::gfx::engine::data::Mesh>(
            std::make_unique<coopa::gfx::engine::loaders::MeshLoader>(ctx_.device(), ctx_.allocator(), ctx_.command_pool()));
        // NEAREST + clamp-to-edge (SamplerDesc::pixel_art()), not gfxcoopa's bilinear default --
        // this is a pixel-art engine, and bilinear filtering blurs texel edges. Was a 99-line
        // fork (toy::loaders::PixelTextureLoader) differing only in this sampler; folded into
        // gfxcoopa's TextureLoader once it grew a sampler_desc parameter for exactly this.
        assets_.register_loader<coopa::gfx::engine::data::Texture>(
            std::make_unique<coopa::gfx::engine::loaders::TextureLoader>(
                ctx_.device(), ctx_.allocator(), ctx_.command_pool(), coopa::gfx::SamplerDesc::pixel_art()));
        coopa::gfx::engine::components::register_render_components(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), assets_);
        scene::register_scene_components();
        coopa::physx::register_physics_components(assets_, config_.physics);

        scene_mgr_.load_scene(resolve_path_(config_.scene.default_scene));
        coopa::physx::system::install_physics_system(scene_mgr_.get_active_scene(), config_.physics);

        // Mesh decode (gfxcoopa's register.h) now runs via load_async() on jobs_'s workers,
        // same as texture decode always has -- activate TransformSystem before the first
        // drain/render so world_matrix() reads below are never asked to resolve a still-dirty
        // transform, then block here until every load issued above has finished, so frame 0
        // (and ONESHOT/CAPTURE_FRAMES captures) see a fully populated scene.
        coopa::scene::install_transform_system(scene_mgr_.get_active_scene());
        drain_pending_assets_();

        // Fail fast on a typo'd/unregistered PBRMaterial::shader -- see
        // PixelRenderPipeline::validate_material_shaders()'s doc for why this can't happen
        // during YAML parsing itself.
        pipeline_.validate_material_shaders(scene_mgr_.get_active_scene());

        apply_cursor_capture_();
    }

    ~Engine() {
        ctx_.wait_idle();
        // register_render_components()'s parser lambdas capture ctx_'s device/allocator/
        // cmd_pool by reference in SceneLoader's function-local static registry, which
        // would otherwise only be destroyed at program exit -- after ctx_ goes out of
        // scope. Clear it now, while they're still alive. Then shut down the asset
        // manager (which holds every loaded Mesh/Texture's GPU allocation) before ctx_
        // (and the device/allocator it owns) destructs.
        coopa::scene::SceneLoader::clear_component_parsers();
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
     *   NO_INPUT=1           zeroes all camera-controller input every frame (see
     *                        drive_camera_controller_()), so a capture running on a live
     *                        desktop isn't perturbed by real mouse/keyboard activity.
     */
    void run() {
        uint32_t captured = 0;
        while (!ctx_.should_close()) {
            if (!tick()) break;

            if (capture_frames_ > 0) {
                capture_sequence_frame_(captured);
                ++captured;
                if (captured >= capture_frames_) break;
            }

            if (ctx_.max_frames() > 0 && ctx_.frame_index() >= ctx_.max_frames()) break;
        }

        if (config_.output.save_on_exit) {
            ctx_.wait_idle();
            save_screenshot(config_.output.filepath, config_.output.save_low_res);
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
    render::PixelRenderConfig& render_config() { return pipeline_.render_config_mut(); }

    /**
     * @brief Writes the current offscreen buffer to a PNG.
     * @param path    Destination file path.
     * @param low_res True writes the internal low-resolution buffer 1:1 (pixel-perfect,
     *                but BEFORE any display-resolution effect such as tilt shift -- see
     *                PixelRenderPipeline::low_res_color_image()). False writes the final
     *                image actually shown in the window (PixelRenderPipeline::
     *                final_color_image()), at DISPLAY resolution.
     */
    void save_screenshot(const std::string& path, bool low_res = true) {
        coopa::gfx::memory::Image& src = low_res ? pipeline_.low_res_color_image()
                                                 : pipeline_.final_color_image();
        coopa::gfx::util::save_image_png(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), src, path);
        if (low_res) {
            std::cout << "[toyengine] Saved " << path << " (" << pipeline_.render_width()
                      << "x" << pipeline_.render_height() << ")\n";
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
        ctx_.poll(); // window_.new_frame() + poll_events() + frame timer update.

        if (input_.is_down("quit", ctx_.input())) {
            ctx_.window().set_should_close(true);
        }

        const float dt = frame_dt_();
        assets_.update(dt);

        if (scene_mgr_.has_scene()) {
            drive_camera_controller_(scene_mgr_.get_active_scene());
        }
        scene_mgr_.update(dt);
        // late_update() runs LateBehaviourSystem (Component::late_update()) and, critically,
        // flushes each worker's deferred SceneCommandBuffer and advances Scene::frame_index() --
        // none of which happened before this call was added. Must run before render() below so
        // a same-frame deferred spawn/destroy is reflected in what's drawn, matching Unity's
        // Update -> LateUpdate -> render frame order.
        scene_mgr_.late_update(dt);

        if (scene_mgr_.has_scene()) {
            gather_debug_lines_(scene_mgr_.get_active_scene());
            pipeline_.render(ctx_.renderer(), scene_mgr_.get_active_scene(), dt);
        }

        return !ctx_.should_close();
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
    coopa::input::InputMap&           input()  { return input_; }
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
#ifdef NDEBUG
        cc.validation = false;
#else
        cc.validation = true;
#endif
        return coopa::gfx::app::ContextConfig::from_env(cc);
    }

    /**
     * @brief Binds the default action set: quit, fly move (as three axes),
     *        and look (as one vector).
     *
     * Orbit no longer has keyboard bindings -- the mouse (yaw/pitch) and
     * scroll wheel (zoom) drive it directly, read in drive_camera_controller_().
     */
    void bind_default_input_() {
        using coopa::input::Key;
        input_.bind("quit", Key::Escape);

        input_.bind_axis("fly_x", Key::D, Key::A); // strafe: +right/-left
        input_.bind_axis("fly_y", Key::W, Key::S); // forward/back
        input_.bind_axis("fly_z", Key::E, Key::Q); // world up/down
        input_.bind_vector("look", Key::Right, Key::Left, Key::Up, Key::Down);
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
    void drive_camera_controller_(coopa::scene::Scene& scene) {
        auto* cc = scene.find_first_component<scene::CameraController>();
        if (!cc) return;

        if (no_input_) {
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
     * @brief Fills pipeline_.debug_lines() from the active scene's PhysicsSystem, when
     *        config_.render.debug_lines_enabled is set -- the physxcoopa <-> toy::render
     *        bridge debug_line_pass.h's file doc describes: the render layer's DebugLine and
     *        pack_gpu_color() know nothing about physics, so this is the one place a
     *        coopa::physx::debug::DebugLine gets translated into one.
     *
     * No-op (and clears any stale lines) when disabled or when no "Physics" system is
     * installed, so flipping the toggle at runtime never leaves last frame's overlay stuck.
     */
    void gather_debug_lines_(coopa::scene::Scene& scene) {
        std::vector<render::DebugLine>& out = pipeline_.debug_lines();
        out.clear();
        if (!config_.render.debug_lines_enabled) return;

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
     * @brief Puts the OS cursor into disabled (hidden + unbounded) mode if the
     *        active scene's CameraController wants it -- called once after
     *        the initial scene load.
     *
     * There is no in-app control to release the cursor once captured; quitting
     * (Escape, still bound) is the only way out. A scene author can opt out
     * entirely via `capture_cursor: false` on the CameraController.
     */
    void apply_cursor_capture_() {
        if (!scene_mgr_.has_scene()) return;
        auto* cc = scene_mgr_.get_active_scene().find_first_component<scene::CameraController>();
        if (cc && cc->capture_cursor) {
            ctx_.input().set_cursor_mode(coopa::input::CursorMode::Disabled);
        }
    }

    /** @brief Resolves a config-relative asset path against ROOT_DIR, unless already absolute. */
    static std::string resolve_path_(const std::string& path) {
        if (std::filesystem::path(path).is_absolute()) return path;
        return std::string(ROOT_DIR) + "/" + path;
    }

    /**
     * @brief Pumps AssetManager until every load issued during scene load has been finalized.
     *
     * Mesh decode now runs on jobs_'s worker threads (see gfxcoopa's register.h), so all of a
     * scene's mesh YAML parses overlap instead of serializing at parse time -- but frame 0
     * must still see a fully loaded scene, or ONESHOT/CAPTURE_FRAMES would capture an empty
     * or partial image. Reports any mesh that failed to decode, since MeshRenderer's parser
     * (register.h) can no longer check is_failed() synchronously once loading is async.
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
        if (const char* v = std::getenv("FIXED_DT")) return std::strtof(v, nullptr);
        return -1.0f;
    }

    /** @brief CAPTURE_FRAMES env override for run()'s sequence capture -- 0 means off. */
    static uint32_t capture_frames_from_env_() {
        if (const char* v = std::getenv("CAPTURE_FRAMES")) return static_cast<uint32_t>(std::atoll(v));
        return 0;
    }

    /** @brief NO_INPUT env override for drive_camera_controller_() -- any non-empty value that
     *  isn't "0" suppresses all camera input, for reproducible headless captures. */
    static bool no_input_from_env_() {
        const char* v = std::getenv("NO_INPUT");
        return v && *v && std::string(v) != "0";
    }

    /** @brief Copies AppConfig's render section into a PixelRenderConfig with shader_dir/palette_path resolved. */
    static render::PixelRenderConfig make_render_config_(const AppConfig& config) {
        render::PixelRenderConfig rc = config.render;
        rc.shader_dir = std::string(ROOT_DIR) + "/assets/shaders";
        // App directory first, gfxcoopa's shared base library second -- the runtime mirror of
        // the glslc -I search order (see assets/shaders/.glslc_flags).
        rc.shaders = coopa::gfx::pipeline::ShaderLibrary::app_over_base(
            rc.shader_dir, std::string(PROJ_DIR) + "/gfxcoopa/assets/shaders");
        if (!rc.palette_path.empty()) rc.palette_path = resolve_path_(rc.palette_path);

        // Derived surface shaders this app ships -- see gfx/surface/*.glsl and the
        // layered-shaders plan. A scene material opts in via `shader: <name>` (see
        // PBRMaterial::shader); a material that never sets it is completely unaffected by
        // this registry existing.
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
            /* cull */ coopa::gfx::CullMode::Back, // matches every other BLEND mesh's cull mode
        });

        return rc;
    }

    AppConfig config_;

    // jobs_ is declared (and constructed) before ctx_/pipeline_/assets_/scene_mgr_, and
    // destroyed after all of them, since every one of those may still be submitting to or
    // waiting on it up through their own destruction.
    coopa::job::JobEngine jobs_;

    // ctx_ is declared before pipeline_ (and constructed first, destroyed
    // last) since pipeline_ holds references into ctx_'s owned objects.
    coopa::gfx::app::Context    ctx_;
    render::PixelRenderPipeline pipeline_;

    coopa::asset::AssetManager assets_;
    coopa::scene::SceneManager scene_mgr_;

    coopa::input::InputMap input_;

    // Deterministic sequence-capture support -- see run()'s own doc. Read once at construction
    // (env vars don't change mid-run); -1.0f / 0 are their respective "off" values.
    float    fixed_dt_       = -1.0f;
    uint32_t capture_frames_ = 0;
    bool     no_input_       = false;
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_ENGINE_H
