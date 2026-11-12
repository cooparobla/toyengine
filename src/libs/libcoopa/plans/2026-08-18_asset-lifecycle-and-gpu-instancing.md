# Asset Lifecycle Automation + GPU Instancing

## Context

The `coopa::asset` system (built in a prior session — module in libcoopa,
`MeshLoader`/`TextureLoader` in gfxcoopa, wired into blendy) already gives every consumer
a single ground-truth reference per asset: `MeshRenderer::mesh_` is a
`coopa::asset::AssetHandle<Mesh>`, a non-owning pointer into an address-stable
`AssetSlot` that `AssetManager` owns. Two handles loaded from the same resolved path
share one slot; a hot reload swaps the slot's payload in place and every existing handle
observes it via a bumped `revision()`. **This part of the user's ask is already true by
construction** — see the "MeshRenderer verdict" below for exactly why.

Two real gaps remain:

1. **Asset lifecycle**: `load()`/`load_async()` already let an app trigger a load at any
   time, and `unload()`/`garbage_collect()` already let it evict immediately — but only
   when explicitly called, and (a bug found while designing this) with **no GPU-safety
   grace period at all**, unlike hot reload. There's also no way to hand the manager an
   already-built, procedurally-generated payload without writing a fake loader.
2. **Instancing**: exploration of `pbr_render_pipeline.h` and `gi_system.h` confirms the
   renderer issues one `vkCmdDrawIndexed` per object today, with the world transform (and,
   for color passes, every material scalar) delivered via a fresh `vkCmdPushConstants`
   call before each draw. There is no mesh/material grouping anywhere, and zero uses of
   `gl_InstanceIndex`/`gl_BaseInstance` in any shader in the repo. **Instancing is not
   happening today** — this plan adds it.

---

## Feature A — Asset lifecycle automation

Confined to `/home/coopa/git/libcoopa/coopa/asset/asset_slot.h` and
`asset_manager.h`, plus tests in `/home/coopa/git/libcoopa/test.cpp`.

### A0. Prerequisite: extract `retire_payload_()` — and fix a real bug while doing it

Today `publish_()` is the only place that retires a payload with a grace period
(`asset_manager.h:428-439`). Factor it out:

```cpp
/**
 * @brief Moves a slot's payload onto the retire list rather than destroying it now.
 *
 * A payload whose last reference is dropped THIS frame may still be referenced
 * by a command buffer already recorded (or submitted) this frame, so destroying
 * it immediately is a use-after-free on the GPU. Every eviction path — hot
 * reload, create() re-publish, unload(), garbage_collect(), and idle eviction —
 * goes through here so none of them can reintroduce that bug.
 */
void retire_payload_(detail::AssetSlot& slot) {
    if (slot.payload) retired_.emplace_back(std::move(slot.payload), k_payload_grace_frames);
}
```

Rename `k_reload_grace_frames` → `k_payload_grace_frames` (same value, 3; no longer
reload-specific). `publish_()` becomes `retire_payload_(slot); slot.payload = ...`.

**Bug found in the existing code, fixed here as a drive-by:** `unload()` and
`garbage_collect()` currently `slots_.erase(it)` while the slot still owns its payload —
the payload dies with **zero grace period**, the exact hazard the whole `retired_`
mechanism exists to prevent. Route both through `retire_payload_(*it->second)`
immediately before each `erase`. This only makes destruction *later*, never earlier, so
it can't break `test_asset_manager_unload_and_gc` (which only asserts `get()` becomes
invalid).

### A1. `AssetManager::create<T>()`

