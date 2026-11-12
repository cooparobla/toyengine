#ifndef TOYENGINE_RENDER_SHADOW_PASS_DESC_H
#define TOYENGINE_RENDER_SHADOW_PASS_DESC_H

/**
 * @file shadow_pass_desc.h
 * @brief The one render-pass shape every shadow atlas pass uses.
 *
 * DirShadowCache and LocalShadowAtlas each keep two passes -- a live one and a cache one --
 * that differ ONLY in the layouts they start and end in. All four, plus ShadowMapTarget's own
 * directional pass, must stay render-pass *compatible*: one D32 depth attachment, one subpass.
 * That compatibility is what lets a single set of shadow pipelines (and every per-material
 * variant of them) draw into any of them, so it is described here once rather than copied into
 * each file where the two could silently drift apart.
 */

#include <gfxcoopa/types/barrier.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/render_pass_desc.h>

namespace toy {
namespace render {

/**
 * @brief A shadow atlas pass that LOADs its depth attachment.
 *
 * LOAD, not CLEAR, because tiles accumulate into one image across several passes: a clear would
 * wipe the tiles already drawn this frame. A level about to be re-rendered is cleared
 * individually instead, with CommandBuffer::clear_depth_rect().
 *
 * @param initial      The layout the attachment is already in when the pass begins.
 * @param final_layout The layout the pass leaves it in.
 */
inline coopa::gfx::RenderPassDesc shadow_atlas_pass_desc(coopa::gfx::TextureUsage initial,
                                                         coopa::gfx::TextureUsage final_layout) {
    using coopa::gfx::ImageAccess;
    coopa::gfx::AttachmentDesc att{};
    att.format  = coopa::gfx::Format::D32_Sfloat;
    att.load    = coopa::gfx::AttachmentLoad::Load;
    att.store   = true;
    att.initial = initial;
    att.final   = final_layout;

    coopa::gfx::RenderPassDesc desc = coopa::gfx::RenderPassDesc::depth_only(att);
    // The external dependency masks name the work on the OTHER side of the pass: a tile copy or
    // a sampler read before it, a tile copy or a sampler read after it.
    //
    // FragmentShaderRead also contributes VK_ACCESS_SHADER_READ_BIT to the entry dependency's
    // SOURCE mask, which the hand-written masks omitted. Harmless: a read bit in a source mask
    // only makes an already-implied write-after-read ordering explicit.
    desc.wait_for        = ImageAccess::TransferWrite | ImageAccess::DepthAttachmentWrite
                         | ImageAccess::FragmentShaderRead;
    desc.make_visible_to = ImageAccess::TransferRead | ImageAccess::FragmentShaderRead;
    return desc;
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_SHADOW_PASS_DESC_H
