/**
 * @file gbuffer_target.h
 * @brief G-Buffer render target for deferred rendering pass.
 */

#ifndef GFXCOOPA_ENGINE_TARGETS_GBUFFER_TARGET_H
#define GFXCOOPA_ENGINE_TARGETS_GBUFFER_TARGET_H

#include <volk/volk.h>
#include <memory>
#include <vector>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace targets {

/**
 * @class GBufferTarget
 * @brief The deferred G-buffer: five colour attachments plus depth, with the
 *        render pass and framebuffer that write them.
 *
 * G0 albedo+AO, G1 normal+metallic, G2 position+roughness, G3 emissive, G4 velocity
 * (xy = the surface's screen motion since last frame in UV units, unjittered-to-
 * unjittered; z = its linear view depth last frame, -1 if it was behind the eye;
 * w = its linear view depth this frame -- see gfx/surface/gbuffer_fs.glsl). Cleared
 * every frame (G4 to zero: the sky has no motion and no depth). begin()/end() bracket
 * the geometry passes that fill it; the depth attachment is left readable afterwards
 * for passes that test against it.
 */
class GBufferTarget {
public:
    GBufferTarget(coopa::gfx::core::Device& device,
                  coopa::gfx::memory::Allocator& allocator,
                  uint32_t width,
                  uint32_t height)
        : device_(device), allocator_(allocator), width_(width), height_(height)
    {
        create_resources_();
    }

    ~GBufferTarget() {
        destroy_resources_();
    }

    GBufferTarget(const GBufferTarget&) = delete;
    GBufferTarget& operator=(const GBufferTarget&) = delete;

    void begin(coopa::gfx::command::CommandBuffer& cmd) const {
        VkClearValue clear_values[6]{};
        clear_values[0].color = {{0.05f, 0.05f, 0.05f, 1.0f}};
        clear_values[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        clear_values[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        // G3 (Emissive) clears to zero, not albedo-grey like G0 -- any non-zero clear here
        // becomes a constant emissive floor added to every background/non-emissive pixel a
        // lighting shader reads this attachment at.
        clear_values[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        // G4 (velocity): zero motion, zero depth -- "no surface here" for every consumer.
        clear_values[4].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        clear_values[5].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = render_pass_;
        rp_info.framebuffer       = framebuffer_;
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = {width_, height_};
        rp_info.clearValueCount   = 6;
        rp_info.pClearValues      = clear_values;

        vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        cmd.set_viewport(0.0f, static_cast<float>(height_),
                         static_cast<float>(width_),
                         -static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);
    }

    void end(coopa::gfx::command::CommandBuffer& cmd) const {
        cmd.end_render_pass();
    }

    void recreate(uint32_t width, uint32_t height) {
        width_ = width;
        height_ = height;
        destroy_resources_();
        create_resources_();
    }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    VkImageView g0_view() const { return g0_image_->view(); }
    VkImageView g1_view() const { return g1_image_->view(); }
    VkImageView g2_view() const { return g2_image_->view(); }
    VkImageView g3_view() const { return g3_image_->view(); }
    VkImageView g4_view() const { return g4_image_->view(); }
    VkImageView depth_view() const { return depth_image_->view(); }
    VkImage depth_image_handle() const { return depth_image_->handle(); }
    /// @brief G2 (world position + roughness, RGBA32F) as a raw image, for texel copies. Left in
    /// 32-bit floats deliberately: world position is consumed by shadow lookups, and a half
    /// float's quantum at a few hundred metres from the origin is a quarter of a METRE --
    /// every shadow silhouette in an open world snapped to that grid until this widened.
    ///        SHADER_READ_ONLY_OPTIMAL by the render pass; a copier transitions it and back.
    VkImage g2_image_handle() const { return g2_image_->handle(); }

    // Sealed TextureView siblings of the six accessors above.
    TextureView g0_view_typed() const    { return g0_image_->view_typed(); }
    TextureView g1_view_typed() const    { return g1_image_->view_typed(); }
    TextureView g2_view_typed() const    { return g2_image_->view_typed(); }
    TextureView g3_view_typed() const    { return g3_image_->view_typed(); }
    /// @brief G4, the velocity attachment (see the class doc for its channels).
    TextureView g4_view_typed() const    { return g4_image_->view_typed(); }
    TextureView depth_view_typed() const { return depth_image_->view_typed(); }

    /// @brief The depth memory::Image itself, for command::CommandBuffer::
    /// transition() -- the sealed sibling of depth_image_handle(), which
    /// only ever gets a raw VkImage (needed by the pass ctors this target
    /// feeds, whose signatures are unchanged here -- see the plan's
    /// "mechanical substitution only" scope for engine/passes+targets).
    memory::Image& depth_image_object() const { return *depth_image_; }

    VkRenderPass render_pass() const { return render_pass_; }

private:
    void create_resources_() {
        // TRANSFER_SRC: a consumer may copy a few texels out (e.g. toyengine's screen-centre
        // focus probe reads world positions from G2) without a full-screen pass.
        VkImageUsageFlags color_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        g0_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_,
            VK_FORMAT_R8G8B8A8_UNORM, color_flags,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO
        );

        g1_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_,
            VK_FORMAT_R16G16B16A16_SFLOAT, color_flags,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO
        );

        g2_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_,
            VK_FORMAT_R32G32B32A32_SFLOAT, color_flags,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO
        );

        g3_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_,
            VK_FORMAT_R16G16B16A16_SFLOAT, color_flags,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO
        );