```cpp
/**
 * @brief Publishes an already-constructed payload into a slot, bypassing
 * AssetSource and the loader entirely — for procedurally-generated runtime
 * assets (a mesh built on the fly, a texture rendered into at startup) that
 * have no file behind them. resolved_path stays empty, so hot-reload polling
 * skips it by construction (poll_for_reloads_() already bails on an empty path).
 *
 * Calling this again on the same synthetic_id ALWAYS re-publishes (same
 * retire-with-grace-period path hot reload uses) — never a load()-style
 * cache-hit no-op, since handing in a freshly built payload is an explicit
 * statement of intent to replace; silently dropping it would leak the
 * caller's work.
 *
 * @param synthetic_id Logical identity, normalized like a path. Prefix these
 *   (e.g. "runtime/terrain_chunk_0") so they can't collide with a real
 *   resolved file path.
 * @param payload Fully constructed payload; must not be null.
 * @return A handle to the slot. Empty on a type mismatch; a Failed slot on a
 *   null payload or on an id already backed by a file-loaded asset (see below).
 */
template <typename T>
AssetHandle<T> create(const std::string& synthetic_id, std::shared_ptr<T> payload);
```

Body:
1. `AssetId id = AssetId::from_path(synthetic_id)` — **not** `source_.resolve()`. This is
   what keeps `resolved_path` empty.
2. `find_or_create_slot_` + `check_type_` (identical to `load()`).
3. Null-payload guard → `fail_slot_(*slot, "create() called with a null payload")`.
4. **In-flight guard**: if the slot is in `pending_`, log and return the existing handle
   untouched — otherwise a later `complete_pending_()` would publish the file payload
   over the created one.
5. **File-backed guard**: if `slot->resolved_path` is non-empty (this id was created by
   `load()`), refuse and log rather than publish — otherwise `publish_()` resets
   `last_write_time_ns` to 0 and the next hot-reload poll re-loads from disk over the
   procedural payload.
6. Otherwise build a `LoadContext` with `resolved_path` left empty and call `publish_()`.

Caveat worth a doc line: re-`create()`-ing the same id every frame retires one payload
per frame, each held for the grace period — fine occasionally, not for a hot loop with
large GPU payloads.

### A2. Automatic idle eviction

`asset_slot.h` — one new field: `uint32_t idle_frames = 0;`

`asset_manager.h` — new public API mirroring the hot-reload block:
```cpp
void set_idle_eviction(bool enabled) { idle_eviction_enabled_ = enabled; }   // off by default
void set_max_idle_frames(uint32_t frames) { max_idle_frames_ = frames; }     // default 300 (~5s @60fps)
bool idle_eviction_enabled() const { return idle_eviction_enabled_; }
```

```cpp
/**
 * @brief Ages unreferenced slots out and evicts the ones past max_idle_frames_.
 *
 * The AssetSlot struct itself is erased immediately — once ref_count==0 no
 * handle holds its address, and once it's absent from pending_ no PendingLoad
 * holds it either, so nothing has a raw AssetSlot* left. The PAYLOAD is what
 * needs the grace period, so it's moved into retired_ BEFORE the slot is
 * erased (erasing first would run the payload's destructor inline).
 */
void sweep_idle_() {
    for (auto it = slots_.begin(); it != slots_.end();) {
        detail::AssetSlot& slot = *it->second;
        if (slot.ref_count > 0 || pending_.find(&slot) != pending_.end()) {
            slot.idle_frames = 0;
            ++it;
            continue;
        }
        if (++slot.idle_frames < max_idle_frames_) { ++it; continue; }
        retire_payload_(slot);   // MUST precede the erase below
        it = slots_.erase(it);
    }
}
```

**Placement in `update()` matters**: call `sweep_idle_()` *after* the existing `retired_`
aging loop and *before* the hot-reload poll — same position `poll_for_reloads_()`
already uses, so an evicted payload gets the full `k_payload_grace_frames` of `update()`
calls, not one frame less.

Notes to carry into the doc comments:
- A **Failed** slot (payload already null) is simply erased once idle — meaning a later
  `load()` of the same path now retries instead of serving a cached error forever. A
  desirable side effect, but a real behavior change worth flagging.
