/**
 * @file cubemap_target.h
 * @brief HDR color cubemap render target for reflection probe capture.
 */

#ifndef GFXCOOPA_ENGINE_TARGETS_CUBEMAP_TARGET_H
#define GFXCOOPA_ENGINE_TARGETS_CUBEMAP_TARGET_H

#include <volk/volk.h>
#include <vector>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace targets {

/**
 * @class CubemapTarget
 * @brief Offscreen multi-face render target utility for reflection probes and skybox capturing.
 */
class CubemapTarget {
public:
    CubemapTarget(core::Device& device, memory::Allocator& allocator,
                  uint32_t face_resolution = 256, uint32_t mip_levels = 1)
        : device_(device), allocator_(allocator),
          res_(face_resolution), mip_levels_(mip_levels)
    {
        create_resources_();
    }

    ~CubemapTarget() {
        destroy_framebuffers_();
    }

    CubemapTarget(const CubemapTarget&) = delete;
    CubemapTarget& operator=(const CubemapTarget&) = delete;

    pipeline::RenderPass& render_pass() { return *render_pass_; }
    const pipeline::RenderPass& render_pass() const { return *render_pass_; }

    void begin_face_pass(command::CommandBuffer& cmd, uint32_t face, glm::vec4 clear_color = {0.0f, 0.0f, 0.0f, 1.0f}) {
        VkClearValue clear_values[2]{};
        clear_values[0].color = {{clear_color.r, clear_color.g, clear_color.b, clear_color.a}};
        clear_values[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = render_pass_->handle();
        rp_info.framebuffer       = face_fbs_[face];
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = {res_, res_};
        rp_info.clearValueCount   = 2;
        rp_info.pClearValues      = clear_values;

        vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(res_), static_cast<float>(res_));
        cmd.set_scissor(0, 0, res_, res_);
    }

    void end_face_pass(command::CommandBuffer& cmd) {
        cmd.end_render_pass();
    }

