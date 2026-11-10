/**
 * @file enums.h
 * @brief gfxcoopa-owned scalar enums for pipeline, resource, and image state.
 *
 * These replace `Vk*` enums in every gfxcoopa public signature. They are
 * deliberately NOT 1:1 mirrors of their Vulkan counterparts — each is
 * narrowed to the values gfxcoopa's API actually needs, with values added on
 * demand rather than up front. This header (and the rest of `gfxcoopa/types/`)
 * must never include volk or GLFW: that invariant is what makes the
 * `coopa::gfx_pure` CMake target and the "headless tests never touch Vulkan"
 * guarantee structural rather than aspirational. Vulkan-side conversion lives
 * only in `gfxcoopa/detail/vk_convert.h`. The keyboard/mouse vocabulary
 * lives in `coopa/input/` in libcoopa for the same reason -- it depends on
 * neither Vulkan nor GLFW. See coopa/input/README.md.
 */

#ifndef COOPA_GFX_TYPES_ENUMS_H
#define COOPA_GFX_TYPES_ENUMS_H

#include <cstdint>

namespace coopa {
namespace gfx {

/**
 * @enum ShaderStage
 * @brief Which programmable stage(s) a shader, descriptor binding, or push
 * constant range is visible to. A bitmask — combine with operator|().
 */
enum class ShaderStage : uint32_t {
    None     = 0,
    Vertex   = 1u << 0,
    Fragment = 1u << 1,
    Geometry = 1u << 2,
    Compute  = 1u << 3,
    TessControl = 1u << 4,   ///< Tessellation control (hull). Needs Device::supports_tessellation().
    TessEval    = 1u << 5,   ///< Tessellation evaluation (domain).
};

/// @brief Combines two shader-stage masks.
constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// @brief Intersects two shader-stage masks.
constexpr ShaderStage operator&(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/// @brief True if `set` contains every bit set in `m`.
constexpr bool any(ShaderStage set, ShaderStage m) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(m)) != 0;
}

/**
 * @enum Filter
 * @brief Sampler minification/magnification filtering. See also SamplerDesc.
 */
enum class Filter {
    Nearest,  ///< Point sampling. Correct for pixel-art atlases.
    Linear,   ///< Bilinear interpolation between texels.
};

/**
 * @enum AddressMode
 * @brief How a sampler handles UV coordinates outside [0, 1].
 */
enum class AddressMode {
    Repeat,          ///< Tiles the texture.
    MirroredRepeat,  ///< Tiles the texture, mirroring every other tile.
    ClampToEdge,     ///< Clamps to the edge texel. Correct for atlases/UI.
    ClampToBorder,   ///< Clamps to a fixed border color (opaque black).
};

/**
 * @enum MipmapMode
 * @brief How a sampler interpolates between mip levels.
 */
enum class MipmapMode {
    Nearest,
    Linear,
};

/**
 * @enum CompareOp
 * @brief Comparison function for depth testing and shadow-map sampler
 * comparison.
 *
 * `Never` doubles as "comparison disabled" wherever a CompareOp appears
 * outside of SamplerDesc::compare's shadow-sampling use — e.g. it is the
 * default for DepthState::compare only when DepthState::test is false. This
 * is an intentional elimination of a separate bool, not an oversight.
 */
enum class CompareOp {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};

/// @brief Input assembly primitive topology.
enum class Topology {
    TriangleList,
    TriangleStrip,
    LineList,
    LineStrip,
    PointList,
    PatchList,   ///< Tessellation patches (PipelineDesc::patch_control_points sets their size).
};

/// @brief Rasterizer fill mode.
enum class PolygonMode {
    Fill,
    Line,
    Point,
};

/**
 * @enum CullMode
 * @brief Face culling mode.
 *
 * A plain enum, not a bitmask: `VkCullModeFlags` technically allows
 * combining FRONT and BACK bits, but no caller in this codebase ever wants
 * anything but these four exclusive states, and FrontAndBack already covers
 * "cull everything".
 */
enum class CullMode {
    None,
    Front,
    Back,
    FrontAndBack,
};

/// @brief Winding order considered front-facing.
enum class FrontFace {
    CounterClockwise,
    Clockwise,
};

/// @brief MSAA sample count. Survives only on RenderPass/Image-level
/// descriptions — see PipelineConfig's docs for why it does not appear there.
enum class SampleCount : uint8_t {
    X1 = 1,
    X2 = 2,
    X4 = 4,
    X8 = 8,
};

/// @brief Index buffer element width. Narrowed from VkIndexType's larger
/// set (which also includes 8-bit and "none" for ray tracing) to the two
/// values any renderer here uses.
enum class IndexType {
    U16,
    U32,
};

/// @brief Per-vertex-attribute vs per-instance step rate.
enum class VertexRate {
    Vertex,
    Instance,
};

/**
 * @enum TextureUsage
 * @brief The role an Image is currently being used in (its current state).
 *
 * Replaces VkImageLayout in the public API entirely. memory::Image tracks
 * its own current TextureUsage, so command::CommandBuffer::transition()
 * takes only a destination — there is no "from" argument for a caller to
 * get wrong.
 *
 * Distinct from ImageUsage below: TextureUsage is a single current STATE
 * (what an image IS right now); ImageUsage is a bitmask of every role an
 * image is ALLOWED to ever be used in, fixed at creation (VkImageUsageFlags'
 * replacement). An image created with `ImageUsage::Sampled |
 * ImageUsage::TransferDst` can later be `transition()`ed between
 * `TextureUsage::TransferDst` and `TextureUsage::ShaderRead`.
 */
enum class TextureUsage {
    Undefined,
    ColorAttachment,
    DepthAttachment,
    ShaderRead,
    TransferSrc,
    TransferDst,
    Present,
};

/**
 * @enum ImageUsage
 * @brief Every role an Image may ever be used in over its lifetime, fixed
 * at creation. A bitmask — combine with operator|(). Replaces
 * VkImageUsageFlags in the public API.
 */
enum class ImageUsage : uint32_t {
    None             = 0,
    Sampled          = 1u << 0, ///< Bound to a descriptor as a sampled texture.
    ColorAttachment  = 1u << 1, ///< Rendered into as a color target.
    DepthAttachment  = 1u << 2, ///< Rendered into as a depth/stencil target.
    Storage          = 1u << 3, ///< Bound as a storage image (compute read/write).
    TransferSrc      = 1u << 4, ///< Source of a copy/blit (e.g. readback).
    TransferDst      = 1u << 5, ///< Destination of a copy/blit (e.g. upload).
};

/// @brief Combines two image-usage masks.
constexpr ImageUsage operator|(ImageUsage a, ImageUsage b) {
    return static_cast<ImageUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// @brief True if `set` contains every bit set in `m`.
constexpr bool any(ImageUsage set, ImageUsage m) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(m)) != 0;
}

