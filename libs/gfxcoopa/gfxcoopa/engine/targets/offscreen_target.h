/**
 * @file offscreen_target.h
 * @brief Single-colour-attachment render target (optionally with depth) at any resolution.
 *
 * Creates a color attachment (RGBA8_Unorm by default; the format is a ctor parameter) and, unless built with
 * kColorOnly, a depth attachment at a configurable resolution, with a matching RenderPass that transitions the
 * color image to SHADER_READ_ONLY_OPTIMAL after rendering so it can be sampled
 * by subsequent passes (post-processing, upscale).
 *
 * The depth image is also exposed as an image view so edge detection shaders can
 * read it for depth-discontinuity calculations.
 */

#ifndef GFXCOOPA_ENGINE_TARGETS_OFFSCREEN_TARGET_H
#define GFXCOOPA_ENGINE_TARGETS_OFFSCREEN_TARGET_H

#include <volk/volk.h>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace targets {

/// Tag selecting OffscreenTarget's colour-only constructor -- see kColorOnly.
struct ColorOnlyTag {};

/**
 * Pass as OffscreenTarget's last constructor argument for a target with NO depth
 * attachment. For targets that only ever receive fullscreen-triangle draws (post
 * effects, resolves, blurs), where a depth buffer is never tested against or read:
 * without it, every such pass would still clear and write back a full-size D32 image
 * (the depth attachment's final layout is SHADER_READ_ONLY_OPTIMAL, which makes it
 * STORE_OP_STORE) -- pure memory bandwidth, which tile-based GPUs feel most.
 * depth_view() must not be called on such a target.
 */
inline constexpr ColorOnlyTag kColorOnly{};

/**
 * @class OffscreenTarget
 * @brief Owns a low-resolution color + depth render target.
 *
 * The color image is transitioned to SHADER_READ_ONLY_OPTIMAL after the render pass,
 * ready to be bound as a combined image sampler in the upscale pass.
 *
 * Usage:
 * @code
 * OffscreenTarget target(device, allocator, 320, 240);
 *
 * // Per frame:
 * target.begin(cmd);
 *   // Record draw calls here...
 * target.end(cmd);
 *
 * // Now target.color_view() can be bound to a sampler for the upscale pass.
 * @endcode
 */
class OffscreenTarget {
public:
    /**
     * @brief Creates the offscreen target at the given resolution.
     *
     * @param device       Vulkan logical device.
     * @param allocator    VMA allocator.
     * @param width        Render width in pixels.
     * @param height       Render height in pixels.
     * @param color_format Color attachment pixel format.
     * @param samples      MSAA sample count.
     */
    OffscreenTarget(core::Device&      device,
                    memory::Allocator& allocator,
                    uint32_t           width,
                    uint32_t           height,
                    Format             color_format = Format::RGBA8_Unorm,
                    SampleCount        samples      = SampleCount::X1)
        : device_(device), allocator_(allocator),
          width_(width), height_(height),
          color_format_(color_format), samples_(samples)
    {
        create_resources_();
    }

    /**
     * @brief Creates a colour-only target -- no depth attachment. See kColorOnly.
     */
    OffscreenTarget(core::Device&      device,
                    memory::Allocator& allocator,
                    uint32_t           width,
                    uint32_t           height,
                    Format             color_format,
                    ColorOnlyTag)
        : device_(device), allocator_(allocator),
          width_(width), height_(height),
          color_format_(color_format), samples_(SampleCount::X1), has_depth_(false)
    {
        create_resources_();
    }

    /**
     * @brief Destroys the framebuffer and resources.
     */
    ~OffscreenTarget() {
        destroy_framebuffer_();
    }

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    // --- Frame recording ---

    /**
     * @brief Begins the offscreen render pass.
     *
     * Records begin_render_pass with the given clear color (dark gray by
     * default) and a far-depth clear (ignored on a colour-only target). Sets
     * dynamic viewport and scissor to the full render resolution.
     *
     * @param cmd   Command buffer to record into.
     * @param clear Color attachment clear value.
     */
    /**
     * @brief Sealed overload: the clear colour as gfxcoopa's own ClearColor, so a consumer
     *        never has to name VkClearColorValue (see tools/check_no_vulkan.sh).
     */
    void begin(command::CommandBuffer& cmd, ClearColor clear) const {
        begin(cmd, VkClearColorValue{{clear.r, clear.g, clear.b, clear.a}});
    }

    void begin(command::CommandBuffer& cmd, VkClearColorValue clear = {{0.05f, 0.05f, 0.05f, 1.0f}}) const {
        cmd.begin_render_pass(
            render_pass_->handle(),
            framebuffer_,
            {width_, height_},
            clear,
            1.0f
        );
        // Use negative viewport height to flip Y for Vulkan NDC (VK_KHR_maintenance1).
        // This avoids the projection-matrix Y-flip that would reverse triangle winding.
        cmd.set_viewport(0.0f, static_cast<float>(height_),
                         static_cast<float>(width_),
                         -static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);
    }

    /**
     * @brief Ends the offscreen render pass.
     * @param cmd Command buffer to record into.
     */
    void end(command::CommandBuffer& cmd) const {
        cmd.end_render_pass();
    }

    // --- Resolution management ---

    /**
     * @brief Recreates the target at a new resolution.
     *
     * Safe to call between frames (after vkDeviceWaitIdle).
     * Pipelines referencing this target must also be recreated.
     *
     * @param width  New render width.
     * @param height New render height.
     */
    void recreate(uint32_t width, uint32_t height) {
        width_  = width;
        height_ = height;
        destroy_framebuffer_();
        color_image_.reset();
        resolve_image_.reset();
        depth_image_.reset();
        render_pass_.reset();
        create_resources_();
    }