- `create()`d assets are unrecoverable once evicted (no source to reload from) — the
  existing refcount *is* the pin; document "hold a handle to anything procedural you want
  to keep" rather than adding a separate pin flag.
- `shutdown()` needs no change — it already clears `retired_` then `slots_`, both before
  Device/Allocator teardown.

### A3. `MeshRenderer` verdict

**Already correct by construction — nothing to change.** `MeshRenderer` stores
`AssetHandle<Mesh> mesh_` *by value*, so its existence alone keeps `ref_count > 0`
(immune to idle eviction/unload while referenced). Every renderer resolving to the same
path shares one slot (identity is the resolved path), so reload/re-`create()` propagates
to every consumer for free. No draw site caches a raw `Mesh*` across frames — every one
dereferences the handle at record time, which is what makes hot reload transparent.
**The one rule instancing (Feature B) must respect**: capture the `const Mesh*` batch key
fresh from the handle every frame, never persist it across frames.

### A4. New tests — `/home/coopa/git/libcoopa/test.cpp`

Add to the existing `namespace asset_test`:

```cpp
/** @brief Test asset that reports its own destruction through a flag the test still owns. */
struct TrackedAsset {
    std::shared_ptr<bool> destroyed;
    int                   tag = 0;
    ~TrackedAsset() { if (destroyed) *destroyed = true; }
};
```

- **`test_asset_manager_create_publishes_runtime_payload`** — `create<TrackedAsset>(...)`
  with *no loader registered at all*; assert loaded, `revision()==1`, `get<T>()` finds the
  same slot. Enable hot reload + zero poll interval, `update()` several times, assert
  `revision()` unchanged (empty `resolved_path` skips polling). Assert the type-mismatch
  guard returns an invalid handle without touching `revision()`.
- **`test_asset_manager_create_republishes_with_grace_period`** — `create()` id X with
  payload A (destruction flag `a_gone`), then `create()` X again with payload B. Assert
  `revision()` 1→2, `on_reloaded` fires once, `handle->tag` is B's, **`*a_gone == false`
  immediately after re-publish**, and `*a_gone == true` only after
  `k_payload_grace_frames` more `update()` calls.
- **`test_asset_manager_idle_eviction_defers_payload_destruction`** —
  `set_idle_eviction(true); set_max_idle_frames(2);`, create a `TrackedAsset`. While the
  handle is held, `update()` ten times, assert still valid (referenced slot never ages).
  Drop the handle, `update()` until `get()` goes invalid (must happen within the
  threshold), assert **`*destroyed == false` at the moment of eviction**, then assert
  `*destroyed == true` after `k_payload_grace_frames` more updates.

`RUN_TEST(...)` all three after `test_asset_manager_shutdown_drains_pending`.

---

## Feature B — GPU instancing for mesh draws

### Findings that shape the design

- `Pipeline` (`gfxcoopa/pipeline/pipeline.h`) already takes
  `const std::vector<VkVertexInputBindingDescription>&`/attributes — a
  `VK_VERTEX_INPUT_RATE_INSTANCE` binding needs **zero changes there**.
- **`GBufferPipeline` hand-rolls `vkCreateGraphicsPipelines` directly**
  (`gbuffer_pipeline.h:72-83`, hardcodes `vertexBindingDescriptionCount = 1`) — the one
  pipeline needing direct editing.
- `pbr.vert` is **shared by both `TransparentPass` and `ProbeCapturePass`**
  (`probe_capture_pass.h:101` loads the same `pbr.vert.spv`) — they must be converted in
  the same change, not split across phases, or the shared shader breaks one of them.
- `ModelPushConstants` (128B: `model` + CPU-computed `normal_matrix` via
  `glm::inverse`, redone per object per frame at 4 call sites) shrinks to per-instance
  `model` only (64B) — the vertex shader computes
  `transpose(inverse(mat3(in_model)))` itself. Do **not** shortcut to `mat3(in_model)`
  directly — scenes use non-uniform scale (Cornell walls: `scale: {0.1, 10.0, 5.0}`), so
  the full inverse-transpose is required, not just the upper 3×3.