/// @brief Whether a render pass attachment is cleared, preserved, or left
/// undefined at the start of the pass.
enum class AttachmentLoad {
    Clear,
    Load,
    DontCare,
};

/**
 * @enum AttachmentUse
 * @brief What happens to a render pass attachment after the pass ends —
 * replaces an explicit "final layout" VkImageLayout with intent.
 */
enum class AttachmentUse {
    Present,        ///< Handed to the swapchain/presentation engine.
    SampledLater,   ///< Read by a later shader pass in the same frame (e.g.
                    ///< an offscreen target recorded via
                    ///< presentation::Renderer's pre_pass callback).
    ReusedAsAttachment, ///< Immediately reused as a render target (e.g. a
                        ///< depth buffer read as both attachment and texture).
};

/**
 * @enum ColorSpace
 * @brief How a decoded texture's pixel data should be interpreted: gamma-encoded color data
 * (Srgb) vs. data read as-is (Linear).
 *
 * Drives which Format a texture loader uploads as (e.g. RGBA8_Srgb vs. RGBA8_Unorm) so the
 * GPU's fixed-function sampler decodes sRGB->linear before filtering, rather than a shader
 * approximating it after filtering with pow(). Albedo/base-color maps are Srgb; normal maps,
 * metallic/roughness maps, and masks are Linear -- see
 * gfx::loaders::TextureLoader::declare_color_space().
 */
