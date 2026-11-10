/**
 * @file asset_slot.h
 * @brief Address-stable, type-erased storage for one loaded asset.
 */

#ifndef COOPA_ASSET_ASSET_SLOT_H
#define COOPA_ASSET_ASSET_SLOT_H

#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_state.h>

#include <memory>
#include <string>
#include <typeindex>
#include <utility>

namespace coopa {
namespace asset {
namespace detail {

/**
 * @struct AssetSlot
 * @brief One asset's identity, lifecycle state, and published payload.
 *
 * AssetManager owns every AssetSlot behind a std::unique_ptr in a map keyed
 * by AssetId::hash(), so a slot's address never changes for the lifetime of
 * the manager — every AssetHandle<T> can safely hold a raw AssetSlot*
 * for as long as its refcount keeps the slot alive. Hot reload swaps
 * `payload` in place and increments `revision`, so outstanding handles
 * observe the new payload without ever needing to be reissued; this
 * indirection is the reason AssetHandle is used instead of a raw
 * shared_ptr<T>, which would have to be reissued on every reload.
 *
 * Main-thread only: like every GPU-owning type in this workspace (Device,
 * Allocator, CommandPool, ...), an AssetSlot is read and written exclusively
 * from the thread that drives AssetManager::update(). A loader's decode()
 * stage may run on a worker thread, but it only ever touches its own
 * decoded intermediate value passed through AssetManager's PendingLoad —
 * never the slot itself.
 */
struct AssetSlot {
    AssetId         id;                          /**< Identity this slot was loaded for. */
    std::type_index type;                         /**< Concrete asset type, for mismatch/debug checks. */
    AssetState      state = AssetState::Unloaded; /**< Current lifecycle state. */
    uint32_t        ref_count = 0;                /**< Outstanding AssetHandle<T> instances referencing this slot. */
    uint32_t        revision = 0;                 /**< Incremented every time payload is (re)published; 0 until first load. */
    std::shared_ptr<void> payload;                 /**< Type-erased current payload; valid when state == Loaded. */
    std::string     error;                         /**< Populated when state == Failed. */
    std::string     resolved_path;                 /**< Filesystem path last read from; used for hot-reload polling. */
    long long       last_write_time_ns = 0;        /**< mtime snapshot (ns since epoch) at last (re)load. */
    uint32_t        idle_frames = 0;                /**< Consecutive AssetManager::update() calls with ref_count == 0 and no load in flight; reset to 0 otherwise. Only consulted when idle eviction is enabled. */

    /**
     * @brief Constructs a slot for the given identity and asset type.
     * @param id_in   Identity this slot represents.
     * @param type_in Concrete asset type (e.g. std::type_index(typeid(Mesh))).
     */
    AssetSlot(AssetId id_in, std::type_index type_in)
        : id(std::move(id_in)), type(type_in) {}
};

} // namespace detail
} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_SLOT_H
