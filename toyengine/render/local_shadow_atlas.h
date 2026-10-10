/**
 * @file local_shadow_atlas.h
 * @brief The point/spot shadow atlas: every shadowed local light's depth tiles in one image,
 *        plus an optional static-caster cache copied in each frame.
 */
#ifndef TOYENGINE_RENDER_LOCAL_SHADOW_ATLAS_H
#define TOYENGINE_RENDER_LOCAL_SHADOW_ATLAS_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>

namespace toy {
namespace render {

/** @brief A tile rectangle in atlas texels. */
struct AtlasRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const AtlasRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

/**
 * @brief Shelf-packs rectangles into a square atlas, in the order given.
 *
 * Each item goes on the first open shelf with enough height and remaining width, else on a new
 * shelf below the last. Deterministic for a given input order, which is what keeps a light's
 * tile (and so its cached static depth) in place frame to frame. An item that fits nowhere gets
 * w = 0.
 */
std::vector<AtlasRect> pack_shadow_atlas(const std::vector<std::pair<uint32_t, uint32_t>>& sizes,
                                                uint32_t atlas);

/// The six cube-face directions and up vectors, in the face order gfx/local_shadow.glsl's
/// gfx_local_shadow_face() selects by major axis: +X, -X, +Y, -Y, +Z, -Z.
glm::mat4 local_shadow_point_face_view(uint32_t face, const glm::vec3& pos);

/**
 * @class LocalShadowAtlas
 * @brief Owns the local-light shadow atlas (D32, sampled by gfx/local_shadow.glsl) and, when
 *        caching, a second atlas of the same layout holding only STATIC casters.
 *
 * Per frame (see ToyRenderPipeline::record_local_shadows_()):
 *  1. begin_frame(): the live atlas goes from shader-read to transfer-dst.
 *  2. Lights whose static content changed re-render it into the cache atlas
 *     (begin_static_pass / clear_tile / draws / end_static_pass).
 *  3. copy_static(): every active light's tiles are copied cache -> live.
 *  4. begin_dynamic_pass(): moving casters (and everything, with caching off) draw on top;
 *     the pass ends in shader-read for the lighting pass.
 *
 * Both render passes LOAD their attachment and are render-pass-compatible with
 * ShadowMapTarget's directional pass (one D32 attachment, one subpass), so the existing
 * directional shadow pipelines and their per-material variants draw into either unchanged.
 */
class LocalShadowAtlas {
public:
    LocalShadowAtlas(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                     uint32_t size, bool static_cache);

    ~LocalShadowAtlas();

    LocalShadowAtlas(const LocalShadowAtlas&) = delete;
    LocalShadowAtlas& operator=(const LocalShadowAtlas&) = delete;

    uint32_t size() const { return size_; }
    bool     caching() const { return caching_; }
    VkImageView view() const { return live_->view(); }
    coopa::gfx::TextureView view_typed() const { return live_->view_typed(); }

    /**
     * @brief Once, before the atlas is first sampled: clears both images to the far plane and
     *        leaves the live one shader-readable (its descriptor is bound from frame 0, whether
     *        or not any light casts) and the cache transfer-readable.
     */
    void initialize(coopa::gfx::command::CommandBuffer& cmd);

    /** @brief Live atlas: shader-read -> transfer-dst, ready for the cache copy / live pass. */
    void begin_frame(coopa::gfx::command::CommandBuffer& cmd) const;

    void begin_static_pass(coopa::gfx::command::CommandBuffer& cmd) const { begin_(cmd, cache_pass_, cache_fb_); }
    void end_static_pass(coopa::gfx::command::CommandBuffer& cmd) const { cmd.end_render_pass(); }
    void begin_dynamic_pass(coopa::gfx::command::CommandBuffer& cmd) const { begin_(cmd, live_pass_, live_fb_); }
    void end_dynamic_pass(coopa::gfx::command::CommandBuffer& cmd) const { cmd.end_render_pass(); }

    /** @brief Viewport + scissor to one tile (positive height, matching the other shadow maps). */
    void set_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const;

    /** @brief Inside an open pass: resets one rectangle to the far plane. */
    void clear_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const;

    /** @brief Copies each rectangle of the static cache into the live atlas (outside any pass). */
    void copy_static(coopa::gfx::command::CommandBuffer& cmd, const std::vector<AtlasRect>& rects) const;

private:
    /// One D32 attachment, LOAD/STORE, with explicit dependencies: in from earlier transfers
    /// (the cache copy, init clears) and depth writes; out to transfer reads (the copy) and
    /// fragment-shader reads (lighting, volumetrics).
    VkRenderPass make_pass_(VkImageLayout initial, VkImageLayout final_layout);

    VkFramebuffer make_fb_(VkRenderPass pass, VkImageView view);

    void begin_(coopa::gfx::command::CommandBuffer& cmd, VkRenderPass pass, VkFramebuffer fb) const;

    void clear_image_(coopa::gfx::command::CommandBuffer& cmd, VkImage image, VkImageLayout to,
                      VkAccessFlags to_access, VkPipelineStageFlags to_stage) const;

    static void barrier_(coopa::gfx::command::CommandBuffer& cmd, VkImage image,
                         VkImageLayout from, VkImageLayout to,
                         VkAccessFlags src_access, VkAccessFlags dst_access,
                         VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

    coopa::gfx::core::Device& device_;
    uint32_t size_;
    bool     caching_;
    bool     initialized_ = false;
    std::unique_ptr<coopa::gfx::memory::Image> live_;
    std::unique_ptr<coopa::gfx::memory::Image> cache_;
    VkRenderPass  live_pass_  = VK_NULL_HANDLE;
    VkRenderPass  cache_pass_ = VK_NULL_HANDLE;
    VkFramebuffer live_fb_    = VK_NULL_HANDLE;
    VkFramebuffer cache_fb_   = VK_NULL_HANDLE;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_LOCAL_SHADOW_ATLAS_H
