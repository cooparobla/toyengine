/**
 * @file asset_manager.h
 * @brief Facade for asset registration, loading, caching, hot reload, and shutdown.
 */

#ifndef COOPA_ASSET_ASSET_MANAGER_H
#define COOPA_ASSET_ASSET_MANAGER_H

#include <coopa/asset/asset_handle.h>
#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_slot.h>
#include <coopa/asset/asset_source.h>
#include <coopa/asset/asset_state.h>

#include <coopa/debug/logger.h>
#include <coopa/event/signal.h>
#include <coopa/job/engine.h>
#include <coopa/job/handle.h>

#include <memory>
#include <chrono>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coopa {
namespace asset {

/**
 * @class AssetManager
 * @brief Owns loader registration, slot caching, async loading, and hot reload for every asset type.
 *
 * Downstream packages register their own asset types by providing a loader
 * (see IAssetLoader/TypedAssetLoader) — gfxcoopa registers Mesh, Texture,
 * Shader; uicoopa registers Sprite, Font — exactly as they already register
 * scene components via coopa::scene::SceneLoader::register_component_parser.
 * AssetManager itself never names any of those types.
 *
 * Main-thread only: construct, register loaders on, load from, and destroy
 * an AssetManager from a single thread — the same thread that owns the
 * Device/Allocator/CommandPool it hands to its registered loaders. The one
 * exception is a loader's decode() stage, which AssetManager itself may run
 * on a worker thread when load_async() is used; decode() implementations
 * must not touch the GPU (see IAssetLoader).
 *
 * @code
 * coopa::job::JobEngine engine;
 * coopa::asset::AssetManager assets(&engine);
 * assets.add_search_root(std::string(ROOT_DIR) + "/assets");
 * assets.register_loader<Mesh>(std::make_unique<MeshLoader>(device, allocator, cmd_pool));
 *
 * coopa::asset::AssetHandle<Mesh> mesh = assets.load<Mesh>("meshes/cube.yaml");
 * // Per frame:
 * assets.update(delta_time);
 * // Before Device/Allocator are destroyed:
 * assets.shutdown();
 * @endcode
 */
class AssetManager {
public:
    /**
     * @brief Constructs an AssetManager, optionally sharing an existing JobEngine.
     *
     * Handles are reclaimed individually (see handle.h's class doc), so
     * decode() jobs can safely share the application's own JobEngine — pass it
     * in, and this AssetManager submits decode() work at Priority::Low so it
     * never preempts frame work. Passing nullptr gets a private engine
     * instead.
     *
     * @param engine Engine to submit decode() jobs on. If nullptr, this
     *   AssetManager constructs and owns a private fallback engine instead.
     * @param fallback_io_threads Worker threads for the private fallback
     *   engine. Ignored if `engine` is non-null.
     */
    explicit AssetManager(coopa::job::JobEngine* engine = nullptr, unsigned int fallback_io_threads = 2)
        : owned_engine_(engine ? nullptr : std::make_unique<coopa::job::JobEngine>(fallback_io_threads)),
          engine_(engine ? engine : owned_engine_.get()),
          logger_(new coopa::debug::Logger("AssetManager")) {}

    ~AssetManager() {
        shutdown();
        delete logger_;
    }

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /// @brief Fired whenever a slot's payload is (re)published after its first load — i.e. on hot reload.
    coopa::event::Signal<const AssetId&> on_reloaded;

    // --- Search roots ---

    /** @brief Registers a directory the resolver should search, in registration order. */
    void add_search_root(const std::string& dir) { source_.add_search_root(dir); }

    /** @brief The underlying path resolver, for loaders that need to resolve sibling files. */
    AssetSource& source() { return source_; }
    const AssetSource& source() const { return source_; }

    // --- Loader registration ---

    /**
     * @brief Registers the loader responsible for asset type T.
     *
     * Registering the same type twice replaces the previous loader.
     * @tparam T Asset payload type, e.g. gfx::data::Mesh.
     * @param loader Loader instance (typically a TypedAssetLoader<T, ...> subclass).
     */
    template <typename T>
    void register_loader(std::unique_ptr<IAssetLoader> loader) {
        loaders_[std::type_index(typeid(T))] = std::move(loader);
    }