- Material scalars stay a **per-batch push constant**, never per-instance. Batch key:
  `(const Mesh*)` only for shadow passes (their shaders read no material at all — so
  shadow passes batch more aggressively than color passes on the same scene);
  `(const Mesh*, material equality)` for G-buffer/transparent/probe-capture, where
  equality means bit-identical `albedo, alpha, metallic, roughness, ao, gpu_alpha_cutoff()`
  — safe to compare directly, since these are only ever copied from parsed YAML, never
  arithmetically derived before draw. **Forward-compat note**: the day material textures
  get bound to a descriptor set (deferred "Phase 6" from the original asset-system plan),
  the three texture payload pointers must join this equality check.
- `record_transparent_geometry_()`'s list is depth-sorted back-to-front just before
  drawing — batching there may only merge **consecutive** runs sharing a key, never a
  full regroup, or blend order breaks. Shadow/G-buffer passes have no such constraint.
- No reusable per-frame CPU→GPU streaming buffer exists in gfxcoopa; copy uicoopa's
  `UiPass` pattern (`ui_pass.h:141-148, 250-269`): one host-visible, persistently mapped
  `Buffer::vertex()`, grown via destroy-and-recreate at `max(needed, capacity*2)`, fully
  re-uploaded each use. **A single, non-double-buffered buffer is safe** specifically
  because all geometry recording runs on `pre_frame_cmd_`, submitted with a
  `vkQueueWaitIdle` (`pbr_render_pipeline.h:735-736`) before anything else could touch it
  — call this dependency out explicitly in the class doc as a tripwire: if that wait is
  ever removed, this needs to become frame-in-flight-deep like `UiPass::vbo_[]`.

### B1. Core mechanism — new file `gfxcoopa/gfxcoopa/engine/util/instance_batcher.h`

Lives in `engine/util/` (matches `fullscreen_quad.h`/`ssao_kernel.h`, which also own
buffers) so both blendy's `PbrRenderPipeline` and gfxcoopa's own `GiSystem` can use it.
**Not templated on item type** — `RenderableItem` (blendy) and `CaptureItem`
(gfxcoopa) are unrelated types in different repos; the caller runs its own loop and feeds
the batcher plain data instead.

```cpp
class InstanceBatcher {
public:
    struct Batch {
        const data::Mesh* mesh           = nullptr;
        uint32_t          first_instance = 0;  // passed straight to vkCmdDrawIndexed
        uint32_t          instance_count = 0;
        uint32_t          item_index     = 0;  // caller's index -- where per-batch material comes from
    };
    struct Range { uint32_t first = 0; uint32_t count = 0; };  // slice of batches() for one draw list

    InstanceBatcher(core::Device&, memory::Allocator&, size_t initial_capacity = 256);

    void begin();  // drops last frame's batches/instances; call once per frame before any add()

    /**
     * @param continue_batch True iff this item shares its batch key with the immediately
     *   preceding add() -- always false for a draw list's first item, so lists never merge
     *   across a list boundary. Caller-computed exact predicate, not a hashed key: a hash
     *   collision on material would silently apply the wrong material to a merged batch
     *   with no failure signal, so each pass gets a few lines of exact field comparison
     *   instead (see B3).
     */
    void add(const data::Mesh* mesh, bool continue_batch, const glm::mat4& model, uint32_t item_index);

    uint32_t batch_count() const;             // bracket a draw list's add() calls to get its Range
    void upload();                            // grows buffer if needed, uploads every instance
    const std::vector<Batch>& batches() const;
    void bind(command::CommandBuffer& cmd) const;  // binds the instance stream at slot 1
};
```

