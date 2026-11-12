# `coopa::asset` — A Generic, Extensible Asset System

## Context

Every repo in this workspace has independently reinvented asset loading, and none of
them share anything. Exploration turned up:

| Concern | Current state |
|---|---|
| Path resolution | 4 uncoordinated mechanisms: `ROOT_DIR + "/assets/..."` string concat in apps, `ctx.scene_dir + "/meshes/"` hardcoded in a parser, `UIResourceCache::resolve_path_`, and `FileUtil` — which **no repo actually uses** and which truncates 3 extra chars off any path containing `assets/` (`libcoopa/coopa/util/file.h:84`, `substr(pos + 10)` where `"assets/"` is 7 chars) |
| Caching | `mesh_cache` in a lambda capture (`gfxcoopa/engine/components/register.h:82`), `UIResourceCache`'s 3 maps (`uicoopa/ui_yaml.h:257-259`), `Font::atlases_`, `UiPass::descriptor_cache_`, + a 5th planned in sfxcoopa |
| Image decode | **Only exists in uicoopa** (`render/texture.h:107`). gfxcoopa has zero image loading — so `PBRMaterial::texture_albedo/normal/metallic_roughness` are parsed from YAML and then read by *nothing* |
| Staging upload | Same recipe copy-pasted 4×: `smaa_textures.h:61`, `ssao_kernel.h:105`, `ssao_pass.h:546`, `uicoopa/render/texture.h:118` |
| Shader loading | No cache/dedup — `hiz_downsample.vert.spv` is read from disk 3× in blendy alone, producing 3 identical `VkShaderModule`s |
| Lifetime | 3 libraries each invented the same manual "call `clear()` before your Device dies or VMA crashes" contract (`blendy/test.cpp:370-375`, `register.h:14-18`, `ui_yaml.h:210-217`) |
| Hot reload | None anywhere |
| Handles | None. Meshes use `shared_ptr`; uicoopa hands out raw `Font*`/`Texture*` that dangle after `clear()` |

Only the *scene* file can be an encrypted `.caml` — its meshes, animations, fonts and
textures cannot, because `register.h:58` deliberately bypasses
`SceneLoader::set_document_loader()`.

**Goal:** one self-contained `coopa::asset` module in libcoopa that owns identity,
resolution, caching, refcounted lifetime, async loading and hot reload — with zero
dependencies on any sibling repo. Downstream packages register their own asset types
(gfxcoopa: Mesh/Texture/Shader; uicoopa: Sprite/Font; later sfxcoopa: AudioClip),
exactly as they already register scene components.

---

## Design

### Conventions to follow

Established by exploration; the new module must match them:

- **Header-only**, `.h` only, no CMake changes needed — a module is just a directory of
  headers plus a `README.md`. `include_directories(${CMAKE_SOURCE_DIR})` already picks it up.
- Guards `COOPA_ASSET_<FILE>_H`; namespace `coopa { namespace asset { ... } }`, one per line,
  closed with `} // namespace asset`.
- `snake_case` methods, `PascalCase` types, `trailing_underscore_` privates (including
  private static helpers like `slots_()`), `k_`-prefixed `inline constexpr`.
- Full Doxygen `@file`/`@class`/`@brief`/`@param`/`@return`/`@throws` (coopadocs parses this).
- Registries as **function-local statics**, mirroring `SceneLoader::parsers_()`.
- **No sibling deps** — only vendored `fkYAML`/`glm` and libcoopa's own modules.

### Files — `/home/coopa/git/libcoopa/coopa/asset/`

| File | Contents |
|---|---|
| `asset_id.h` | `AssetId` — normalized virtual path + FNV-1a 64 hash |
| `asset_state.h` | `enum class AssetState { Unloaded, Loading, Loaded, Failed }` |
| `asset_slot.h` | `detail::AssetSlot` — type-erased, address-stable, refcounted storage |
| `asset_handle.h` | `AssetHandle<T>` — 8-byte typed refcounted handle |
| `asset_loader.h` | `IAssetLoader` (2-stage) + `TypedAssetLoader<T, Intermediate>` helper |
| `asset_source.h` | Search-root path resolution + `read_bytes` |
| `asset_manager.h` | Facade: registration, load/load_async, `update()`, hot reload, shutdown |
| `README.md` | Per the `add-module-readme` skill (ASCII diagram + table + File Breakdown + usage) |

