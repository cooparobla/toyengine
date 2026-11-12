/**
 * @file render_pass.h
 * @brief Vulkan render pass creation with sensible OpenGL-like defaults.
 *
 * A render pass describes the framebuffer attachments and how they are
 * loaded/stored around a draw call. Analogous to binding an FBO in OpenGL.
 * Defaults to a single color attachment + optional depth/stencil.
 */

#ifndef COOPA_GFX_PIPELINE_RENDER_PASS_H
#define COOPA_GFX_PIPELINE_RENDER_PASS_H

#include <volk/volk.h>
#include <vector>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/util/format.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/render_pass_desc.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @class RenderPass
 * @brief RAII wrapper around VkRenderPass with one color + optional depth attachment.
 *
 * The default configuration mirrors the most common OpenGL FBO setup:
 * - Color attachment: LOAD_OP_CLEAR, STORE_OP_STORE, final layout PRESENT_SRC_KHR.
 * - Depth attachment (optional): LOAD_OP_CLEAR, STORE_OP_DONT_CARE, final layout
 *   DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
 * - Single graphics subpass with implicit external dependencies.
 */
class RenderPass {
public:
    /**
     * @brief Creates a render pass with one color attachment.
     * @param device        The logical device.
     * @param color_format  The VkFormat of the swapchain/color attachment.
     * @param depth_format  The VkFormat of the depth attachment.
     *                      Pass VK_FORMAT_UNDEFINED to disable depth.
     * @param color_final_layout The final image layout for the color attachment.
     *                           Use VK_IMAGE_LAYOUT_PRESENT_SRC_KHR for swapchain passes
     *                           and VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for
     *                           offscreen targets that will be sampled as a texture.
     * @param depth_final_layout The final image layout for the depth attachment.
     *                           Default DEPTH_STENCIL_ATTACHMENT_OPTIMAL (normal rendering).
     *                           Use SHADER_READ_ONLY_OPTIMAL to enable depth sampling
     *                           in post-processing (e.g. edge detection).
     * @throws std::runtime_error if render pass creation fails.
     */
    RenderPass(core::Device& device,
               VkFormat color_format,
               VkFormat depth_format = VK_FORMAT_D32_SFLOAT,
               VkImageLayout color_final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VkImageLayout depth_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT)
        : device_(device)
    {
        bool has_color = color_format != VK_FORMAT_UNDEFINED;
        bool has_depth = depth_format != VK_FORMAT_UNDEFINED;
        bool has_msaa  = has_color && (samples > VK_SAMPLE_COUNT_1_BIT);

        // --- Attachment descriptions ---
        std::vector<VkAttachmentDescription> attachments;

        VkAttachmentReference color_ref{};
        if (has_color) {
            VkAttachmentDescription color_attachment{};
            color_attachment.format         = color_format;
            color_attachment.samples        = samples;
            color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment.storeOp        = has_msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
            color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            color_attachment.finalLayout    = has_msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : color_final_layout;

            color_ref.attachment = static_cast<uint32_t>(attachments.size());
            color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments.push_back(color_attachment);
        }

        VkAttachmentReference depth_ref{};
        if (has_depth) {
            VkAttachmentDescription depth_attachment{};
            depth_attachment.format         = depth_format;
            depth_attachment.samples        = samples;
            depth_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp        = (depth_final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                              ? VK_ATTACHMENT_STORE_OP_STORE
                                              : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_attachment.finalLayout    = depth_final_layout;

            depth_ref.attachment = static_cast<uint32_t>(attachments.size());
            depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(depth_attachment);
        }

        VkAttachmentReference resolve_ref{};
        if (has_msaa) {
            VkAttachmentDescription resolve_attachment{};
            resolve_attachment.format         = color_format;
            resolve_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
            resolve_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolve_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            resolve_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolve_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            resolve_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            resolve_attachment.finalLayout    = color_final_layout;

            resolve_ref.attachment = static_cast<uint32_t>(attachments.size());
            resolve_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments.push_back(resolve_attachment);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = has_color ? 1 : 0;
        subpass.pColorAttachments       = has_color ? &color_ref : nullptr;
        subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
        subpass.pResolveAttachments     = has_msaa ? &resolve_ref : nullptr;

        // --- Subpass dependencies ---
        VkSubpassDependency entry_dependency{};
        entry_dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        entry_dependency.dstSubpass    = 0;
        entry_dependency.srcStageMask  = (has_color ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0) |
                                         (has_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0);
        entry_dependency.srcAccessMask = 0;
        entry_dependency.dstStageMask  = (has_color ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0) |
                                         (has_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0);
        entry_dependency.dstAccessMask = (has_color ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0) |
                                         (has_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);

        std::vector<VkSubpassDependency> dependencies{entry_dependency};

        // An EXIT dependency is only needed when a later stage will sample this
        // pass's output as a texture (e.g. an offscreen target feeding a blit/
        // post-process pass) -- without it, that later sampled read races the
        // automatic layout transition this render pass performs on end, which
        // is exactly the hazard synchronization validation flags once two such
        // passes are recorded into the same command buffer back-to-back with no
        // intervening vkQueueWaitIdle to (accidentally) mask it.
        bool needs_exit_dependency =
            (has_color && color_final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
            (has_depth && depth_final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (needs_exit_dependency) {
            VkSubpassDependency exit_dependency{};
            exit_dependency.srcSubpass    = 0;
            exit_dependency.dstSubpass    = VK_SUBPASS_EXTERNAL;
            exit_dependency.srcStageMask  = (has_color ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0) |
                                            (has_depth ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0);
            exit_dependency.srcAccessMask = (has_color ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0) |
                                            (has_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);
            exit_dependency.dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            exit_dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dependencies.push_back(exit_dependency);
        }

        VkRenderPassCreateInfo create_info{};
        create_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        create_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        create_info.pAttachments    = attachments.data();
        create_info.subpassCount    = 1;
        create_info.pSubpasses      = &subpass;
        create_info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        create_info.pDependencies   = dependencies.data();

        GFX_VK_CHECK(vkCreateRenderPass(device_.handle(), &create_info, nullptr, &render_pass_));

        samples_ = samples;
        color_attachment_count_ = has_color ? 1u : 0u;
    }

    /**
     * @brief Destroys the VkRenderPass.
     */
    /**
     * @brief Creates a render pass from a RenderPassDesc -- the general, sealed form.
     *
     * Everything the positional constructor above cannot express: LOAD rather than CLEAR, an
     * explicit non-Undefined initial layout, a depth-only pass, several colour attachments, or
     * custom external dependency masks.
     *
     * Exactly one subpass is emitted, with the colour attachments in `desc.color` order and the
     * depth attachment last. Keeping the attachment count and formats stable across two descs is
     * what makes their passes render-pass compatible.
     *
     * @throws std::runtime_error if the desc has no attachments at all, or if an attachment asks
     *         to LOAD contents it also declares Undefined (which discards them).
     */
    RenderPass(core::Device& device, const RenderPassDesc& desc)
        : device_(device)
    {
        if (desc.color.empty() && !desc.depth.has_value()) {
            throw std::runtime_error("[gfxcoopa] RenderPassDesc has no attachments");
        }

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference>   color_refs;
        attachments.reserve(desc.color.size() + 1);
        color_refs.reserve(desc.color.size());

        auto add = [&](const AttachmentDesc& a, bool is_depth) {
            if (a.load == AttachmentLoad::Load && a.initial == TextureUsage::Undefined) {
                throw std::runtime_error("[gfxcoopa] RenderPassDesc: an attachment cannot LOAD "
                                         "contents while declaring its initial layout Undefined, "
                                         "which discards them");
            }
            VkAttachmentDescription d{};
            d.format         = detail::to_vk(a.format);
            d.samples        = detail::to_vk(a.samples);
            d.loadOp         = detail::to_vk(a.load);
            d.storeOp        = a.store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            d.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            d.initialLayout  = detail::to_vk_layout(a.initial);
            d.finalLayout    = detail::to_vk_layout(a.final);
            const uint32_t index = static_cast<uint32_t>(attachments.size());
            attachments.push_back(d);
            if (!is_depth) {
                color_refs.push_back({index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            }
            return index;
        };

        for (const AttachmentDesc& a : desc.color) add(a, false);
        VkAttachmentReference depth_ref{};
        const bool has_depth = desc.depth.has_value();
        if (has_depth) {
            depth_ref = {add(*desc.depth, true), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            samples_  = detail::to_vk(desc.depth->samples);
        }
        if (!desc.color.empty()) samples_ = detail::to_vk(desc.color.front().samples);
        color_attachment_count_ = static_cast<uint32_t>(desc.color.size());

        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = static_cast<uint32_t>(color_refs.size());
        sub.pColorAttachments       = color_refs.empty() ? nullptr : color_refs.data();
        sub.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

        // The in-pass side of both dependencies is what this subpass does to its attachments;
        // only the external side comes from the desc.
        VkPipelineStageFlags in_stage  = 0;
        VkAccessFlags        in_access = 0;
        if (!desc.color.empty()) {
            in_stage  |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            in_access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        }
        if (has_depth) {
            in_stage  |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            in_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }

        detail::BarrierMasks entry{in_access, in_stage};
        if (desc.wait_for != ImageAccess::None) entry = detail::barrier_masks_for(desc.wait_for);
        detail::BarrierMasks exit{in_access, in_stage};
        if (desc.make_visible_to != ImageAccess::None) exit = detail::barrier_masks_for(desc.make_visible_to);

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = entry.stage;
        deps[0].srcAccessMask = entry.access;
        deps[0].dstStageMask  = in_stage;
        deps[0].dstAccessMask = in_access;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = in_stage;
        deps[1].srcAccessMask = in_access;
        deps[1].dstStageMask  = exit.stage;
        deps[1].dstAccessMask = exit.access;

        VkRenderPassCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments    = attachments.data();
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = deps;
        GFX_VK_CHECK(vkCreateRenderPass(device_.handle(), &info, nullptr, &render_pass_));
        attachment_count_ = static_cast<uint32_t>(attachments.size());
    }

    ~RenderPass() {
        if (render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_.handle(), render_pass_, nullptr);
        }
    }

    /// @brief Non-copyable.
    RenderPass(const RenderPass&) = delete;
    /// @brief Non-copyable.
    RenderPass& operator=(const RenderPass&) = delete;

    /**
     * @brief Returns the underlying VkRenderPass handle.
     * @return Raw VkRenderPass.
     */
    VkRenderPass handle() const { return render_pass_; }

    /**
     * @brief A borrowed reference to this pass, for a pipeline built against it by code that
     *        does not own it.
     *
     * The ref names the pass without extending its life -- see types/render_pass_ref.h.
     */
    RenderPassRef ref() const { return detail::wrap_render_pass(render_pass_); }

    /**
     * @brief Returns this render pass's MSAA sample count.
     *
     * A Pipeline built against this render pass must use the same sample
     * count -- see PipelineConfig::samples' removal from the sealed
     * pipeline::PipelineDesc API: rather than let a caller set a pipeline's
     * sample count independently (and risk it mismatching the render pass,
     * which Vulkan requires), Pipeline's sealed constructor reads it from
     * here automatically.
     */
    SampleCount samples() const { return detail::from_vk(samples_); }

    /**
     * @brief Returns the number of color attachments this render pass has
     * (0 or 1 -- this class supports at most one color attachment).
     *
     * A Pipeline's blend-attachment-state array must be sized to match; see
     * samples()'s docs for why the sealed Pipeline constructor reads this
     * automatically rather than taking it as a separate caller-supplied count.
     */
    uint32_t color_attachment_count() const { return color_attachment_count_; }

    /**
     * @brief How many attachments a Framebuffer built against this pass must supply.
     *
     * Colour attachments plus the depth one. Zero for a pass built by the positional
     * constructor, which does not record it.
     */
    uint32_t attachment_count() const { return attachment_count_; }

private:
    core::Device& device_;                           /**< Owning logical device (not owned). */
    VkRenderPass  render_pass_ = VK_NULL_HANDLE;     /**< The Vulkan render pass. */
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT; /**< See samples(). */
    uint32_t      color_attachment_count_ = 0;       /**< See color_attachment_count(). */
    uint32_t      attachment_count_ = 0;             /**< See attachment_count(). */
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_RENDER_PASS_H
