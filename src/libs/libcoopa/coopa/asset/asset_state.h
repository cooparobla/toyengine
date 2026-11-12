/**
 * @file asset_state.h
 * @brief Lifecycle states an AssetSlot moves through.
 */

#ifndef COOPA_ASSET_ASSET_STATE_H
#define COOPA_ASSET_ASSET_STATE_H

namespace coopa {
namespace asset {

/**
 * @brief Lifecycle state of an asset slot.
 *
 * Unloaded -> Loading -> Loaded, or Unloaded -> Loading -> Failed. A Loaded
 * slot can return to Loading -> Loaded again via hot reload, without ever
 * passing back through Unloaded — outstanding AssetHandle<T>s stay valid the
 * whole time (see coopa/asset/asset_slot.h).
 */
enum class AssetState {
    Unloaded, /**< No load has been requested for this slot yet. */
    Loading,  /**< A load is in flight; decode() and/or finalize() have not both completed. */
    Loaded,   /**< A payload is published and safe to dereference through a handle. */
    Failed,   /**< The load failed; see the owning AssetSlot/AssetHandle's error() for details. */
};

} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_STATE_H