### Identity — path-based

```cpp
class AssetId {
public:
    static AssetId from_path(const std::string& virtual_path);  // lexically normalized, '/' separators
    uint64_t hash() const;
    const std::string& path() const;   // retained for hot reload, logging, errors
    bool operator==(const AssetId&) const;
};
```

Existing YAML/scene files keep referencing assets by path — no migration of asset data.

### Handle — refcounted, survives hot reload

```cpp
template <typename T>
class AssetHandle {
public:
    AssetHandle() = default;                  // null handle
    AssetHandle(const AssetHandle&);          // ++slot_->ref_count
    ~AssetHandle();                           // --slot_->ref_count
    bool         is_valid()  const;
    AssetState   state()     const;
    bool         is_loaded() const;
    const T*     get()       const;           // nullptr unless Loaded
    const T&     operator*() const;
    const T*     operator->() const;
    const AssetId& id()      const;
    uint32_t     revision()  const;           // bumps on every hot reload
private:
    detail::AssetSlot* slot_ = nullptr;
};
```

**The critical invariant:** slots are stored as `std::unordered_map<uint64_t,
std::unique_ptr<AssetSlot>>`, so slot addresses are stable for life. Hot reload swaps the
*payload inside* the slot, so every outstanding handle keeps working and simply observes a
bumped `revision()`. This indirection is the entire reason handles beat raw `shared_ptr`.

`revision()` is what lets a consumer detect it must rebuild derived GPU state (a pipeline
built from a reloaded shader, a descriptor set pointing at a reloaded texture).

### Two-stage loader — the seam that makes async *and* GPU safety work

```cpp
class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;
    /// Runs on a worker thread. Read + decode only — MUST NOT touch the GPU or Vulkan.
    virtual std::shared_ptr<void> decode(const AssetId&, const LoadContext&) = 0;
    /// Runs on the main thread inside update(). GPU upload / finalization happens here.
    virtual std::shared_ptr<void> finalize(std::shared_ptr<void> decoded,
                                           const AssetId&, const LoadContext&) = 0;
    virtual const char* type_name() const = 0;
};
```

`TypedAssetLoader<T, Intermediate>` wraps this so downstream code writes typed
`decode()`/`finalize()` overrides and never touches `void*`.

Registration mirrors `SceneLoader::register_component_parser`, keyed by
`std::type_index(typeid(T))`:

```cpp
assets.register_loader<Mesh>(std::make_unique<MeshLoader>(device, allocator, cmd_pool));
```

### Async — leveraging `coopa/job`, with a hazard to design around

**Constraint discovered during exploration:** `CounterPool::reset()`
(`coopa/job/handle.h:96`) is a bare bump-pointer reset with *no generation tag*, and
`JobHandle` is just `{pool*, index}`. A handle held across a `begin_frame()` silently
aliases a freshly-recycled slot. So asset loads **cannot** be tracked by handles from the
app's per-frame `JobEngine`.

Resolution — still uses `coopa::job` as requested, but safely:

1. `AssetManager` owns its **own dedicated** `coopa::job::JobEngine io_engine_`
   (default 2 threads, its own `JobType k_asset_io_job_type`). It is never driven by the
   app's frame loop.
2. `load_async<T>()` sets the slot to `Loading`, allocates a `JobHandle` from
   `io_engine_`, submits `decode()`, and records
   `PendingLoad { AssetSlot*, JobHandle, std::shared_ptr<void> decoded }`.
3. `AssetManager::update()` — main thread, once per frame — scans `pending_`; for each
   complete handle it runs `finalize()` on the main thread, publishes the payload, sets
   `Loaded`, bumps `revision`, and fires `on_reloaded`.
4. **`io_engine_.begin_frame()` is called only when `pending_` is empty.** That is the
   one provably safe reclaim point for the counter pool, and `AssetManager` is the only
   thing that knows it.
5. If `create_handle()` returns invalid (pool exhausted while loads are in flight and
   therefore un-resettable), fall back to a **synchronous** load rather than dropping it.