    /** @brief Clears every registered loader. Call before the objects a loader's closure/ctor captured (Device, Allocator, ...) are destroyed. */
    void clear_loaders() { loaders_.clear(); }

    /**
     * @brief Returns the loader registered for asset type T, or nullptr if none is registered.
     *
     * Lets a caller reach loader-specific configuration methods (e.g.
     * gfx::loaders::TextureLoader::declare_color_space()) that IAssetLoader's type-erased
     * interface doesn't expose — the caller is responsible for downcasting to the concrete
     * loader type it registered.
     *
     * @tparam T Asset payload type whose loader to look up.
     * @return The registered IAssetLoader, or nullptr if register_loader<T>() was never called.
     */
    template <typename T>
    IAssetLoader* loader() {
        auto it = loaders_.find(std::type_index(typeid(T)));
        return it != loaders_.end() ? it->second.get() : nullptr;
    }

    /** @brief Const overload of loader<T>(). */
    template <typename T>
    const IAssetLoader* loader() const {
        auto it = loaders_.find(std::type_index(typeid(T)));
        return it != loaders_.end() ? it->second.get() : nullptr;
    }

    // --- Loading ---

    /**
     * @brief Loads (or returns the cached handle for) an asset, synchronously.
     *
     * If a load for this id is already in flight via load_async(), this
     * completes it immediately on the calling thread rather than starting a
     * duplicate load.
     *
     * @tparam T Asset payload type; must have a loader registered via register_loader<T>().
     * @param virtual_path Path as referenced from YAML/scene files.
     * @param base_dir     Optional extra search root tried first (e.g. the loading scene's directory).
     * @return A handle to the slot. Check is_loaded()/is_failed() — this never throws.
     */
    template <typename T>
    AssetHandle<T> load(const std::string& virtual_path, const std::string& base_dir = "") {
        std::string resolved_path = source_.resolve(virtual_path, base_dir);
        AssetId id = AssetId::from_path(resolved_path);
        detail::AssetSlot* slot = find_or_create_slot_(id, std::type_index(typeid(T)));
        if (!check_type_(*slot, std::type_index(typeid(T)), id)) {
            return AssetHandle<T>();
        }

        if (slot->state == AssetState::Loaded || slot->state == AssetState::Failed) {
            return AssetHandle<T>(slot);
        }

        if (slot->state == AssetState::Loading) {
            auto it = pending_.find(slot);
            if (it != pending_.end()) {
                engine_->wait_for(it->second.handle);
                complete_pending_(it->second);
                pending_.erase(it);
            }
            return AssetHandle<T>(slot);
        }

        auto loader_it = loaders_.find(std::type_index(typeid(T)));
        if (loader_it == loaders_.end()) {
            fail_slot_(*slot, "No loader registered for this asset type");
            return AssetHandle<T>(slot);
        }

        LoadContext ctx = make_context_(resolved_path, *slot);
        slot->state = AssetState::Loading;
        try {
            std::shared_ptr<void> decoded = loader_it->second->decode(id, ctx);
            std::shared_ptr<void> payload = loader_it->second->finalize(std::move(decoded), id, ctx);
            publish_(*slot, std::move(payload), ctx);
        } catch (const std::exception& e) {
            fail_slot_(*slot, e.what());
        }

        return AssetHandle<T>(slot);
    }

