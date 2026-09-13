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
#include <uicoopa/layout/canvas.h>
#include <uicoopa/ui_yaml.h>

#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/kinematic_control_system.h>
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
        // NEAREST + clamp-to-edge (SamplerDesc::pixel_art()), not gfxcoopa's bilinear default:
        // this is a pixel-art engine, and bilinear filtering blurs texel edges.
        assets_.register_loader<coopa::gfx::engine::data::Texture>(
            std::make_unique<coopa::gfx::engine::loaders::TextureLoader>(
                ctx_.device(), ctx_.allocator(), ctx_.command_pool(), coopa::gfx::SamplerDesc::pixel_art()));
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

        scene_mgr_.load_scene(resolve_path_(scene_path_from_env_(config_.scene.default_scene)));

        // BEFORE the physics system in numeric order (50 vs 100), which is the whole point: a
        // component driving a kinematic body's Transform has to write it before PhysicsSystem reads
        // it, or physics spends the frame solving against the previous pose while the renderer draws
        // the new one. See kinematic_control_system.h's file doc.
        scene::install_kinematic_control_system(scene_mgr_.get_active_scene());
        coopa::physx::system::install_physics_system(scene_mgr_.get_active_scene(), config_.physics);

        // Activate TransformSystem before the first drain or render, so the world_matrix()
        // reads below are never asked to resolve a still-dirty transform; then block until
        // every load issued above has finished, so frame 0 sees a fully populated scene.
        coopa::scene::install_transform_system(scene_mgr_.get_active_scene());
        drain_pending_assets_();

        // Fail fast on a typo'd/unregistered PBRMaterial::shader -- see
        // PixelRenderPipeline::validate_material_shaders()'s doc for why this can't happen
        // during YAML parsing itself.
        pipeline_.validate_material_shaders(scene_mgr_.get_active_scene());

        apply_cursor_capture_();
        read_cursor_pos_override_();
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
     *   NO_INPUT=1           zeroes all camera-controller input every frame (see
     *                        drive_camera_controller_()), so a capture running on a live
     *                        desktop isn't perturbed by real mouse/keyboard activity.
     *
     * One further env var is read by the CONSTRUCTOR, not this loop:
     *   SCENE=<name|path>    loads a different scene than assets/config.yaml's
     *                        scene.default_scene -- see scene_path_from_env_().
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
        coopa::gfx::memory::Image& src = low_res ? pipeline_.low_res_color_image()
                                                 : pipeline_.final_color_image();
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
        apply_cursor_pos_override_();
        assets_.update(dt);

        if (scene_mgr_.has_scene()) {
            drive_camera_controller_(scene_mgr_.get_active_scene());
            drive_kinematic_controllers_(scene_mgr_.get_active_scene());
        }
        scene_mgr_.update(dt);
        if (scene_mgr_.has_scene()) {
            // Between update() and late_update(), and that window is the only correct slot:
            // world matrices are current only after UpdatePhase::TransformResolve (350) has
            // run inside update(), and a canvas needs its pointer position before
            // EventSystem::process() is dispatched from inside late_update() (400).
            drive_ui_canvases_(scene_mgr_.get_active_scene());
        }
        // late_update() runs LateBehaviourSystem, flushes each worker's deferred
        // SceneCommandBuffer and advances Scene::frame_index(). Must precede render() so a
        // same-frame deferred spawn or destroy is reflected in what is drawn, matching
        // Unity's Update -> LateUpdate -> render order.
        scene_mgr_.late_update(dt);

        if (scene_mgr_.has_scene()) {
            upload_dynamic_meshes_(scene_mgr_.get_active_scene());
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
        cc.visible    = config.window.visible;
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

        const glm::vec2 move = no_input_
            ? glm::vec2(0.0f)
            : glm::vec2(input_.axis("move_x", ctx_.input()), input_.axis("move_y", ctx_.input()));
        for (scene::KinematicController* kc : controllers) kc->move_input = move;
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
    void drive_ui_canvases_(coopa::scene::Scene& scene) {
        std::vector<coopa::ui::CanvasComponent*> canvases = coopa::ui::collect_canvases(scene);
        if (canvases.empty()) return;

        const uint32_t rw = pipeline_.render_width();
        const uint32_t rh = pipeline_.render_height();
        const uint32_t ww = ctx_.swapchain().extent().width;
        const uint32_t wh = ctx_.swapchain().extent().height;
        const coopa::gfx::TextureView white        = pipeline_.world_ui_white_view();
        const coopa::gfx::TextureView screen_white = pipeline_.screen_ui_white_view();

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
                // The WINDOW extent, not the render extent: UiPass draws this canvas into the
                // swapchain-sized overlay target, and ctx_.input()'s cursor is in window
                // pixels, so this is the sizing that makes both agree. See the doc above.
                canvas->set_viewport(ww, wh);
                canvas->set_input(ctx_.input());
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
        const uint32_t sw = ctx_.swapchain().extent().width;
        const uint32_t sh = ctx_.swapchain().extent().height;
        render::LetterboxRect box = render::compute_display_rect(config_.render, sw, sh, rw, rh);
        if (box.w == 0 || box.h == 0) return false;

        glm::vec2 cursor = ctx_.input().cursor_position() - glm::vec2(box.x, box.y);
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
        if (no_input_ || !config_.window.visible) return;
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
        if (const char* v = std::getenv("FIXED_DT")) return std::strtof(v, nullptr);
        return -1.0f;
    }

    /** @brief CAPTURE_FRAMES env override for run()'s sequence capture -- 0 means off. */
    static uint32_t capture_frames_from_env_() {
        if (const char* v = std::getenv("CAPTURE_FRAMES")) return static_cast<uint32_t>(std::atoll(v));
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
        const char* v = std::getenv("CURSOR_POS");
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
     * or an explicit path to a .yaml. Matches the ONESHOT/MAX_FRAMES/FIXED_DT/CAPTURE_FRAMES/
     * NO_INPUT family: a scripted or one-off run should not have to edit assets/config.yaml,
     * which is version-controlled and describes the DEFAULT scene.
     *
     * @param configured The config file's own scene.default_scene, returned unchanged when
     *                   SCENE is unset or empty.
     */
    static std::string scene_path_from_env_(const std::string& configured) {
        const char* v = std::getenv("SCENE");
        if (!v || !*v) return configured;
        std::string scene(v);
        if (scene.size() >= 5 && scene.compare(scene.size() - 5, 5, ".yaml") == 0) return scene;
        return "assets/scenes/" + scene + "/scene.yaml";
    }

    bool      cursor_pos_override_ = false;
    glm::vec2 cursor_pos_{0.0f};

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
        // the glslc -I search order (see assets/shaders/.glslc_flags). uicoopa's own shader
        // directory is a third root rather than app_over_base()'s two, for the UI passes'
        // ui*.vert/frag; it goes LAST so that a future uicoopa file sharing a logical name
        // with a gfxcoopa base shader can never shadow the base copy (ShaderLibrary::resolve()
        // is first-match-wins). There are no collisions across the three roots today.
        rc.shaders = coopa::gfx::pipeline::ShaderLibrary(std::vector<std::string>{
            rc.shader_dir,
            std::string(PROJ_DIR) + "/gfxcoopa/assets/shaders",
            std::string(PROJ_DIR) + "/uicoopa/assets/shaders",
        });
        if (!rc.palette_path.empty()) rc.palette_path = resolve_path_(rc.palette_path);

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
