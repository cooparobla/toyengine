/**
 * @file renderer.h
 * @brief High-level frame orchestration: acquire → record → submit → present.
 *
 * Renderer owns the swapchain framebuffers, per-frame sync objects, and
 * command buffers. begin_frame() acquires the next swapchain image and
 * returns a CommandBuffer ready for recording. end_frame() submits and
 * presents. This mirrors the mental model of:
 *
 *   glClear()  →  record commands  →  glfwSwapBuffers()
 */

#ifndef COOPA_GFX_PRESENTATION_RENDERER_H
#define COOPA_GFX_PRESENTATION_RENDERER_H

#include <volk/volk.h>
#include <chrono>
#include <vector>
#include <stdexcept>
#include <functional>
#include <cstring>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/command/sync.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace presentation {

/**
 * @brief Maximum number of frames that may be in flight simultaneously.
 *
 * 2 means the CPU is at most one frame ahead of the GPU, balancing
 * throughput and latency similarly to OpenGL's implicit double-buffering.
 */
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

/**
 * @class Renderer
 * @brief Orchestrates the acquire → record → submit → present frame loop.
 *
 * Manages:
 * - One VkFramebuffer per swapchain image (bound to the given render pass).
 * - MAX_FRAMES_IN_FLIGHT sets of sync objects and command buffers.
 * - Swapchain resize detection and recreation.
 *
 * Usage:
 * @code
 * while (!window.should_close()) {
 *     window.poll_events();
 *     renderer.begin_frame([&](coopa::gfx::command::CommandBuffer& cmd) {
 *         cmd.bind_pipeline(my_pipeline);
 *         cmd.draw(3);
 *     });
 * }
 * device.wait_idle();
 * @endcode
 */
class Renderer {
public:
    /**
     * @brief Creates the renderer, framebuffers, and per-frame sync objects.
     * @param device      The logical device.
     * @param swapchain   The swapchain providing presentable images.
     * @param render_pass The render pass the framebuffers are compatible with.
     * @param cmd_pool    The command pool used to allocate command buffers.
     */
    Renderer(core::Device&         device,
             core::Swapchain&      swapchain,
             pipeline::RenderPass& render_pass,
             command::CommandPool& cmd_pool)
        : device_(device), swapchain_(swapchain),
          render_pass_(render_pass), cmd_pool_(cmd_pool)
    {
        create_framebuffers();
        create_sync_objects();
        create_render_finished_semaphores();
        create_command_buffers();
    }

    /**
     * @brief Destroys framebuffers and sync objects.
     *
     * The device must be idle (device.wait_idle()) before destruction.
     */
    ~Renderer() {
        destroy_framebuffers();
        // Sync objects and command buffers are automatically destroyed via their RAII wrappers.
    }

    /// @brief Non-copyable.
    Renderer(const Renderer&) = delete;
    /// @brief Non-copyable.
    Renderer& operator=(const Renderer&) = delete;

    /**
     * @brief Returns the index of the current frame-in-flight slot (0 .. MAX_FRAMES_IN_FLIGHT-1).
     * @return Current frame index.
     */
    uint32_t current_frame() const { return current_frame_; }

    /**
     * @brief Milliseconds the last frame's vkQueueSubmit call took.
     *
     * Together with last_present_ms() this splits the frame's submit half, which a profile
     * otherwise shows as one number: a queue submit that blocks is the driver, while a present
     * that blocks is the presentation engine not retiring images (a compositor that has stopped
     * consuming the window's buffers, so the driver's present queue is full).
     */
    double last_submit_ms() const { return last_submit_ms_; }

    /** @brief Milliseconds the last frame's vkQueuePresentKHR call took. See last_submit_ms(). */
    double last_present_ms() const { return last_present_ms_; }

