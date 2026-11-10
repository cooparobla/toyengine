/**
 * @file shadow_map_target.h
 * @brief Depth render targets for Directional Light, Point Light Cubemap, and
 *        Spot Light shadow maps.
 */

#ifndef GFXCOOPA_ENGINE_TARGETS_SHADOW_MAP_TARGET_H
#define GFXCOOPA_ENGINE_TARGETS_SHADOW_MAP_TARGET_H

#include <volk/volk.h>
#include <algorithm>
#include <utility>
#include <vector>
#include <memory>
#include <stdexcept>
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
 * @class ShadowMapTarget
 * @brief Manages the directional shadow atlas, point light cubemap depth target,
 *        and spot light depth map.
 *
 * The directional map is an ATLAS of `dir_cascades` square tiles (up to 8), each `dir_res`
 * texels on a side, laid out 1x1 / 2x1 / 2x2 / 3x2 / 4x2 (see grid_for()). Each tile holds
 * one level -- an independent ortho fit, either to one slice of the camera's depth range
 * (the frustum/focus fits) or to one concentric ring around the camera (the clipmap fit) --
 * so near geometry gets a shadow box a few metres across while distant geometry keeps a
 * coarse one. A one-tile target is bit-identical to a plain single shadow map.
 *
 * The tiles share one image, one VkRenderPass and one framebuffer: the cascade loop
 * (record_directional_shadow_() in toyengine's toy_render_pipeline.h) calls
 * set_cascade_viewport() between draws rather than beginning a pass per cascade. That keeps
 * the sampler a plain sampler2D/sampler2DShadow, so every shadow-sampling kernel
 * (toyengine's gfx/shadow_sampling.glsl) works on a cascade unchanged -- only the uv remap
 * into the tile is cascade-specific.
 */
class ShadowMapTarget {
public:
    ShadowMapTarget(core::Device& device, memory::Allocator& allocator,
                    uint32_t dir_res = 2048, uint32_t cube_res = 512, uint32_t spot_res = 1024,
                    uint32_t dir_cascades = 1)
        : device_(device), allocator_(allocator),
          dir_res_(dir_res), cube_res_(cube_res), spot_res_(spot_res),
          dir_cascades_(std::clamp(dir_cascades, 1u, 8u)),
          dir_grid_(grid_for(dir_cascades))
    {
        create_resources_();
    }

    /**
     * @brief The atlas tile grid a cascade count uses: `.first` tiles per row, `.second` rows.
     *
     * 1 -> 1x1, 2 -> 2x1, 3/4 -> 2x2, 5/6 -> 3x2, 7/8 -> 4x2. Widest-first and never a row
     * that sits empty: the image is allocated at `dir_res * grid`, so a wasted row is wasted
     * VRAM. Mirrors toy::render::cascade_atlas_grid(), which the shader-side uv math is
     * derived from -- the two must agree or every tile lookup lands in the wrong place.
     */
    static std::pair<uint32_t, uint32_t> grid_for(uint32_t cascades) {
        const uint32_t n = std::clamp(cascades, 1u, 8u);
        const uint32_t columns = n <= 2u ? n : n <= 4u ? 2u : n <= 6u ? 3u : 4u;
        return {columns, (n + columns - 1u) / columns};
    }

    ~ShadowMapTarget() {
        destroy_framebuffers_();
    }

    ShadowMapTarget(const ShadowMapTarget&) = delete;
    ShadowMapTarget& operator=(const ShadowMapTarget&) = delete;

    // --- Directional Shadow Pass ---

    /**
     * @brief Opens the one render pass that covers the WHOLE atlas and clears every tile.
     *
     * The viewport starts at the full atlas; a multi-cascade caller narrows it per cascade
     * with set_cascade_viewport() before each cascade's draws. Clearing once here (rather
     * than per tile) is why the cascade loop needs no extra pass or barrier.
     */
    void begin_directional_pass(command::CommandBuffer& cmd) const {
        VkClearValue clear_value{};
        clear_value.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = dir_render_pass_->handle();
        rp_info.framebuffer       = dir_framebuffer_;
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = {dir_atlas_width(), dir_atlas_height()};
        rp_info.clearValueCount   = 1;
        rp_info.pClearValues      = &clear_value;

        vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(dir_atlas_width()),
                         static_cast<float>(dir_atlas_height()));
        cmd.set_scissor(0, 0, dir_atlas_width(), dir_atlas_height());
    }

    /**
     * @brief Restricts subsequent draws to cascade `index`'s tile of the open atlas pass.
     *
     * Pure dynamic viewport/scissor state -- the directional shadow pipeline already
     * declares both dynamic (begin_directional_pass sets them), so a cascade needs no
     * pipeline variant and no second render pass.
     *
     * @param cmd   Command buffer inside begin_directional_pass()/end_directional_pass().
     * @param index Cascade index; clamped to the live cascade count.
     */
    void set_cascade_viewport(command::CommandBuffer& cmd, uint32_t index) const {
        const uint32_t i = std::min(index, dir_cascades_ - 1u);
        const uint32_t x = (i % dir_grid_.first) * dir_res_;
        const uint32_t y = (i / dir_grid_.first) * dir_res_;
        cmd.set_viewport(static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(dir_res_), static_cast<float>(dir_res_));
        cmd.set_scissor(static_cast<int32_t>(x), static_cast<int32_t>(y), dir_res_, dir_res_);
    }

    void end_directional_pass(command::CommandBuffer& cmd) const {
        cmd.end_render_pass();
    }

    // --- Spot Light Pass ---
    //
    // Deliberately reuses dir_render_pass_ (not a new VkRenderPass) -- its attachment
    // description (D32_SFLOAT, UNDEFINED -> SHADER_READ_ONLY_OPTIMAL) is identical to
    // what a spot map needs, and render-pass *compatibility* (matching attachment
    // descriptions, not matching framebuffer size) is what lets the existing
    // directional shadow pipeline record into spot_framebuffer_ with no new pipeline
    // variant. Only the framebuffer and resolution differ.

    void begin_spot_pass(command::CommandBuffer& cmd) const {
        VkClearValue clear_value{};
        clear_value.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = dir_render_pass_->handle();
        rp_info.framebuffer       = spot_framebuffer_;
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = {spot_res_, spot_res_};
        rp_info.clearValueCount   = 1;
        rp_info.pClearValues      = &clear_value;

        vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(spot_res_), static_cast<float>(spot_res_));
        cmd.set_scissor(0, 0, spot_res_, spot_res_);
    }

    void end_spot_pass(command::CommandBuffer& cmd) const {
        cmd.end_render_pass();
    }

    // --- Point Light Cubemap Face Pass ---

    void begin_cube_face_pass(command::CommandBuffer& cmd, uint32_t face_index) const {
        VkClearValue clear_value{};
        clear_value.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = cube_render_pass_->handle();
        rp_info.framebuffer       = cube_face_framebuffers_[face_index];
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = {cube_res_, cube_res_};
        rp_info.clearValueCount   = 1;
        rp_info.pClearValues      = &clear_value;

        vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(cube_res_), static_cast<float>(cube_res_));
        cmd.set_scissor(0, 0, cube_res_, cube_res_);
    }

    void end_cube_face_pass(command::CommandBuffer& cmd) const {
        cmd.end_render_pass();
    }

    // --- Layout Transitions ---

    void transition_cube_to_shader_read(command::CommandBuffer& cmd) const {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = cube_image_;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 6;
        barrier.srcAccessMask                   = 0;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            cmd.handle(),
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );
    }

    void transition_spot_to_shader_read(command::CommandBuffer& cmd) const {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = spot_depth_image_->handle();
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barrier.srcAccessMask                   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            cmd.handle(),
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );
    }

    void transition_dir_to_shader_read(command::CommandBuffer& cmd) const {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = dir_depth_image_->handle();
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barrier.srcAccessMask                   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            cmd.handle(),
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );
    }

    // --- Cubemap View Matrices ---

    static glm::mat4 get_cube_face_matrix(uint32_t face_index, const glm::vec3& light_pos, float range) {
        glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, range);

        // 6 cubemap directions: +X, -X, +Y, -Y, +Z, -Z
        glm::vec3 targets[6] = {
            light_pos + glm::vec3( 1.0f,  0.0f,  0.0f), // +X
            light_pos + glm::vec3(-1.0f,  0.0f,  0.0f), // -X
            light_pos + glm::vec3( 0.0f,  1.0f,  0.0f), // +Y
            light_pos + glm::vec3( 0.0f, -1.0f,  0.0f), // -Y
            light_pos + glm::vec3( 0.0f,  0.0f,  1.0f), // +Z
            light_pos + glm::vec3( 0.0f,  0.0f, -1.0f)  // -Z
        };
        glm::vec3 ups[6] = {
            glm::vec3(0.0f, -1.0f,  0.0f), // +X
            glm::vec3(0.0f, -1.0f,  0.0f), // -X
            glm::vec3(0.0f,  0.0f,  1.0f), // +Y
            glm::vec3(0.0f,  0.0f, -1.0f), // -Y
            glm::vec3(0.0f, -1.0f,  0.0f), // +Z
            glm::vec3(0.0f, -1.0f,  0.0f)  // -Z
        };

        glm::mat4 view = glm::lookAt(light_pos, targets[face_index], ups[face_index]);
        return proj * view;
    }

    /**
     * @brief Builds a spot light's light-space (projection * view) matrix.
     *
     * A single perspective frustum aimed along `dir`, sized to exactly cover the
     * cone at `outer_degrees` -- unlike the cube map's fixed 90-degree faces, a
     * spot's FOV is the cone itself, so no shadow-map texels are wasted outside it.
     * Uses the same RH_ZO convention (no Y-flip) as compute_dir_shadow_fit()'s
     * orthoRH_ZO, so both maps share the shader's `proj_coords.xy * 0.5 + 0.5`
     * mapping in calc_dir_shadow()/calc_spot_shadow().
     *
     * @param pos            World-space light position.
     * @param dir            Normalized world-space aim direction.
     * @param outer_degrees  Cone outer half-angle, in degrees (clamped to [1, 89]
     *                       by the caller -- SpotLightComponent::clamped_outer_angle()).
     * @param range          Light range; used as the far clip plane.
     * @return Combined projection * view matrix.
     */
    static glm::mat4 get_spot_matrix(const glm::vec3& pos, const glm::vec3& dir,
                                     float outer_degrees, float range) {
        // Same degenerate-up guard as compute_dir_shadow_fit() (toy_render_math.h) --
        // this engine is Z-up, so a near-vertical aim needs a different up axis to
        // keep lookAt() from degenerating.
        const glm::vec3 up = (std::abs(dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                        : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(2.0f * outer_degrees), 1.0f,
                                               0.1f, std::max(range, 0.2f));
        glm::mat4 view = glm::lookAt(pos, pos + dir, up);
        return proj * view;
    }

    // --- Accessors ---

    /// Cascades this atlas holds; 1 means a plain single shadow map.
    uint32_t dir_cascade_count() const { return dir_cascades_; }
    /// Edge length of one cascade TILE in texels -- what the per-cascade ortho fit snaps to.
    uint32_t dir_tile_resolution() const { return dir_res_; }
    uint32_t dir_atlas_width() const { return dir_res_ * dir_grid_.first; }
    /** @brief Cascade tiles per atlas row (cascade c sits at column c % this, row c / this). */
    uint32_t dir_grid_columns() const { return dir_grid_.first; }
    uint32_t dir_atlas_height() const { return dir_res_ * dir_grid_.second; }

    VkImageView dir_shadow_view() const { return dir_depth_image_->view(); }
    /// The directional atlas image itself -- for the static-caster cache, which copies tiles
    /// into it (toyengine's DirShadowCache; see the TRANSFER_DST note in create_resources_()).
    VkImage dir_shadow_image() const { return dir_depth_image_->handle(); }
    VkImageView cube_shadow_view() const { return cube_array_view_; }
    VkImageView spot_shadow_view() const { return spot_depth_image_->view(); }

    /// TextureView identities of the same maps, for consumers whose binding API
    /// takes gfxcoopa's opaque handle (e.g. FullscreenStage's descriptor sets).
    coopa::gfx::TextureView dir_shadow_view_typed() const { return dir_depth_image_->view_typed(); }
    /// The cube array view is built by hand here (not owned by an Image), so it is wrapped
    /// directly rather than delegating to Image::view_typed().
    coopa::gfx::TextureView cube_shadow_view_typed() const { return detail::wrap(cube_array_view_); }
    coopa::gfx::TextureView spot_shadow_view_typed() const { return spot_depth_image_->view_typed(); }

    pipeline::RenderPass& dir_render_pass() const { return *dir_render_pass_; }
    pipeline::RenderPass& cube_render_pass() const { return *cube_render_pass_; }

private:
    void create_resources_() {
        // 1. Directional Depth Atlas (2D) -- dir_cascades_ tiles of dir_res_ in a
        //    dir_grid_ layout; one cascade makes this exactly a dir_res_ square map.
        // TRANSFER_DST is for the static-caster cache: toyengine's DirShadowCache renders a
        // level's unmoving depth once into its own image and copies the tile in each frame
        // (vkCmdCopyImage), so the one image every shader samples stays the live atlas and no
        // descriptor is ever rebound. Declared unconditionally -- a usage flag the cache
        // happens not to exercise costs nothing.
        dir_depth_image_ = std::make_unique<memory::Image>(
            device_, allocator_, dir_atlas_width(), dir_atlas_height(),
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT
        );

        dir_render_pass_ = std::make_unique<pipeline::RenderPass>(
            device_,
            VK_FORMAT_UNDEFINED, // no color
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL // depth final layout
        );

        VkImageView dir_view = dir_depth_image_->view();
        VkFramebufferCreateInfo fb_info{};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = dir_render_pass_->handle();
        fb_info.attachmentCount = 1;
        fb_info.pAttachments    = &dir_view;
        fb_info.width           = dir_atlas_width();
        fb_info.height          = dir_atlas_height();
        fb_info.layers          = 1;
        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr, &dir_framebuffer_));

        // 2. Spot Light Depth Image (2D) -- reuses dir_render_pass_ (see begin_spot_pass()'s
        //    doc for why that's compatible), its own image/view/framebuffer at spot_res_.
        spot_depth_image_ = std::make_unique<memory::Image>(
            device_, allocator_, spot_res_, spot_res_,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT
        );

        VkImageView spot_view = spot_depth_image_->view();
        VkFramebufferCreateInfo spot_fb_info{};
        spot_fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        spot_fb_info.renderPass      = dir_render_pass_->handle();
        spot_fb_info.attachmentCount = 1;
        spot_fb_info.pAttachments    = &spot_view;
        spot_fb_info.width           = spot_res_;
        spot_fb_info.height          = spot_res_;
        spot_fb_info.layers          = 1;
        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &spot_fb_info, nullptr, &spot_framebuffer_));

        // 3. Point Light Cubemap Depth Image (Cubemap, 6 layers)
        VkImageCreateInfo image_info{};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.extent.width  = cube_res_;
        image_info.extent.height = cube_res_;
        image_info.extent.depth  = 1;
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 6;
        image_info.format        = VK_FORMAT_D32_SFLOAT;
        image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        GFX_VK_CHECK(vmaCreateImage(allocator_.handle(), &image_info, &alloc_info,
                                    &cube_image_, &cube_allocation_, nullptr));

        // Create Cubemap ImageView (all 6 layers)
        VkImageViewCreateInfo view_info{};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                           = cube_image_;
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
        view_info.format                          = VK_FORMAT_D32_SFLOAT;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 6;
        GFX_VK_CHECK(vkCreateImageView(device_.handle(), &view_info, nullptr, &cube_array_view_));

        // Create per-face 2D ImageViews & Framebuffers
        cube_render_pass_ = std::make_unique<pipeline::RenderPass>(
            device_,
            VK_FORMAT_UNDEFINED,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );

        cube_face_views_.resize(6);
        cube_face_framebuffers_.resize(6);
        for (uint32_t i = 0; i < 6; ++i) {
            VkImageViewCreateInfo face_view_info{};
            face_view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            face_view_info.image                           = cube_image_;
            face_view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            face_view_info.format                          = VK_FORMAT_D32_SFLOAT;
            face_view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
            face_view_info.subresourceRange.baseMipLevel   = 0;
            face_view_info.subresourceRange.levelCount     = 1;
            face_view_info.subresourceRange.baseArrayLayer = i;
            face_view_info.subresourceRange.layerCount     = 1;
            GFX_VK_CHECK(vkCreateImageView(device_.handle(), &face_view_info, nullptr, &cube_face_views_[i]));

            VkFramebufferCreateInfo face_fb_info{};
            face_fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            face_fb_info.renderPass      = cube_render_pass_->handle();
            face_fb_info.attachmentCount = 1;
            face_fb_info.pAttachments    = &cube_face_views_[i];
            face_fb_info.width           = cube_res_;
            face_fb_info.height          = cube_res_;
            face_fb_info.layers          = 1;
            GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &face_fb_info, nullptr, &cube_face_framebuffers_[i]));
        }
    }

    void destroy_framebuffers_() {
        if (dir_framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.handle(), dir_framebuffer_, nullptr);
            dir_framebuffer_ = VK_NULL_HANDLE;
        }
        if (spot_framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.handle(), spot_framebuffer_, nullptr);
            spot_framebuffer_ = VK_NULL_HANDLE;
        }
        for (auto fb : cube_face_framebuffers_) {
            if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_.handle(), fb, nullptr);
        }
        cube_face_framebuffers_.clear();
        for (auto v : cube_face_views_) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(device_.handle(), v, nullptr);
        }
        cube_face_views_.clear();
        if (cube_array_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.handle(), cube_array_view_, nullptr);
            cube_array_view_ = VK_NULL_HANDLE;
        }
        if (cube_image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_.handle(), cube_image_, cube_allocation_);
            cube_image_ = VK_NULL_HANDLE;
            cube_allocation_ = VK_NULL_HANDLE;
        }
    }

    core::Device&      device_;
    memory::Allocator& allocator_;
    uint32_t           dir_res_;   ///< One cascade TILE's edge, not the atlas edge.
    uint32_t           cube_res_;
    uint32_t           spot_res_;
    uint32_t           dir_cascades_ = 1;
    std::pair<uint32_t, uint32_t> dir_grid_{1u, 1u}; ///< Tiles per row, rows.

    std::unique_ptr<memory::Image>        dir_depth_image_;
    std::unique_ptr<pipeline::RenderPass> dir_render_pass_;
    VkFramebuffer                         dir_framebuffer_ = VK_NULL_HANDLE;

    std::unique_ptr<memory::Image>        spot_depth_image_;
    VkFramebuffer                         spot_framebuffer_ = VK_NULL_HANDLE; // uses dir_render_pass_

    VkImage                               cube_image_      = VK_NULL_HANDLE;
    VmaAllocation                         cube_allocation_ = VK_NULL_HANDLE;
    VkImageView                           cube_array_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView>              cube_face_views_;
    std::vector<VkFramebuffer>            cube_face_framebuffers_;
    std::unique_ptr<pipeline::RenderPass> cube_render_pass_;
};

} // namespace targets
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_SHADOW_MAP_TARGET_H