    /// Transitions [base_mip, base_mip + level_count) of all 6 layers to
    /// SHADER_READ_ONLY_OPTIMAL. Split out from transition_to_shader_read()
    /// because a real geometry capture writes mip 0 and the prefilter pass
    /// then READS mip 0 (via mip0_cube_view()) while separately WRITING mips
    /// 1..N-1 as color attachments -- those two ranges transition at two
    /// different points in the bake, not one.
    void transition_mip_range_to_shader_read(command::CommandBuffer& cmd,
                                             uint32_t base_mip, uint32_t level_count,
                                             VkImageLayout old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) const {
        if (level_count == 0) return;

        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = old_layout;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = cubemap_image_;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = base_mip;
        barrier.subresourceRange.levelCount     = level_count;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 6;
        barrier.srcAccessMask                   = (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

        VkPipelineStageFlags src_stage = (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        vkCmdPipelineBarrier(
            cmd.handle(),
            src_stage,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier
        );
    }

    /// Transitions the whole mip chain (all 6 layers) to SHADER_READ_ONLY_OPTIMAL.
    void transition_to_shader_read(command::CommandBuffer& cmd, VkImageLayout old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) const {
        transition_mip_range_to_shader_read(cmd, 0, mip_levels_, old_layout);
    }

    VkImageView cubemap_view() const { return cubemap_view_; }

    /// Cube view exposing ONLY mip 0. Bound as the prefilter pass's read
    /// source once mip 0 holds a real geometry capture -- a disjoint
    /// subresource from the mips 1..N-1 the prefilter pass writes, so no
    /// feedback loop.
    VkImageView mip0_cube_view() const { return mip0_cube_view_; }

    /// TextureView identities of the two views above, for consumers binding through
    /// gfxcoopa's opaque handle rather than a raw Vulkan one.
    TextureView cubemap_view_typed() const    { return detail::wrap(cubemap_view_); }
    TextureView mip0_cube_view_typed() const  { return detail::wrap(mip0_cube_view_); }
    uint32_t resolution() const { return res_; }
    uint32_t mip_levels() const { return mip_levels_; }

    /// Resolution of a given mip level (mip 0 == resolution()).
    uint32_t mip_resolution(uint32_t mip) const {
        return std::max(1u, res_ >> mip);
    }

    /// Color-only render pass used by begin_face_mip_pass(). Distinct from
    /// render_pass() (color + depth) because it targets a single mip level of
    /// a single face with no depth attachment -- suited to a prefilter/convolve
    /// pass rather than a geometry capture.
    pipeline::RenderPass& prefilter_render_pass() { return *prefilter_pass_; }

    /// Begins a color-only render pass targeting a single (face, mip)
    /// subresource. No depth attachment; viewport/scissor are sized to
    /// mip_resolution(mip). Pair with the existing end_face_pass().
    void begin_face_mip_pass(command::CommandBuffer& cmd, uint32_t face, uint32_t mip,
                              glm::vec4 clear_color = {0.0f, 0.0f, 0.0f, 1.0f}) {
        const uint32_t m_res = mip_resolution(mip);

        VkClearValue clear_value{};
        clear_value.color = {{clear_color.r, clear_color.g, clear_color.b, clear_color.a}};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = prefilter_pass_->handle();
        rp_info.framebuffer       = face_mip_fbs_[face * mip_levels_ + mip];
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = {m_res, m_res};
        rp_info.clearValueCount   = 1;
        rp_info.pClearValues      = &clear_value;

        vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(m_res), static_cast<float>(m_res));
        cmd.set_scissor(0, 0, m_res, m_res);
    }

    /// World-to-view matrix for a cube face (face order +X,-X,+Y,-Y,+Z,-Z,
    /// matching the cubemap's array-layer order). The `ups[]` table is chosen
    /// so that, paired with get_face_projection() (no Y-flip) and a
    /// POSITIVE-height viewport (as begin_face_pass()/begin_face_mip_pass()
    /// use), the resulting NDC.xy reproduces cube_face_direction() from
    /// assets/shaders/cubemap_faces.glsl exactly -- verified by hand for all
    /// 6 faces plus corner spot-checks. Do not change independently of that
    /// shader function.
    static glm::mat4 get_face_view(uint32_t face, const glm::vec3& position) {
        glm::vec3 targets[6] = {
            position + glm::vec3( 1.0f,  0.0f,  0.0f), // +X
            position + glm::vec3(-1.0f,  0.0f,  0.0f), // -X
            position + glm::vec3( 0.0f,  1.0f,  0.0f), // +Y
            position + glm::vec3( 0.0f, -1.0f,  0.0f), // -Y
            position + glm::vec3( 0.0f,  0.0f,  1.0f), // +Z
            position + glm::vec3( 0.0f,  0.0f, -1.0f)  // -Z
        };
        glm::vec3 ups[6] = {
            glm::vec3(0.0f, -1.0f,  0.0f), // +X
            glm::vec3(0.0f, -1.0f,  0.0f), // -X
            glm::vec3(0.0f,  0.0f,  1.0f), // +Y
            glm::vec3(0.0f,  0.0f, -1.0f), // -Y
            glm::vec3(0.0f, -1.0f,  0.0f), // +Z
            glm::vec3(0.0f, -1.0f,  0.0f)  // -Z
        };

        return glm::lookAt(position, targets[face], ups[face]);
    }

    /// Projection matrix for a cube face: 90 deg FOV, aspect 1.
    ///
    /// Deliberately NO `proj[1][1] *= -1` Vulkan Y-flip. The cube-map
    /// (sc,tc,ma) face-selection convention is left-handed; combined with the
    /// POSITIVE-height viewport begin_face_pass() uses, applying the usual
    /// projection Y-flip on top would require a left-handed view basis that
    /// glm::lookAt (right-handed) cannot produce for any `up` -- i.e. it is
    /// wrong for every face, not a per-face sign issue. Dropping the flip and
    /// keeping get_face_view()'s ups[] as-is is what makes NDC.xy agree with
    /// cube_face_direction(). (This mirrors how the main render pipeline
    /// avoids the same flip via a negative-height viewport instead --
    /// see camera_component.h / offscreen_target.h.)
    ///
    /// Consequence: framebuffer-space winding here is mirrored relative to
    /// the main pass, so a pipeline drawing into render_pass() must use
    /// VK_CULL_MODE_NONE (or FRONT_FACE_CLOCKWISE), not the main pass's
    /// back-face/CCW convention.
    static glm::mat4 get_face_projection(float near_plane = 0.05f, float far_plane = 200.0f) {
        return glm::perspective(glm::radians(90.0f), 1.0f, near_plane, far_plane);
    }

    /// Combined proj * view for a cube face. Kept for callers that just need
    /// a single matrix; the geometry-capture path needs view/proj separate
    /// (pbr.vert multiplies them itself), so use get_face_view()/
    /// get_face_projection() directly there.
    static glm::mat4 get_face_matrix(uint32_t face, const glm::vec3& position,
                                     float near_plane = 0.05f, float far_plane = 200.0f) {
        return get_face_projection(near_plane, far_plane) * get_face_view(face, position);
    }

private:
    void create_resources_() {
        // 1. Create Cubemap Color Image (R16G16B16A16_SFLOAT, 6 array layers)
        VkImageCreateInfo image_info{};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.extent.width  = res_;
        image_info.extent.height = res_;
        image_info.extent.depth  = 1;
        image_info.mipLevels     = mip_levels_;
        image_info.arrayLayers   = 6;
        image_info.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        GFX_VK_CHECK(vmaCreateImage(allocator_.handle(), &image_info, &alloc_info,
                                    &cubemap_image_, &cubemap_allocation_, nullptr));

        // 2. Full Cubemap View for Sampling
        VkImageViewCreateInfo view_info{};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                           = cubemap_image_;
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
        view_info.format                          = VK_FORMAT_R16G16B16A16_SFLOAT;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = mip_levels_;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 6;
        GFX_VK_CHECK(vkCreateImageView(device_.handle(), &view_info, nullptr, &cubemap_view_));

        // 2b. Mip-0-only Cube View (prefilter pass's read source once mip 0
        //     holds a real geometry capture).
        VkImageViewCreateInfo mip0_view_info = view_info;
        mip0_view_info.subresourceRange.levelCount = 1;
        GFX_VK_CHECK(vkCreateImageView(device_.handle(), &mip0_view_info, nullptr, &mip0_cube_view_));

        // 3. Per-face Image Views for Render Targets
        for (uint32_t f = 0; f < 6; ++f) {
            VkImageViewCreateInfo face_view_info{};
            face_view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            face_view_info.image                           = cubemap_image_;
            face_view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            face_view_info.format                          = VK_FORMAT_R16G16B16A16_SFLOAT;
            face_view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            face_view_info.subresourceRange.baseMipLevel   = 0;
            face_view_info.subresourceRange.levelCount     = 1;
            face_view_info.subresourceRange.baseArrayLayer = f;
            face_view_info.subresourceRange.layerCount     = 1;
            GFX_VK_CHECK(vkCreateImageView(device_.handle(), &face_view_info, nullptr, &face_views_[f]));
        }

        // 4. Depth Attachment Image for face pass
        depth_image_ = std::make_unique<memory::Image>(
            device_, allocator_, res_, res_,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT
        );

        // 5. Render Pass (Color + Depth)
        render_pass_ = std::make_unique<pipeline::RenderPass>(
            device_,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        );

        // 6. Framebuffers per face
        for (uint32_t f = 0; f < 6; ++f) {
            VkImageView attachments[] = { face_views_[f], depth_image_->view() };

            VkFramebufferCreateInfo fb_info{};
            fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass      = render_pass_->handle();
            fb_info.attachmentCount = 2;
            fb_info.pAttachments    = attachments;
            fb_info.width           = res_;
            fb_info.height          = res_;
            fb_info.layers          = 1;
            GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr, &face_fbs_[f]));
        }

