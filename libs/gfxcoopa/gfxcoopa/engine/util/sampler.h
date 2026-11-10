/**
 * @file sampler.h
 * @brief RAII VkSampler wrapper with nearest-neighbour and linear factory methods.
 *
 * Sampler::nearest() is the critical one for pixel-art upscaling — it ensures
 * no bilinear interpolation blurs the retro low-res render.
 */

#ifndef GFXCOOPA_ENGINE_UTIL_SAMPLER_H
#define GFXCOOPA_ENGINE_UTIL_SAMPLER_H

#include <volk/volk.h>
#include <stdexcept>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace util {

/**
 * @class Sampler
 * @brief RAII VkSampler wrapper.
 *
 * Factory methods:
 *   - Sampler::nearest() — pixel-perfect, no interpolation (for upscale pass)
 *   - Sampler::linear()  — bilinear filtering (for post-processing intermediates)
 *   - Sampler::shadow()  — hardware depth-compare + PCF (for sampler2DShadow/samplerCubeShadow)
 */
class Sampler {
public:
    /**
     * @brief Creates a VkSampler with the given filter and address mode.
     * @param device        Logical device.
     * @param filter        Min/mag filter (VK_FILTER_NEAREST or VK_FILTER_LINEAR).
     * @param address_mode  UV wrap mode (default: clamp to edge).
     * @param max_lod       Maximum LOD clamp for explicit-LOD sampling (e.g. textureLod) as well
     *                      as implicit mip selection. Defaults to 0.0f, correct for the
     *                      single-mip images that are the common case; pass the image's mip
     *                      count to sample a real mip chain (e.g. a Hi-Z pyramid).
     * @param mipmap_mode   Mip selection filter (VK_SAMPLER_MIPMAP_MODE_NEAREST or _LINEAR).
     *                      Defaults to NEAREST. Pass LINEAR for smooth roughness-driven
     *                      mip blending (e.g. a prefiltered reflection cubemap).
     * @param compare_op    Depth-compare op for shadow-map hardware PCF (sampler2DShadow /
     *                      samplerCubeShadow in GLSL). Defaults to VK_COMPARE_OP_NEVER, which
     *                      leaves compareEnable off. Pass
     *                      VK_COMPARE_OP_GREATER via Sampler::shadow() to enable it: GLSL's
     *                      `texture(sampler2DShadow, vec3(uv, ref))` evaluates `ref OP sampled`
     *                      and bilinearly filters the per-tap compare *result*, not the raw
     *                      depth -- see gfx/shadow_sampling.glsl's sampler2DShadow overloads.
     */
    Sampler(core::Device& device,
            VkFilter filter,
            VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            float max_lod = 0.0f,
            VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VkCompareOp compare_op = VK_COMPARE_OP_NEVER,
            float max_anisotropy = 0.0f)
        : device_(device)
    {
        VkSamplerCreateInfo info{};
        info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter               = filter;
        info.minFilter               = filter;
        info.mipmapMode              = mipmap_mode;
        info.addressModeU            = address_mode;
        info.addressModeV            = address_mode;
        info.addressModeW            = address_mode;
        info.mipLodBias              = 0.0f;
        info.anisotropyEnable        = max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy           = max_anisotropy > 0.0f ? max_anisotropy : 1.0f;
        info.compareEnable           = compare_op != VK_COMPARE_OP_NEVER ? VK_TRUE : VK_FALSE;
        info.compareOp               = compare_op;
        info.minLod                  = 0.0f;
        info.maxLod                  = max_lod;
        info.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;

        GFX_VK_CHECK(vkCreateSampler(device_.handle(), &info, nullptr, &sampler_));
    }

    /**
     * @brief Creates a VkSampler from a sealed SamplerDesc -- the sealed sibling of the raw
     * ctor above, for callers that don't need the raw ctor's max_lod/mip-chain support
     * (SamplerDesc has no max_lod field; a mip-chain sampler, e.g. a prefiltered cubemap or
     * Hi-Z pyramid, still needs the raw ctor).
     * @param device Logical device.
     * @param desc   Sealed filter/address/mipmap/compare/anisotropy configuration.
     */
    // The delegated ctor takes a single `filter` for both min and mag, so desc.min is
    // not forwarded. No SamplerDesc preset sets min != mag; a sampler that needs them to
    // differ has to use the raw constructor above.
    // max_lod is a separate parameter rather than a SamplerDesc field because only the code
    // that created the image knows its mip count; a desc is authored where only the FILTERING
    // intent is known (SamplerDesc::mipmapped).
    Sampler(core::Device& device, const SamplerDesc& desc, float max_lod = 0.0f)
        : Sampler(device,
                  detail::to_vk(desc.mag),
                  detail::to_vk(desc.address),
                  max_lod,
                  detail::to_vk(desc.mipmap),
                  detail::to_vk(desc.compare),
                  desc.max_anisotropy)
    {}

    ~Sampler() {
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_.handle(), sampler_, nullptr);
        }
    }

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    /**
     * @brief Move constructor: transfers VkSampler ownership.
     * @param other The Sampler to move from (left in a null state).
     */
    Sampler(Sampler&& other) noexcept
        : device_(other.device_), sampler_(other.sampler_)
    {
        other.sampler_ = VK_NULL_HANDLE;
    }

    /** @brief Returns the raw VkSampler handle. */
    VkSampler handle() const { return sampler_; }

    // --- Factory methods ---

    /**
     * @brief Creates a nearest-neighbour sampler (pixel-perfect, no blurring).
     *
     * Use this for the upscale pass to preserve the retro pixel aesthetic.
     *
     * @param device Logical device.
     * @return Nearest-neighbour Sampler.
     */
    static Sampler nearest(core::Device& device) {
        return Sampler(device, VK_FILTER_NEAREST);
    }

    /**
     * @brief Creates a linear (bilinear) sampler.
     *
     * Use this for post-processing intermediate steps where smooth sampling is acceptable.
     *
     * @param device Logical device.
     * @return Linear Sampler.
     */
    static Sampler linear(core::Device& device) {
        return Sampler(device, VK_FILTER_LINEAR);
    }

    /**
     * @brief Creates a hardware depth-compare sampler for shadow maps.
     *
     * Backs GLSL `sampler2DShadow`/`samplerCubeShadow`: the GPU compares the
     * reference depth (third `texture()` coordinate) against the stored depth
     * with VK_COMPARE_OP_GREATER *before* bilinearly filtering, so a single
     * tap already averages a 2x2 neighbourhood of pass/fail results instead
     * of returning one binary value. See gfx/shadow_sampling.glsl for the
     * kernels that consume this.
     *
     * @param device Logical device.
     * @return Linear, depth-compare-enabled Sampler.
     */
    static Sampler shadow(core::Device& device) {
        return Sampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                       0.0f, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_COMPARE_OP_GREATER);
    }

private:
    core::Device& device_;                    /**< Owning logical device (not owned). */
    VkSampler     sampler_ = VK_NULL_HANDLE; /**< The Vulkan sampler. */
};

} // namespace util
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_SAMPLER_H