    /**
     * @brief Kicks off an asynchronous load on this manager's JobEngine, at
     *        Priority::Low so it never preempts ordinary frame work.
     *
     * decode() runs on a worker thread; finalize() runs on the main thread
     * inside a later update() call. If a load is already in flight or
     * complete for this id, this simply returns a handle to the existing
     * slot without starting anything new. If the engine's handle pool is
     * exhausted, the slot is marked Failed rather than blocking -- handle
     * pool exhaustion recovers on its own as in-flight handles are closed
     * (see handle.h), so a retried load_async() shortly after will typically
     * succeed.
     *
     * @tparam T Asset payload type; must have a loader registered via register_loader<T>().
     * @param virtual_path Path as referenced from YAML/scene files.
     * @param base_dir     Optional extra search root tried first.
     * @return A handle to the slot; check state()/is_loaded() over subsequent frames.
     */
    template <typename T>
    AssetHandle<T> load_async(const std::string& virtual_path, const std::string& base_dir = "") {
        std::string resolved_path = source_.resolve(virtual_path, base_dir);
        AssetId id = AssetId::from_path(resolved_path);
        detail::AssetSlot* slot = find_or_create_slot_(id, std::type_index(typeid(T)));
        if (!check_type_(*slot, std::type_index(typeid(T)), id)) {
            return AssetHandle<T>();
        }

        if (slot->state != AssetState::Unloaded) {
            return AssetHandle<T>(slot);
        }

        auto loader_it = loaders_.find(std::type_index(typeid(T)));
        if (loader_it == loaders_.end()) {
            fail_slot_(*slot, "No loader registered for this asset type");
            return AssetHandle<T>(slot);
        }
        IAssetLoader* loader = loader_it->second.get();

        LoadContext ctx = make_context_(resolved_path, *slot);

        coopa::job::JobHandle handle = engine_->create_handle();
        if (!handle.is_valid()) {
            fail_slot_(*slot, "Asset IO job handle pool exhausted; retry load_async() shortly");
            return AssetHandle<T>(slot);
        }

        slot->state = AssetState::Loading;

        PendingLoad& pending = pending_[slot];
        pending.slot = slot;
        pending.handle = handle;
        pending.loader = loader;
        pending.ctx = ctx;

        PendingLoad* pending_ptr = &pending;
        engine_->submit([pending_ptr, id, ctx]() {
            try {
                pending_ptr->decoded = pending_ptr->loader->decode(id, ctx);
            } catch (const std::exception& e) {
                pending_ptr->decode_failed = true;
                pending_ptr->error = e.what();
            }
        }, k_asset_io_job_type, pending.handle, nullptr, 0u, coopa::job::Priority::Low);

        return AssetHandle<T>(slot);
    }

    /**
     * @brief Looks up a handle for an already-known id without triggering a load.
     *
     * Resolves virtual_path/base_dir the same way load()/load_async() do, so
     * a lookup here reliably finds a slot created by either of them.
     *
     * @tparam T Asset payload type.
     * @param virtual_path Path as referenced from YAML/scene files.
     * @param base_dir     Optional extra search root, matching whatever load() call created the slot.
     * @return A handle to the existing slot, or an empty (invalid) handle if none exists.
     */
    template <typename T>
    AssetHandle<T> get(const std::string& virtual_path, const std::string& base_dir = "") const {
        AssetId id = AssetId::from_path(source_.resolve(virtual_path, base_dir));
        auto it = slots_.find(id);
        if (it == slots_.end()) return AssetHandle<T>();
        if (it->second->type != std::type_index(typeid(T))) return AssetHandle<T>();
        return AssetHandle<T>(it->second.get());
    }

