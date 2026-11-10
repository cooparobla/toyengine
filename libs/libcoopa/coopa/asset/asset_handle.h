/**
 * @file asset_handle.h
 * @brief Refcounted, typed reference to an asset owned by an AssetManager.
 */

#ifndef COOPA_ASSET_ASSET_HANDLE_H
#define COOPA_ASSET_ASSET_HANDLE_H

#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_slot.h>
#include <coopa/asset/asset_state.h>

#include <string>

namespace coopa {
namespace asset {

class AssetManager;  // AssetHandle's constructor is private; only AssetManager builds one.

/**
 * @class AssetHandle
 * @brief Refcounted, typed reference to an asset managed by AssetManager.
 *
 * A handle keeps its slot's ref_count above zero for as long as it (or any
 * copy of it) is alive; AssetManager may evict a slot's payload once
 * ref_count drops to zero (see AssetManager::unload()/garbage_collect()).
 * Because AssetSlot is address-stable (owned in a unique_ptr map), a hot
 * reload can swap the payload inside the slot without invalidating any
 * outstanding handle — get()/operator-> simply start returning the new
 * payload, and revision() lets a consumer detect that its own derived state
 * (e.g. a GPU pipeline built from a reloaded shader) needs rebuilding.
 *
 * Main-thread only, matching every other GPU-adjacent type in this
 * workspace: construct, copy, query and destroy handles from the same
 * thread that drives AssetManager::update().
 *
 * @tparam T The asset's concrete payload type (e.g. gfx::data::Mesh).
 *
 * @code
 * coopa::asset::AssetHandle<Mesh> mesh = assets.load<Mesh>("meshes/cube.yaml");
 * if (mesh.is_loaded()) {
 *     mesh->bind(cmd);
 * }
 * @endcode
 */
template <typename T>
class AssetHandle {
public:
    /** @brief Constructs an empty (invalid) handle referencing no slot. */
    AssetHandle() = default;

    AssetHandle(const AssetHandle& other) : slot_(other.slot_) { retain_(); }

    AssetHandle(AssetHandle&& other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }

    AssetHandle& operator=(const AssetHandle& other) {
        if (this == &other) return *this;
        release_();
        slot_ = other.slot_;
        retain_();
        return *this;
    }

    AssetHandle& operator=(AssetHandle&& other) noexcept {
        if (this == &other) return *this;
        release_();
        slot_ = other.slot_;
        other.slot_ = nullptr;
        return *this;
    }

    ~AssetHandle() { release_(); }

    /** @brief True if this handle references a slot (regardless of its load state). */
    bool is_valid() const { return slot_ != nullptr; }

    /** @brief Current lifecycle state, or AssetState::Unloaded if this handle is empty. */
    AssetState state() const { return slot_ ? slot_->state : AssetState::Unloaded; }

    /** @brief True once the payload is published and safe to dereference. */
    bool is_loaded() const { return slot_ && slot_->state == AssetState::Loaded; }

    /** @brief True once the load has failed; see error() for details. */
    bool is_failed() const { return slot_ && slot_->state == AssetState::Failed; }

    /** @brief Error message set when is_failed() is true; empty otherwise. */
    const std::string& error() const {
        static const std::string empty;
        return slot_ ? slot_->error : empty;
    }

    /** @brief The identity this handle was loaded for, or an empty AssetId if invalid. */
    const AssetId& id() const {
        static const AssetId empty;
        return slot_ ? slot_->id : empty;
    }

    /**
     * @brief Bumped every time the underlying payload is (re)published, including the first load.
     *
     * Cache this value alongside derived state built from get(); when it
     * changes, the derived state is stale and must be rebuilt.
     */
    uint32_t revision() const { return slot_ ? slot_->revision : 0; }

    /** @brief Pointer to the current payload, or nullptr unless is_loaded(). */
    const T* get() const {
        if (!slot_ || slot_->state != AssetState::Loaded) return nullptr;
        return static_cast<const T*>(slot_->payload.get());
    }

    const T& operator*() const { return *get(); }
    const T* operator->() const { return get(); }

    /** @brief True when is_loaded() — lets a handle be used directly as a boolean condition. */
    explicit operator bool() const { return is_loaded(); }

private:
    friend class AssetManager;

    explicit AssetHandle(detail::AssetSlot* slot) : slot_(slot) { retain_(); }

    void retain_() {
        if (slot_) ++slot_->ref_count;
    }

    void release_() {
        if (slot_ && slot_->ref_count > 0) --slot_->ref_count;
        slot_ = nullptr;
    }

    detail::AssetSlot* slot_ = nullptr; /**< Non-owning; the owning AssetManager keeps the slot alive. */
};

} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_HANDLE_H