    /**
     * @brief Blocks until the command buffer previously submitted for the CURRENT frame slot
     *        has completed, without acquiring or submitting anything.
     *
     * draw_frame() already does this as its first statement, but only after the caller has had
     * a chance to write that slot's own per-frame-in-flight host buffers. A caller that writes
     * slot-indexed resources BEFORE draw_frame() -- the normal shape for a per-frame-in-flight
     * instance/uniform stream -- calls this first so those writes are provably ordered after
     * the frame that last used the slot, rather than racing it. Cheap and idempotent: fences
     * are created signaled (see command/sync.h), so an already-signaled wait returns
     * immediately.
     */
    void wait_for_current_frame() const { in_flight_fences_[current_frame_]->wait(); }

    /**
     * @brief Acquires the next swapchain image, executes the record callback, submits and presents.
     *
     * This is the primary API. The callback receives a ready CommandBuffer already
     * inside a begin_render_pass with the swapchain framebuffer and extent set.
     *
     * @param record_fn A callable that receives a CommandBuffer& and records draw commands.
     *                  Called between begin_render_pass and end_render_pass.
     * @param clear_color RGBA clear color applied at the start of the render pass.
     * @param on_resize   Optional callback invoked when the swapchain is recreated.
     * @param pre_pass_fn Optional callable invoked after cmd.begin() but BEFORE this
     *                  render pass opens -- the seam for recording an earlier, separate
     *                  render pass into the same command buffer (e.g. an offscreen target
     *                  a later blit will sample), so it shares this frame's single submit
     *                  rather than needing its own vkQueueSubmit + wait. Pair with a
     *                  RenderPass built with a SHADER_READ_ONLY_OPTIMAL final layout so its
     *                  exit subpass dependency (see pipeline/render_pass.h) synchronizes the
     *                  handoff instead of leaving it to chance.
     * @return True if the frame was presented successfully, false if the window was minimized.
     */
    /**
     * @brief Sealed overloads: the clear colour as gfxcoopa's own ClearColor, so a consumer
     *        never names VkClearColorValue (see tools/check_no_vulkan.sh's layering rule).
     */
    bool draw_frame(std::function<void(command::CommandBuffer&)> record_fn,
                    ClearColor clear_color,
                    std::function<void()> on_resize = nullptr,
                    std::function<void(command::CommandBuffer&)> pre_pass_fn = nullptr,
                    std::function<void(command::CommandBuffer&)> post_pass_fn = nullptr)
    {
        return draw_frame(std::move(record_fn),
                          VkClearColorValue{{clear_color.r, clear_color.g, clear_color.b, clear_color.a}},
                          std::move(on_resize), std::move(pre_pass_fn), std::move(post_pass_fn));
    }

    /// @copydoc draw_frame(std::function<void(command::CommandBuffer&)>, ClearColor, std::function<void()>, std::function<void(command::CommandBuffer&)>, std::function<void(command::CommandBuffer&)>)
    bool begin_frame(std::function<void(command::CommandBuffer&)> record_fn,
                     ClearColor clear_color,
                     std::function<void()> on_resize = nullptr,
                     std::function<void(command::CommandBuffer&)> pre_pass_fn = nullptr,
                     std::function<void(command::CommandBuffer&)> post_pass_fn = nullptr)
    {
        return draw_frame(std::move(record_fn), clear_color, std::move(on_resize),
                          std::move(pre_pass_fn), std::move(post_pass_fn));
    }