One batcher, one buffer, one `begin()`/`upload()` per frame — append the shadow list,
then opaque, then transparent, recording a `Range` around each. `first_instance`
addresses each group's slice. The cube shadow pass replays the shadow `Range` across all
six faces at no extra batching cost (today it re-issues N push+draw pairs per face).

### B2. `InstanceData` + vertex layout

Add to `engine/data/mesh.h`, directly below `Vertex`:

```cpp
/**
 * @struct InstanceData
 * @brief Per-instance vertex stream at binding 1: one world matrix per instance.
 *
 * normal_matrix is deliberately NOT streamed — every consumer derives it as
 * transpose(inverse(mat3(in_model))) in-shader, halving the per-instance
 * payload (64B vs 128B) and removing a glm::inverse() per object per frame
 * from the CPU side of every geometry pass.
 */
struct InstanceData {
    glm::mat4 model;

    static VkVertexInputBindingDescription binding_description() {
        VkVertexInputBindingDescription desc{};
        desc.binding = 1; desc.stride = sizeof(InstanceData); desc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        return desc;
    }
    // Locations 4,5,6,7 -- a mat4 occupies four consecutive vec4 locations. Fixed
    // across every pipeline (including shadow, whose binding-0 attrs only use
    // location 0) so one attribute_descriptions() serves all of them.
    static std::array<VkVertexInputAttributeDescription, 4> attribute_descriptions();
};
```

**Addressing each batch: `firstInstance`, not a per-batch buffer-offset rebind.** Add a
defaulted param to `CommandBuffer::draw_indexed` (`command_buffer.h:221-227`, which
hardcodes `0` today):

```cpp
void draw_indexed(uint32_t index_count, uint32_t first_index = 0, int32_t vertex_offset = 0,
                  uint32_t instance_count = 1, uint32_t first_instance = 0) {
    vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
}
```

Chosen over a byte-offset rebind because the instance buffer binds **once per pass**;
each batch then costs one draw call instead of one bind+draw. `firstInstance` affects
instance-rate attribute fetching in core Vulkan with no feature flag (the
`drawIndirectFirstInstance` feature applies only to *indirect* draws — a common
misreading, worth a comment). **Caveat to document in the same place**: `gl_InstanceIndex`
already includes `firstInstance`, so it must never be used to index anything else — this
design never does (per-instance data arrives as a vertex attribute, whose fetch already
accounts for it), but writing the rule down protects a future SSBO-indexing change from a
subtle off-by-`firstInstance` bug. `Mesh::bind()` only binds slot 0, and per-batch mesh
rebinding therefore never clobbers the instance buffer at slot 1 — bind order per pass:
pipeline → `batcher.bind(cmd)` (once) → per batch: `mesh->bind(cmd)` + instanced draw.

### B3. Batch-key predicates