    /**
     * @brief Publishes an already-constructed payload into a slot, bypassing
     * AssetSource and the loader entirely.
     *
     * For procedurally-generated runtime assets (a mesh built on the fly, a
     * texture rendered into at startup) that have no file behind them: no
     * resolve(), no decode(), no finalize(), and no loader needs to be
     * registered for T. Because the slot's resolved_path is left empty,
     * hot-reload polling skips it by construction (poll_for_reloads_()
     * already bails on an empty resolved_path).
     *
     * Calling this again with the same synthetic_id ALWAYS re-publishes: the
     * previous payload is retired with the same grace period a hot reload
     * uses, revision is bumped, and on_reloaded fires. This differs
     * deliberately from load(), whose second call is a convenience cache
     * hit — handing in a freshly constructed payload is an explicit
     * statement of intent to replace, and silently dropping it on the floor
     * would leak the caller's work.
     *
     * Calling create() repeatedly on the same id every frame retires one
     * payload per frame, each held for k_payload_grace_frames — fine
     * occasionally, not something to do in a hot loop with large GPU payloads.
     *
     * @tparam T Asset payload type.
     * @param synthetic_id Logical identity, normalized exactly like a path
     *   (see AssetId). Prefix these (e.g. "runtime/terrain_chunk_0") to keep
     *   them from colliding with a real asset's resolved path.
     * @param payload Fully constructed payload; must not be null.
     * @return A handle to the slot. Empty on a type mismatch; a Failed slot
     *   on a null payload or on an id already backed by a file-loaded asset.
     */
    template <typename T>
    AssetHandle<T> create(const std::string& synthetic_id, std::shared_ptr<T> payload) {
        AssetId id = AssetId::from_path(synthetic_id);  // no source_.resolve() -- not a real file
        detail::AssetSlot* slot = find_or_create_slot_(id, std::type_index(typeid(T)));
        if (!check_type_(*slot, std::type_index(typeid(T)), id)) {
            return AssetHandle<T>();
        }

        if (!payload) {
            fail_slot_(*slot, "create() called with a null payload");
            return AssetHandle<T>(slot);
        }

        if (pending_.find(slot) != pending_.end()) {
            // An async load is already in flight for this id -- publishing
            // now would be clobbered by complete_pending_() later anyway,
            // and would itself get silently overwritten. Refuse rather than
            // race it.
            if (logger_) {
                logger_->error("[" + id.path() + "] create() called while a load_async() is still in flight for this id");
            }
            return AssetHandle<T>(slot);
        }

        if (!slot->resolved_path.empty()) {
            // This id was previously created by load()/load_async() from a
            // real file. Publishing here would clear resolved_path's
            // association with that file only in appearance -- publish_()
            // would still stamp last_write_time_ns from ctx.resolved_path
            // (empty), so the next hot-reload poll would see the *real*
            // file's mtime as newer than 0 and silently reload over this
            // procedural payload. Refuse instead of fighting that.
            if (logger_) {
                logger_->error("[" + id.path() + "] create() called on an id already backed by a loaded file ('" +
                                slot->resolved_path + "'); refusing to overwrite it");
            }
            return AssetHandle<T>(slot);
        }

        LoadContext ctx;  // resolved_path deliberately left empty
        ctx.source = &source_;
        publish_(*slot, std::move(payload), ctx);
        return AssetHandle<T>(slot);
    }

    /**
     * @brief Requests eviction of an asset's slot.
     *
     * If any AssetHandle still references the slot (or a load for it is in
     * flight), this is a no-op — call garbage_collect() once handles are
     * dropped, or rely on it running periodically.
     * @param id Identity to evict.
     */
    void unload(const AssetId& id) {
        auto it = slots_.find(id);
        if (it == slots_.end()) return;
        if (it->second->ref_count > 0) return;
        if (pending_.find(it->second.get()) != pending_.end()) return;
        retire_payload_(*it->second);
        slots_.erase(it);
    }

