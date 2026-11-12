/**
 * @file render_pass_ref.h
 * @brief An opaque reference to a render pass somebody else owns.
 *
 * pipeline::RenderPass is the owning type, and a pipeline built for a pass that gfxcoopa owns
 * takes that object. But several passes are built against a render pass owned by the *host* --
 * a guest pass drawing into the application's own forward pass, or into a target's internally
 * created one -- and there the pipeline needs the pass's identity without owning it.
 *
 * That is what this is. Identity only, and borrowed: a RenderPassRef must not outlive the pass
 * it names, exactly as a VkRenderPass handle must not. It replaces passing the raw handle, so a
 * consumer never has to name a Vulkan type to hand one pass to another.
 *
 * A pipeline built from a ref cannot read the pass's attachment count or sample count (there is
 * no object to ask), so its PipelineDesc has to state both -- which is the real cost of the
 * borrow, and the reason the owning RenderPass overload is the one to prefer.
 */

#ifndef COOPA_GFX_TYPES_RENDER_PASS_REF_H
#define COOPA_GFX_TYPES_RENDER_PASS_REF_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace coopa {
namespace gfx {

/**
 * @class RenderPassRef
 * @brief An opaque, copyable, hashable identity token for a borrowed render pass.
 */
class RenderPassRef {
public:
    /// @brief Constructs a null reference -- "no render pass".
    constexpr RenderPassRef() = default;

    /**
     * @brief Constructs a reference with an explicit opaque identity.
     *
     * For gfxcoopa's own wrap/unwrap in detail/vk_convert.h, and for test code fabricating
     * distinct-but-fake pass identities.
     *
     * @param id Opaque identity value. Zero is reserved for null().
     */
    constexpr explicit RenderPassRef(uint64_t id) : id_(id) {}

    /// @brief The null reference.
    static constexpr RenderPassRef null() { return RenderPassRef{}; }

    /// @brief Whether this names a render pass.
    constexpr explicit operator bool() const { return id_ != 0; }

    /// @brief The opaque identity value.
    constexpr uint64_t id() const { return id_; }

    friend constexpr bool operator==(RenderPassRef, RenderPassRef) = default;
    friend constexpr auto operator<=>(RenderPassRef, RenderPassRef) = default;

private:
    uint64_t id_ = 0;
};

} // namespace gfx
} // namespace coopa

namespace std {

/// @brief Hash support, so a RenderPassRef can key a per-pass pipeline cache.
template <>
struct hash<coopa::gfx::RenderPassRef> {
    size_t operator()(coopa::gfx::RenderPassRef r) const noexcept {
        return hash<uint64_t>{}(r.id());
    }
};

} // namespace std

#endif // COOPA_GFX_TYPES_RENDER_PASS_REF_H