*Verify during implementation:* that `JobEngine`'s counter decrement uses release ordering,
so `is_complete()`'s acquire load properly publishes the worker's write into
`PendingLoad::decoded`. If it does not, add an explicit release store in the decode task.

### Hot reload

- `set_hot_reload(bool)` + `set_poll_interval(seconds)`; polling `last_write_time` inside
  `update()` (no inotify — keeps the module portable and dependency-free).
- Only slots with `ref_count > 0` are polled.
- A changed file re-enters the same two-stage pipeline; `finalize()` swaps the payload and
  bumps `revision`.
- The superseded payload moves to a `retired_` list held for `k_reload_grace_frames` (3)
  before release — **required for Vulkan**, so in-flight GPU work isn't yanked out from
  under a command buffer.
- `coopa::event::Signal<const AssetId&> on_reloaded` (reusing `coopa/event/signal.h`) is
  how a render pipeline learns it must rebuild.

### Path resolution — supersede `FileUtil`, don't build on it

`FileUtil` is global-namespace legacy with a live truncation bug and no users outside
libcoopa's own tests. The new `AssetSource` owns resolution:

```cpp
void add_search_root(const std::string& dir);   // app registers ROOT_DIR "/assets", etc.
std::string resolve(const std::string& virtual_path) const;  // absolute → first existing root → passthrough
std::vector<std::byte> read_bytes(const std::string& virtual_path) const;
```

Leave `FileUtil` in place (removing it is out of scope) but **fix its `substr(pos + 10)`
bug** as a one-line drive-by so it stops being a trap. Note that sfxcoopa's plan currently
commits to `FileUtil::get_asset_path`; that plan should be redirected to this module.

### Shutdown

`AssetManager::shutdown()` — drains pending loads, releases every payload, clears slots.
This replaces the three separately-invented manual teardown contracts. It must be called
while the Device/Allocator are still alive, and this requirement gets documented once, in
the module README, instead of three times in three repos.

---

## Implementation Phases

### Phase 1 — Core module, synchronous path

- Create the 8 files above under `/home/coopa/git/libcoopa/coopa/asset/`.
- Implement `AssetId`, `AssetState`, `AssetSlot`, `AssetHandle<T>`, `AssetSource`,
  `IAssetLoader`/`TypedAssetLoader`, and `AssetManager` with `register_loader<T>`,
  `load<T>` (sync), `get<T>`, `unload`, `shutdown`.
- No `coopa/job` dependency yet — `load<T>` calls `decode()` then `finalize()` inline.
- Add `test_asset_*()` functions to `/home/coopa/git/libcoopa/test.cpp` + `RUN_TEST` lines
  in `main()` (that single file is the entire test suite; there is no GoogleTest/CTest).

### Phase 2 — Async + hot reload

- Add the dedicated `io_engine_`, `PendingLoad`, `load_async<T>()`, and `update()`.
- Implement the "reset only when `pending_` is empty" reclaim rule and the sync fallback
  on pool exhaustion.
- Add mtime polling, payload swap, `revision` bump, `retired_` grace list, `on_reloaded`.
- Tests: concurrent `load_async` of the same id coalesces to one slot; handle stays valid
  across a simulated reload while `revision()` increments; pool-exhaustion falls back
  rather than failing.

### Phase 3 — gfxcoopa

- **Fix `gfxcoopa/CMakeLists.txt` first:** it never includes libcoopa, so its own test
  target cannot compile `engine/components/` (10 headers already `#include <coopa/scene/...>`).
  Add the libcoopa include dirs.
- Consolidate the 4 duplicated staging uploads into one
  `gfx::memory::upload_image_2d(device, allocator, cmd_pool, ...)` helper; repoint
  `smaa_textures.h`, `ssao_kernel.h`, `ssao_pass.h` at it.
- Add `gfx::data::Texture` + `TextureLoader` (moving `stbi_load` **down from uicoopa into
  gfxcoopa**, where it belongs) — decode off-thread, upload in `finalize()`.