        // G4 velocity: fp16 is plenty for a sub-pixel UV delta (relative precision) and for
        // the two linear view depths a 5% disocclusion test compares.
        g4_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_,
            VK_FORMAT_R16G16B16A16_SFLOAT, color_flags,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO
        );

        depth_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, VMA_MEMORY_USAGE_AUTO
        );

        // Render pass definition with 5 color attachments + 1 depth attachment
        VkAttachmentDescription attachments[6]{};

        // G0 (Albedo + AO)
        attachments[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // G1 (Normal + Metallic)
        attachments[1].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // G2 (Position + Roughness)
        attachments[2].format         = VK_FORMAT_R32G32B32A32_SFLOAT;
        attachments[2].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[2].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[2].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // G3 (Emissive, HDR)
        attachments[3].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[3].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // G4 (Velocity: uv motion xy, previous/current linear view depth zw)
        attachments[4].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[4].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[4].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[4].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[4].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[4].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[4].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Depth
        attachments[5].format         = VK_FORMAT_D32_SFLOAT;
        attachments[5].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[5].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[5].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[5].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[5].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[5].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[5].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_refs[5]{
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
        };
        VkAttachmentReference depth_ref{5, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 5;
        subpass.pColorAttachments       = color_refs;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dependency{};
        dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass    = 0;
        dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp_info{};
        rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_info.attachmentCount = 6;
        rp_info.pAttachments    = attachments;
        rp_info.subpassCount    = 1;
        rp_info.pSubpasses      = &subpass;
        rp_info.dependencyCount = 1;
        rp_info.pDependencies   = &dependency;

        GFX_VK_CHECK(vkCreateRenderPass(device_.handle(), &rp_info, nullptr, &render_pass_));

        // Framebuffer
        std::vector<VkImageView> views = {
            g0_image_->view(),
            g1_image_->view(),
            g2_image_->view(),
            g3_image_->view(),
            g4_image_->view(),
            depth_image_->view()
        };

        VkFramebufferCreateInfo fb_info{};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = render_pass_;
        fb_info.attachmentCount = static_cast<uint32_t>(views.size());
        fb_info.pAttachments    = views.data();
        fb_info.width           = width_;
        fb_info.height          = height_;
        fb_info.layers          = 1;

        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr, &framebuffer_));
    }

    void destroy_resources_() {
        if (framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.handle(), framebuffer_, nullptr);
            framebuffer_ = VK_NULL_HANDLE;
        }
        if (render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_.handle(), render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
        }
        g0_image_.reset();
        g1_image_.reset();
        g2_image_.reset();
        g3_image_.reset();
        g4_image_.reset();
        depth_image_.reset();
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    uint32_t                       width_;
    uint32_t                       height_;

    std::unique_ptr<coopa::gfx::memory::Image> g0_image_;
    std::unique_ptr<coopa::gfx::memory::Image> g1_image_;
    std::unique_ptr<coopa::gfx::memory::Image> g2_image_;
    std::unique_ptr<coopa::gfx::memory::Image> g3_image_;
    std::unique_ptr<coopa::gfx::memory::Image> g4_image_;
    std::unique_ptr<coopa::gfx::memory::Image> depth_image_;

    VkRenderPass  render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
};

} // namespace targets
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_TARGETS_GBUFFER_TARGET_H