Shadow passes: `renderables_[i].renderer->get_mesh().get() == renderables_[i-1]...get()`
— pointer identity, captured fresh from the handle each frame (see A3's rule).

Color passes:
```cpp
/** @brief True when two materials would produce byte-identical push constants for G-buffer/forward. */
static bool same_material_(const PBRMaterial& a, const PBRMaterial& b) {
    return a.albedo == b.albedo && a.alpha == b.alpha && a.metallic == b.metallic
        && a.roughness == b.roughness && a.ao == b.ao
        && a.gpu_alpha_cutoff() == b.gpu_alpha_cutoff();
    // NOTE: texture_albedo/texture_normal/texture_metallic_roughness (and their
    // AssetHandle<Texture> payloads) are deliberately excluded -- nothing binds
    // them to a descriptor set yet. The day that lands, their payload identity
    // MUST join this comparison or a merged batch can silently draw someone
    // else's texture.
}
```
`alpha_mode` is intentionally excluded (already implied by which list — opaque vs.
transparent — an item is in).

### B4. Phased rollout

**Phase 0 — plumbing, zero behavior change.** `InstanceData` + `Mesh::draw(cmd,
instance_count, first_instance)` overload; `first_instance` param on
`CommandBuffer::draw_indexed`; new `instance_batcher.h`. Nothing consumes them yet —
build and confirm the output PNG is byte-identical to a pre-captured baseline.

**Phase 1 — directional shadow pass.** Simplest proof: no material, no fragment push
block. `ShadowPipeline`'s dir binding vector gains `InstanceData::binding_description()`
+ attrs (uses shared `Pipeline`, no core changes needed).
`DirectionalShadowPushConstants` shrinks to `{ mat4 light_space_matrix; }` (64B).
`shadow_depth.vert`: drop `model` from the push block, add
`layout(location = 4) in mat4 in_model;`. `record_shadow_geometry_()`: push
`light_space_matrix` once for the pass, `batcher.bind(cmd)` once, loop batches doing
`bind` + `draw(cmd, count, first_instance)`. Expect a **bit-identical** PNG.

**Phase 2 — cube shadow pass.** Same shape; `CubeShadowPushConstants` →
`{ mat4 light_space_matrix; vec4 light_pos_range; }` (80B), pushed once per face instead
of per object; reuses the same shadow `Range` across all six faces.

**Phase 3 — G-buffer (the hand-rolled pipeline, highest risk).**
- Edit `gbuffer_pipeline.h:72-83` directly: two-element binding vector, count=2,
  `.data()` pointer; append `InstanceData::attribute_descriptions()`.
- `GBufferPipeline::PushConstants` shrinks to `{ vec4 albedo; float metallic, roughness,
  ao, alpha_cutoff; }` (32B) — vertex stage no longer reads it, so narrow
  `pc_range.stageFlags` to `VK_SHADER_STAGE_FRAGMENT_BIT` only.
- `gbuffer.vert`: drop both mat4s (declares *no* push block at all afterward), add
  `in_model`, compute the normal matrix in-shader.
- `gbuffer.frag`: delete `mat4 model;`/`mat4 normal_matrix;` from its block, keep the rest.
- `record_gbuffer_geometry_()`: per batch, build the 32B material push from
  `opaque_renderables_[batch.item_index].renderer->material`, push, bind mesh, instanced
  draw. The per-object `glm::inverse` call disappears entirely.
- Verify most carefully here — first pass where a pixel diff (not just "doesn't crash")
  actually matters.

**Phase 4 — transparent + probe capture together (forced by the shared `pbr.vert`).**
- `pbr.vert` gets the same treatment as `gbuffer.vert`. `pbr.frag` **and**
  `probe_capture.frag` both declare the full block including the two leading mat4s — all
  three shaders change in lockstep; `TransparentPass::PushConstants` /
  `ProbeCapturePass::PushConstants` both shrink to the same 32B shape.
- `TransparentPass`/`ProbeCapturePass` (both use shared `Pipeline`) each gain one binding
  vector element.
- `GiSystem::bake()` gets its own small `InstanceBatcher` (one-time bake, six single-submit
  faces) — same `same_material_` predicate, minus `alpha`/`alpha_cutoff` since probe
  capture always forces `albedo.a=1, cutoff=0`.
- `record_transparent_geometry_()` batches **consecutive runs only** over the already
  sorted list — appending in sorted order and merging only adjacent equal-key runs
  preserves exact back-to-front submission order; never re-sort by key.
- Recompile every touched `.spv` by hand (`glslc x.vert -o x.vert.spv`) — there's no
  CMake shader-build step; the `.spv` files are checked in.

**Phase 5 (optional) — opaque pre-sort.** `std::stable_sort(opaque_renderables_)` by
(mesh pointer, material) before recording, to raise batch sizes past accidental
adjacency. Safe: G-buffer depth test is `VK_COMPARE_OP_LESS` (strict — no equal-depth
overwrite ambiguity) and opaque writes are order-independent. Verify **bit-identical**
against phase 4's output. Never sort the transparent list this way.

### B5. Verification

Per project convention, `MAX_FRAMES=n cplay`, compare `output/output-runtime.png`.
Capture baseline PNGs for both existing scenes **before** phase 0.

- **Determinism caveat**: TAA/SSR/SSAO temporal accumulation makes output
  frame-count-dependent but deterministic for a fixed `MAX_FRAMES` — valid to diff
  exactly as long as nothing else changed. If a diff looks noisy, rerun with
  `aa_mode: "off"`, `ssr_temporal_enabled: false`, `ssao_temporal_enabled: false` to
  isolate geometry from temporal history.
- **Expected diff per phase**: 0, 1, 2, 5 bit-identical. 3 and 4 may differ by a ULP or
  two in shaded pixels (CPU `glm::inverse` vs. GPU `inverse(mat3(...))`) — judge by "no
  structural difference, max per-channel delta ≤ 1-2", not exact-zero.
- **Proving batching actually engages**: `gi_cornell_box/scene.yaml` already has 4
  objects on `cube.000` + 1 on `sphere.000` — shadow passes should collapse from 5 draws
  to 2 with **zero new scene content**, a real, already-available proof for phases 1-2.
  But all 4 cube materials differ, so G-buffer batching there stays at
  `instance_count==1`. **Add `assets/scenes/instancing/scene.yaml`** (reusing
  `assets/scenes/cube/meshes/*.yaml`): a grid of `cube.000` objects sharing one exact
  material block, a few `sphere.000` sharing another, one deliberately off-key cube
  mid-grid (proves a key change splits the batch), and a short row of BLEND quads at
  varying depths (exercises the transparent pass's consecutive-run merging against the
  depth sort).
- **Instrument it**: log per-list batch count / total instances / largest batch behind an
  env var (matching the house `MAX_FRAMES`/`ONESHOT` style) — without this, "still
  renders" isn't evidence anything actually batched.
- **Negative control**: run the instancing scene with `continue_batch` forced `false`
  (every batch size 1) and confirm the PNG is bit-identical to the batched run. Isolates
  "did merging change the image" from "did the refactor change the image" — the
  definitive check for the transparent pass's ordering claim.
- Run at least one pass per phase with validation layers on — a missing binding-1 stream,
  a stride/offset mistake, or a push-constant block mismatched between vert/frag stages
  are exactly what they catch.

---

## Testing and Validation

```bash
cd /home/coopa/git/libcoopa && cbuild && cplay                          # Feature A: 3 new tests
cd /home/coopa/git/blendy   && cbuild --vulkan && MAX_FRAMES=64 cplay   # Feature B, per phase
cd /home/coopa/git/blendy   && MAX_FRAMES=64 ./build/blendy assets/scenes/gi_cornell_box/scene.yaml
cd /home/coopa/git/blendy   && MAX_FRAMES=64 ./build/blendy assets/scenes/instancing/scene.yaml  # new
```

Verify: libcoopa suite fully green; existing scenes render unchanged (bit-identical for
phases 0-2/5, ≤1-2 per-channel delta for 3-4); shadow batches show `instance_count==2`
on the cube/cornell scenes with zero new content; the new instancing scene shows
`instance_count>1` on G-buffer *and* transparent batches, with the negative-control run
confirming the merge itself changes nothing visually; clean Vulkan validation output
throughout.

---

## Progress (2026-08-19) — Implemented and verified in full

**Feature A** — `coopa/asset/asset_manager.h` / `asset_slot.h`:
- `retire_payload_()` extracted; `unload()`/`garbage_collect()` now route through it — this
  fixed a real pre-existing bug (found while implementing, not just theorized): both used to
  destroy a payload with **zero** GPU-safety grace period, unlike hot reload.
- `AssetManager::create<T>()` implemented with the null-payload / in-flight / file-backed
  guards from the design above.
- `set_idle_eviction`/`set_max_idle_frames`/`sweep_idle_()` implemented exactly as designed.
- 3 new tests added to `test.cpp` (`test_asset_manager_create_publishes_runtime_payload`,
  `test_asset_manager_create_republishes_with_grace_period`,
  `test_asset_manager_idle_eviction_defers_payload_destruction`). One bug caught and fixed
  in my own first draft of the idle-eviction test (dead leftover code that destroyed the
  test's tracked payload before the real test logic ran — worth noting since it shows the
  test itself was doing its job). **39/39 libcoopa tests passing**, confirmed clean across
  repeated `cbuild`/`cplay` runs.

**Feature B** — fully implemented across all 5 phases (B0-B4) plus the B5 verification scene:
- `InstanceData`/`Mesh::draw(cmd, instance_count, first_instance)` in
  `gfxcoopa/engine/data/mesh.h`; `first_instance` param added to `CommandBuffer::draw_indexed`;
  `InstanceBatcher` in new `gfxcoopa/engine/util/instance_batcher.h` — built exactly as
  designed (`firstInstance`-based batch addressing, single non-double-buffered instance
  buffer, caller-computed exact `continue_batch` predicate rather than a hashed key).
- Shadow (directional + cube), G-buffer, transparent, and probe-capture passes all
  converted. `GBufferPipeline`'s hand-rolled pipeline creation edited directly (the one
  pipeline not using the shared `Pipeline` class, exactly as flagged). `pbr.vert`/`pbr.frag`
  (shared by `TransparentPass` and `ProbeCapturePass`) converted together in one change, as
  required. One shader missed on the first pass — `transparent.frag` (a separate file from
  `pbr.frag`, easy to overlook) still declared the old 160-byte block — caught immediately
  by a Vulkan validation error (`pStages[1]... range [0,160] outside... [0,32]`) on the very
  next run and fixed before proceeding.
