/**
 * @file vk_convert.h
 * @brief INTERNAL. Conversions between gfxcoopa's sealed vocabulary
 * (the headers under gfxcoopa/types/) and the underlying Vulkan/VMA types.
 *
 * Consumer code must never include this header directly -- `detail::` is
 * banned by tools/check_no_vulkan.sh precisely so this stays an
 * implementation seam, not a second public API.
 */

#ifndef COOPA_GFX_DETAIL_VK_CONVERT_H
#define COOPA_GFX_DETAIL_VK_CONVERT_H

#include <volk/volk.h>
#include <vma/vk_mem_alloc.h>

#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/types/clear.h>

namespace coopa {
namespace gfx {
namespace detail {

/**
 * @struct RawRenderPass
 * @brief Internal escape hatch wrapping a raw VkRenderPass, for the ~5
 * internal gfxcoopa passes/targets that hand-roll multi-render-target
 * VkRenderPass objects render_pass.h's single-color-attachment RenderPass
 * can't express yet (see pipeline::Pipeline's RawRenderPass-taking
 * constructor).
 *
 * Deliberately a `detail::` type: a consumer would have to write
 * `coopa::gfx::detail::RawRenderPass` to construct one, and `detail::` is
 * banned by tools/check_no_vulkan.sh, so this stays gfxcoopa-internal
 * without needing a real MRT-capable RenderPassDesc built first.
 */
struct RawRenderPass { VkRenderPass handle; };

// --- TextureView <-> VkImageView -------------------------------------------
//
// The #if is required, not defensive boilerplate: on 32-bit platforms
// Vulkan's non-dispatchable handles are plain uint64_t, not pointers, so
// reinterpret_cast<VkImageView> would not compile there. These repos only
// ever target x86-64 Linux, but the guard costs four lines.
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES == 1
inline TextureView  wrap(VkImageView v)   { return TextureView{reinterpret_cast<uint64_t>(v)}; }
inline VkImageView  unwrap(TextureView v) { return reinterpret_cast<VkImageView>(v.id()); }
#else
inline TextureView  wrap(VkImageView v)   { return TextureView{static_cast<uint64_t>(v)}; }
inline VkImageView  unwrap(TextureView v) { return static_cast<VkImageView>(v.id()); }
#endif

// --- Format ------------------------------------------------------------

inline VkFormat to_vk(Format f) {
    switch (f) {
        case Format::Undefined:          return VK_FORMAT_UNDEFINED;
        case Format::R8_Unorm:           return VK_FORMAT_R8_UNORM;
        case Format::R8_Srgb:            return VK_FORMAT_R8_SRGB;
        case Format::RG8_Unorm:          return VK_FORMAT_R8G8_UNORM;
        case Format::RG8_Srgb:           return VK_FORMAT_R8G8_SRGB;
        case Format::RGBA8_Unorm:        return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_Srgb:         return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8_Unorm:        return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_Srgb:         return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R16_Sfloat:         return VK_FORMAT_R16_SFLOAT;
        case Format::RG16_Sfloat:        return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16_Sfloat:      return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::A2B10G10R10_Unorm:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case Format::R32_Sfloat:         return VK_FORMAT_R32_SFLOAT;
        case Format::RG32_Sfloat:        return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGB32_Sfloat:       return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::RGBA32_Sfloat:      return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::R32_Uint:           return VK_FORMAT_R32_UINT;
        case Format::RG32_Uint:          return VK_FORMAT_R32G32_UINT;
        case Format::RGBA32_Uint:        return VK_FORMAT_R32G32B32A32_UINT;
        case Format::D16_Unorm:          return VK_FORMAT_D16_UNORM;
        case Format::D32_Sfloat:         return VK_FORMAT_D32_SFLOAT;
        case Format::D24_Unorm_S8_Uint:  return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_Sfloat_S8_Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

inline Format from_vk(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM:                  return Format::R8_Unorm;
        case VK_FORMAT_R8_SRGB:                   return Format::R8_Srgb;
        case VK_FORMAT_R8G8_UNORM:                return Format::RG8_Unorm;
        case VK_FORMAT_R8G8_SRGB:                 return Format::RG8_Srgb;
        case VK_FORMAT_R8G8B8A8_UNORM:             return Format::RGBA8_Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:              return Format::RGBA8_Srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:             return Format::BGRA8_Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:              return Format::BGRA8_Srgb;
        case VK_FORMAT_R16_SFLOAT:                 return Format::R16_Sfloat;
        case VK_FORMAT_R16G16_SFLOAT:              return Format::RG16_Sfloat;
        case VK_FORMAT_R16G16B16A16_SFLOAT:        return Format::RGBA16_Sfloat;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:   return Format::A2B10G10R10_Unorm;
        case VK_FORMAT_R32_SFLOAT:                 return Format::R32_Sfloat;
        case VK_FORMAT_R32G32_SFLOAT:              return Format::RG32_Sfloat;
        case VK_FORMAT_R32G32B32_SFLOAT:           return Format::RGB32_Sfloat;
        case VK_FORMAT_R32G32B32A32_SFLOAT:        return Format::RGBA32_Sfloat;
        case VK_FORMAT_R32_UINT:                   return Format::R32_Uint;
        case VK_FORMAT_R32G32_UINT:                return Format::RG32_Uint;
        case VK_FORMAT_R32G32B32A32_UINT:          return Format::RGBA32_Uint;
        case VK_FORMAT_D16_UNORM:                  return Format::D16_Unorm;
        case VK_FORMAT_D32_SFLOAT:                 return Format::D32_Sfloat;
        case VK_FORMAT_D24_UNORM_S8_UINT:          return Format::D24_Unorm_S8_Uint;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:         return Format::D32_Sfloat_S8_Uint;
        default:                                   return Format::Undefined;
    }
}

// --- ShaderStage (bitmask) ----------------------------------------------

inline VkShaderStageFlags to_vk(ShaderStage s) {
    VkShaderStageFlags f = 0;
    if (any(s, ShaderStage::Vertex))   f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (any(s, ShaderStage::Fragment)) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (any(s, ShaderStage::Geometry)) f |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (any(s, ShaderStage::Compute))  f |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (any(s, ShaderStage::TessControl)) f |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (any(s, ShaderStage::TessEval))    f |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    return f;
}

/// @brief Converts a single-bit ShaderStage to its VkShaderStageFlagBits.
/// For Shader's ctor, which (like VkPipelineShaderStageCreateInfo) takes
/// exactly one stage bit, not a mask.
inline VkShaderStageFlagBits to_vk_bit(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderStage::TessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TessEval:    return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        default:                    return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

// --- Sampler enums --------------------------------------------------------

inline VkFilter to_vk(Filter f) {
    return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

inline VkSamplerMipmapMode to_vk(MipmapMode m) {
    return m == MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

inline VkSamplerAddressMode to_vk(AddressMode a) {
    switch (a) {
        case AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

inline VkCompareOp to_vk(CompareOp c) {
    switch (c) {
        case CompareOp::Never:          return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:           return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:          return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessOrEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:        return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:       return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:         return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

// --- Rasterization enums --------------------------------------------------

inline VkPrimitiveTopology to_vk(Topology t) {
    switch (t) {
        case Topology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case Topology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case Topology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case Topology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case Topology::PatchList:     return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

inline VkPolygonMode to_vk(PolygonMode p) {
    switch (p) {
        case PolygonMode::Fill:  return VK_POLYGON_MODE_FILL;
        case PolygonMode::Line:  return VK_POLYGON_MODE_LINE;
        case PolygonMode::Point: return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

inline VkCullModeFlags to_vk(CullMode c) {
    switch (c) {
        case CullMode::None:         return VK_CULL_MODE_NONE;
        case CullMode::Front:        return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:         return VK_CULL_MODE_BACK_BIT;
        case CullMode::FrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
    }
    return VK_CULL_MODE_BACK_BIT;
}

inline VkFrontFace to_vk(FrontFace f) {
    return f == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

inline VkSampleCountFlagBits to_vk(SampleCount s) {
    switch (s) {
        case SampleCount::X1: return VK_SAMPLE_COUNT_1_BIT;
        case SampleCount::X2: return VK_SAMPLE_COUNT_2_BIT;
        case SampleCount::X4: return VK_SAMPLE_COUNT_4_BIT;
        case SampleCount::X8: return VK_SAMPLE_COUNT_8_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

inline VkIndexType to_vk(IndexType i) {
    return i == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

inline SampleCount from_vk(VkSampleCountFlagBits s) {
    switch (s) {
        case VK_SAMPLE_COUNT_2_BIT: return SampleCount::X2;
        case VK_SAMPLE_COUNT_4_BIT: return SampleCount::X4;
        case VK_SAMPLE_COUNT_8_BIT: return SampleCount::X8;
        case VK_SAMPLE_COUNT_1_BIT:
        default:                   return SampleCount::X1;
    }
}

// --- TextureUsage -> VkImageLayout / access & stage masks -----------------
//
// One mapping from TextureUsage to layout + access/stage masks, so
// command::CommandBuffer::transition() needs no "from" argument (Image tracks
// its own current TextureUsage) and no call site picks barrier masks by hand.
// Transposed src/dst stage masks are the classic way to get that wrong.

inline VkImageLayout to_vk_layout(TextureUsage u) {
    switch (u) {
        case TextureUsage::Undefined:       return VK_IMAGE_LAYOUT_UNDEFINED;
        case TextureUsage::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case TextureUsage::DepthAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case TextureUsage::ShaderRead:      return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case TextureUsage::TransferSrc:     return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case TextureUsage::TransferDst:     return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case TextureUsage::Present:         return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

/// @brief The (access mask, pipeline stage mask) pair a barrier must use
/// when an image ENTERS the given TextureUsage. Depth vs. color attachment
/// access bits are disambiguated by `is_depth_format` since both map to
/// TextureUsage::DepthAttachment/ColorAttachment respectively -- callers
/// pass the image's own Format so this never has to guess.
struct BarrierMasks { VkAccessFlags access; VkPipelineStageFlags stage; };

inline BarrierMasks barrier_masks_for(TextureUsage u, bool is_depth_format) {
    switch (u) {
        case TextureUsage::Undefined:
            return { 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
        case TextureUsage::ColorAttachment:
            return { VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        case TextureUsage::DepthAttachment:
            return { VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };
        case TextureUsage::ShaderRead:
            return { VK_ACCESS_SHADER_READ_BIT,
                     static_cast<VkPipelineStageFlags>(
                         is_depth_format
                             ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                             : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) };
        case TextureUsage::TransferSrc:
            return { VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
        case TextureUsage::TransferDst:
            return { VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
        case TextureUsage::Present:
            return { 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };
    }
    return { 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
}

/// @brief The aspect mask for an image of the given Format -- depth(+stencil)
/// for depth formats, color otherwise. Deriving it from the format is what
/// lets barriers, views and copies handle a depth image without the caller
/// having to supply an aspect mask by hand.
inline VkImageAspectFlags aspect_mask_for(Format format) {
    if (!is_depth(format)) return VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags mask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (is_stencil(format)) mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return mask;
}

// --- ImageUsage (bitmask) ---------------------------------------------------

inline VkImageUsageFlags to_vk(ImageUsage u) {
    VkImageUsageFlags f = 0;
    if (any(u, ImageUsage::Sampled))         f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(u, ImageUsage::ColorAttachment)) f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(u, ImageUsage::DepthAttachment)) f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (any(u, ImageUsage::Storage))         f |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(u, ImageUsage::TransferSrc))     f |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (any(u, ImageUsage::TransferDst))     f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return f;
}

// --- DescriptorType --------------------------------------------------------

inline VkDescriptorType to_vk(DescriptorType d) {
    switch (d) {
        case DescriptorType::UniformBuffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:         return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::CombinedImageSampler:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::StorageImage:          return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

// --- BufferUsage (bitmask) -------------------------------------------------

inline VkBufferUsageFlags to_vk(BufferUsage u) {
    VkBufferUsageFlags f = 0;
    if (any(u, BufferUsage::Vertex))      f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (any(u, BufferUsage::Index))       f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (any(u, BufferUsage::Uniform))     f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(u, BufferUsage::Storage))     f |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(u, BufferUsage::Indirect))    f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (any(u, BufferUsage::TransferSrc)) f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (any(u, BufferUsage::TransferDst)) f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return f;
}

// --- BufferAccess (bitmask) -> barrier masks -------------------------------

/// @brief The (access, stage) masks of one side of a buffer memory barrier. Reuses
/// BarrierMasks; an empty set maps to TOP_OF_PIPE / no access, the "nothing to wait on"
/// source.
inline BarrierMasks barrier_masks_for(BufferAccess a) {
    BarrierMasks m{ 0, 0 };
    auto add = [&](BufferAccess bit, VkAccessFlags access, VkPipelineStageFlags stage) {
        if (any(a, bit)) { m.access |= access; m.stage |= stage; }
    };
    add(BufferAccess::TransferRead,       VK_ACCESS_TRANSFER_READ_BIT,           VK_PIPELINE_STAGE_TRANSFER_BIT);
    add(BufferAccess::TransferWrite,      VK_ACCESS_TRANSFER_WRITE_BIT,          VK_PIPELINE_STAGE_TRANSFER_BIT);
    add(BufferAccess::ComputeRead,        VK_ACCESS_SHADER_READ_BIT,             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    add(BufferAccess::ComputeWrite,       VK_ACCESS_SHADER_WRITE_BIT,            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    add(BufferAccess::VertexAttribute,    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,   VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    add(BufferAccess::Index,              VK_ACCESS_INDEX_READ_BIT,              VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    add(BufferAccess::Indirect,           VK_ACCESS_INDIRECT_COMMAND_READ_BIT,   VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
    add(BufferAccess::VertexShaderRead,   VK_ACCESS_SHADER_READ_BIT,             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    add(BufferAccess::FragmentShaderRead, VK_ACCESS_SHADER_READ_BIT,             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    add(BufferAccess::HostRead,           VK_ACCESS_HOST_READ_BIT,               VK_PIPELINE_STAGE_HOST_BIT);
    if (m.stage == 0) m.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    return m;
}

// --- MemoryResidency -> VMA -------------------------------------------------

/// @brief A MemoryResidency split into the VMA usage + allocation flags pair it
/// maps to. CpuToGpu/GpuToCpu always carry the persistently-mapped flags, so the
/// two cannot be set inconsistently with each other.
struct VmaResidency { VmaMemoryUsage usage; VmaAllocationCreateFlags flags; };

inline VmaResidency to_vma(MemoryResidency r) {
    switch (r) {
        case MemoryResidency::GpuOnly:
            return { VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0 };
        case MemoryResidency::CpuToGpu:
            return { VMA_MEMORY_USAGE_AUTO,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT };
        case MemoryResidency::GpuToCpu:
            return { VMA_MEMORY_USAGE_AUTO,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT };
    }
    return { VMA_MEMORY_USAGE_AUTO, 0 };
}

// --- ClearColor / Extent2D --------------------------------------------------

inline VkClearColorValue to_vk(ClearColor c) {
    VkClearColorValue v{};
    v.float32[0] = c.r; v.float32[1] = c.g; v.float32[2] = c.b; v.float32[3] = c.a;
    return v;
}

inline VkExtent2D to_vk(Extent2D e) {
    return VkExtent2D{ e.width, e.height };
}

inline Extent2D from_vk(VkExtent2D e) {
    return Extent2D{ e.width, e.height };
}

} // namespace detail
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_DETAIL_VK_CONVERT_H