- `MeshLoader` — replaces the `mesh_cache` lambda capture in
  `engine/components/register.h:82`. Key on the **resolved path**, not the logical name
  (`"cube.000"` currently collides silently across scenes). `MeshRenderer` holds
  `AssetHandle<Mesh>` instead of `shared_ptr<Mesh>`, removing the atomic refcount bump that
  `get_mesh()`-by-value costs on every draw.
- `ShaderLoader` — dedups `VkShaderModule` by path, killing the triple-read of
  `hiz_downsample.vert.spv`.

### Phase 4 — uicoopa

- Retire `UIResourceCache` (`ui_yaml.h:125-260`) in favour of `AssetManager`.
- Register `SpriteLoader` and `FontLoader`; `Font`'s per-pixel-size `atlases_` stays an
  internal detail of the Font asset.
- Keep the `UIResources` named-registry lookup taking precedence over path lookup, so
  existing scene YAML keeps working unchanged.
- uicoopa's `Texture` becomes a thin alias/adapter over gfxcoopa's.

### Phase 5 — blendy wiring

- Construct `AssetManager` in `test.cpp`, `add_search_root(std::string(ROOT_DIR) + "/assets")`.
- Register gfx + ui loaders alongside the existing
  `register_render_components` / `register_ui_components` calls.
- Call `assets.update()` once per frame; call `assets.shutdown()` where
  `SceneLoader::clear_component_parsers()` is currently called (`test.cpp:370-375`).
- Subscribe `on_reloaded` to rebuild pipelines when a `.spv` changes — the visible payoff.

### Phase 6 — *Flagged, recommend deferring*

Actually **binding** material textures (`texture_albedo`/`normal`/`metallic_roughness`) is
what this system unblocks, but it is a real renderer change, not an asset change: it needs
a material descriptor set layout, `GBufferPipeline::PushConstants` extension, and
`blendy/src/blendy/render/pbr_render_pipeline.h:242` currently passes `VK_NULL_HANDLE` for
`material_layout`. I recommend a separate plan. Phases 1-5 leave it one small step away.

---

## Testing and Validation

**Automated** — append to `/home/coopa/git/libcoopa/test.cpp`, register in `main()`:

```bash
cd /home/coopa/git/libcoopa && cbuild && cplay
```

Cases: id normalization + hash stability; search-root resolution incl. absolute passthrough;
refcount reaches zero → slot freed; two `load<T>` of one path share a slot; failed load →
`Failed` + non-empty error, no crash; `load_async` completes after `update()`; reload swaps
payload, keeps handle valid, bumps `revision`; `shutdown()` with loads in flight.

**Manual, end-to-end** (per project convention — never kill the process, it saves on exit):

```bash
cd /home/coopa/git/gfxcoopa && cbuild --vulkan          # engine/ now compiles standalone
cd /home/coopa/git/uicoopa  && cbuild --vulkan && MAX_FRAMES=5 cplay
cd /home/coopa/git/blendy   && cbuild --vulkan && MAX_FRAMES=5 cplay
```

Verify:
1. `blendy` renders `assets/scenes/cube` and `gi_cornell_box` identically to before —
   compare `output/output-runtime.png` against the current committed image.
2. `uicoopa_test_window` renders text and sprites unchanged.
3. Clean shutdown with **no VMA leak/assert** — this is the regression that the three
   ad-hoc `clear()` contracts existed to prevent.
4. Log shows each `.spv` and mesh read from disk **once** (previously 3× for
   `hiz_downsample.vert.spv`).
5. Hot reload: with the app running, edit and recompile a fragment shader; confirm the
   pipeline rebuilds and the frame changes without a restart.

**Docs:**

```bash
cd /home/coopa/git/libcoopa && coopadocs build     # must report no parse warnings
```

Also add the module to `/home/coopa/git/libcoopa/README.md`, whose "four main modules"
line is already stale (there are seven directories — `event` and `scene` are undocumented
there, and `event/` is the one module still missing a `README.md`).

---

## Progress (2026-08-18)

**Done and verified:**
- Phase 1+2 — full `coopa::asset` module (`asset_id.h`, `asset_state.h`, `asset_slot.h`,
  `asset_handle.h`, `asset_loader.h`, `asset_source.h`, `asset_manager.h`, `README.md`),
  sync + async load, hot reload, shutdown drain. 36/36 tests passing in `test.cpp`
  (10 new: `test_asset_*`, plus 2 job-system regression tests below).
