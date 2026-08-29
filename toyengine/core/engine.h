/**
 * @file engine.h
 * @brief Owns the engine lifetime: window, Vulkan objects, assets, scene, and
 *        the render loop.
 *
 * Composes a gfx::app::Context (which owns the Window -> Instance -> Surface
 * -> Device -> Allocator -> Swapchain -> CommandPool -> RenderPass ->
 * Renderer bring-up chain, plus frame timing and resize handling) as its
 * first member, then layers PixelRenderPipeline, AssetManager, and
 * SceneManager on top. Engine no longer orders or constructs any Vulkan/
 * windowing object itself -- see gfxcoopa/app/context.h.
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

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/presentation/input_adapter.h>
#include <gfxcoopa/input/input_map.h>
#include <gfxcoopa/util/image_readback.h>
#include <gfxcoopa/engine/loaders/mesh_loader.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/components/camera_component.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_manager.h>

#include <toyengine/core/config.h>
#include <toyengine/loaders/pixel_texture_loader.h>
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
          ctx_(make_context_config_(config_)),
          pipeline_(ctx_.device(), ctx_.allocator(), ctx_.swapchain(), ctx_.render_pass(),
                   ctx_.command_pool(), make_render_config_(config_))
    {
        // Read once here rather than in the initializer list -- these are declared after
        // assets_/scene_mgr_/input_, and initializing them there regardless of list order
        // (member init always follows DECLARATION order) would trip -Wreorder for no benefit,
        // since neither env read depends on any other member.
        fixed_dt_       = fixed_dt_from_env_();
        capture_frames_ = capture_frames_from_env_();

        bind_default_input_();

        assets_.add_search_root(std::string(ROOT_DIR) + "/assets");
        assets_.register_loader<coopa::gfx::engine::data::Mesh>(
            std::make_unique<coopa::gfx::engine::loaders::MeshLoader>(ctx_.device(), ctx_.allocator(), ctx_.command_pool()));
        assets_.register_loader<coopa::gfx::engine::data::Texture>(
            std::make_unique<loaders::PixelTextureLoader>(ctx_.device(), ctx_.allocator(), ctx_.command_pool()));
        coopa::gfx::engine::components::register_render_components(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), assets_);
        scene::register_scene_components();

        scene_mgr_.load_scene(resolve_path_(config_.scene.default_scene));
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
            save_screenshot(config_.output.filepath, /*low_res=*/true);
        }
    }

    /**
     * @brief Writes the current low-resolution offscreen buffer to a PNG.
     * @param path    Destination file path.
     * @param low_res True writes the internal low-resolution buffer 1:1.
     *                Native-resolution (upscaled swapchain) capture is not
     *                yet wired -- requesting it falls back to low_res.
     */
    void save_screenshot(const std::string& path, bool low_res = true) {
        if (!low_res) {
            std::cerr << "[toyengine] Native-resolution screenshot not yet implemented; saving low-res instead.\n";
        }
        coopa::gfx::util::save_image_png(ctx_.device(), ctx_.allocator(), ctx_.command_pool(),
                                         pipeline_.low_res_color_image(), path);
        std::cout << "[toyengine] Saved " << path << " (" << pipeline_.render_width()
                  << "x" << pipeline_.render_height() << ")\n";
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
        save_screenshot(path, /*low_res=*/true);
    }

    /**
     * @brief Advances and renders exactly one frame.
     * @return False when the loop should stop (window close requested).
     */
    bool tick() {
        ctx_.poll(); // window_.new_frame() + poll_events() + frame timer update.

        auto is_pressed = coopa::gfx::presentation::key_state_of(ctx_.window());
        if (input_.is_down("quit", is_pressed)) {
            ctx_.window().set_should_close(true);
        }

        const float dt = frame_dt_();
        assets_.update(dt);

        if (scene_mgr_.has_scene()) {
            drive_camera_controller_(scene_mgr_.get_active_scene());
        }
        scene_mgr_.update(dt);

        if (scene_mgr_.has_scene()) {
            pipeline_.render(ctx_.renderer(), scene_mgr_.get_active_scene());
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
    coopa::gfx::input::InputMap&      input()  { return input_; }
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

    /** @brief Binds the default action set: quit, orbit yaw/zoom, and fly move/look. */
    void bind_default_input_() {
        using coopa::gfx::input::Key;
        input_.bind("quit", Key::Escape);

        input_.bind("orbit_yaw_left",  Key::A);
        input_.bind("orbit_yaw_right", Key::D);
        input_.bind("orbit_zoom_in",   Key::W);
        input_.bind("orbit_zoom_out",  Key::S);

        input_.bind("fly_forward", Key::W);
        input_.bind("fly_back",    Key::S);
        input_.bind("fly_left",    Key::A);
        input_.bind("fly_right",   Key::D);
        input_.bind("fly_up",      Key::E);
        input_.bind("fly_down",    Key::Q);
        input_.bind("look_yaw_left",   Key::Left);
        input_.bind("look_yaw_right",  Key::Right);
        input_.bind("look_pitch_up",   Key::Up);
        input_.bind("look_pitch_down", Key::Down);
    }

    /**
     * @brief Pushes this frame's keyboard state into the active scene's
     *        CameraController (if any) before Scene::update() consumes it.
     *
     * WASD drives orbit yaw/zoom OR fly movement depending on the
     * controller's configured mode -- both are bound to the same keys since
     * only one mode is ever active on a given camera.
     */
    void drive_camera_controller_(coopa::scene::Scene& scene) {
        auto* cc = scene.find_first_component<scene::CameraController>();
        if (!cc) return;

        auto is_pressed = coopa::gfx::presentation::key_state_of(ctx_.window());
        auto pressed = [&](const char* action) {
            return input_.is_down(action, is_pressed);
        };

        cc->yaw_input  = (pressed("orbit_yaw_right") ? 1.0f : 0.0f) - (pressed("orbit_yaw_left") ? 1.0f : 0.0f);
        cc->zoom_input = (pressed("orbit_zoom_in") ? 1.0f : 0.0f) - (pressed("orbit_zoom_out") ? 1.0f : 0.0f);

        cc->move_input = glm::vec3(
            (pressed("fly_right") ? 1.0f : 0.0f) - (pressed("fly_left") ? 1.0f : 0.0f),
            (pressed("fly_forward") ? 1.0f : 0.0f) - (pressed("fly_back") ? 1.0f : 0.0f),
            (pressed("fly_up") ? 1.0f : 0.0f) - (pressed("fly_down") ? 1.0f : 0.0f));
        cc->look_input = glm::vec2(
            (pressed("look_yaw_right") ? 1.0f : 0.0f) - (pressed("look_yaw_left") ? 1.0f : 0.0f),
            (pressed("look_pitch_up") ? 1.0f : 0.0f) - (pressed("look_pitch_down") ? 1.0f : 0.0f));
    }

    /** @brief Resolves a config-relative asset path against ROOT_DIR, unless already absolute. */
    static std::string resolve_path_(const std::string& path) {
        if (std::filesystem::path(path).is_absolute()) return path;
        return std::string(ROOT_DIR) + "/" + path;
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

    /** @brief Copies AppConfig's render section into a PixelRenderConfig with shader_dir/palette_path resolved. */
    static render::PixelRenderConfig make_render_config_(const AppConfig& config) {
        render::PixelRenderConfig rc = config.render;
        rc.shader_dir = std::string(ROOT_DIR) + "/assets/shaders";
        // App directory first, gfxcoopa's shared base library second -- the runtime mirror of
        // the glslc -I search order (see assets/shaders/.glslc_flags).
        rc.shaders = coopa::gfx::pipeline::ShaderLibrary::app_over_base(
            rc.shader_dir, std::string(PROJ_DIR) + "/gfxcoopa/assets/shaders");
        if (!rc.palette_path.empty()) rc.palette_path = resolve_path_(rc.palette_path);
        return rc;
    }

    AppConfig config_;

    // ctx_ is declared before pipeline_ (and constructed first, destroyed
    // last) since pipeline_ holds references into ctx_'s owned objects.
    coopa::gfx::app::Context    ctx_;
    render::PixelRenderPipeline pipeline_;

    coopa::asset::AssetManager assets_;
    coopa::scene::SceneManager scene_mgr_;

    coopa::gfx::input::InputMap input_;

    // Deterministic sequence-capture support -- see run()'s own doc. Read once at construction
    // (env vars don't change mid-run); -1.0f / 0 are their respective "off" values.
    float    fixed_dt_       = -1.0f;
    uint32_t capture_frames_ = 0;
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_ENGINE_H