    bool draw_frame(std::function<void(command::CommandBuffer&)> record_fn,
                    VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}},
                    std::function<void()> on_resize = nullptr,
                    std::function<void(command::CommandBuffer&)> pre_pass_fn = nullptr,
                    std::function<void(command::CommandBuffer&)> post_pass_fn = nullptr)
    {
        // Wait for this frame slot to be free.
        in_flight_fences_[current_frame_]->wait();

        // Acquire the next image.
        uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(
            device_.handle(), swapchain_.handle(),
            UINT64_MAX,
            image_available_semaphores_[current_frame_]->handle(),
            VK_NULL_HANDLE, &image_index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            handle_resize(on_resize);
            return false; // Skip this frame.
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("[gfxcoopa] vkAcquireNextImageKHR: " +
                                     util::vk_result_string(result));
        }

        // If this swapchain image was last written by a *different* frame-in-flight slot,
        // wait for that submission to finish before touching the image again. With
        // image_count() != MAX_FRAMES_IN_FLIGHT (typically 3 images, 2 frames in flight),
        // the slot-only fence wait above does not by itself guarantee this.
        if (images_in_flight_[image_index] != VK_NULL_HANDLE) {
            GFX_VK_CHECK(vkWaitForFences(device_.handle(), 1,
                                         &images_in_flight_[image_index], VK_TRUE, UINT64_MAX));
        }
        images_in_flight_[image_index] = in_flight_fences_[current_frame_]->handle();

        // Reset the fence only after we know we are going to submit.
        in_flight_fences_[current_frame_]->reset();

        // Record commands.
        VkCommandBuffer raw_cmd = raw_cmd_buffers_[current_frame_];
        GFX_VK_CHECK(vkResetCommandBuffer(raw_cmd, 0));

        command::CommandBuffer cmd(raw_cmd);
        cmd.begin();

        if (pre_pass_fn) pre_pass_fn(cmd);

        cmd.begin_render_pass(render_pass_.handle(),
                              framebuffers_[image_index],
                              swapchain_.extent(), clear_color);

        record_fn(cmd);

        cmd.end_render_pass();
        // After the swapchain pass, before the command buffer closes -- outside any render
        // pass, e.g. for a closing GPU timestamp.
        if (post_pass_fn) post_pass_fn(cmd);
        cmd.end();

        // Submit.
        //
        // wait_semaphores stays frame-slot-indexed (signaled by vkAcquireNextImageKHR above,
        // scoped to this frame-in-flight slot). signal_semaphores must instead be indexed by
        // image_index: it's waited on by vkQueuePresentKHR below keyed to that same image, and
        // with image_count() != MAX_FRAMES_IN_FLIGHT a frame-slot-indexed signal semaphore can
        // be re-signaled while the presentation engine may still be waiting on its earlier use
        // (VUID-vkQueueSubmit-pSignalSemaphores-00067) — one semaphore per swapchain image,
        // never per frame-in-flight slot, is the fix.
        VkSemaphore wait_semaphores[]   = { image_available_semaphores_[current_frame_]->handle() };
        VkSemaphore signal_semaphores[] = { render_finished_semaphores_[image_index]->handle() };
        VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

        VkSubmitInfo submit_info{};
        submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount   = 1;
        submit_info.pWaitSemaphores      = wait_semaphores;
        submit_info.pWaitDstStageMask    = wait_stages;
        submit_info.commandBufferCount   = 1;
        submit_info.pCommandBuffers      = &raw_cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores    = signal_semaphores;

        const auto submit_call_start = std::chrono::steady_clock::now();
        GFX_VK_CHECK(vkQueueSubmit(device_.graphics_queue(), 1, &submit_info,
                                   in_flight_fences_[current_frame_]->handle()));
        const auto present_call_start = std::chrono::steady_clock::now();
        last_submit_ms_ = std::chrono::duration<double, std::milli>(present_call_start -
                                                                    submit_call_start).count();

        // Present.
        VkSwapchainKHR swapchains[] = { swapchain_.handle() };
        VkPresentInfoKHR present_info{};
        present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores    = signal_semaphores;
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = swapchains;
        present_info.pImageIndices      = &image_index;

        result = vkQueuePresentKHR(device_.present_queue(), &present_info);
        last_present_ms_ = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - present_call_start).count();
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            handle_resize(on_resize);
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("[gfxcoopa] vkQueuePresentKHR: " +
                                     util::vk_result_string(result));
        }

        current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
        return true;
    }

    /**
     * @brief Alias for draw_frame(), identical in behaviour.
     * @deprecated Prefer draw_frame(). This name suggests a matching
     * end_frame() that does not exist -- the call runs the whole
     * acquire-record-submit-present cycle on its own.
     */
    bool begin_frame(std::function<void(command::CommandBuffer&)> record_fn,
                     VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}},
                     std::function<void()> on_resize = nullptr,
                     std::function<void(command::CommandBuffer&)> pre_pass_fn = nullptr,
                     std::function<void(command::CommandBuffer&)> post_pass_fn = nullptr)
    {
        return draw_frame(std::move(record_fn), clear_color, std::move(on_resize), std::move(pre_pass_fn),
                          std::move(post_pass_fn));
    }

    /**
     * @brief Installs a handler invoked INSTEAD OF the internal resize path
     * (device_.wait_idle() + recreate_framebuffers() + the per-call
     * on_resize callback) whenever the swapchain reports out-of-date or
     * suboptimal.
     *
     * The internal path never calls Swapchain::recreate() (see
     * recreate_framebuffers()'s docs) -- it only rebuilds framebuffers
     * against whatever extent the swapchain already has, which is a latent
     * bug if the caller forgets the recreate() call themselves. This seam
     * exists so gfx::app::Context can install the FULL correct sequence
     * (wait_idle -> poll framebuffer size -> swapchain_.recreate() ->
     * recreate_framebuffers() -> user callback) once, instead of every
     * caller needing to get that ordering right by hand.
     *
     * When a handler is installed, draw_frame()/begin_frame()'s own
     * `on_resize` parameter is ignored -- the handler is expected to invoke
     * whatever it needs to itself (Context's handler calls the user's
     * FrameCallbacks::on_resize internally).
     *
     * @param handler Callback to invoke on resize, or nullptr to restore
     *   the default internal-only behavior.
     */
    void set_resize_handler(std::function<void()> handler) {
        resize_handler_ = std::move(handler);
    }

    /**
     * @brief Recreates framebuffers after a swapchain recreate.
     *
     * Must be called if the swapchain is recreated externally (e.g. after
     * Swapchain::recreate()). Automatically called internally when
     * VK_ERROR_OUT_OF_DATE_KHR is detected.
     */
    void recreate_framebuffers() {
        device_.wait_idle();
        destroy_framebuffers();
        create_framebuffers();
        // image_count() can change across a recreate (different present mode / surface
        // capabilities), so the per-image semaphore array — and the fence-aliasing array
        // indexed the same way — must be rebuilt to match.
        create_render_finished_semaphores();
    }

