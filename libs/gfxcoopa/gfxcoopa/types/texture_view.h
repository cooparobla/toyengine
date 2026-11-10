/**
 * @file texture_view.h
 * @brief Opaque, hashable texture identity handle — gfxcoopa's replacement
 * for returning raw VkImageView from public accessors.
 */

#ifndef COOPA_GFX_TYPES_TEXTURE_VIEW_H
#define COOPA_GFX_TYPES_TEXTURE_VIEW_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace coopa {
namespace gfx {

/**
 * @class TextureView
 * @brief An opaque, copyable identity token for a GPU texture view.
 *
 * Returned by every gfxcoopa accessor that names a texture view
 * (Texture::view(), every render target's `*_view()`, etc.) in place of a
 * raw `VkImageView`. It serves three needs consumers have:
 *
 *  - **Hashable**: usable as an `unordered_map`/`unordered_set` key for
 *    per-texture descriptor-set caches and packed sort keys.
 *  - **A well-defined null/invalid value**: TextureView::null() (default
 *    constructed) replaces `VK_NULL_HANDLE` as an "asset not resident yet"
 *    sentinel.
 *  - **Cheaply fabricable in tests**: the explicit `uint64_t` constructor
 *    lets test code build fake-but-distinct identities without touching
 *    Vulkan at all, replacing patterns like
 *    `reinterpret_cast<VkImageView>(0x1000)`.
 *
 * TextureView deliberately carries no state beyond identity — it does NOT
 * track image layout/usage (that lives on memory::Image, see TextureUsage).
 * A copyable value that also carried mutable GPU state would silently go
 * stale the moment the underlying image transitioned; keeping TextureView a
 * pure identity token avoids that trap entirely.
 *
 * Conversion to/from the underlying VkImageView is intentionally NOT
 * available here — it lives in the internal `gfxcoopa/detail/vk_convert.h`,
 * which only gfxcoopa's own implementation may include.
 */
class TextureView {
public:
    /// @brief Constructs a null/invalid TextureView.
    constexpr TextureView() = default;

    /**
     * @brief Constructs a TextureView with an explicit opaque identity.
     *
     * Intended for test code fabricating distinct-but-fake texture
     * identities (e.g. `TextureView{0x1000}`), and for gfxcoopa's own
     * internal wrap()/unwrap() conversion in detail/vk_convert.h.
     *
     * @param id Opaque identity value. Zero is reserved for null().
     */
    constexpr explicit TextureView(uint64_t id) : id_(id) {}

    /// @brief Returns the canonical null/invalid TextureView.
    static constexpr TextureView null() { return TextureView{}; }

    /// @brief True if this handle is not null().
    constexpr bool valid() const { return id_ != 0; }

    /// @brief Equivalent to valid().
    constexpr explicit operator bool() const { return valid(); }

    /// @brief The opaque identity value. Meaningful only for hashing,
    /// equality, and ordering — never interpret this as an address.
    constexpr uint64_t id() const { return id_; }

    friend constexpr bool operator==(TextureView, TextureView) = default;
    friend constexpr auto operator<=>(TextureView, TextureView) = default;

private:
    uint64_t id_ = 0;
};

} // namespace gfx
} // namespace coopa

/// @brief Hash support so TextureView can key `unordered_map`/`unordered_set`,
/// exactly as VkImageView was being used for before the seal.
template <>
struct std::hash<coopa::gfx::TextureView> {
    size_t operator()(coopa::gfx::TextureView v) const noexcept {
        return std::hash<uint64_t>{}(v.id());
    }
};

#endif // COOPA_GFX_TYPES_TEXTURE_VIEW_H
