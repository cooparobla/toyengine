#ifndef GFXCOOPA_TYPES_IMAGE_HANDLE_H
#define GFXCOOPA_TYPES_IMAGE_HANDLE_H

/**
 * @file image_handle.h
 * @brief An opaque identity token for a GPU image -- the sibling of TextureView.
 */

#include <cstdint>
#include <functional>

namespace coopa {
namespace gfx {

/**
 * @class ImageHandle
 * @brief An opaque, copyable identity token for a GPU image, and gfxcoopa's replacement for
 *        handing a raw VkImage to a consumer. TextureView names a VIEW; this names the image.
 *
 * Carries identity only -- no layout, no format, no extent -- for the same reason TextureView
 * carries none: a copyable value holding mutable GPU state goes stale the moment a pass
 * transitions the image behind it. A caller that needs the layout states it in the barrier.
 */
class ImageHandle {
public:
    constexpr ImageHandle() = default;
    /** @brief Wraps an opaque id. gfxcoopa-internal; consumers receive these from accessors. */
    constexpr explicit ImageHandle(uint64_t id) : id_(id) {}

    /** @brief The null handle, which every barrier and copy treats as "no image". */
    static constexpr ImageHandle null() { return ImageHandle{}; }

    constexpr bool valid() const { return id_ != 0; }
    constexpr explicit operator bool() const { return valid(); }
    constexpr uint64_t id() const { return id_; }

    friend constexpr bool operator==(ImageHandle, ImageHandle) = default;

private:
    uint64_t id_ = 0;
};

} // namespace gfx
} // namespace coopa

namespace std {
/** @brief Hash support, so an ImageHandle can key a cache. */
template <> struct hash<coopa::gfx::ImageHandle> {
    size_t operator()(coopa::gfx::ImageHandle h) const noexcept { return hash<uint64_t>{}(h.id()); }
};
}

#endif // GFXCOOPA_TYPES_IMAGE_HANDLE_H