    /** @brief Evicts every slot whose ref_count has dropped to zero and has no load in flight. */
    void garbage_collect() {
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (it->second->ref_count == 0 && pending_.find(it->second.get()) == pending_.end()) {
                retire_payload_(*it->second);
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // --- Idle eviction ---

    /**
     * @brief Enables/disables automatic eviction of slots unreferenced for max_idle_frames() (checked inside update()). Off by default.
     *
     * Complements, not replaces, garbage_collect(): garbage_collect() is for
     * a caller that knows eviction is safe *right now* (e.g. immediately
     * after a vkQueueWaitIdle at a level transition); idle eviction is for
     * unattended, steady-state background reclamation. Both retire a slot's
     * payload with the same grace period (see retire_payload_()).
     *
     * A Failed slot (payload already null) is evicted like any other once
     * idle — meaning a later load() of the same path retries instead of
     * serving a cached error forever. A create()d slot has no file to
     * reload from once evicted; the existing refcount is what pins it —
     * hold a handle to any procedural asset you want to keep.
     */
    void set_idle_eviction(bool enabled) { idle_eviction_enabled_ = enabled; }

    /** @brief Consecutive idle update() calls before a slot is evicted. Default 300 (~5s at 60fps). 0 evicts on the first idle update(). */
    void set_max_idle_frames(uint32_t frames) { max_idle_frames_ = frames; }

    bool idle_eviction_enabled() const { return idle_eviction_enabled_; }

    uint32_t max_idle_frames() const { return max_idle_frames_; }

    // --- Hot reload ---

    /** @brief Enables/disables mtime-polling hot reload (checked inside update()). Off by default. */
    void set_hot_reload(bool enabled) { hot_reload_enabled_ = enabled; }

    /**
     * @brief Runs hot reload's change check now, regardless of set_hot_reload() and the poll
     *        interval: every referenced, loaded asset whose file changed on disk is re-decoded
     *        and re-published (old payloads retired with the usual grace period). For a tool
     *        that just wrote an asset and needs every user -- render meshes, colliders -- current
     *        before the next frame, not up to a poll interval later.
     */
    void reload_changed() { poll_for_reloads_(); }

    /** @brief Seconds between hot-reload polling passes. Default 1.0. */
    void set_poll_interval(float seconds) { poll_interval_ = seconds; }

    bool hot_reload_enabled() const { return hot_reload_enabled_; }

    /// @brief Number of async loads still in flight; 0 once every load_async() has been finalized.
    std::size_t pending_load_count() const { return pending_.size(); }

    // --- Per-frame pump ---

    /**
     * @brief Drains completed async loads, ages out retired payloads, sweeps idle slots, and (if enabled) polls for hot reload.
     *
     * Call once per frame from the main thread, after any GPU work that
     * might reference a previous frame's assets has been submitted.
     *
     * @param delta_time Frame delta time in seconds; only used for hot-reload poll timing.
     * @param finalize_budget_ms Caps the main-thread finalize() time this call spends (GPU
     *        uploads, mostly): once it is used up, the remaining decoded loads wait for the next
     *        update(). At least one load is finalized per call, so loading always progresses.
     *        Negative (the default) finalizes everything that is ready.
     */
    void update(float delta_time = 0.0f, float finalize_budget_ms = -1.0f) {
        const bool budgeted = finalize_budget_ms >= 0.0f;
        const auto t0 = budgeted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        bool finalized_any = false;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (budgeted && finalized_any &&
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count() >= finalize_budget_ms) {
                break;
            }
            if (it->second.handle.is_complete()) {
                complete_pending_(it->second);
                it = pending_.erase(it);
                finalized_any = true;
            } else {
                ++it;
            }
        }

        for (auto it = retired_.begin(); it != retired_.end();) {
            if (--(it->second) <= 0) {
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }

        // Must run after the retired_ aging loop above (and before the
        // hot-reload poll below, which is the same position
        // poll_for_reloads_() already occupies) so a payload evicted this
        // call gets the full k_payload_grace_frames of update() calls, not
        // one frame less.
        if (idle_eviction_enabled_) {
            sweep_idle_();
        }

        if (hot_reload_enabled_) {
            poll_accum_ += delta_time;
            if (poll_accum_ >= poll_interval_) {
                poll_accum_ = 0.0f;
                poll_for_reloads_();
            }
        }
    }

    /**
     * @brief Drains in-flight loads, releases every payload, and clears loaders and slots.
     *
     * Must be called while whatever a registered loader's finalize() needs
     * (Device, Allocator, CommandPool, ...) is still alive. Safe to call more
     * than once; the destructor calls it too.
     *
     * This destroys every AssetSlot, including ones still referenced by an
     * outstanding AssetHandle<T> — do not dereference a handle after calling
     * this. In-flight loads are drained first (their decode() job is waited
     * on and finalize() is run) so no worker thread is left touching freed
     * state, but the resulting payload is simply discarded rather than
     * handed to whatever held the handle.
     */
    void shutdown() {
        for (auto& kv : pending_) {
            engine_->wait_for(kv.second.handle);
            complete_pending_(kv.second);
        }
        pending_.clear();
        retired_.clear();
        slots_.clear();
        loaders_.clear();
    }

private:
    /** @brief Bookkeeping for one in-flight load_async() call. */
    struct PendingLoad {
        detail::AssetSlot*    slot = nullptr;
        coopa::job::JobHandle handle;
        std::shared_ptr<void> decoded;       /**< Written by the worker; read only after handle.is_complete(). */
        std::string           error;         /**< Set by the worker on a decode() failure. */
        bool                  decode_failed = false;
        IAssetLoader*         loader = nullptr;
        LoadContext            ctx;
    };

    /// @brief Distinct JobType tag for asset IO jobs (never collides with an app's own JobEngine job types).
    static constexpr coopa::job::JobType k_asset_io_job_type = 0xA55E7000u;

    /// @brief Grace period (frames) a superseded payload is kept alive before actual destruction, so in-flight GPU work isn't yanked out from under it. Applies to every eviction path — hot reload, create() re-publish, unload(), garbage_collect(), and idle eviction — not just hot reload.
    static constexpr int k_payload_grace_frames = 3;

    detail::AssetSlot* find_or_create_slot_(const AssetId& id, std::type_index type) {
        auto it = slots_.find(id);
        if (it != slots_.end()) return it->second.get();
        auto slot = std::make_unique<detail::AssetSlot>(id, type);
        detail::AssetSlot* raw = slot.get();
        slots_.emplace(id, std::move(slot));
        return raw;
    }

    /** @brief Builds a LoadContext from an already-resolved path (see load()/load_async(), which resolve before this is called so AssetId reflects the resolved path, not the raw virtual one). */
    LoadContext make_context_(const std::string& resolved_path, detail::AssetSlot& slot) {
        LoadContext ctx;
        ctx.source = &source_;
        ctx.resolved_path = resolved_path;
        slot.resolved_path = resolved_path;
        return ctx;
    }

    /**
     * @brief Guards against two different C++ types being loaded from the same virtual path.
     *
     * AssetId is purely path-based (see coopa/asset/asset_id.h) and a slot is
     * keyed by AssetId::hash() alone, so a second load<Other>() of a path
     * already claimed by T would otherwise reinterpret that slot's
     * std::shared_ptr<void> payload as an unrelated type — undefined
     * behavior. Rather than allow that, any mismatch fails loudly (logged)
     * and the caller gets back an empty handle, never one bound to a slot
     * holding a different type's payload.
     */
    bool check_type_(detail::AssetSlot& slot, std::type_index requested, const AssetId& id) {
        if (slot.type == requested) return true;
        if (logger_) {
            logger_->error("[" + id.path() + "] Already loaded as a different asset type; "
                            "refusing to reinterpret its payload as the newly requested type");
        }
        return false;
    }

    void fail_slot_(detail::AssetSlot& slot, const std::string& message) {
        slot.state = AssetState::Failed;
        slot.error = message;
        if (logger_) logger_->error("[" + slot.id.path() + "] " + message);
    }

    /**
     * @brief Moves a slot's payload onto the retire list rather than destroying it now.
     *
     * A payload whose last reference is dropped THIS frame may still be
     * referenced by a command buffer already recorded (or submitted) this
     * frame, so destroying it immediately is a use-after-free on the GPU.
     * Every eviction path — hot reload (via publish_()), create() re-publish,
     * unload(), garbage_collect(), and idle eviction — goes through here so
     * none of them can reintroduce that bug.
     */
    void retire_payload_(detail::AssetSlot& slot) {
        if (slot.payload) {
            retired_.emplace_back(std::move(slot.payload), k_payload_grace_frames);
        }
    }

    /** @brief Publishes a finalized payload into a slot, retiring any payload it replaces and firing on_reloaded for anything past the first load. */
    void publish_(detail::AssetSlot& slot, std::shared_ptr<void> payload, const LoadContext& ctx) {
        bool is_reload = static_cast<bool>(slot.payload);
        retire_payload_(slot);
        slot.payload = std::move(payload);
        slot.state = AssetState::Loaded;
        slot.error.clear();
        ++slot.revision;
        slot.last_write_time_ns = AssetSource::last_write_time_ns(ctx.resolved_path);
        if (is_reload) on_reloaded.emit(slot.id);
    }

    /**
     * @brief Runs finalize() for a load whose decode() has completed,
     *        publishing or failing the slot, then closes its JobHandle.
     *
     * Closing here (rather than leaving it to the caller) means every path
     * that drains a PendingLoad -- update()'s poll loop, load()'s
     * synchronous-completion path, and shutdown()'s drain -- recycles the
     * handle's slot exactly once, in the one place that actually knows the
     * handle is done being examined.
     */
    void complete_pending_(PendingLoad& pending) {
        if (pending.decode_failed) {
            fail_slot_(*pending.slot, pending.error);
        } else {
            try {
                std::shared_ptr<void> payload = pending.loader->finalize(std::move(pending.decoded), pending.slot->id, pending.ctx);
                publish_(*pending.slot, std::move(payload), pending.ctx);
            } catch (const std::exception& e) {
                fail_slot_(*pending.slot, e.what());
            }
        }
        pending.handle.close();
    }

    /**
     * @brief Ages unreferenced slots out and evicts the ones past max_idle_frames_.
     *
     * The AssetSlot struct itself is erased immediately: once ref_count==0 no
     * handle holds its address, and once it's absent from pending_ no
     * PendingLoad holds it either, so nothing left in this system has a raw
     * AssetSlot* into it. The PAYLOAD is what needs the grace period, so it
     * is moved into retired_ (via retire_payload_()) BEFORE the slot is
     * erased — erasing first would run the payload's destructor inline,
     * defeating the whole point.
     */
    void sweep_idle_() {
        for (auto it = slots_.begin(); it != slots_.end();) {
            detail::AssetSlot& slot = *it->second;
            if (slot.ref_count > 0 || pending_.find(it->second.get()) != pending_.end()) {
                slot.idle_frames = 0;
                ++it;
                continue;
            }
            if (++slot.idle_frames < max_idle_frames_) {
                ++it;
                continue;
            }
            retire_payload_(slot);  // MUST precede the erase below
            it = slots_.erase(it);
        }
    }

    /** @brief Synchronously re-decodes/finalizes any referenced, Loaded slot whose file changed on disk. */
    void poll_for_reloads_() {
        for (auto& kv : slots_) {
            detail::AssetSlot& slot = *kv.second;
            if (slot.ref_count == 0) continue;
            if (slot.state != AssetState::Loaded) continue;
            if (slot.resolved_path.empty()) continue;

            long long mtime = AssetSource::last_write_time_ns(slot.resolved_path);
            if (mtime == 0 || mtime <= slot.last_write_time_ns) continue;

            auto loader_it = loaders_.find(slot.type);
            if (loader_it == loaders_.end()) continue;

            LoadContext ctx;
            ctx.source = &source_;
            ctx.resolved_path = slot.resolved_path;
            try {
                std::shared_ptr<void> decoded = loader_it->second->decode(slot.id, ctx);
                std::shared_ptr<void> payload = loader_it->second->finalize(std::move(decoded), slot.id, ctx);
                publish_(slot, std::move(payload), ctx);
            } catch (const std::exception& e) {
                if (logger_) logger_->warn("[hot-reload] " + slot.id.path() + ": " + e.what());
                // Keep serving the last-good payload rather than flipping to
                // Failed on a transient reload error (e.g. editor mid-save).
            }
        }
    }

    /// Keyed by the whole AssetId (hash + resolved path), not by the bare
    /// hash: two different paths that collided on the 64-bit hash would
    /// otherwise be served the same slot.
    std::unordered_map<AssetId, std::unique_ptr<detail::AssetSlot>> slots_;
    std::unordered_map<detail::AssetSlot*, PendingLoad>               pending_;
    std::unordered_map<std::type_index, std::unique_ptr<IAssetLoader>> loaders_;
    std::vector<std::pair<std::shared_ptr<void>, int>>                 retired_; /**< {payload, frames_remaining}. */

    AssetSource            source_;
    std::unique_ptr<coopa::job::JobEngine> owned_engine_; /**< Only set when no external engine was supplied. */
    coopa::job::JobEngine*  engine_ = nullptr;            /**< Non-owning; submits decode() jobs here at Priority::Low. */

    bool  hot_reload_enabled_ = false;
    float poll_interval_ = 1.0f;
    float poll_accum_ = 0.0f;

    bool     idle_eviction_enabled_ = false;
    uint32_t max_idle_frames_ = 300;

    coopa::debug::Logger* logger_ = nullptr;
};

} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_MANAGER_H
