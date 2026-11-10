/**
 * @file asset_manager_test.cpp
 * @brief coopa::asset::AssetManager and AssetId: path identity, sync/async load and caching,
 *        the type-mismatch and base-dir collision guards, hot reload, unload/GC, shutdown
 *        draining in-flight loads, runtime create()d payloads, and the payload grace period on
 *        every eviction path.
 *
 * Handle-lifetime note that applies to every test here: AssetManager::shutdown() destroys every
 * AssetSlot, and an AssetHandle's destructor dereferences its slot -- so a test only calls
 * shutdown() explicitly once no handle is alive; otherwise `assets` (declared before its
 * handles) shuts itself down after they are destroyed. Not covered: AssetSource on its own
 * (every load here goes through it, and hot reload covers its mtime query).
 */
#include <coopa/testing/test.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/event/signal.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("asset_manager");

namespace {

/** @brief Trivial test asset: an in-memory string, decoded from a file's raw bytes. */
struct TextAsset {
    std::string contents;
};

/** @brief Loader for TextAsset: decode() reads bytes off-thread, finalize() just wraps them (no GPU step). */
class TextAssetLoader : public coopa::asset::TypedAssetLoader<TextAsset, std::vector<std::byte>> {
public:
    std::shared_ptr<std::vector<std::byte>> decode_typed(const coopa::asset::AssetId&,
                                                         const coopa::asset::LoadContext& ctx) override {
        return std::make_shared<std::vector<std::byte>>(coopa::asset::AssetSource::read_bytes(ctx.resolved_path));
    }
    std::shared_ptr<TextAsset> finalize_typed(std::shared_ptr<std::vector<std::byte>> decoded,
                                              const coopa::asset::AssetId&,
                                              const coopa::asset::LoadContext&) override {
        auto asset = std::make_shared<TextAsset>();
        asset->contents.assign(reinterpret_cast<const char*>(decoded->data()), decoded->size());
        return asset;
    }
    const char* type_name() const override { return "TextAsset"; }
};

/** @brief Test asset that reports its own destruction through a flag the test still owns. */
struct TrackedAsset {
    std::shared_ptr<bool> destroyed;
    int                   tag = 0;
    ~TrackedAsset() { if (destroyed) *destroyed = true; }
};

/** @brief Writes `content` to `<scratch>/<name>` and returns the absolute path. */
std::string write_text(const std::string& name, const std::string& content) {
    const std::filesystem::path path = coopa::test::scratch_dir() / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
    return path.string();
}

} // namespace

COOPA_TEST(asset_id_normalizes_equivalent_paths) {
    using coopa::asset::AssetId;
    AssetId a = AssetId::from_path("widgets/foo.txt");
    AssetId b = AssetId::from_path("./widgets/foo.txt");
    AssetId c = AssetId::from_path("widgets\\foo.txt");
    AssetId d = AssetId::from_path("widgets/bar.txt");

    EXPECT_TRUE(a.is_valid());
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a == c);
    EXPECT_TRUE(a.hash() == b.hash());
    EXPECT_TRUE(a != d);
    EXPECT_EQ(a.path(), std::string("widgets/foo.txt"));
    EXPECT_FALSE(AssetId().is_valid());
}

COOPA_TEST(sync_load_caches_and_unregistered_type_fails_cleanly) {
    coopa::asset::AssetManager assets;
    assets.register_loader<TextAsset>(std::make_unique<TextAssetLoader>());
    const std::string file = write_text("sync.txt", "sync contents");

    auto h1 = assets.load<TextAsset>(file);
    ASSERT_TRUE(h1.is_loaded());
    EXPECT_EQ(h1->contents, "sync contents");
    EXPECT_EQ(h1.revision(), 1u);

    // Second load of the same path must hit the cached slot, not re-decode.
    auto h2 = assets.load<TextAsset>(file);
    EXPECT_TRUE(h2.get() == h1.get());
    EXPECT_EQ(h2.revision(), h1.revision());

    // A type with no registered loader must fail cleanly, never throw out of
    // load() -- use a distinct path so this hits the "no loader" branch
    // rather than the type-mismatch guard.
    struct Unregistered {};
    auto h3 = assets.load<Unregistered>(write_text("unregistered.txt", "x"));
    EXPECT_TRUE(h3.is_failed());
    EXPECT_FALSE(h3.error().empty());
}