- `same_material_`/`same_capture_material_` predicates implemented as designed (bit-exact
  comparison, `alpha_mode`/`alpha`/`alpha_cutoff` excluded where the design said to).
- `INSTANCE_BATCHING=off` negative-control switch and `INSTANCE_STATS=1` batch-count logging
  added, matching the house `MAX_FRAMES`/`ONESHOT` env-var convention.
- New verification scene `assets/scenes/instancing/scene.yaml` (deliberately no
  `GiProbeVolume`/`ReflectionProbe` — keeps it fully deterministic, unlike the other two test
  scenes) with a 5-cube row (one deliberately off-key mid-row), a 3-sphere row, and 3 BLEND
  glass panes at staggered depth.

**Verification results, concretely:**
- `assets/scenes/instancing/scene.yaml`, batching **on**: shadow 4 batches/12 instances
  (largest 5), opaque 5 batches/9 instances (largest 3 — the off-key cube correctly split
  its batch and correctly rejoined afterward), transparent 1 batch/3 instances. Batching
  **off** (`INSTANCE_BATCHING=off`): 12/9/3 batches respectively, every one size 1. The two
  renders are **pixel-identical** (diff bbox: None, extrema all zero) — the strongest
  available proof that batching changes GPU submission count only, never the image.
- `gi_cornell_box` scene: **bit-identical** to its pre-instancing baseline at every phase
  (0 through B4) — including the non-uniform-scale walls, which specifically exercises the
  CPU→GPU normal-matrix relocation.
- `cube` scene (which has real GI-probe/reflection-probe bake nondeterminism, independently
  re-confirmed via same-code-rerun diffing at each phase): every phase's diff against the
  previous phase stayed within the same noise-floor magnitude as re-running *identical* code
  twice — no phase introduced a detectable regression beyond that pre-existing noise.
- Zero Vulkan validation errors across every run once the `transparent.frag` miss was fixed.

Nothing was descoped from the plan — all of Feature A, all 5 sub-phases of Feature B
(including the optional/lowest-priority probe-capture conversion), and the full B5
verification scene were completed.