private:
    /**
     * @brief Creates one VkFramebuffer per swapchain image.
     */
    void create_framebuffers() {
        const auto& views = swapchain_.image_views();
        framebuffers_.resize(views.size());
        VkExtent2D extent = swapchain_.extent();

        for (size_t i = 0; i < views.size(); ++i) {
            VkFramebufferCreateInfo fb_info{};
            fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass      = render_pass_.handle();
            fb_info.attachmentCount = 1;
            fb_info.pAttachments    = &views[i];
            fb_info.width           = extent.width;
            fb_info.height          = extent.height;
            fb_info.layers          = 1;

            GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr,
                                             &framebuffers_[i]));
        }
    }

    /**
     * @brief Destroys all framebuffers.
     */
    void destroy_framebuffers() {
        for (VkFramebuffer fb : framebuffers_) {
            if (fb != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_.handle(), fb, nullptr);
            }
        }
        framebuffers_.clear();
    }

    /**
     * @brief Creates MAX_FRAMES_IN_FLIGHT image-available Semaphores and in-flight Fences.
     *
     * render_finished_semaphores_ is deliberately not created here — see
     * create_render_finished_semaphores(), which sizes it by swapchain image count instead.
     */
    void create_sync_objects() {
        image_available_semaphores_.reserve(MAX_FRAMES_IN_FLIGHT);
        in_flight_fences_.reserve(MAX_FRAMES_IN_FLIGHT);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            image_available_semaphores_.push_back(
                std::make_unique<command::Semaphore>(device_));
            in_flight_fences_.push_back(
                std::make_unique<command::Fence>(device_, /*signaled=*/true));
        }
    }

    /**
     * @brief (Re)creates one render-finished Semaphore per swapchain image.
     *
     * Unlike image_available_semaphores_/in_flight_fences_ (scoped to a frame-in-flight
     * slot), render_finished_semaphores_ must be scoped to the swapchain image it's paired
     * with at present time — see the wait_semaphores/signal_semaphores comment in
     * draw_frame() for why. Also resets images_in_flight_ to match the (possibly new)
     * image count, since old entries reference no-longer-valid in-flight state.
     */
    void create_render_finished_semaphores() {
        render_finished_semaphores_.clear();
        uint32_t count = swapchain_.image_count();
        render_finished_semaphores_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            render_finished_semaphores_.push_back(std::make_unique<command::Semaphore>(device_));
        }
        images_in_flight_.assign(count, VK_NULL_HANDLE);
    }

    /**
     * @brief Allocates one command buffer per frame-in-flight slot.
     */
    void create_command_buffers() {
        raw_cmd_buffers_ = cmd_pool_.allocate(MAX_FRAMES_IN_FLIGHT);
    }

    /**
     * @brief Handles a swapchain out-of-date or suboptimal condition.
     *
     * Defers entirely to the handler from set_resize_handler() when one is
     * installed. Otherwise it rebuilds framebuffers against the swapchain's
     * CURRENT extent and invokes `on_resize` -- it does NOT call
     * Swapchain::recreate(), because Renderer does not own the window and so
     * cannot query the new framebuffer size. A caller relying on this path
     * must call Swapchain::recreate() itself; gfx::app::Context installs a
     * handler that does the whole sequence instead.
     *
     * @param on_resize Optional user-supplied callback for post-resize work.
     */
    void handle_resize(std::function<void()> on_resize) {
        if (resize_handler_) {
            resize_handler_();
            return;
        }
        recreate_framebuffers();  // waits for the device to go idle itself
        if (on_resize) on_resize();
    }

    core::Device&         device_;       /**< The logical device (not owned). */
    core::Swapchain&      swapchain_;    /**< The target swapchain (not owned). */
    pipeline::RenderPass& render_pass_;  /**< Compatible render pass (not owned). */
    command::CommandPool& cmd_pool_;     /**< Command pool for buffer allocation (not owned). */

    std::vector<VkFramebuffer>                          framebuffers_;                     /**< One framebuffer per swapchain image. */
    std::vector<std::unique_ptr<command::Semaphore>>    image_available_semaphores_;       /**< Signaled when a swapchain image is acquired. Indexed by current_frame_. */
    std::vector<std::unique_ptr<command::Semaphore>>    render_finished_semaphores_;       /**< Signaled when rendering is complete. Indexed by image_index — see draw_frame(). */
    std::vector<std::unique_ptr<command::Fence>>        in_flight_fences_;                 /**< Fences to throttle CPU ahead of GPU. Indexed by current_frame_. */
    std::vector<VkFence>                                images_in_flight_;                 /**< Non-owning: aliases the in_flight_fences_ handle that last wrote each swapchain image. Indexed by image_index. */
    std::vector<VkCommandBuffer>                        raw_cmd_buffers_;                  /**< Command buffers (allocated from cmd_pool_). */
    uint32_t                                            current_frame_ = 0;                /**< Current frame-in-flight index. */
    double                                              last_submit_ms_ = 0.0;             /**< See last_submit_ms(). */
    double                                              last_present_ms_ = 0.0;            /**< See last_present_ms(). */
    std::function<void()>                               resize_handler_;                   /**< See set_resize_handler(). */
};

} // namespace presentation
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PRESENTATION_RENDERER_H
