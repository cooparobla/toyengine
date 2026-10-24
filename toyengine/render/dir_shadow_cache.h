/**
 * @file dir_shadow_cache.h
 * @brief Static-caster depth cache for the directional shadow atlas: a level's unmoving
 *        geometry is rendered once and copied in each frame, so only movers are redrawn.
 *
 * Unreal's static shadow caching, applied to the sun's cascades -- and the reason the clipmap
 * fit exists. A clipmap level's matrix is piecewise constant in the camera position (see
 * compute_dir_shadow_fit_sphere's snap step), so the depth of everything static in that level
 * stays valid until the camera crosses a snap cell. Caching it turns a zoomed-out frame from
 * "redraw the whole world eight times" into "copy eight tiles and draw the few things that
 * moved".
 *
 * The flow mirrors LocalShadowAtlas, which does the same job for point and spot lights, with
 * one difference that drives the whole design: **the live image is BORROWED from
 * ShadowMapTarget, not owned.** Its view is bound into descriptor sets once at pipeline
 * construction (the shadow set's compare and raw samplers, plus both volumetrics passes), and
 * the pipeline overlaps frames in flight with no per-frame wait, so no descriptor may ever be
 * rebound. Compositing the cache INTO that one image before lighting keeps every consumer --
 * PCF, the PCSS blocker search on the raw binding, the volumetrics march, the debug views --
 * reading one coherent map with no new bindings at all.
 *
 * Per frame:
 *   1. begin_frame()                     live atlas shader-read -> transfer-dst
 *   2. begin_static_pass() ... end()     re-render only the levels whose content changed,
 *                                        into the CACHE image (LOAD, so untouched tiles keep
 *                                        their depth)
 *   3. copy_levels()                     cache -> live, per tile
 *   4. begin_dynamic_pass() ... end()    draw movers over the copied depth, in the LIVE image;
 *                                        ends shader-read for the lighting pass
 *
 * Both render passes LOAD their attachment and are render-pass-compatible with
 * ShadowMapTarget's directional pass (one D32 depth attachment, one subpass), so the shadow
 * pipelines draw into either without a variant.
 */

#ifndef TOYENGINE_RENDER_DIR_SHADOW_CACHE_H
#define TOYENGINE_RENDER_DIR_SHADOW_CACHE_H

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <volk/volk.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace toy {
namespace render {

/**
 * @class DirShadowCache
 * @brief A second copy of the directional atlas holding static casters' depth, plus the
 *        render passes and barriers that composite it into the live atlas each frame.
 *
 * Owns only the cache image. The live image, its extent and its tile layout all come from the
 * ShadowMapTarget handed to the constructor.
 */
class DirShadowCache {
public:
    /**
     * @brief Allocates the cache image and its passes against an existing directional atlas.
     *
     * @param device      Logical device.
     * @param allocator   VMA allocator.
     * @param live_image  The directional atlas image (ShadowMapTarget::dir_depth_image_handle()).
     * @param live_view   That image's view -- the one already bound into the descriptor sets.
     * @param width       Atlas width in texels.
     * @param height      Atlas height in texels.
     */
    DirShadowCache(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                   VkImage live_image, VkImageView live_view, uint32_t width, uint32_t height);
    ~DirShadowCache();

    DirShadowCache(const DirShadowCache&) = delete;
    DirShadowCache& operator=(const DirShadowCache&) = delete;

    /// Clears the CACHE image to the far plane once, leaving it transfer-src. The live atlas
    /// is deliberately untouched -- it is borrowed and may hold a frame still being read; see
    /// the implementation.
    void initialize(coopa::gfx::command::CommandBuffer& cmd);

    /// Live atlas shader-read -> transfer-dst, ordering the copy after the previous frame's
    /// lighting reads. The one barrier that makes overlapping frames in flight safe here.
    void begin_frame(coopa::gfx::command::CommandBuffer& cmd) const;

    /// Begins the pass that draws static casters into the CACHE image (LOAD: tiles not
    /// re-rendered this frame keep the depth they already hold).
    void begin_static_pass(coopa::gfx::command::CommandBuffer& cmd) const {
        begin_(cmd, cache_pass_, cache_fb_);
    }
    void end_static_pass(coopa::gfx::command::CommandBuffer& cmd) const {
        vkCmdEndRenderPass(cmd.handle());
    }

    /// Begins the pass that draws movers into the LIVE image over the copied static depth.
    /// Ends in SHADER_READ_ONLY_OPTIMAL, ready for the lighting pass.
    void begin_dynamic_pass(coopa::gfx::command::CommandBuffer& cmd) const {
        begin_(cmd, live_pass_, live_fb_);
    }
    void end_dynamic_pass(coopa::gfx::command::CommandBuffer& cmd) const {
        vkCmdEndRenderPass(cmd.handle());
    }

    /// Restricts drawing to one atlas tile. A POSITIVE-height viewport, matching the
    /// unflipped light matrices and FrontFace::Clockwise the shadow pipelines are built with
    /// (see toy_render_math.h's compute_dir_shadow_fit_sphere on why there is no Y-flip).
    void set_tile(coopa::gfx::command::CommandBuffer& cmd, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h) const;

    /// Clears one tile to the far plane inside an open pass -- a level about to be
    /// re-rendered must not keep its previous content.
    void clear_tile(coopa::gfx::command::CommandBuffer& cmd, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h) const;

    /**
     * @brief Copies the named tiles cache -> live (vkCmdCopyImage), outside any pass.
     *
     * Called for EVERY live tile every frame, not only re-rendered ones: a tile the previous
     * frame drew movers into must be reset to its static depth before this frame's movers go
     * on top, or last frame's silhouettes linger. The copy is pure bandwidth with no shading,
     * which is exactly the cost the cache is trading draw calls for.
     */
    void copy_levels(coopa::gfx::command::CommandBuffer& cmd,
                     const std::vector<VkImageCopy>& regions) const;

    /// Builds the copy region for one square tile at (x, y).
    static VkImageCopy tile_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

private:
    VkRenderPass  make_pass_(VkImageLayout initial, VkImageLayout final_layout);
    VkFramebuffer make_fb_(VkRenderPass pass, VkImageView view);
    void begin_(coopa::gfx::command::CommandBuffer& cmd, VkRenderPass pass, VkFramebuffer fb) const;
    void clear_image_(coopa::gfx::command::CommandBuffer& cmd, VkImage image, VkImageLayout to,
                      VkAccessFlags to_access, VkPipelineStageFlags to_stage) const;
    static void barrier_(coopa::gfx::command::CommandBuffer& cmd, VkImage image,
                         VkImageLayout from, VkImageLayout to,
                         VkAccessFlags src_access, VkAccessFlags dst_access,
                         VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

    coopa::gfx::core::Device& device_;
    VkImage  live_image_ = VK_NULL_HANDLE;   // borrowed: ShadowMapTarget owns it
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    std::unique_ptr<coopa::gfx::memory::Image> cache_;
    VkRenderPass  live_pass_  = VK_NULL_HANDLE;
    VkRenderPass  cache_pass_ = VK_NULL_HANDLE;
    VkFramebuffer live_fb_    = VK_NULL_HANDLE;
    VkFramebuffer cache_fb_   = VK_NULL_HANDLE;
    bool initialized_ = false;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_DIR_SHADOW_CACHE_H
