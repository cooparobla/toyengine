/**
 * @file context.h
 * @brief Owns the full Vulkan/windowing bring-up (Window -> Instance ->
 * Surface -> Device -> Allocator -> Swapchain -> CommandPool -> RenderPass
 * -> Renderer) plus timing and the frame loop, so a consumer application
 * never has to construct and order these nine objects by hand.
 *
 * Two things Context takes responsibility for that a hand-written bring-up
 * sequence has to get right on its own:
 *
 *  - Resize. presentation::Renderer's own resize path rebuilds framebuffers
 *    but never calls core::Swapchain::recreate(), so it depends on the
 *    caller doing that. Context installs the full sequence (wait_idle ->
 *    poll framebuffer size -> recreate -> rebuild framebuffers -> user
 *    callback) via Renderer::set_resize_handler().
 *  - Headless runs. ONESHOT/MAX_FRAMES are read in one place, by
 *    ContextConfig::from_env().
 *
 * A consumer with its own asset/scene/config layer should COMPOSE a Context
 * as its first member (constructed first, destroyed last) rather than
 * reproduce the bring-up sequence.
 */

#ifndef COOPA_GFX_APP_CONTEXT_H
#define COOPA_GFX_APP_CONTEXT_H

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <gfxcoopa/core/instance.h>
#include <gfxcoopa/core/surface.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/presentation/window.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/clear.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <coopa/util/time.h>

namespace coopa {
namespace gfx {
namespace app {

/**
 * @struct ContextConfig
 * @brief Everything Context needs to bring up a window + device + swapchain.
 */
struct ContextConfig {
    std::string title      = "gfxcoopa";
    uint32_t    width      = 1280;
    uint32_t    height     = 720;
    bool        resizable  = true;
    /**
     * @brief False creates the window without ever mapping it -- see
     * presentation::Window's constructor. Rendering and presentation are
     * unaffected; the window simply never appears and never takes focus,
     * which is what an automated render test wants on a desktop someone is
     * still using.
     */
    bool        visible    = true;
    /**
     * @brief True creates a `visible` window UNMAPPED, for Context::show_window() to map once
     * the caller has presented something into it.
     *
     * A window the window system maps before anything has been presented shows whatever was
     * on that part of the screen before -- the whole duration of an app's startup, which for a
     * renderer is pipeline creation plus asset loading. An app that paints a loading screen
     * first sets this and calls show_window() right after its first present. No effect when
     * `visible` is false: that window is never mapped at all.
     */
    bool        show_when_ready = false;
    bool        vsync      = true;
    bool        validation = true;
    Format      depth_format = Format::D32_Sfloat;

    bool        headless_oneshot = false; ///< True if ONESHOT was set via from_env().
    uint32_t    max_frames = 0;           ///< 0 = unlimited; run() stops after this many frames otherwise.

    /**
     * @brief Overlays the ONESHOT/MAX_FRAMES environment variables (the
     * convention every headless test target in this workspace already
     * uses) onto `base`.
     *
     * ONESHOT (if set, to any value) forces max_frames to 1 and sets
     * headless_oneshot. It is applied last, so it wins over MAX_FRAMES when
     * both are set.
     *
     * @param base Config to start from (e.g. one with title/size already set).
     * @return `base` with the env overlay applied.
     */
    static ContextConfig from_env(ContextConfig base) {
        if (const char* mf = std::getenv("MAX_FRAMES")) {
            base.max_frames = static_cast<uint32_t>(std::atoll(mf));
        }
        if (std::getenv("ONESHOT")) {
            base.max_frames = 1;
            base.headless_oneshot = true;
        }
        return base;
    }