        // 7. Color-only render pass for the prefilter mip chain. finalLayout stays
        //    COLOR_ATTACHMENT_OPTIMAL so that a single transition_to_shader_read(cmd)
        //    issued after the whole chain is written (srcAccess = COLOR_ATTACHMENT_WRITE)
        //    is both correct and sufficient -- nothing reads this image between the
        //    per-(face,mip) draws, so no inter-mip barrier is required.
        prefilter_pass_ = std::make_unique<pipeline::RenderPass>(
            device_,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        );

        // 8. Per-(face, mip) image views and framebuffers for the prefilter pass.
        face_mip_views_.assign(6 * mip_levels_, VK_NULL_HANDLE);
        face_mip_fbs_.assign(6 * mip_levels_, VK_NULL_HANDLE);

        for (uint32_t f = 0; f < 6; ++f) {
            for (uint32_t m = 0; m < mip_levels_; ++m) {
                const uint32_t idx   = f * mip_levels_ + m;
                const uint32_t m_res = mip_resolution(m);

                VkImageViewCreateInfo vi{};
                vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vi.image                           = cubemap_image_;
                vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
                vi.format                          = VK_FORMAT_R16G16B16A16_SFLOAT;
                vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                vi.subresourceRange.baseMipLevel   = m;
                vi.subresourceRange.levelCount     = 1;
                vi.subresourceRange.baseArrayLayer = f;
                vi.subresourceRange.layerCount     = 1;
                GFX_VK_CHECK(vkCreateImageView(device_.handle(), &vi, nullptr, &face_mip_views_[idx]));

                VkFramebufferCreateInfo fb{};
                fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                fb.renderPass      = prefilter_pass_->handle();
                fb.attachmentCount = 1;
                fb.pAttachments    = &face_mip_views_[idx];
                fb.width           = m_res;
                fb.height          = m_res;
                fb.layers          = 1;
                GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb, nullptr, &face_mip_fbs_[idx]));
            }
        }
    }

    void destroy_framebuffers_() {
        for (auto fb : face_mip_fbs_) {
            if (fb != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_.handle(), fb, nullptr);
            }
        }
        face_mip_fbs_.clear();
        for (auto v : face_mip_views_) {
            if (v != VK_NULL_HANDLE) {
                vkDestroyImageView(device_.handle(), v, nullptr);
            }
        }
        face_mip_views_.clear();

        for (uint32_t f = 0; f < 6; ++f) {
            if (face_fbs_[f] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_.handle(), face_fbs_[f], nullptr);
                face_fbs_[f] = VK_NULL_HANDLE;
            }
            if (face_views_[f] != VK_NULL_HANDLE) {
                vkDestroyImageView(device_.handle(), face_views_[f], nullptr);
                face_views_[f] = VK_NULL_HANDLE;
            }
        }
        if (cubemap_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.handle(), cubemap_view_, nullptr);
            cubemap_view_ = VK_NULL_HANDLE;
        }
        if (mip0_cube_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.handle(), mip0_cube_view_, nullptr);
            mip0_cube_view_ = VK_NULL_HANDLE;
        }
        if (cubemap_image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_.handle(), cubemap_image_, cubemap_allocation_);
            cubemap_image_ = VK_NULL_HANDLE;
            cubemap_allocation_ = VK_NULL_HANDLE;
        }
    }

    core::Device& device_;
    memory::Allocator& allocator_;
    uint32_t res_;
    uint32_t mip_levels_;

    VkImage       cubemap_image_      = VK_NULL_HANDLE;
    VmaAllocation cubemap_allocation_ = VK_NULL_HANDLE;
    VkImageView   cubemap_view_       = VK_NULL_HANDLE;
    VkImageView   mip0_cube_view_     = VK_NULL_HANDLE;
    VkImageView   face_views_[6]      = {VK_NULL_HANDLE};
    VkFramebuffer face_fbs_[6]        = {VK_NULL_HANDLE};

    std::unique_ptr<memory::Image> depth_image_;
    std::unique_ptr<pipeline::RenderPass> render_pass_;

    // Per-(face, mip) views/framebuffers for the color-only prefilter pass.
    // Indexed as face * mip_levels_ + mip.
    std::vector<VkImageView>   face_mip_views_;
    std::vector<VkFramebuffer> face_mip_fbs_;
    std::unique_ptr<pipeline::RenderPass> prefilter_pass_;
};

} // namespace targets
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_CUBEMAP_TARGET_H
