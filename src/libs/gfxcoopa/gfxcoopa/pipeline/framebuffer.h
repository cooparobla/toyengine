#ifndef GFXCOOPA_PIPELINE_FRAMEBUFFER_H
#define GFXCOOPA_PIPELINE_FRAMEBUFFER_H

/**
 * @file framebuffer.h
 * @brief RAII VkFramebuffer: the attachment views a render pass renders into.
 */

#include <volk/volk.h>

#include <stdexcept>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/types/clear.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @class Framebuffer
 * @brief The set of attachment views one render pass renders into -- an OpenGL FBO's
 *        attachment set.
 *
 * Takes TextureView rather than raw VkImageView, so every `*_view_typed()` accessor in this
 * library feeds it directly. Movable but not copyable, so a render target can rebuild one on
 * resize without a unique_ptr indirection.
 */
class Framebuffer {
public:
    /** @brief Single-attachment form -- a depth-only shadow pass, or one colour target. */
    Framebuffer(core::Device& device, const RenderPass& pass, TextureView attachment,
                Extent2D extent, uint32_t layers = 1)
        : Framebuffer(device, pass, std::vector<TextureView>{attachment}, extent, layers) {}

    /**
     * @brief Several attachments, in the render pass's own attachment order.
     *
     * @throws std::runtime_error if the count disagrees with the pass. That mismatch otherwise
     *         surfaces as a validation error at vkCmdBeginRenderPass, far from its cause.
     */
    Framebuffer(core::Device& device, const RenderPass& pass,
                const std::vector<TextureView>& attachments, Extent2D extent, uint32_t layers = 1)
        : device_(device), extent_(extent)
    {
        const uint32_t expected = pass.attachment_count();
        if (expected != 0 && attachments.size() != expected) {
            throw std::runtime_error("[gfxcoopa] Framebuffer attachment count (" +
                                     std::to_string(attachments.size()) +
                                     ") does not match its RenderPass (" +
                                     std::to_string(expected) + ")");
        }
        std::vector<VkImageView> views;
        views.reserve(attachments.size());
        for (TextureView v : attachments) views.push_back(detail::unwrap(v));

        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = pass.handle();
        info.attachmentCount = static_cast<uint32_t>(views.size());
        info.pAttachments    = views.data();
        info.width           = extent.width;
        info.height          = extent.height;
        info.layers          = layers;
        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &info, nullptr, &fb_));
    }

    ~Framebuffer() {
        if (fb_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_.handle(), fb_, nullptr);
    }

    /// @brief Non-copyable: it owns a VkFramebuffer.
    Framebuffer(const Framebuffer&) = delete;
    /// @brief Non-copyable.
    Framebuffer& operator=(const Framebuffer&) = delete;

    Framebuffer(Framebuffer&& other) noexcept
        : device_(other.device_), fb_(other.fb_), extent_(other.extent_) {
        other.fb_ = VK_NULL_HANDLE;
    }

    Framebuffer& operator=(Framebuffer&& other) noexcept {
        if (this != &other) {
            if (fb_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_.handle(), fb_, nullptr);
            fb_        = other.fb_;
            extent_    = other.extent_;
            other.fb_  = VK_NULL_HANDLE;
        }
        return *this;
    }

    /** @brief The framebuffer's dimensions. */
    Extent2D extent() const { return extent_; }

    /** @brief The raw handle. gfxcoopa-internal. */
    VkFramebuffer handle() const { return fb_; }

private:
    core::Device& device_;
    VkFramebuffer fb_     = VK_NULL_HANDLE;
    Extent2D      extent_{};
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_PIPELINE_FRAMEBUFFER_H
