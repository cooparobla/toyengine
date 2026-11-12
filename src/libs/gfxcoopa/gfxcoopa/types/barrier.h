#ifndef GFXCOOPA_TYPES_BARRIER_H
#define GFXCOOPA_TYPES_BARRIER_H

/**
 * @file barrier.h
 * @brief Image memory barriers in gfxcoopa's own vocabulary: which aspect, which mip/layer
 *        window, and optionally which work to order on each side.
 *
 * Vulkan-free, like the rest of types/, so these are usable from a coopa::gfx_pure target.
 */

#include <cstdint>

#include <gfxcoopa/types/enums.h>

namespace coopa {
namespace gfx {

/**
 * @enum ImageAspect
 * @brief Which aspect(s) of an image a barrier or copy addresses.
 *
 * Derived from a Format wherever one is available, so it only needs naming for an image
 * gfxcoopa does not own and whose Format it therefore cannot read.
 */
enum class ImageAspect : uint8_t {
    Color,        ///< VK_IMAGE_ASPECT_COLOR_BIT
    Depth,        ///< VK_IMAGE_ASPECT_DEPTH_BIT
    DepthStencil, ///< depth + stencil together
};

/**
 * @struct SubresourceRange
 * @brief A mip/layer window into an image.
 *
 * Defaults to level 0, layer 0, one of each -- the single-mip single-layer shape -- so a `{}`
 * argument means "the whole of an ordinary 2D image".
 */
struct SubresourceRange {
    /// @brief Stands for VK_REMAINING_MIP_LEVELS / VK_REMAINING_ARRAY_LAYERS.
    static constexpr uint32_t kRemaining = ~0u;

    uint32_t base_mip   = 0;
    uint32_t mip_count  = 1;
    uint32_t base_layer = 0;
    uint32_t layer_count = 1;

    /** @brief Every mip and every layer. */
    static constexpr SubresourceRange all() { return {0, kRemaining, 0, kRemaining}; }
    /** @brief One mip level, layer 0. */
    static constexpr SubresourceRange mip(uint32_t level) { return {level, 1, 0, 1}; }
    /** @brief `count` mip levels from `base`, layer 0. */
    static constexpr SubresourceRange mips(uint32_t base, uint32_t count) { return {base, count, 0, 1}; }
    /** @brief One array layer, every mip. */
    static constexpr SubresourceRange layer(uint32_t index) { return {0, kRemaining, index, 1}; }
    /** @brief `count` array layers from `base`, every mip. */
    static constexpr SubresourceRange layers(uint32_t base, uint32_t count) { return {0, kRemaining, base, count}; }
};

/**
 * @enum ImageAccess
 * @brief How an image is touched on ONE side of a barrier. A bitmask -- combine with
 *        operator|(). Each bit implies its pipeline stage, exactly as BufferAccess does, so a
 *        barrier never names a stage mask separately.
 *
 * Distinct from TextureUsage, which is a LAYOUT. A barrier names the layout it moves between
 * (TextureUsage) and, when it must, the work it orders (ImageAccess). The two are usually
 * redundant -- CommandBuffer::transition() derives the access from the layout and this type
 * never appears -- and diverge only in two situations:
 *
 *  - Several kinds of work touched the image since the last barrier, so one layout cannot
 *    describe the producers (a target written as a colour attachment, sampled, AND copied).
 *  - The old contents are being discarded (`from` is Undefined, which implies no access) but a
 *    previous *reader* of them must still be waited on -- a write-after-read hazard.
 */
enum class ImageAccess : uint32_t {
    None                 = 0,
    TransferRead         = 1u << 0,  ///< Copy/blit source.
    TransferWrite        = 1u << 1,  ///< Copy/blit/clear destination.
    ColorAttachmentRead  = 1u << 2,  ///< Blended against.
    ColorAttachmentWrite = 1u << 3,  ///< Written as a colour target.
    DepthAttachmentRead  = 1u << 4,  ///< Depth-tested against.
    DepthAttachmentWrite = 1u << 5,  ///< Depth written.
    FragmentShaderRead   = 1u << 6,  ///< Sampled in a fragment shader.
    VertexShaderRead     = 1u << 7,  ///< Sampled in a vertex/tessellation shader (displacement).
    ComputeRead          = 1u << 8,
    ComputeWrite         = 1u << 9,
    Present              = 1u << 10, ///< Handed to the presentation engine.
    /**
     * Every stage, no access bits (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT).
     *
     * For a barrier downstream of a pass chain whose last writer the call site genuinely cannot
     * determine. Never a default: it is a correctness hammer that also serialises the pipeline,
     * so a call site using it should say why.
     */
    AllCommands          = 1u << 11,
};

/** @brief Combines two image-access masks. */
constexpr ImageAccess operator|(ImageAccess a, ImageAccess b) {
    return static_cast<ImageAccess>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/** @brief True when `set` contains every bit of `mask`. */
constexpr bool any(ImageAccess set, ImageAccess mask) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(mask)) != 0;
}

/**
 * @struct ImageBarrier
 * @brief One image memory barrier: the layouts to move between, the aspect and sub-range it
 *        covers, and optionally the work to order on each side.
 *
 * `wait_for` / `make_visible_to` left at None are DERIVED from `from` / `to`, which is what
 * CommandBuffer::transition() does and is correct for the ordinary case. A non-None value
 * REPLACES the derived mask rather than widening it, so a barrier out of Undefined can still
 * wait on a real reader (see ImageAccess).
 */
struct ImageBarrier {
    TextureUsage     from   = TextureUsage::Undefined;
    TextureUsage     to     = TextureUsage::Undefined;
    ImageAspect      aspect = ImageAspect::Color;
    SubresourceRange range{};
    ImageAccess      wait_for        = ImageAccess::None;
    ImageAccess      make_visible_to = ImageAccess::None;
};

} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_TYPES_BARRIER_H