enum class ColorSpace : uint8_t {
    Linear,
    Srgb,
};

/// @brief Descriptor resource kind. Survives internally for descriptor pool
/// sizing; callers building layouts use DescriptorLayoutBuilder's named
/// methods instead of naming this directly.
enum class DescriptorType {
    UniformBuffer,
    StorageBuffer,
    CombinedImageSampler,
    StorageImage,
};

/**
 * @enum BufferUsage
 * @brief What a Buffer will be bound as. A bitmask — combine with
 * operator|(). Most callers never name this directly; see Buffer's named
 * constructors (vertex/index/uniform_dynamic/staging/readback).
 */
enum class BufferUsage : uint32_t {
    None         = 0,
    Vertex       = 1u << 0,
    Index        = 1u << 1,
    Uniform      = 1u << 2,
    Storage      = 1u << 3,
    Indirect     = 1u << 4,
    TransferSrc  = 1u << 5,
    TransferDst  = 1u << 6,
};

/// @brief Combines two buffer-usage masks.
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// @brief Intersects two buffer-usage masks.
constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/// @brief True if `set` contains every bit set in `m`.
constexpr bool any(BufferUsage set, BufferUsage m) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(m)) != 0;
}

/**
 * @enum MemoryResidency
 * @brief Where a Buffer's or Image's memory lives and how the CPU may
 * access it. Replaces VMA's separate VmaMemoryUsage + VmaAllocationCreateFlags
 * pair in the public API. CpuToGpu/GpuToCpu are always persistently mapped,
 * so the mapping flags cannot be set inconsistently with the residency.
 */
enum class MemoryResidency {
    GpuOnly,   ///< Device-local, not host-visible. Fastest for GPU-only access.
    CpuToGpu,  ///< Host-visible + persistently mapped, for frequent CPU writes
               ///< the GPU reads (uniform buffers, dynamic vertex data).
    GpuToCpu,  ///< Host-visible + persistently mapped + cached, for GPU
               ///< writes the CPU reads back (readback/staging-from-GPU).
};

/**
 * @enum BufferAccess
 * @brief How a buffer is touched on one side of a buffer memory barrier (see
 * command::CommandBuffer::buffer_barrier()). A bitmask -- combine with
 * operator|() to make a write visible to several consumers in one barrier,
 * e.g. `ComputeWrite -> VertexAttribute | Indirect`. Each value implies its
 * pipeline stage, so a barrier never names a stage mask separately.
 */
enum class BufferAccess : uint32_t {
    None             = 0,
    TransferRead     = 1u << 0,  ///< Copy source (copy_buffer / readback).
    TransferWrite    = 1u << 1,  ///< Copy or fill destination (copy_buffer, fill_buffer).
    ComputeRead      = 1u << 2,  ///< Storage/uniform read in a compute shader.
    ComputeWrite     = 1u << 3,  ///< Storage write (or atomic) in a compute shader.
    VertexAttribute  = 1u << 4,  ///< Bound as a vertex buffer.
    Index            = 1u << 5,  ///< Bound as an index buffer.
    Indirect         = 1u << 6,  ///< Read as draw/dispatch indirect arguments.
    VertexShaderRead = 1u << 7,  ///< Storage/uniform read in a vertex shader.
    FragmentShaderRead = 1u << 8, ///< Storage/uniform read in a fragment shader.
    HostRead         = 1u << 9,  ///< Read by the CPU through a mapping after a fence.
};

/// @brief Combines two buffer-access masks.
constexpr BufferAccess operator|(BufferAccess a, BufferAccess b) {
    return static_cast<BufferAccess>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// @brief True if `set` contains any bit set in `m`.
constexpr bool any(BufferAccess set, BufferAccess m) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(m)) != 0;
}

} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_TYPES_ENUMS_H