    // --- Accessors ---

    /** @brief Returns the render width in pixels. */
    uint32_t width()  const { return width_; }
    /** @brief Returns the render height in pixels. */
    uint32_t height() const { return height_; }

    /** @brief Returns the color image view for sampling in subsequent passes. */
    VkImageView color_view() const {
        return (samples_ != SampleCount::X1 && resolve_image_)
            ? resolve_image_->view()
            : color_image_->view();
    }
    /** @brief Sealed sibling of color_view(). */
    coopa::gfx::TextureView color_view_typed() const {
        return (samples_ != SampleCount::X1 && resolve_image_)
            ? resolve_image_->view_typed()
            : color_image_->view_typed();
    }
    /** @brief Returns the color memory::Image object (resolved if MSAA). */
    memory::Image* color_image_object() const {
        return (samples_ != SampleCount::X1 && resolve_image_)
            ? resolve_image_.get()
            : color_image_.get();
    }
    /** @brief Returns the depth image view (for edge detection shaders). Not on a kColorOnly target. */
    VkImageView depth_view() const { return depth_image_or_throw_().view(); }
    /** @brief Sealed sibling of depth_view(). */
    coopa::gfx::TextureView depth_view_typed() const { return depth_image_or_throw_().view_typed(); }

    /** @brief False for a target built with kColorOnly. */
    bool has_depth() const { return has_depth_; }

    /** @brief Returns the render pass handle. */
    VkRenderPass render_pass() const { return render_pass_->handle(); }

    /** @brief Returns the pipeline::RenderPass object for Pipeline construction. */
    pipeline::RenderPass& render_pass_object() const { return *render_pass_; }

private:
    /**
     * @brief Creates all resources: color image, resolve image (if MSAA), depth image, render pass, framebuffer.
     */
    void create_resources_() {
        bool is_msaa = samples_ != SampleCount::X1;

        // Color attachment: used as color attachment (+ sampled if not MSAA).
        ImageUsage color_usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc;
        if (!is_msaa) {
            color_usage = color_usage | ImageUsage::Sampled;
        }

        color_image_ = std::make_unique<memory::Image>(
            device_, allocator_,
            width_, height_,
            color_format_,
            color_usage,
            MemoryResidency::GpuOnly,
            samples_
        );

        if (is_msaa) {
            // Resolve attachment: single-sampled image that receives MSAA resolve output.
            resolve_image_ = std::make_unique<memory::Image>(
                device_, allocator_,
                width_, height_,
                color_format_,
                ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc,
                MemoryResidency::GpuOnly,
                SampleCount::X1
            );
        }

        // Depth attachment (none on a kColorOnly target).
        if (has_depth_) {
            depth_image_ = std::make_unique<memory::Image>(
                device_, allocator_,
                width_, height_,
                Format::D32_Sfloat,
                ImageUsage::DepthAttachment | ImageUsage::Sampled,
                MemoryResidency::GpuOnly,
                samples_
            );
        }

        render_pass_ = std::make_unique<pipeline::RenderPass>(
            device_,
            detail::to_vk(color_format_),
            has_depth_ ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,   // color final
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,   // depth final
            detail::to_vk(samples_)
        );

        create_framebuffer_();
    }

    /**
     * @brief Creates the VkFramebuffer from the current images and render pass.
     */
    void create_framebuffer_() {
        std::vector<VkImageView> attachments;
        attachments.push_back(color_image_->view());
        if (depth_image_) attachments.push_back(depth_image_->view());
        if (samples_ != SampleCount::X1 && resolve_image_) {
            attachments.push_back(resolve_image_->view());
        }

        VkFramebufferCreateInfo fb_info{};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = render_pass_->handle();
        fb_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        fb_info.pAttachments    = attachments.data();
        fb_info.width           = width_;
        fb_info.height          = height_;
        fb_info.layers          = 1;

        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr, &framebuffer_));
    }

    const memory::Image& depth_image_or_throw_() const {
        if (!depth_image_) {
            throw std::logic_error("[gfxcoopa] OffscreenTarget: depth requested from a kColorOnly target");
        }
        return *depth_image_;
    }

    /**
     * @brief Destroys the VkFramebuffer (called before recreating or on destruction).
     */
    void destroy_framebuffer_() {
        if (framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.handle(), framebuffer_, nullptr);
            framebuffer_ = VK_NULL_HANDLE;
        }
    }

    core::Device&      device_;    /**< Logical device (not owned). */
    memory::Allocator& allocator_; /**< VMA allocator (not owned). */
    uint32_t           width_;     /**< Render width in pixels. */
    uint32_t           height_;    /**< Render height in pixels. */
    Format             color_format_ = Format::RGBA8_Unorm;
    SampleCount        samples_      = SampleCount::X1;
    bool               has_depth_    = true;   /**< False for a kColorOnly target. */

    std::unique_ptr<memory::Image>          color_image_;   /**< Color attachment. */
    std::unique_ptr<memory::Image>          resolve_image_; /**< MSAA resolve attachment. */
    std::unique_ptr<memory::Image>          depth_image_;   /**< Depth attachment. */
    std::unique_ptr<pipeline::RenderPass>   render_pass_;   /**< Offscreen render pass. */
    VkFramebuffer                           framebuffer_ = VK_NULL_HANDLE; /**< Framebuffer. */
};

} // namespace targets
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_OFFSCREEN_TARGET_H
