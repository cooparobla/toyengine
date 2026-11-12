#ifndef GFXCOOPA_TYPES_RENDER_PASS_DESC_H
#define GFXCOOPA_TYPES_RENDER_PASS_DESC_H

/**
 * @file render_pass_desc.h
 * @brief A render pass described in gfxcoopa's own vocabulary, for the shapes RenderPass's
 *        positional constructor cannot reach.
 *
 * Vulkan-free, like the rest of types/, so a desc stays unit-testable from a
 * coopa::gfx_pure-linked target.
 */

#include <optional>
#include <vector>

#include <gfxcoopa/types/barrier.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>

namespace coopa {
namespace gfx {

/**
 * @struct AttachmentDesc
 * @brief One attachment of a RenderPassDesc.
 */
struct AttachmentDesc {
    Format         format  = Format::Undefined;
    AttachmentLoad load    = AttachmentLoad::Clear;
    bool           store   = true;

    /**
     * The layout the attachment is ALREADY in when the pass begins. Undefined discards its
     * contents, which `load == AttachmentLoad::Load` therefore contradicts.
     */
    TextureUsage   initial = TextureUsage::Undefined;

    /**
     * The layout the pass leaves it in.
     *
     * Spelled as a layout rather than reusing AttachmentUse because a LOAD/STORE pass that
     * neither presents nor is sampled afterwards -- one that stays TransferSrc throughout, as a
     * shadow cache's copy-out pass does -- fits none of AttachmentUse's three cases.
     */
    TextureUsage   final   = TextureUsage::ShaderRead;

    SampleCount    samples = SampleCount::X1;
};

/**
 * @struct RenderPassDesc
 * @brief The general form: several colour attachments, an optional depth attachment, explicit
 *        initial layouts, and LOAD as well as CLEAR.
 *
 * The two dependency fields describe only the EXTERNAL side of the implicit VK_SUBPASS_EXTERNAL
 * dependencies: `wait_for` is the entry dependency's source masks, `make_visible_to` the exit
 * dependency's destination masks. The in-pass sides are derived from the attachments themselves,
 * so a caller cannot transpose them.
 *
 * Exactly one subpass. Two passes built from descs with the same attachment formats, sample
 * counts and count are render-pass *compatible*, so one pipeline can draw into either -- which
 * is what lets a shadow atlas keep a live pass and a cache pass without duplicating every
 * per-material pipeline variant.
 */
struct RenderPassDesc {
    std::vector<AttachmentDesc>   color;   ///< In shader location order; may be empty.
    std::optional<AttachmentDesc> depth;

    ImageAccess wait_for        = ImageAccess::None; ///< None: derived from each `initial`.
    ImageAccess make_visible_to = ImageAccess::None; ///< None: derived from each `final`.

    /** @brief A depth-only pass, which is what every shadow pass is. */
    static RenderPassDesc depth_only(AttachmentDesc d) {
        RenderPassDesc out;
        out.depth = d;
        return out;
    }
};

} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_TYPES_RENDER_PASS_DESC_H