COOPA_TEST(loading_one_path_as_two_types_is_rejected) {
    // AssetId is purely path-based -- loading the same path as two
    // different C++ types must never let the second load reinterpret the
    // first type's payload. It must fail loudly instead.
    struct OtherAsset { int x = 0; };

    coopa::asset::AssetManager assets;
    assets.register_loader<TextAsset>(std::make_unique<TextAssetLoader>());
    const std::string file = write_text("shared.txt", "shared path");

    auto text_handle = assets.load<TextAsset>(file);
    ASSERT_TRUE(text_handle.is_loaded());

    // No loader is even registered for OtherAsset -- if the type-mismatch
    // guard were missing, this would still "succeed" by handing back a
    // handle whose get() reinterprets TextAsset's payload as OtherAsset.
    auto other_handle = assets.load<OtherAsset>(file);
    EXPECT_FALSE(other_handle.is_valid());
    EXPECT_FALSE(other_handle.is_loaded());

    // The original handle must be completely unaffected.
    EXPECT_TRUE(text_handle.is_loaded());
    EXPECT_EQ(text_handle->contents, "shared path");
}

COOPA_TEST(same_relative_path_under_two_base_dirs_gets_two_slots) {
    // An AssetId must be built from the RESOLVED path, never from the raw
    // virtual_path. Two scene directories that both reference the same
    // relative filename (e.g. "meshes/cube.000.yaml" in gfxcoopa's real usage)
    // would otherwise share one cached slot, and one would serve the other's
    // asset.
    coopa::asset::AssetManager assets;
    assets.register_loader<TextAsset>(std::make_unique<TextAssetLoader>());
    write_text("dir_a/shared.txt", "from A");
    write_text("dir_b/shared.txt", "from B");
    const std::string dir_a = (coopa::test::scratch_dir() / "dir_a").string();
    const std::string dir_b = (coopa::test::scratch_dir() / "dir_b").string();

    auto handle_a = assets.load<TextAsset>("shared.txt", dir_a);
    auto handle_b = assets.load<TextAsset>("shared.txt", dir_b);

    ASSERT_TRUE(handle_a.is_loaded());
    ASSERT_TRUE(handle_b.is_loaded());
    EXPECT_TRUE(handle_a.get() != handle_b.get()); // distinct slots, not aliased
    EXPECT_EQ(handle_a->contents, "from A");
    EXPECT_EQ(handle_b->contents, "from B");

    // get() with the matching base_dir must find each one back.
    EXPECT_TRUE(assets.get<TextAsset>("shared.txt", dir_a).get() == handle_a.get());
    EXPECT_TRUE(assets.get<TextAsset>("shared.txt", dir_b).get() == handle_b.get());
}