    /// @brief Overload of from_env() defaulting `base` to ContextConfig{}.
    /// Split from the overload above (rather than a default argument)
    /// because a default argument of `ContextConfig{}` inside
    /// ContextConfig's own member function hits a genuine C++ aggregate-
    /// initialization-in-a-default-argument limitation (verified: GCC 13
    /// rejects it with "could not convert '{}' ... to 'ContextConfig'").
    static ContextConfig from_env() { return from_env(ContextConfig{}); }
};

/**
 * @struct FrameCallbacks
 * @brief The callbacks Context::frame()/run() invoke each frame, mirroring
 * presentation::Renderer::draw_frame()'s callback shape in sealed types.
 */
struct FrameCallbacks {
    /// @brief Records draw commands inside the swapchain render pass. Required.
    std::function<void(command::CommandBuffer&)> record;
    /// @brief Optional: records an earlier, separate render pass into the
    /// same command buffer before `record` runs -- see
    /// presentation::Renderer::draw_frame()'s pre_pass_fn docs.
    std::function<void(command::CommandBuffer&)> pre_pass;
    /// @brief Optional: invoked after a swapchain resize/recreate, with the new extent.
    std::function<void(Extent2D)> on_resize;
    /// @brief Clear color for the swapchain render pass.
    ClearColor clear{0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * @class Context
 * @brief Owns Window/Instance/Surface/Device/Allocator/Swapchain/
 * CommandPool/RenderPass/Renderer, plus frame timing and the main loop.
 *
 * Usage:
 * @code
 * gfx::app::Context ctx(gfx::app::ContextConfig::from_env({.title = "My App"}));
 * ctx.run([&](float dt) { my_scene.update(dt); },
 *         gfx::app::FrameCallbacks{ .record = [&](command::CommandBuffer& cmd) {
 *             my_pipeline.render(cmd, my_scene);
 *         }});
 * @endcode
 * or, driving the loop by hand:
 * @code
 * while (!ctx.should_close()) {
 *     ctx.poll();
 *     ctx.frame([&](command::CommandBuffer& cmd) { ... });
 * }
 * @endcode
 */
class Context {
public:
    /// @brief Brings up the full window + device + swapchain stack.
    explicit Context(const ContextConfig& config = {})
        : config_(config)
    {
        window_   = std::make_unique<presentation::Window>(config.title, config.width, config.height,
                                                          config.resizable,
                                                          config.visible && !config.show_when_ready);
        instance_ = std::make_unique<core::Instance>(config.title, config.validation);
        surface_  = std::make_unique<core::Surface>(*instance_, *window_);
        device_   = std::make_unique<core::Device>(*instance_, *surface_);
        allocator_= std::make_unique<memory::Allocator>(*instance_, *device_);

        auto [fb_w, fb_h] = window_->framebuffer_size();
        swapchain_ = std::make_unique<core::Swapchain>(*device_, *surface_, fb_w, fb_h, config.vsync);

        cmd_pool_ = std::make_unique<command::CommandPool>(*device_, device_->graphics_family());

        // Renderer::create_framebuffers() only ever binds ONE color view per
        // swapchain framebuffer -- it has no depth image/view of its own to
        // bind as a second attachment. A RenderPass built with a real depth
        // format therefore does NOT match the framebuffers Renderer builds
        // against it (verified: triggers VUID-VkFramebufferCreateInfo-
        // attachmentCount-00876). A RenderPass paired with Renderer must
        // therefore pass VK_FORMAT_UNDEFINED, even though RenderPass's raw
        // constructor defaults depth_format to VK_FORMAT_D32_SFLOAT -- that
        // default suits a render pass the caller builds framebuffers for
        // itself. config.depth_format is intentionally NOT used here; it
        // is exposed via depth_format() for a consumer building its OWN
        // separate depth-supporting render pass/target (e.g. a G-buffer),
        // which is unrelated to the swapchain present pass Context owns.
        render_pass_ = std::make_unique<pipeline::RenderPass>(*device_, swapchain_->image_format(),
                                                               VK_FORMAT_UNDEFINED);
        renderer_    = std::make_unique<presentation::Renderer>(*device_, *swapchain_, *render_pass_, *cmd_pool_);

        install_resize_handler();
    }

    /// @brief Waits for the device to go idle, then tears down in reverse
    /// bring-up order automatically via the unique_ptr members' destructors.
    ~Context() {
        if (device_) device_->wait_idle();
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // --- Owned object access ---
    presentation::Window&   window()       { return *window_; }
    /// @brief This context's keyboard/mouse state -- shorthand for window().input().
    coopa::input::Input&    input()        { return window_->input(); }
    core::Device&           device()       { return *device_; }
    core::Swapchain&        swapchain()    { return *swapchain_; }
    memory::Allocator&      allocator()    { return *allocator_; }
    command::CommandPool&   command_pool() { return *cmd_pool_; }
    pipeline::RenderPass&   render_pass()  { return *render_pass_; }
    presentation::Renderer& renderer()     { return *renderer_; }

    // --- Derived state ---
    Extent2D extent() const       { return detail::from_vk(swapchain_->extent()); }
    /// @brief The swapchain/present render pass's color format.
    Format   color_format() const { return detail::from_vk(swapchain_->image_format()); }
    /// @brief The depth format from ContextConfig, for a consumer's OWN
    /// depth-supporting render pass/target (e.g. a G-buffer) -- NOT used by
    /// Context's own swapchain render pass, which is always depth-less; see
    /// the constructor's comment on why.
    Format   depth_format() const { return config_.depth_format; }
    uint32_t frames_in_flight() const { return presentation::MAX_FRAMES_IN_FLIGHT; }
    uint32_t current_frame() const    { return renderer_->current_frame(); }
    /// @brief The configured frame limit (0 = unlimited), from ContextConfig::max_frames
    /// -- for a consumer driving its own loop (rather than run()) that still wants
    /// to honor ONESHOT/MAX_FRAMES without re-parsing the env vars itself.
    uint32_t max_frames() const { return config_.max_frames; }

    // --- Loop driving ---
    bool should_close() const { return window_->should_close(); }

    /**
     * @brief Maps a window held back by ContextConfig::show_when_ready.
     *
     * A no-op for a window that is already mapped, and for an invisible (headless) one -- so a
     * caller can call it unconditionally once its first frame is on the surface.
     */
    void show_window() {
        if (!config_.visible || window_->visible()) return;
        window_->show();
    }

    /// @brief Clears per-frame input state, polls OS events, and advances
    /// the frame timer. Call once per iteration before frame().
    ///
    /// new_frame() is passed the PREVIOUS iteration's delta_time() -- the
    /// only duration known at this point -- so Input's held-time accounting
    /// (see coopa::input::Input::begin_frame()) accumulates the frame that
    /// actually just elapsed, not a stale or zero value.
    void poll() {
        window_->new_frame(time_.delta_time());
        window_->poll_events();
        time_.update();
    }

    float    delta_time() const  { return time_.delta_time(); }
    double   elapsed() const     { return time_.elapsed(); }
    uint64_t frame_index() const { return time_.frame_count(); }

    /**
     * @brief Acquires, records, submits, and presents one frame.
     * @param cb Callbacks for this frame (see FrameCallbacks).
     * @return False if the frame was skipped (e.g. mid-resize); true otherwise.
     */
    bool frame(const FrameCallbacks& cb) {
        pending_on_resize_ = cb.on_resize;
        VkClearColorValue vk_clear = detail::to_vk(cb.clear);
        return renderer_->draw_frame(cb.record, vk_clear, nullptr, cb.pre_pass);
    }

    /// @brief Convenience overload taking just a record callback.
    bool frame(std::function<void(command::CommandBuffer&)> record) {
        FrameCallbacks cb;
        cb.record = std::move(record);
        return frame(cb);
    }

    /// @brief Records and submits a one-time command buffer outside the
    /// per-frame loop (e.g. a BRDF LUT bake, a GI probe capture at load
    /// time), blocking until the GPU finishes. See command::CommandPool::submit_once().
    void submit_once(std::function<void(command::CommandBuffer&)> record) {
        cmd_pool_->submit_once(std::move(record));
    }

    /// @brief Blocks until all GPU work has completed.
    void wait_idle() { device_->wait_idle(); }

    /**
     * @brief Runs the full poll -> update -> frame loop until the window
     * closes or (in headless test mode) config's max_frames is reached.
     * @param update Called once per iteration with the frame's delta time,
     *   before frame(). May be empty.
     * @param cb     Callbacks passed to frame() every iteration.
     */
    void run(std::function<void(float dt)> update, const FrameCallbacks& cb) {
        uint32_t frames_done = 0;
        while (!should_close()) {
            poll();
            if (update) update(delta_time());
            frame(cb);
            ++frames_done;
            if (config_.max_frames != 0 && frames_done >= config_.max_frames) break;
        }
    }

private:
    /**
     * @brief Installs the FULL correct resize sequence on the Renderer:
     * wait_idle -> poll framebuffer size until non-zero (handles minimize)
     * -> Swapchain::recreate() -> Renderer::recreate_framebuffers() -> the
     * caller's FrameCallbacks::on_resize. See presentation::Renderer::
     * set_resize_handler()'s docs for why the internal default path alone
     * is insufficient (it never calls Swapchain::recreate()).
     */
    void install_resize_handler() {
        renderer_->set_resize_handler([this]() {
            device_->wait_idle();
            auto [w, h] = window_->framebuffer_size();
            while ((w == 0 || h == 0) && !window_->should_close()) {
                window_->wait_events();
                std::tie(w, h) = window_->framebuffer_size();
            }
            swapchain_->recreate(w, h);
            renderer_->recreate_framebuffers();
            window_->reset_resized();
            if (pending_on_resize_) pending_on_resize_(Extent2D{w, h});
        });
    }

    ContextConfig config_;

    // Declaration order is bring-up/teardown order.
    std::unique_ptr<presentation::Window>   window_;
    std::unique_ptr<core::Instance>         instance_;
    std::unique_ptr<core::Surface>          surface_;
    std::unique_ptr<core::Device>           device_;
    std::unique_ptr<memory::Allocator>      allocator_;
    std::unique_ptr<core::Swapchain>        swapchain_;
    std::unique_ptr<command::CommandPool>   cmd_pool_;
    std::unique_ptr<pipeline::RenderPass>   render_pass_;
    std::unique_ptr<presentation::Renderer> renderer_;

    coopa::util::Time              time_;
    std::function<void(Extent2D)>  pending_on_resize_; ///< Set by frame(); read by install_resize_handler()'s lambda.
};

} // namespace app
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_APP_CONTEXT_H