- **Two pre-existing, unplanned `coopa::job` concurrency bugs found and fixed** while
  building AssetManager's dedicated `JobEngine` (both reproduced as an intermittent hang
  or a silent "job never ran but counter says it did" false-completion in a tight
  create/submit/wait_for stress loop — see `test_job_engine_repeated_engines_no_lost_or_phantom_jobs`
  and `test_work_stealing_deque_no_torn_moves_under_contention` in `test.cpp`):
  1. `JobEngine::submit()`'s no-dependency path used `wake_one_worker()` (`notify_one()`)
     for a job deposited into one *specific* thread's inbox — could wake an unrelated idle
     thread while the real owner stayed asleep forever. Fixed to `wake_all_workers()`
     (`engine.h`), matching `submit_jobs()`'s already-correct behavior.
  2. `wake_one_worker()`/`wake_all_workers()` didn't hold `worker_mutex_`, so a worker's
     wait-predicate check could race the producer's state update — classic lost wakeup.
     Fixed by locking around the `notify_*` call (`engine.h`).
  3. `WorkStealingDeque::pop()` and `steal()` both moved their output item out of the
     shared buffer slot *before* confirming their CAS on `top_` won — on the contended
     "last element" path, the loser had already done an unsynchronized move-read of the
     same slot the winner was moving from. Harmless for a POD `int` (what the existing
     test used); silently corrupts a `Job` (whose `TaskWrapper` move nulls the source).
     Fixed by resolving the CAS first in both (`work_stealing_deque.h`).
- **Identity bug found and fixed in `AssetManager` itself**: id used to be built from the
  raw `virtual_path` before resolution, so two different `base_dir`s referencing the same
  relative filename would collide into one slot — the exact "cube.000 collides across
  scenes" hazard this plan set out to fix. Now built from the *resolved* path
  (`test_asset_manager_base_dir_prevents_collision`).
- Drive-by fix: `FileUtil::get_asset_path`'s `substr(pos + 10)` bug for `"assets/"` (7
  chars) — now `pos + ASSETS_PREFIX.length()` (and the resource-path sibling, for symmetry).
- Phase 3 — gfxcoopa: CMakeLists now includes libcoopa; `gfx::memory::upload_image_2d()`
  consolidates the 3 duplicated staging-upload recipes (`SmaaTextures`, `SsaoNoiseTexture`,
  `SsaoPass::create_neutral_texture_`); new `gfx::data::Texture` + `TextureLoader` (stb_image
  moved down from uicoopa, closing the "texture_albedo parsed but never loaded" gap);
  `MeshLoader` replaces the old `mesh_cache` lambda in `register.h`; `MeshRenderer` now
  holds `AssetHandle<Mesh>` instead of `shared_ptr<Mesh>`. Verified via a controlled
  git-stash A/B render comparison (staging-upload consolidation: no regression beyond
  pre-existing GI-bake nondeterminism) and two full end-to-end blendy renders
  (`assets/scenes/cube`, `assets/scenes/gi_cornell_box`) with clean Vulkan validation output.
- Phase 5 (core) — blendy's `test.cpp` constructs one `AssetManager`, registers
  `MeshLoader`/`TextureLoader`, calls `assets.update(dt)` per frame and
  `assets.shutdown()` before Device/Allocator teardown.

**Deferred (explicitly, not attempted):**
- Phase 3 `ShaderLoader` — deduping `VkShaderModule` across ~15+ pass/pipeline files is a
  much larger, more diffuse change than the staging-upload consolidation; recommend its
  own follow-up pass with dedicated before/after render verification.
- Phase 4 — uicoopa migration (`UIResourceCache` retirement, `SpriteLoader`/`FontLoader`).
  Not started.
- `on_reloaded` isn't yet wired to rebuild any pipeline in blendy; hot reload is off by
  default (`assets.set_hot_reload(true)` was never called there).
- Phase 6 (material texture binding into the G-buffer descriptor set) — was already
  out of scope per the original plan.
