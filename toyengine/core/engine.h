/**
 * @file engine.h
 * @brief Owns the engine lifetime: window, Vulkan objects, assets, scene, and
 *        the render loop.
 *
 * Replaces blendy's 378-line main() (see blendy/test.cpp) with a reusable
 * class. Construction performs initialization in the order Window -> Instance
 * -> Surface -> Device -> Allocator -> Swapchain -> CommandPool -> RenderPass
 * -> Renderer -> PixelRenderPipeline -> AssetManager -> SceneManager, matching
 * blendy/pixengine's init sequence.
 */

#ifndef TOYENGINE_CORE_ENGINE_H
#define TOYENGINE_CORE_ENGINE_H

#include <volk/volk.h>
#include <vma/vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <gfxcoopa/util/volk_init.h>
#include <gfxcoopa/util/debug_messenger.h>
#include <gfxcoopa/core/instance.h>
#include <gfxcoopa/core/surface.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/presentation/window.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/engine/loaders/mesh_loader.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/components/camera_component.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_manager.h>

#include <toyengine/core/config.h>
#include <toyengine/core/time.h>
#include <toyengine/input/input_map.h>
#include <toyengine/loaders/pixel_texture_loader.h>
#include <toyengine/render/pixel_render_config.h>
#include <toyengine/render/pixel_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/register.h>
#include <toyengine/util/screenshot.h>

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
          window_(config_.window.title, config_.window.width, config_.window.height, /*resizable=*/true),
          instance_("toyengine", kEnableValidation),
          surface_(instance_, window_.handle()),
          device_(instance_, surface_),
          allocator_(instance_, device_),
          swapchain_(device_, surface_, config_.window.width, config_.window.height, config_.window.vsync),
          cmd_pool_(device_, device_.graphics_family()),
          swapchain_pass_(device_, swapchain_.image_format(), VK_FORMAT_UNDEFINED),
          renderer_(device_, swapchain_, swapchain_pass_, cmd_pool_),
          pipeline_(device_, allocator_, swapchain_, swapchain_pass_, cmd_pool_, make_render_config_(config_))
    {
        bind_default_input_();

        assets_.add_search_root(std::string(ROOT_DIR) + "/assets");
        assets_.register_loader<coopa::gfx::engine::data::Mesh>(
            std::make_unique<coopa::gfx::engine::loaders::MeshLoader>(device_, allocator_, cmd_pool_));
        assets_.register_loader<coopa::gfx::engine::data::Texture>(
            std::make_unique<loaders::PixelTextureLoader>(device_, allocator_, cmd_pool_));
        coopa::gfx::engine::components::register_render_components(device_, allocator_, cmd_pool_, assets_);
        scene::register_scene_components();

        scene_mgr_.load_scene(resolve_path_(config_.scene.default_scene));
    }

    ~Engine() {
        device_.wait_idle();
        // register_render_components()'s parser lambdas capture device_/allocator_/cmd_pool_
        // by reference in SceneLoader's function-local static registry, which would
        // otherwise only be destroyed at program exit -- after those members go out of
        // scope. Clear it now, while they're still alive. Then shut down the asset
        // manager (which holds every loaded Mesh/Texture's GPU allocation) before
        // device_/allocator_ destruct.
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
     */
    void run() {
        uint64_t max_frames = 0;
        if (const char* mf = std::getenv("MAX_FRAMES")) max_frames = static_cast<uint64_t>(std::atoll(mf));
        if (std::getenv("ONESHOT")) max_frames = 1;

        while (!window_.should_close()) {
            if (!tick()) break;
            if (max_frames > 0 && time_.frame_count() >= max_frames) break;
        }

        if (config_.output.save_on_exit) {
            device_.wait_idle();
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
        util::save_image_png(device_, allocator_, cmd_pool_,
                             pipeline_.low_res_color_image(),
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             pipeline_.render_width(), pipeline_.render_height(),
                             path);
    }

    /**
     * @brief Advances and renders exactly one frame.
     * @return False when the loop should stop (window close requested).
     */
    bool tick() {
        window_.poll_events();
        if (input_.is_action_down("quit", [&](int k) { return window_.is_key_pressed(k); })) {
            window_.set_should_close(true);
        }

        time_.update();
        assets_.update(time_.delta_time());

        if (scene_mgr_.has_scene()) {
            drive_camera_controller_(scene_mgr_.get_active_scene());
        }
        scene_mgr_.update(time_.delta_time());

        if (scene_mgr_.has_scene()) {
            pipeline_.render(renderer_, scene_mgr_.get_active_scene());
        }

        window_.new_frame();
        return !window_.should_close();
    }

    coopa::gfx::presentation::Window& window() { return window_; }
    coopa::gfx::core::Device&         device() { return device_; }
    input::InputMap&                  input()  { return input_; }
    Time&                             time()   { return time_; }

private:
#ifdef NDEBUG
    static constexpr bool kEnableValidation = false;
#else
    static constexpr bool kEnableValidation = true;
#endif

    /** @brief Binds the default action set: quit, orbit yaw/zoom, and fly move/look. */
    void bind_default_input_() {
        input_.bind_key("quit", GLFW_KEY_ESCAPE);

        input_.bind_key("orbit_yaw_left",  GLFW_KEY_A);
        input_.bind_key("orbit_yaw_right", GLFW_KEY_D);
        input_.bind_key("orbit_zoom_in",   GLFW_KEY_W);
        input_.bind_key("orbit_zoom_out",  GLFW_KEY_S);

        input_.bind_key("fly_forward", GLFW_KEY_W);
        input_.bind_key("fly_back",    GLFW_KEY_S);
        input_.bind_key("fly_left",    GLFW_KEY_A);
        input_.bind_key("fly_right",   GLFW_KEY_D);
        input_.bind_key("fly_up",      GLFW_KEY_E);
        input_.bind_key("fly_down",    GLFW_KEY_Q);
        input_.bind_key("look_yaw_left",   GLFW_KEY_LEFT);
        input_.bind_key("look_yaw_right",  GLFW_KEY_RIGHT);
        input_.bind_key("look_pitch_up",   GLFW_KEY_UP);
        input_.bind_key("look_pitch_down", GLFW_KEY_DOWN);
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

        auto pressed = [&](const char* action) {
            return input_.is_action_down(action, [&](int k) { return window_.is_key_pressed(k); });
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

    coopa::gfx::presentation::Window   window_;
    coopa::gfx::core::Instance         instance_;
    coopa::gfx::core::Surface          surface_;
    coopa::gfx::core::Device           device_;
    coopa::gfx::memory::Allocator      allocator_;
    coopa::gfx::core::Swapchain        swapchain_;
    coopa::gfx::command::CommandPool   cmd_pool_;
    coopa::gfx::pipeline::RenderPass   swapchain_pass_;
    coopa::gfx::presentation::Renderer renderer_;
    render::PixelRenderPipeline        pipeline_;

    coopa::asset::AssetManager assets_;
    coopa::scene::SceneManager scene_mgr_;

    input::InputMap input_;
    Time            time_;
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_ENGINE_H
