/**
 * @file sampler_handle.h
 * @brief Opaque identity handle for a sampler -- gfxcoopa's replacement for returning a raw
 *        VkSampler from a public accessor.
 *
 * The sampler sibling of TextureView and ImageHandle, and it exists for the same reason: a
 * consumer that binds a texture has to name the sampler alongside it, and naming it as a
 * `VkSampler` is a Vulkan type crossing the library boundary. Carries identity only.
 */

#ifndef COOPA_GFX_TYPES_SAMPLER_HANDLE_H
#define COOPA_GFX_TYPES_SAMPLER_HANDLE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace coopa {
namespace gfx {

/**
 * @class SamplerHandle
 * @brief An opaque, copyable, hashable identity token for a sampler.
 */
class SamplerHandle {
public:
    /// @brief Constructs a null handle.
    constexpr SamplerHandle() = default;

    /**
     * @brief Constructs a handle with an explicit opaque identity.
     *
     * For gfxcoopa's own wrap/unwrap in detail/vk_convert.h, and for test code fabricating
     * distinct-but-fake sampler identities.
     *
     * @param id Opaque identity value. Zero is reserved for null().
     */
    constexpr explicit SamplerHandle(uint64_t id) : id_(id) {}

    /// @brief The null handle -- "no sampler".
    static constexpr SamplerHandle null() { return SamplerHandle{}; }

    /// @brief Whether this names a sampler.
    constexpr explicit operator bool() const { return id_ != 0; }

    /// @brief The opaque identity value.
    constexpr uint64_t id() const { return id_; }

    friend constexpr bool operator==(SamplerHandle, SamplerHandle) = default;
    friend constexpr auto operator<=>(SamplerHandle, SamplerHandle) = default;

private:
    uint64_t id_ = 0;
};

} // namespace gfx
} // namespace coopa

namespace std {

/// @brief Hash support, so a SamplerHandle can key a descriptor-set cache.
template <>
struct hash<coopa::gfx::SamplerHandle> {
    size_t operator()(coopa::gfx::SamplerHandle h) const noexcept {
        return hash<uint64_t>{}(h.id());
    }
};

} // namespace std

#endif // COOPA_GFX_TYPES_SAMPLER_HANDLE_H