COOPA_TEST(async_load_resolves_through_update_and_reuses_the_slot) {
    coopa::asset::AssetManager assets;
    assets.register_loader<TextAsset>(std::make_unique<TextAssetLoader>());
    const std::string file = write_text("async.txt", "async contents");

    auto handle = assets.load_async<TextAsset>(file);
    EXPECT_TRUE(handle.state() == coopa::asset::AssetState::Loading || handle.is_loaded());

    // Bounded poll: the decode runs on the asset IO thread.
    for (int guard = 0; !handle.is_loaded() && !handle.is_failed() && guard < 2000; ++guard) {
        assets.update(0.001f);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    ASSERT_TRUE(handle.is_loaded());
    EXPECT_EQ(handle->contents, "async contents");

    // A second load_async() for the same id while the first is already
    // resolved must reuse the slot rather than kicking off another job.
    auto handle2 = assets.load_async<TextAsset>(file);
    EXPECT_TRUE(handle2.is_loaded());
    EXPECT_TRUE(handle2.get() == handle.get());
}

COOPA_TEST(hot_reload_swaps_the_payload_behind_the_same_handle) {
    coopa::asset::AssetManager assets;
    assets.register_loader<TextAsset>(std::make_unique<TextAssetLoader>());
    assets.set_hot_reload(true);
    assets.set_poll_interval(0.0f);
    const std::string file = write_text("reload.txt", "original");

    auto handle = assets.load<TextAsset>(file);
    ASSERT_TRUE(handle.is_loaded());
    EXPECT_EQ(handle.revision(), 1u);

    bool reload_fired = false;
    coopa::asset::AssetId reloaded_id;
    coopa::event::ScopedConnection conn = assets.on_reloaded.connect_scoped(
        [&](const coopa::asset::AssetId& id) {
            reload_fired = true;
            reloaded_id = id;
        });

    // Rewrite, then push the mtime forward explicitly rather than sleeping for the filesystem
    // clock to tick: the poll only reloads on a strictly newer mtime.
    const auto before = std::filesystem::last_write_time(file);
    write_text("reload.txt", "reloaded contents");
    std::filesystem::last_write_time(file, before + std::chrono::seconds(2));

    assets.update(0.0f);

    EXPECT_TRUE(reload_fired);
    EXPECT_TRUE(reloaded_id == handle.id());
    EXPECT_EQ(handle.revision(), 2u);
    EXPECT_EQ(handle->contents, "reloaded contents"); // same handle, new payload
}

COOPA_TEST(unload_waits_for_last_handle_then_gc_frees_the_slot) {
    coopa::asset::AssetManager assets;
    assets.register_loader<TextAsset>(std::make_unique<TextAssetLoader>());
    const std::string file = write_text("unload.txt", "unload me");

    auto handle = assets.load<TextAsset>(file);
    ASSERT_TRUE(handle.is_loaded());
    const coopa::asset::AssetId id = handle.id();

    // unload() is a no-op while a handle still references the slot.
    assets.unload(id);
    EXPECT_TRUE(assets.get<TextAsset>(file).is_valid());

    handle = coopa::asset::AssetHandle<TextAsset>(); // drop the last handle
    assets.garbage_collect();
    EXPECT_FALSE(assets.get<TextAsset>(file).is_valid());

    assets.shutdown();
}

COOPA_TEST(shutdown_drains_an_in_flight_load) {
    // A loader that reports whether finalize() actually ran, via a flag
    // outside the AssetManager -- the handle itself must not be dereferenced
    // after shutdown() (it clears slots_, exactly like every other
    // manual-teardown contract in this workspace: SceneLoader::
    // clear_component_parsers(), UIResourceCache::clear()), so this is the
    // only safe way to observe that the drain happened.
    struct TrackingLoader : public coopa::asset::TypedAssetLoader<TextAsset, std::vector<std::byte>> {
        std::shared_ptr<bool> finalized;
        std::shared_ptr<std::vector<std::byte>> decode_typed(const coopa::asset::AssetId&,
                                                             const coopa::asset::LoadContext& ctx) override {
            return std::make_shared<std::vector<std::byte>>(coopa::asset::AssetSource::read_bytes(ctx.resolved_path));
        }
        std::shared_ptr<TextAsset> finalize_typed(std::shared_ptr<std::vector<std::byte>>,
                                                  const coopa::asset::AssetId&,
                                                  const coopa::asset::LoadContext&) override {
            *finalized = true;
            return std::make_shared<TextAsset>();
        }
        const char* type_name() const override { return "TextAsset"; }
    };

    auto finalized = std::make_shared<bool>(false);
    coopa::asset::AssetManager assets;
    auto loader = std::make_unique<TrackingLoader>();
    loader->finalized = finalized;
    assets.register_loader<TextAsset>(std::move(loader));
    const std::string file = write_text("shutdown.txt", "in flight");

    {
        // Scoped so `handle` is destroyed (a harmless ref_count decrement on
        // a still-live slot) BEFORE shutdown() runs below -- there is no
        // safe way to touch this handle again, including by reassigning it,
        // once shutdown() has executed.
        auto handle = assets.load_async<TextAsset>(file);
        ASSERT_TRUE(handle.is_valid());
    }

    // Deliberately do not call update() first -- shutdown() itself must
    // wait for the in-flight decode() job and run finalize() before tearing
    // down pending_, rather than destroying that job's state out from under
    // a still-running worker thread.
    assets.shutdown();

    EXPECT_TRUE(*finalized);
}

COOPA_TEST(create_publishes_a_runtime_payload_that_never_hot_reloads) {
    // No loader registered for TrackedAsset at all -- create() must never
    // touch loaders_.
    coopa::asset::AssetManager assets;
    assets.set_hot_reload(true);
    assets.set_poll_interval(0.0f);

    auto destroyed = std::make_shared<bool>(false);
    auto payload = std::make_shared<TrackedAsset>();
    payload->destroyed = destroyed;
    payload->tag = 42;

    auto handle = assets.create<TrackedAsset>("runtime/generated", payload);
    ASSERT_TRUE(handle.is_loaded());
    EXPECT_EQ(handle.revision(), 1u);
    EXPECT_EQ(handle->tag, 42);
    EXPECT_TRUE(assets.get<TrackedAsset>("runtime/generated").get() == handle.get());

    // resolved_path is empty for a create()d slot, so poll_for_reloads_()
    // must skip it entirely -- revision must never move on its own.
    for (int i = 0; i < 5; ++i) assets.update(0.0f);
    EXPECT_EQ(handle.revision(), 1u);

    // Type mismatch: create()-ing the same id as a different type must fail
    // loudly and leave the existing slot completely alone.
    auto mismatched = assets.create<TextAsset>("runtime/generated", std::make_shared<TextAsset>());
    EXPECT_FALSE(mismatched.is_valid());
    EXPECT_EQ(handle.revision(), 1u);
    EXPECT_TRUE(handle.is_loaded());

    payload.reset();
    handle = coopa::asset::AssetHandle<TrackedAsset>();
    assets.garbage_collect();
    EXPECT_FALSE(assets.get<TrackedAsset>("runtime/generated").is_valid());
    // garbage_collect() retires the payload with the same grace period as
    // every other eviction path (see retire_payload_()) -- it is not
    // destroyed synchronously just because the slot is gone.
    EXPECT_FALSE(*destroyed);
    for (int i = 0; i < 3; ++i) assets.update(0.0f);
    EXPECT_TRUE(*destroyed);

    assets.shutdown();
}

COOPA_TEST(create_republish_keeps_the_old_payload_alive_for_the_grace_period) {
    coopa::asset::AssetManager assets;

    auto a_gone = std::make_shared<bool>(false);
    auto payload_a = std::make_shared<TrackedAsset>();
    payload_a->destroyed = a_gone;
    payload_a->tag = 1;

    auto handle = assets.create<TrackedAsset>("runtime/replaceable", payload_a);
    ASSERT_TRUE(handle.is_loaded());
    EXPECT_EQ(handle.revision(), 1u);

    bool reload_fired = false;
    coopa::asset::AssetId reloaded_id;
    coopa::event::ScopedConnection conn = assets.on_reloaded.connect_scoped(
        [&](const coopa::asset::AssetId& id) {
            reload_fired = true;
            reloaded_id = id;
        });

    // Drop the test's own reference to A -- only the slot's payload (plus,
    // shortly, the retire list) should be keeping it alive.
    payload_a.reset();

    auto payload_b = std::make_shared<TrackedAsset>();
    payload_b->tag = 2;
    assets.create<TrackedAsset>("runtime/replaceable", payload_b);
    payload_b.reset();

    EXPECT_TRUE(reload_fired);
    EXPECT_TRUE(reloaded_id == handle.id());
    EXPECT_EQ(handle.revision(), 2u);
    EXPECT_EQ(handle->tag, 2); // same handle, new payload (address-stable slot)
    EXPECT_FALSE(*a_gone);     // still within the grace period

    for (int i = 0; i < 3; ++i) assets.update(0.0f);
    EXPECT_TRUE(*a_gone);
}

COOPA_TEST(idle_eviction_spares_referenced_slots_and_defers_payload_destruction) {
    coopa::asset::AssetManager assets;
    assets.set_idle_eviction(true);
    assets.set_max_idle_frames(2);

    auto destroyed = std::make_shared<bool>(false);

    // Hold a handle for the first stretch, so we can prove a referenced
    // slot never ages regardless of how long update() keeps running.
    auto tracked = std::make_shared<TrackedAsset>();
    tracked->destroyed = destroyed;
    auto handle = assets.create<TrackedAsset>("runtime/evictable", tracked);
    tracked.reset();
    ASSERT_TRUE(handle.is_loaded());

    for (int i = 0; i < 10; ++i) assets.update(0.0f);
    EXPECT_TRUE(assets.get<TrackedAsset>("runtime/evictable").is_valid());
    EXPECT_FALSE(*destroyed);

    // Drop the last handle -- the slot is now idle and should age out within
    // max_idle_frames() updates, but the payload must survive the grace
    // period after that.
    handle = coopa::asset::AssetHandle<TrackedAsset>();

    uint32_t updates_to_evict = 0;
    while (assets.get<TrackedAsset>("runtime/evictable").is_valid() && updates_to_evict < 10) {
        assets.update(0.0f);
        ++updates_to_evict;
    }
    EXPECT_FALSE(assets.get<TrackedAsset>("runtime/evictable").is_valid());
    EXPECT_LE(updates_to_evict, assets.max_idle_frames());
    EXPECT_FALSE(*destroyed); // evicted from the slot, but still in the grace-period retire list

    for (int i = 0; i < 3; ++i) assets.update(0.0f);
    EXPECT_TRUE(*destroyed);

    assets.shutdown();
}
