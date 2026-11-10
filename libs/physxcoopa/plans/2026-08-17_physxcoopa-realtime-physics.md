# physxcoopa — Real-Time Rigid Body Physics for the coopa Stack

> **Status: implemented.** Phases 0–9 are done. The library has since gone past this plan:
> compound colliders, hinge joints, XPBD cloth, speculative contacts for sphere/box pairs (the
> `max_linear_velocity` clamp remains as a backstop), and optional job-engine parallelism for
> broadphase, narrowphase and transform sync. Of the "v1.1 — deferred" list, CCD and job-engine
> parallelism are done; the rest is still open. The README and the per-module READMEs describe the
> current code; this plan is kept as design history.

## Context

`/home/coopa/git/physxcoopa` is an empty repo (`.git/` and `plans/` only). It becomes the physics sibling of `libcoopa` / `gfxcoopa` / `uicoopa` / `sfxcoopa` / `toyengine`: a header-only C++20 rigid body engine with Unity-equivalent semantics — box/capsule/sphere/static-mesh colliders, physics materials, static/kinematic/dynamic bodies, scene queries, triggers, collision layers — consumed by **toyengine**, the team's primary game engine.

There is no collision code of any kind anywhere in the stack: no `AABB`, `Ray`, `Sphere`, or `Capsule` type, no broadphase, no BVH, no 3D raycast. `coopa/util/math.h` (150 lines) has only a legacy, non-namespaced `Mat4` wrapper and a `MathUtil` static-method bag — no vector/quaternion/geometry math at all. The single ray/primitive routine in the entire stack is `intersect_box` in `gfxcoopa/engine/gi/gi_baker.h:48-80`, welded to GI baking (`SceneBox` at `:33-38`, `HitInfo` at `:40-46`). This is greenfield.

### Settled decisions

- **From scratch, header-only, zero third-party.** Sequential-impulse solver, dynamic AABB tree broadphase, SAT narrowphase — all in the coopa house style. Only `glm` and `fkYAML` (vendored in `libcoopa/includes/`) are used; no GJK/EPA/QuickHull (see "Design rationale").
- **Extend `coopa::util::Transform` with a quaternion path** — see the Transform extension section below.
- **In scope:** scene queries (raycast/overlap/sweep), triggers + collision callbacks, collision layers + layer matrix, debug collider visualizer.
- **Primary consumer: toyengine** (`/home/coopa/git/toyengine`), a pixel-art 3D engine on the same `coopa::gfx`/`coopa::scene` substrate as blendy.

### Constraints

1. **`Transform` is Euler-degrees-only, local-space-only.** `/home/coopa/git/libcoopa/coopa/util/transform.h:268-277` stores `glm::vec3 rotation_degrees_` (`:269`) and composes `R = glm::eulerAngleZYX(z, y, x)` at `:241-245` — so the documented "XYZ" is really `Rz*Ry*Rx`. There is no quaternion field, no `set_world_position`, no basis accessors. `<glm/gtc/quaternion.hpp>` is included at `:16` but `glm::quat` appears nowhere in the file. The Transform extension section below adds a quaternion path without disturbing this.
2. **The world is Blender Z-up, not Unity Y-up — verified numerically against toyengine's own scene.** `toyengine/assets/scenes/pixel_demo/meshes/plane.000.yaml` spans x/y `[-1,1]`, z `[0,0]` with normal `[0,0,1]`; `cube.000.yaml` spans z `[0,1]` (**and is corner-origin, not centre-origin** — see the toyengine integration section). `scene.yaml` places the ground plane at world z = −0.5. **Default gravity is `(0, 0, -9.81)`** and every "up" assumption — capsule axis, contact classification, ground checks — is +Z.
3. **No CPU mesh data survives GPU upload.** `gfxcoopa/engine/data/mesh.h:199-356` builds function-local vertex/index vectors (fan-triangulated at `:272-289`, one vertex per face corner — a cube becomes 24, not 8), uploads, and lets them die; `Mesh` keeps two `Buffer`s, `index_count_`, and an object-space AABB (`bounds_min_`/`bounds_max_`, `:341-351, 398-401, 422-423`, used for shadow-frustum fitting) — but still no vertex data. Readback is not viable — `gfxcoopa/memory/buffer.h`'s `vertex()`/`index()` (`:189-195, 205-211`) allocate `HOST_ACCESS_SEQUENTIAL_WRITE_BIT` with no `download()`. Phase 8 registers a dedicated `TriangleMeshLoader` with `coopa::asset::AssetManager` instead of deriving vertex data from the GPU path; no gfxcoopa change is required.
4. **No fixed timestep exists.** Neither toyengine's `frame_dt_()` (`toyengine/core/engine.h:221-223`, wrapping `libcoopa/coopa/util/time.h:53-64`) nor blendy's equivalent clamps or accumulates. toyengine does have a **`FIXED_DT` env override** (`engine.h:73, :222, :332-335`) for deterministic capture — useful for testing, but it replaces `dt` outright rather than substepping, so physxcoopa still owns its own accumulator and clamp.
5. **`SceneLoader`'s parser registry normalizes tags by stripping a leading `!`, but still drops unrecognized ones silently.** `register_component_parser(name, fn)` (`scene_loader.h:220-222`) stores under `normalize_tag_(name)` (`:260-263`), and dispatch (`parse_component_`, `:328-352`) normalizes the same way before lookup — so registering `"BoxCollider"` once matches both a `!BoxCollider` YAML tag and a `type: BoxCollider` key. Register each component name once, not twice. The remaining hazard: a tag with no registered parser at all still falls through with no warning, no log (`:350`, explicit comment: *"Unrecognized names fall through silently for forward compatibility"*) — every physics component name must be spelled identically between the exporter/scene-author and `register_physics_components()`.

---

## Architecture

physxcoopa is a two-layer split: a Scene-agnostic simulation core, and a thin adapter that plugs it into libcoopa's scene-system pipeline.

```
PhysicsWorld            — the simulation. No dependency on coopa::scene at all.
  step(dt)                accumulates and runs 0..max_substeps fixed calls to step_fixed(h)
  step_fixed(h)            ONE substep, no accumulator — what test.cpp and the
                            determinism harness drive directly
  add_body / remove_body, raycast/overlap, on_substep signal, world_state_hash()

PhysicsSystem : ISceneSystem   — the scene adapter. Owns a PhysicsWorld.
  execute(Scene&, FrameContext)
    1. reconcile Collider/Rigidbody components -> bodies (diff, not rebuild)
    2. sync world transforms -> bodies (kinematic velocity derivation, teleport detection)
    3. world_.step(ctx.delta_time)
    4. write interpolated transforms back; diff + emit queued collision/trigger events
```

Inside one `PhysicsWorld::step_fixed(h)`:

```
1. drain deferred structural commands (create/destroy body, set shape) queued last substep
2. integrate forces (gravity, drag) -> velocities
3. broadphase: refit + query trees -> pairs (brute-force O(n^2) until Phase 6)
4. layer matrix + kinematic/static pair reject
5. narrowphase -> persistent manifolds (warm-started by feature id / position match)
6. build islands (union-find over the contact graph)
7. fire on_substep signal (gameplay may add_force/add_torque/set_velocity/wake here; immediate)
8. solve velocity (8 it, zero positional bias) -> relax (1-3 it) -> solve position (3 it)
9. integrate velocities -> positions/orientations (quaternion exponential-map step)
10. sleep islands below threshold; wake islands touched by a moving kinematic body
```

**Everything runs on the main thread in v1.** `coopa::job` is available via `FrameContext::jobs`, but it is `nullptr` in both toyengine and blendy (neither calls `Scene::set_job_engine()`), so v1 ships zero job-dispatch code — see "Determinism and parallelism" below for what is built now to keep that a drop-in later.

**physxcoopa contains no Vulkan and no gfxcoopa dependency.** `PhysicsWorld` depends only on glm, `coopa::event::Signal`, and `coopa::debug::Logger`. It must **not** include `coopa/scene/scene.h` — that dependency belongs solely to `PhysicsSystem`. This split is what lets `test.cpp` exercise the solver, narrowphase, and broadphase (the large majority of the suite) with no `Scene` at all, and what lets gameplay code reach the world via `scene->find_system("Physics")` without ever seeing `PhysicsWorld`'s internals.

The debug visualizer produces a plain line list; the consuming engine renders it (see the toyengine integration section — toyengine has no ready-made overlay seam).

### File tree to create

```
physxcoopa/
├── CMakeLists.txt · README.md · .coopadocs · .gitignore
├── test.cpp                            headless suite (analytic + determinism)
├── configuration/root_directory.h.in
├── plans/2026-08-17_physxcoopa-realtime-physics.md
└── physxcoopa/
    ├── util/         math.h · error.h · config.h · transform_bridge.h
    │                                                          coopa::physx::util
    ├── geometry/     aabb.h · ray.h · sphere.h · obb.h · capsule.h
    │                 triangle_mesh.h · mesh_bvh.h            coopa::physx::geometry
    ├── collision/    shape.h · sat.h · clip.h · segment.h
    │                 manifold.h · narrowphase.h              coopa::physx::collision
    ├── broadphase/   aabb_tree.h · layer_matrix.h · pair_cache.h
    │                                                          coopa::physx::broadphase
    ├── dynamics/     physics_material.h · body.h · inertia.h
    │                 island.h · solver.h · integrator.h       coopa::physx::dynamics
    ├── query/        queries.h                                coopa::physx::query
    ├── debug/        debug_draw.h                             coopa::physx::debug
    ├── loaders/      triangle_mesh_loader.h                   coopa::physx::loaders
    ├── components/   collider.h · box_collider.h · capsule_collider.h
    │                 sphere_collider.h · mesh_collider.h
    │                 rigidbody.h · fixed_update_behaviour.h    coopa::physx::components
    ├── system/       physics_system.h                          coopa::physx::system
    ├── world.h       PhysicsWorld — the object test.cpp constructs directly
    └── physx_yaml.h  register_physics_components(AssetManager&) + install_physics_system()
```

There is no GJK/EPA or convex-hull builder anywhere in this tree — capsule–box and every other v1 shape pair is solved analytically (Phase 5, "Design rationale"). `util/transform_bridge.h` holds world-space `Transform` helpers that stay out of libcoopa; `loaders/triangle_mesh_loader.h` integrates with `coopa::asset::AssetManager`; `system/physics_system.h` is the `ISceneSystem` adapter — `PhysicsWorld` itself (`world.h`) has no Scene dependency at all.

House style: `#ifndef PHYSXCOOPA_<DIR>_<FILE>_H`, nested `namespace coopa { namespace physx { namespace <mod> {`, `PascalCase` types / `snake_case` methods / `private_members_`, `enum class`, `inline constexpr k_snake_case`, `throw std::runtime_error("[physxcoopa] ...")`, `coopa::debug::Logger("Physics")`, `coopa::event::Signal` for all callbacks, `@file`/`@class`/`@code` Doxygen on every file and public type, per-module `README.md` — matching gfxcoopa, libcoopa and toyengine's actual practice (sfxcoopa has none; do not follow that here).

**CMake follows sfxcoopa's `CMakeLists.txt`, not uicoopa's** — uicoopa never references a `LIBCOOPA_DIR` (it gets glm/fkYAML transitively through `coopa::gfx`) and is pinned to `cmake_minimum_required(3.16)`, one full minor behind every other sibling. sfxcoopa (`:1-17`) gives the right sibling-locate idiom: `cmake_minimum_required(VERSION 3.20)`, `set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")` + `if(NOT TARGET coopa::lib) add_subdirectory(...) endif()`. sfxcoopa's INTERFACE-target pattern (`:34-45`) also applies directly: `add_library(physxcoopa_lib INTERFACE)` + `add_library(coopa::physx ALIAS physxcoopa_lib)`, with the test executable guarded by `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)` (`:53-72`) so a consumer's `add_subdirectory()` doesn't inherit a stray `physxcoopa` test binary. Unlike sfxcoopa, physxcoopa has no impl translation unit, so it needs neither an `_impl` static library nor `pthread`/`m` linkage — pure INTERFACE, same shape as `libcoopa_lib` itself.

---

## Phase 0 — Scaffolding

`.coopadocs` (`include:\n  - physxcoopa`), `.gitignore` (`build/`, `.docs/`), `configuration/root_directory.h.in` copied from libcoopa, and a `CMakeLists.txt` mirroring sfxcoopa's: C++20, `ROOT_DIR_PARENT`, `LIBCOOPA_DIR` guarded `add_subdirectory`, `add_library(physxcoopa_lib INTERFACE)` + `add_library(coopa::physx ALIAS physxcoopa_lib)` linking `coopa::lib`, then (guarded) `add_executable(physxcoopa test.cpp)`, `enable_testing()`, `add_test(NAME physxcoopa_tests COMMAND physxcoopa)`.

**Ends when:** the repo builds an empty test binary and `ctest` runs it green.

## Phase 1 — Math, geometry primitives, and the accumulator skeleton

- **`util/math.h`** — `inline constexpr float k_epsilon = 1e-6f`, `k_gravity_z = -9.81f`, `k_default_fixed_dt = 1.0f/60.0f`, `k_max_frame_time = 0.25f`, plus `safe_normalize`, `orthonormal_basis(n, t1, t2)` (needed per contact for friction), `glm::mat3 skew(const glm::vec3&)`.
- **`util/config.h`** — solver tunables in one struct, discoverable and testable: `velocity_iterations = 16`, `relax_iterations = 2`, `position_iterations = 4`, `position_correction = 0.2f`, `linear_slop = 0.005f`, `max_linear_correction = 0.2f`, `restitution_threshold = 0.5f`, `sleep_linear = 0.01f`, `sleep_angular = 0.02f`, `sleep_time = 0.5f`, `aabb_margin = 0.1f`, `max_substeps = 8`, `max_linear_velocity = 200.0f` (the CCD-replacement clamp — see "Design rationale"). `velocity_iterations`/`position_iterations` end up higher than a first pass would guess — see Phase 3's solver bullets for the empirical reason.
- **`geometry/aabb.h`** — `struct AABB { glm::vec3 min, max; }` with `merge`, `overlaps`, `contains`, `expand(float)`, `surface_area()` (SAH), `center()`, `extents()`, `transform(const glm::mat4&)`.
- **`geometry/ray.h`** — `struct Ray { glm::vec3 origin, direction; float max_distance; }` + slab test `intersect(const AABB&, float& t)`. Crib the slab structure from `gi_baker.h:48-80` but strip the GI-specific `HitInfo::albedo` payload.
- **`geometry/obb.h` · `sphere.h` · `capsule.h`** — `Capsule` is a segment `{a, b}` plus `radius`. Include `closest_point_on_segment`, `closest_points_segment_segment` (parallel-degenerate branch handled), `closest_point_on_obb`.
- **`geometry/triangle_mesh.h`** — format-agnostic: `std::vector<glm::vec3> vertices; std::vector<uint32_t> indices;` plus per-triangle normals, per-edge adjacency (`(tri_a, tri_b)` or boundary/non-manifold flag), `AABB bounds()`. Construction from raw arrays only — the asset-loading path is Phase 8's `TriangleMeshLoader`, kept fully separate so physxcoopa is never welded to a scene format.

**Ends when:** ray–AABB intersection matches a hand-computed `t`; `closest_points_segment_segment`'s degenerate branch is exercised.

## Phase 2 — Bodies, inertia, integrator, fixed-step world — no collision yet

- **`dynamics/physics_material.h`** — Unity's `PhysicMaterial`: `dynamic_friction`, `static_friction`, `restitution`, `friction_combine`/`restitution_combine` as `enum class CombineMode { Average, Minimum, Maximum, Multiply }`, `inline float combine(float a, float b, CombineMode)`. Unity's asymmetric tie-break applies when the two materials disagree: Multiply > Maximum > Minimum > Average. Shared by pointer; default material used when none assigned.
- **`dynamics/inertia.h`** — analytic inverse inertia tensors for box, sphere and capsule (cylinder + two hemispheres, parallel-axis-shifted). `inverse_inertia_world = R * I_local⁻¹ * Rᵀ` recomputed each substep.
- **`dynamics/body.h`** — `enum class BodyType { Static, Kinematic, Dynamic }`; position, `glm::quat orientation`, linear/angular velocity, force/torque accumulators, `inv_mass`, `inv_inertia_local`, drag, `use_gravity`, a `constraints` bitmask (freeze position/rotation per axis by zeroing inverse-mass terms), sleep state/timer, `prev_position`/`prev_orientation` (render interpolation), and `last_written_position`/`last_written_orientation` for teleport detection on dynamic bodies (see "Design rationale"). Bodies live in a flat `std::vector<Body>` referenced by `BodyId { uint32_t index, generation; }`. This shape matches `coopa::job::JobHandle`'s own design (`libcoopa/coopa/job/handle.h:459-461`: `{CounterPool*, index, generation}`, with generation-checked `is_current()`/`is_complete()` guards at `:134` and `:381-387`, and no bump-pointer `reset()` at all) — though `BodyId` needs only a plain, non-atomic generation bump checked serially, not `JobHandle`'s lock-free Treiber-stack/CAS reclaim, since v1's body array is touched only from physxcoopa's single owning thread, unlike `JobHandle`'s lock-free multi-producer free list.
- **Kinematic velocity derivation**: `(new_pos - old_pos) / h` each step, so a kinematic body imparts correct impulses instead of acting like an infinite-mass teleporter.
- **Quaternion integration**: `q += 0.5 * ω * q * h` then renormalize, for **world-space** ω (consistent with `inverse_inertia_world` above) — state this convention explicitly in the doc comment, since a reader could otherwise assume body-space ω. Offer the exact exponential-map step (`q *= exp(0.5*ω*h)`) behind a config flag for fast spinners, where the first-order form visibly gains energy.
- **`world.h`** skeleton: `PhysicsWorld` ctor takes `PhysicsConfig`; `step(float dt)` — `accumulator_ += min(dt, k_max_frame_time)`, `while (accumulator_ >= h && n++ < max_substeps) step_fixed(h)`; `step_fixed(float h)` runs gravity + integration only (no collision yet); `set_gravity`; `interpolation_alpha()`. `world_state_hash()` — FNV-1a over every body's position/orientation/velocity bits — ships here, not later, so every subsequent phase is checked against it from day one.

**Ends when:** free fall after 1 s at `h = 1/60` matches `z ≈ -0.5·9.81` within semi-implicit Euler's known error bound; 600 calls to `step_fixed(h)`, run twice, produce a bit-identical `world_state_hash()`.

## Phase 3 — Sphere narrowphase and the whole solver (brute-force broadphase)

Deliberately the first phase with any collision, and it pulls in the entire solver rather than deferring it — this is the fastest path to something whose correctness can be judged by eye and by a closed-form answer.

- **`collision/shape.h`** — `enum class ShapeType { Sphere, Box, Capsule, TriangleMesh }` (no `ConvexHull`) and a `Shape` carrying geometry plus a `glm::vec3 local_center` (Unity's collider `center`).
- **Narrowphase, sphere-only**: sphere–sphere (analytic center distance) and sphere–plane/sphere–box (closest point). No dispatch table needed yet.
- **`collision/manifold.h`** — `struct ContactPoint { glm::vec3 position; float penetration; float normal_impulse = 0; float tangent_impulse[2] = {0,0}; uint32_t feature_id; }` and `struct ContactManifold { BodyId a, b; glm::vec3 normal; ContactPoint points[4]; uint8_t count; float friction, restitution; }`. For analytic sphere pairs `feature_id` is trivially stable (there is exactly one contact point); the harder cases needing an explicit id scheme land in Phase 4.
- **Broadphase**: brute-force O(n²) pair generation only — the point of this phase is to validate the *solver*, not the tree. The tree arrives in Phase 6 with this as its oracle.
- **`dynamics/solver.h`** — sequential impulses, split-impulse design (see "Design rationale"):
  1. **Islands** (`dynamics/island.h`): union-find over the contact graph; static bodies never merge islands; each island sleeps as a unit.
  2. **Warm start**: apply last step's impulses for feature-matched contact points.
  3. **Velocity iterations** (16, `config.velocity_iterations` — higher than the 8 a textbook sequential-impulse solver uses; see below for why): solve the normal constraint with **zero positional bias and zero restitution target** — clamped `jn >= 0` — then two tangent directions from `orthonormal_basis(normal)` with Coulomb clamp `|jt| <= friction * jn` on the accumulated normal impulse. This converges the base resting/sliding solution across every contact. **Points within one manifold are solved Gauss-Seidel (sequentially, each point's impulse applied immediately), not Jacobi** — a multi-point contact under an off-axis load (a box resting on a slope) needs friction to redistribute normal force asymmetrically across the contact face, which only converges in a reasonable iteration count when each point reacts to its neighbors' just-applied impulses. The cost of sequential order is a small, direction-consistent torque bias for contacts that *don't* need asymmetry (a box resting flat on another box) — pure reversal doesn't cancel it (it only swaps a 2-way bias), so **which point is solved first rotates every iteration** (`(k + iteration_index) % point_count`), cycling the advantage through all points evenly over a few iterations. This combination (Gauss-Seidel + rotating start point, 16 iterations) is what actually makes both a 10-box stack and a box on a 30° slope stable — confirmed empirically, not just in theory: forcing infinite inertia (no shape for torque to act through) eliminated a stack's lateral drift entirely, proving the mechanism, and iteration count alone (without rotating the start point) made the drift *worse*, not better, ruling out "just needs more iterations" as a fix by itself.
  4. **Restitution iterations** (1–3, `config.relax_iterations`, run *after* the base pass, not before it): the same normal-constraint solve, but targeting `v_target = -e * v_approach` (captured pre-solve, applied only when `v_approach > restitution_threshold` = 0.5 m/s) instead of 0. Applying restitution as a final correction on top of the already-converged base — rather than mixed into the main pass or followed by a target-0 pass — is what stops it from being immediately cancelled: nothing downstream can tell "genuinely bouncing" apart from "should be resting" once both sit at the same target, so restitution has to be the last word.
  5. **Position iterations** (3, `config.position_iterations`): pseudo-velocity / non-linear Gauss-Seidel pass using `config.position_correction`, draining penetration above `linear_slop` without injecting velocity — this is the only place positional correction happens.
  6. **Integration**: semi-implicit Euler for position; quaternion integration per Phase 2.
  7. **Sleeping**: accumulate sleep time while `|v| < sleep_linear && |ω| < sleep_angular`; sleep the whole island at `sleep_time` (0.5 s). Wake on: a new contact from an awake body, an applied force/impulse, a transform teleport, or a touching kinematic body whose derived velocity becomes non-zero (see "Design rationale" for why the last condition matters).
- **`on_substep` signal**: `coopa::event::Signal<PhysicsWorld&, float>`, fired after step 1 (kinematic sync) and before step 3 (force integration) of `step_fixed`, i.e. inside the loop this phase builds out. Immediate mutations (`add_force`, `add_torque`, `set_velocity`, `wake`) apply directly to `Body` fields. Structural mutations (`create_body`, `destroy_body`, `set_shape`) check an `in_step_` flag and, if set, push onto a `DeferredCommand` queue drained at the very top of the *next* `step_fixed` — a second, separate queue from Phase 9's collision-event queue, needed because a substep callback that destroys a body must not invalidate arrays the solver is mid-iteration over. This is a physics-internal queue, not `coopa::scene::SceneCommandBuffer` (`scene_commands.h`) — `PhysicsWorld` has no dependency on `coopa::scene`, and one queue suffices here since v1 physics is single-threaded, unlike `SceneCommandBuffer`'s one-per-worker design for job-dispatched scene mutation.

**Ends when:** a sphere dropped above a static plane falls, bounces at the analytically correct restitution, comes to rest with `penetration < linear_slop` and `|v| < sleep_linear`, and sleeps within 1 s. `world_state_hash()` stays bit-identical across two runs.

## Phase 4 — Boxes: SAT, feature ids, warm starting

- **`geometry/obb.h`** promoted to full use; **`collision/sat.h` / `clip.h`** — box–box SAT: 6 face axes + 9 edge-cross axes, reference/incident face selection, Sutherland–Hodgman clipping → up to 4 contact points. Sphere–box closest-point-on-OBB completes the pair table for this phase.
- **Feature ids, specified per pair:**
  - Box–box (SAT + clipping): pack `(reference_face, incident_face, clipped_vertex_index)` into `feature_id` — stable across steps because clipping is deterministic for a fixed pair of faces.
  - Sphere–anything: trivial, single point, always matches.
  - Any pair without a natural combinatorial id (capsule pairs, Phase 5): **position-based matching** — match to the previous step's point within ~2 cm in the manifold's tangent frame.
- Dispatch table, cheapest analytic path first:

  | Pair | Method |
  |---|---|
  | Sphere–Sphere | analytic (center distance) |
  | Sphere–Box | closest point on OBB |
  | Box–Box | SAT + Sutherland–Hodgman clipping, feature-id as above |

**Ends when:** a 10-box tower is stable for 1000 steps — no box drifts > 1 cm laterally, all asleep by the end; a box on a 30° slope with `μ = 0.8` (> tan30° ≈ 0.577) stays static after 5 s, `μ = 0.3` slides at the analytically expected acceleration.

## Phase 5 — Capsules (analytic only — no GJK/EPA)

- Segment–segment closest points (capsule–capsule) and segment-vs-OBB (capsule–box), both from Phase 1's geometry helpers.
- **Capsule–box is solved by clipping the capsule's segment against the box's 6 slabs**, taking the closest points on the clipped segment to the box surface, and promoting to a 2-point manifold when the segment lies parallel to a face. This is exact, cheap, reuses machinery capsule–capsule already needs, produces stable feature ids without a fallback, and has no iteration cap that can fail to converge (see "Design rationale" for why GJK/EPA is excluded entirely rather than used here).
- Capsule inertia from Phase 2 (cylinder + hemispheres) gets its analytic cross-check here.

**Ends when:** a capsule rests stably on a box floor; a pile of capsules settles without interpenetration.

## Phase 6 — Broadphase (with the brute-force path as its oracle)

- **`broadphase/aabb_tree.h`** — dynamic AABB tree (Box2D `b2DynamicTree` design): SAH insertion, rotation-based rebalancing, fat AABBs enlarged by `aabb_margin`. API: `create_proxy`, `move_proxy`, `destroy_proxy`, `template<typename Fn> void query(const AABB&, Fn&&)`, `template<typename Fn> void raycast(const Ray&, Fn&&)`.
- **Two trees**: a static tree (colliders with no `Rigidbody`; built once, refit only on explicit change) and a dynamic tree. Each step queries dynamic-vs-dynamic (self) and dynamic-vs-static (cross). **Static-static, static-kinematic and kinematic-kinematic pairs are never generated** — no broadphase test, no narrowphase, no solver constraint. Sleeping dynamic bodies are skipped as query *initiators* while still queryable as targets.
- **`broadphase/layer_matrix.h`** — `class LayerMatrix`: 32 layers, `uint32_t mask_[32]`, `should_collide(a, b)`, `set_layer_collision(a, b, bool)`. Tested before any geometry work.
- **`broadphase/pair_cache.h`** — persistent pair set for manifold/trigger persistence. This is a sorted `std::vector` keyed on `(proxy_a, proxy_b)` with `proxy_a < proxy_b`, never a pointer-keyed `unordered_map` — see "Determinism and parallelism" below.

**Ends when:** the tree's query returns the **identical pair set** as brute-force O(n²) over 1000 randomized AABBs, proxy move/refit preserves that equivalence, and — critically — every Phase 3–5 test still produces the same `world_state_hash()` it did with brute-force broadphase. This ordering — tree after solver, not before — means the tree has an exact oracle to check against from the moment it exists; building the tree first would leave it unverified until the whole stack is up.

## Phase 7 — Scene binding: components, YAML, `PhysicsSystem`, the Transform extension

### Components — passive descriptors, no `start()`-time body creation

`components/` — Unity-equivalent, all deriving `coopa::scene::Component`:

- **`Collider`** (base): `PhysicsMaterial* material`, `bool is_trigger`, `uint32_t layer`, `glm::vec3 center`, six `coopa::event::Signal`s (`on_collision_enter/stay/exit`, `on_trigger_enter/stay/exit`), plus `BodyId body_id_` (invalid until bound) and `uint32_t revision_`, bumped by every setter (`set_size`, `set_radius`, `set_center`, `set_is_trigger`, `set_layer`).
- **`BoxCollider`**: `glm::vec3 size` (full extents, Unity semantics; half-extents internally).
- **`CapsuleCollider`**: `float radius, height; int direction` — **default `2` (Z)** given the Z-up world, not Unity's Y default.
- **`SphereCollider`**: `float radius`.
- **`MeshCollider`**: `coopa::asset::AssetHandle<TriangleMesh> mesh` (see Phase 8) — not a `std::string mesh_path` holding raw data; the handle is populated by the parser via `AssetManager::load<TriangleMesh>()`.
- **`RigidbodyComponent`**: `mass`, `drag`, `angular_drag`, `use_gravity`, `is_kinematic`, `interpolation`, `constraints`, `add_force/add_torque/add_force_at_position`, `velocity()`, `angular_velocity()`, `move_position/move_rotation`.
- **`FixedUpdateBehaviour`**: a base class for gameplay code that needs a per-substep callback without a libcoopa `Component::fixed_update` hook (which does not exist and should not be added just for this). On its first `update()`, it looks up `scene->find_system("Physics")`, casts to `PhysicsSystem`, and calls `world().on_substep.connect(...)` once; disconnects in its destructor. This gets Unity-style `FixedUpdate()` ergonomics with zero libcoopa change.

**Physics components override `start()` with nothing.** They are pure data until `PhysicsSystem` gathers them — because `SceneLoader::load()` calls `Scene::start()` itself, at `scene_loader.h:202`, before the app has a chance to call `install_physics_system()`. A `start()`-time body-creation model cannot work here at all, since the world genuinely does not exist yet when `start()` runs.

### `PhysicsSystem` — reconcile, don't rebuild

`system/physics_system.h` — `class PhysicsSystem : public coopa::scene::ISceneSystem`, modelled on `coopa::anim::AnimationSystem` (`libcoopa/coopa/animation/animation_system.h`) for the lazy-gather shape, but not for its full-rebuild-on-dirty behavior:

- `execute(Scene&, const FrameContext&)`: on a `dirty_` flag, gather `scene.get_components<Collider>()` fresh, then **diff** against an `unordered_map<Collider*, BodyId>` — create bodies for new colliders, destroy bodies for colliders no longer present, and for survivors rebuild only the shape when `revision_` changed since last reconcile. This matters because `AnimationSystem`'s stateless rebuild is fine for its data (a `vector<Animator*>` loses nothing on rebuild), but a `Collider` owns a live broadphase proxy, warm-start impulses, and a sleep timer — a full rebuild on every `refresh()` would cold-start every stack in the scene and make one runtime `spawn()` visibly jitter everything else.
- The reconcile map is touched only during this diff, never during `world_.step()` — see the determinism note in Phase 6, since this is the one place a hash map is allowed.
- **Safe under a missed `refresh()`**: the diff only *erases* map entries absent from the fresh gather; it never dereferences a stale one. A forgotten `refresh()` after destroying an object leaks a body until the next refresh call — not a use-after-free. Document this contract exactly as `AnimationSystem::refresh()` already does ("call after adding/removing").
- `Scene::get_components<T>()` already skips `!obj.active()` (`scene.h:315-316`), so `SceneObject::set_active(false)` disables a collider for free on the next refresh — this is intended Unity-ish behavior, not an accident.
- After reconcile: sync transforms into bodies (deriving kinematic velocity, detecting a script-driven teleport on a dynamic body by comparing against `Body::last_written_position/orientation` from Phase 2), call `world_.step(ctx.delta_time)`, write interpolated transforms back via the world-space bridge (below), and drain the queued collision/trigger event diff (Phase 9) into `Signal::emit` calls.
- Free function `install_physics_system(Scene& scene, int order = static_cast<int>(UpdatePhase::Physics)) -> PhysicsSystem*`, mirroring `install_animation_system` (`animation_system.h:168`). Takes a raw `int`, not `UpdatePhase`, because `Scene::add_system(unique_ptr, int)` (`scene.h:395`) supports arbitrary orders, and an app whose gameplay is entirely animation-driven platforms with dynamic riders may want physics to run after `Animation` (300) rather than at the default 100 — order **325** works for this (after `Animation`=300, before the optional `TransformResolve`=350 and `LateBehaviour`=400, so a `TransformSystem` if installed still resolves physics' writes the same frame). Default to `UpdatePhase::Physics` (100) because:
  - it is the slot `scene_system.h:49` reserves for exactly this ("Reserved. No implementation ships with this change.");
  - install order must be < `UpdatePhase::LateBehaviour` (400) regardless of the value chosen — neither toyengine (`engine.h:208`, calls only `scene_mgr_.update(dt)`) nor blendy ever calls `Scene::late_update()`, so a system installed at ≥ 400 simply never executes;
  - at 100, physics runs before `Animation` (300), so a kinematic body driven by an `Animator` has its transform read one frame stale by physics — `v = (T_n - T_{n-1})/h` is still correct in magnitude, just delayed ~16 ms, which is a materially smaller cost than physics running after Animation: that would make `Behaviour` (200) scripts read pre-physics state and delay every collision-event reaction by a full frame.

### Transform resolution and read safety

`UpdatePhase` has five slots: `Physics=100, Behaviour=200, Animation=300, TransformResolve=350, LateBehaviour=400` (`scene_system.h:49-55`). `TransformResolve` belongs to `coopa::scene::systems::TransformSystem` (`coopa/scene/systems/transform_system.h`), which walks every root subtree once per frame and calls `Transform::recompute()` on dirty nodes — parallelized across independent root subtrees via `ctx.jobs->parallel_for_blocking()` when a job engine is present and there's more than one root, else serial. It is not auto-installed by `Scene`'s constructor (only `BehaviourSystem`/`LateBehaviourSystem` are); opt in via `install_transform_system(scene)`, same shape as `install_animation_system`.

Installing `TransformSystem` alongside physics is optional, not required for v1 correctness: the render path reads via the lazy-recomputing `get_world_matrix()` (`transform.h:181-184`), which recomputes on demand and is safe single-threaded, so physics writing transforms with nothing installing `TransformSystem` still renders correctly.

`PhysicsSystem`'s sync-in and write-back passes must always use `get_world_matrix()` (and the local `position()`/`rotation_degrees()`/`rotation_quat()` accessors, which need no resolve at all) — never the newer non-recomputing `world_matrix()` accessor (`transform.h:196-201`, which debug-asserts the cache isn't dirty). Physics runs at order 100, before `Animation`, `TransformResolve`, and `LateBehaviour` all run this frame, so it cannot assume any resolve pass has completed yet for objects it wasn't itself responsible for dirtying.

### `util/transform_bridge.h` — world-space access without touching libcoopa

`Transform::parent()` (`transform.h:139`) and `get_world_matrix()` (`:181-184`) are both already public, so world-space read/write does not require a libcoopa change:
- `Trs world_trs(const coopa::util::Transform&)` — decomposes `get_world_matrix()`.
- `void set_world_trs(coopa::util::Transform&, const glm::vec3& pos, const glm::quat& rot)` — pre-multiplies `inverse(parent()->get_world_matrix())` when parented, then calls `set_position` / `set_rotation_quat` (below).

### libcoopa `Transform` extension — the minimal diff

A mode bit that made the Euler and quaternion paths mutually exclusive would not survive contact with `coopa::anim`: `AnimatedProperty`'s rotation binding **reads** `rotation_degrees()` and **writes** `set_rotation(vec3)` (`libcoopa/coopa/animation/animated_property.h:281-288`). Under a mode bit, an object with both an `Animator` and a `Rigidbody` would have physics and Animation toggling the bit against each other every frame, and the Animator's blend baseline would read a `rotation_degrees_` that physics stopped updating — silently blending from a frozen pose.

**Fix: one representation, expressed twice, always in sync.** `set_rotation_quat()` writes **both** `rotation_quat_` and `rotation_degrees_`, converting via `glm::extractEulerAngleZYX` — present in the vendored glm 0.9.9.3 at `includes/glm/gtx/euler_angles.hpp:319`, and the exact analytic inverse of the `glm::eulerAngleZYX` composition `recompute_()` already uses (`transform.h:241-245`). ~40 lines, backwards compatible:

```cpp
    void set_rotation_quat(const glm::quat& q) {
        rotation_quat_ = q;
        glm::extractEulerAngleZYX(glm::mat4_cast(q),
            rotation_degrees_.z, rotation_degrees_.y, rotation_degrees_.x);
        rotation_degrees_ = glm::degrees(rotation_degrees_);
        use_quat_ = true;
        mark_dirty();
    }
    const glm::quat& rotation_quat() const {
        return use_quat_ ? rotation_quat_
                          : (rotation_quat_ = glm::quat(glm::radians(rotation_degrees_)));
        // (mutable rotation_quat_, or compute-on-read without caching — either is fine)
    }
private:
    glm::quat rotation_quat_{1,0,0,0};
    bool      use_quat_ = false;
```

`recompute_()` (`:238-266`) gains one branch: `R = use_quat_ ? glm::mat4_cast(rotation_quat_) : glm::eulerAngleZYX(...)` (existing line, `:241-245`). `set_rotation(vec3)` (`:74-77`) clears `use_quat_ = false`, so Euler authoring and every existing scene produce byte-identical matrices. Because `set_rotation_quat` keeps `rotation_degrees_` current, `AnimatedProperty`'s getter/setter pair keeps working unmodified on any object physics also touches — there is exactly one rotation, not two that can disagree.

`set_world_position` / `set_world_rotation` / `world_position()` do **not** go into libcoopa at all — they're fully achievable externally (see `transform_bridge.h` above) via the already-public `parent()`/`get_world_matrix()`, so the libcoopa diff is limited to the quaternion field and its writer.

**`mark_dirty()` (`transform.h:223-233`)** is iterative (an explicit `thread_local std::vector<Transform*> stack`, not recursion) with no early-out — still O(subtree) per call. No cycle guard is warranted: a cycle can only arise from a hand-built hierarchy (`SceneLoader` always builds a tree), and paying a visited-set on the hottest write path to defend against an authoring bug that cannot occur through the normal path is the wrong trade — an `#ifndef NDEBUG` depth cap is the most defensible addition, and it is optional. The O(subtree) cost is real (physics writes position and rotation separately — two full walks per body per frame); the correct fix is two lines: an early-out `if (t->dirty_.load(relaxed)) continue;` right after `stack.pop_back()` in the loop body, plus a `child->mark_dirty()` call inside `add_child()` (`:145-147` — currently the only place that can register an already-dirty parent's child as clean, breaking the "a dirty node's descendants are all dirty" invariant the early-out depends on). File as a libcoopa micro-PR gated on a profiler showing it matters; at toyengine's current scale (one scene, a handful of bodies, shallow hierarchies) it is very unlikely to.

**Guard the whole addition** with a libcoopa `test.cpp` case asserting `set_rotation(vec3)` still produces the identical matrix it does today, plus a new round-trip case: `set_rotation_quat(q)` → `rotation_degrees()` → `set_rotation(that vec3)` → same matrix as `mat4_cast(q)`.

### `physx_yaml.h`

`inline void register_physics_components(coopa::asset::AssetManager& assets)`, modelled on `gfxcoopa/engine/components/register.h:217-220`'s dependency-capture pattern: registers each component name once (tags are normalized by `SceneLoader` itself — see the constraints section), and the `MeshCollider` parser captures `assets` by reference to call `assets.load<TriangleMesh>(...)` using the `ParseContext::resolve()` the parser signature already provides (`scene_loader.h:99-138`). Parsers only populate component fields; body creation happens entirely in `PhysicsSystem`'s reconcile pass.

**Ends when:** in a headless scene-loading test, an object with `Transform` + `BoxCollider` + `Rigidbody` produces exactly one physics body after `install_physics_system()` + one `execute()` call, and dropping it onto a `BoxCollider`-only static plane settles and sleeps.

## Phase 8 — Mesh colliders via `TriangleMeshLoader`

- **`geometry/mesh_bvh.h`** — SAH-built static BVH over triangles, built once at load. `query(const AABB&, Fn&&)` and `raycast(const Ray&, Fn&&)`.
- **`loaders/triangle_mesh_loader.h`** — `class TriangleMeshLoader : public coopa::asset::TypedAssetLoader<TriangleMesh, TriangleMesh>`. Unlike gfxcoopa's `MeshLoader` (which splits `Intermediate = fkyaml::node` from the finalized GPU-uploaded `Mesh`, because `finalize()` must run on the main thread to touch Vulkan), physics has no GPU step at all — `decode_typed()` does everything, off-thread, and `finalize_typed()` returns its argument unchanged.

  `decode_typed()` pipeline, in order:
  1. Parse the mesh YAML (`vertices:`, `faces:`).
  2. **Fan-triangulate** each `faces:` entry — they are arbitrary polygons (the shipped cube mesh is 6 quads); warn-and-skip any face with fewer than 3 indices.
  3. **Weld** vertex positions with a spatial hash, at an epsilon expressed as a **fraction of the mesh's bounds diagonal**, not an absolute distance — a 100 m terrain and a 10 cm prop need different absolute tolerances, but the same fraction works for both. This step is mandatory, not an optimization: source meshes are already per-face-corner split (24 vertices for a cube), and the edge-adjacency table below cannot find shared edges without it.
  4. Drop triangles made degenerate by welding, keeping an index remap so triangle IDs used elsewhere stay stable.
  5. Build the edge table, keyed `(min_v, max_v) -> (tri_a, tri_b)`. Explicitly flag **boundary** edges (exactly 1 owning triangle — mesh perimeter) versus **non-manifold** edges (> 2 — malformed input); a boundary edge must be treated by internal-edge correction as "no constraint," not clamped against a neighbour that doesn't exist. This is the most common way internal-edge correction ships broken.
  6. Flatten per-triangle normal plus its three neighbour-triangle normals into one array indexed by triangle (never a map — the narrowphase inner loop touches this per contact).
  7. Build the SAH BVH; compute bounds.
- Register: `assets.register_loader<TriangleMesh>(std::make_unique<TriangleMeshLoader>())`. Sharing, hot reload and off-thread decode all come free from `AssetManager` — no separate cache to maintain. `AssetManager` keys slots by `type_index` (`asset_manager.h`), so a `TriangleMesh` and a `gfx::data::Mesh` loaded from the same `.yaml` path are two independent slots with no conflict.
- Use **synchronous** `assets.load<TriangleMesh>(...)` in v1, not `load_async` — async would force `MeshCollider`/`PhysicsSystem` to carry a "collider pending its mesh" state that has to be re-checked every frame for no benefit yet; switching later is a one-line change plus that state.
- **Convex-vs-triangle with internal-edge correction**: the classic ghost-collision bug — a box sliding across a flat mesh floor catches on the shared edge between coplanar triangles because the per-triangle contact normal points along the edge. Fixed by clamping each contact normal into the triangle's valid normal cone using the adjacency built above. Must be in from the start — retrofitting it means redoing manifold generation.
- **Non-convex mesh colliders are static-only**, matching Unity. A `MeshCollider` on a non-kinematic `Rigidbody` throws at bind time with a clear message. No `convex = true` hull path in v1 — a dynamic mesh collider needs QuickHull, cut along with GJK/EPA (see "Design rationale").

**Ends when:** a box slides across a two-triangle floor with no velocity discontinuity at the shared edge (the internal-edge-correction regression test).

## Phase 9 — Queries, triggers, events, debug draw, docs

### Scene queries

`query/queries.h`:

```cpp
struct RaycastHit { glm::vec3 point, normal; float distance; ColliderId collider; coopa::scene::SceneObject* object; };
bool raycast(const glm::vec3& origin, const glm::vec3& dir, float max_distance, RaycastHit& hit, uint32_t layer_mask = ~0u);
std::vector<RaycastHit> raycast_all(...);
bool sphere_cast(const glm::vec3& origin, float radius, const glm::vec3& dir, float max_distance, RaycastHit& hit, ...);
std::vector<ColliderId> overlap_sphere(const glm::vec3& center, float radius, uint32_t layer_mask = ~0u);
std::vector<ColliderId> overlap_box(const OBB& box, uint32_t layer_mask = ~0u);
```

Ray traversal walks both AABB trees front-to-back with early-out on the current best `t`, then exact ray-vs-shape (analytic for sphere/box/capsule, BVH descent for meshes). Gameplay reaches these through `PhysicsWorld` directly, obtained once via `scene->find_system("Physics")` — no `Scene` reference needed at the call site. **These queries are only valid between phases, not from inside `on_substep`** — a raycast issued from a substep callback observes mid-solve state; document this explicitly next to the signal.

### Triggers, events, sleeping wake rule

- **Trigger colliders** generate overlap records but no solver constraint; enter/stay/exit computed by diffing this step's overlap set against last step's, via the same persistent pair keys as `pair_cache.h`.
- **Events are queued during the substep loop and emitted once at the end of `PhysicsSystem::execute()`**, on the main thread — `coopa::event::Signal` is re-entrancy-safe but not concurrency-safe, and a callback that spawns/destroys bodies mid-solve would invalidate arrays the solver is iterating. This is a *different* queue from the `on_substep`-triggered `DeferredCommand` queue in Phase 3 — that one defers structural *mutation*, this one defers *notification*; both exist because either kind of callback could otherwise corrupt an in-flight solve.
- **Kinematic wake rule**: a sleeping island touching a kinematic body whose derived velocity becomes non-zero wakes immediately, regardless of contact freshness — otherwise a box asleep on a stationary platform that starts moving hovers in place while the platform slides out from under it. Needs its own test; the "kinematic imparts correct impulse" test only covers the already-awake case.

### Debug visualizer

`debug/debug_draw.h` — pure data, no Vulkan: `struct DebugLine { glm::vec3 a, b; uint32_t color; }`, `void PhysicsWorld::debug_draw(DebugDraw& out, DebugDrawFlags flags)` emitting collider wireframes, BVH nodes, contact points/normals, colored by state (white awake, green sleeping, red contact normal, yellow trigger).

toyengine renders it — see the toyengine integration section for exactly where, since toyengine has no existing overlay/UI draw-list seam to hook into.

### Documentation

Per-module `README.md` (mermaid `graph TD` + `### Components` + usage example, matching gfxcoopa/libcoopa/toyengine's convention — not sfxcoopa's, which has none) and a root README leading with the Z-up gravity default, the kinematic/animation interaction warning, and the "queries are invalid inside `on_substep`" caveat. Then `coopadocs build` warning-free.

**Ends when:** raycast at a unit box from a known origin returns exact `t`, point, and face normal; `raycast_all` returns hits sorted by distance; trigger enter fires exactly once, stay every step inside, exit exactly once on leaving; `coopadocs build` reports no warnings and `coopadocs show` renders every module.

---

## toyengine integration

`toy::core::Engine` (`toyengine/toyengine/core/engine.h`) is a **sealed facade** — `scene_mgr_` (`:401`) and `assets_` (`:400`) are private with no accessors, and there is no per-frame update-callback hook at all.

1. **`toyengine/CMakeLists.txt`** — insert immediately after the existing sibling-locate block (`:9-12`):
   ```cmake
   set(PHYSXCOOPA_DIR "${ROOT_DIR_PARENT}/physxcoopa")
   if(NOT TARGET coopa::physx)
       add_subdirectory(${PHYSXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/physxcoopa-build)
   endif()
   ```
   Add `coopa::physx` to **both** `target_link_libraries` calls — `:43` (`toyengine`) and `:48` (`toyengine_tests`). No `include_directories` change needed; the INTERFACE target carries its own.
2. **`toyengine/toyengine/core/engine.h` constructor** — all three additions land inside the existing ctor, no loop change required:
   - `assets_.register_loader<physxcoopa::TriangleMesh>(std::make_unique<physxcoopa::TriangleMeshLoader>());` alongside the two existing loader registrations (`:80-83`).
   - `physxcoopa::register_physics_components(assets_);` alongside `scene::register_scene_components();` (`:85`).
   - `physxcoopa::install_physics_system(scene_mgr_.get_active_scene());` immediately after `scene_mgr_.load_scene(...)` (`:87`).
   - Teardown needs no change — `SceneLoader::clear_component_parsers()` (`:105`) already clears the whole global parser registry, physics parsers included.
   - `tick()` needs no physics call at all. `scene_mgr_.update(dt)` at `:208` already drives every installed `ISceneSystem`, physics included.
   - Add a public accessor — `coopa::scene::Scene& scene()` or similar — since currently no application or test code can reach the scene at all. Needed for both physics-focused headless tests and any future gameplay code.
3. **A dedicated `assets/scenes/physics_test/scene.yaml`** (already authored, with its own `meshes/` copied from `pixel_demo`) rather than adding colliders to the shared demo scene — keeps `pixel_demo` and its existing render tests (`toyengine_render`) completely untouched. It has a static `ground` (`plane.000`, `BoxCollider` sized as a thin slab: `size {2,2,0.2}`, `center {0,0,-0.1}` so the collider's top sits at the visible surface, world z = 0) and three dynamic bodies dropped from above it: `box_a` and `box_b` (`cube.000` + `BoxCollider` + `Rigidbody`, offset in x/y so they land near rather than exactly on top of each other) and `sphere_a` (`sphere.000` + `SphereCollider` + `Rigidbody`). **`cube.000` is corner-origin `[0,1]³`, not centre-origin** (confirmed from `meshes/cube.000.yaml`'s vertex range) — both box objects place their `Transform.position` at `center - 0.5*scale` (matching `pixel_demo`'s own convention for the same mesh) and give their `BoxCollider` a `center` of `{0.5, 0.5, 0.5}` in local space so the collider lines up with what's actually drawn; `sphere.000` is already centre-origin, so `sphere_a` needs no such offset. All physics component tags (`BoxCollider`, `SphereCollider`, `Rigidbody`) are silently dropped by `SceneLoader` until `register_physics_components()` runs (constraint 5), so this scene loads and renders inertly today and starts simulating the moment physxcoopa is wired in — no toyengine code needs to change for the scene itself to exist.
4. **Pointing the engine at it.** `toy::core::Engine`'s constructor hardcodes `AppConfig::load(ROOT_DIR "/assets/config.yaml")` (`main.cpp`) and `AppConfig::scene.default_scene` (`config.yaml:7-8`) has no env or CLI override — there is no `SCENE=...` equivalent to `MAX_FRAMES`/`FIXED_DT`. Validating physics therefore means pointing `default_scene` at `assets/scenes/physics_test/scene.yaml`, either by editing `config.yaml` locally for the test run (revert after) or by duplicating it as `assets/config.physics_test.yaml` and using that path in `main.cpp` for a physics-focused build — the plan does not prescribe which, since it's a one-line, easily reverted local change either way.
5. **The accumulator clamps its own input regardless of `FIXED_DT`.** There is no dt clamp anywhere in the stack — `frame_dt_()` (`engine.h:221-223`) returns raw, unclamped `ctx_.delta_time()`. toyengine's existing `FIXED_DT` env override (`:73, :222, :332-335`) replaces `dt` outright for deterministic capture and is the ready-made harness for physics determinism tests, but it does not substitute for the accumulator's own `min(dt, k_max_frame_time)` clamp, which still must exist for ordinary (non-`FIXED_DT`) runs.
6. **The animation/physics ordering warning should ship even though it's currently dormant.** toyengine has no `Animator` component anywhere and never calls `install_animation_system` — the demo scene is entirely static except for mouse-driven camera orbit. This makes physics the first source of autonomous motion in toyengine, a clean regression-testing property, but the warning belongs in the README so the first person who adds an `Animator` to a physics object finds it there. `PhysicsSystem`'s reconcile pass should also log once if it finds an object with both a non-kinematic `Rigidbody` and an `Animator` binding to `Transform` — twenty lines, saves hours.
7. **Debug visualizer placement.** toyengine has no existing line/gizmo/overlay draw path and does not use uicoopa at all, so there is no overlay/UI draw-list seam to hook into. It renders at a low internal resolution (480×270 default) and upscales, which constrains where a line pass can go:
   - **Recommended: post-upscale.** A second draw call in the swapchain record lambda (`toy_render_pipeline.h:1096`), immediately after `upscale_pass_->draw(cmd, letterbox)`. Full display resolution, crisp lines, unaffected by palette quantization; must apply the `letterbox` rect itself as a viewport/scissor transform to stay aligned with the world.
   - Rejected: pre-upscale, inside `post_target_`'s single `begin()`/`end()` bracket before `:1352`. Gets pixel-grid-locked lines, but they are 1 texel wide at 480×270 and get palette-quantized/dithered along with everything else — and `gfxcoopa`'s `RenderPass` is always `LOAD_OP_CLEAR` (`render_pass.h:75`), so the draw cannot happen in a separately reopened pass; it would have to share the one bracket that's already crowded with the full opaque/transparent/fog/bloom/stylize chain.
   - Line-rendering pipeline state already exists and needs no gfxcoopa change: `RasterState{ .topology = Topology::LineList, .polygon = PolygonMode::Line, .cull = CullMode::None, .line_width = ... }` (`gfxcoopa/pipeline/pipeline.h:81-87`), plumbed to Vulkan at `:465, :489, :493`.
8. **Reuse existing screen-space helpers rather than reimplementing them**: `compute_sdf_clip_rect` (`toyengine/render/toy_render_math.h:178-207`, world-AABB → NDC) and `sdf_clip_rect_to_pixels` (`:237-255`, NDC → pixel with the Y-flip already correct) are exactly the primitives a collider-overlay screen-space pass needs.
9. **Input**: toyengine already uses `coopa::input::InputMap` (`engine.h:227, :403`), so a raycast/mouse-picking demo is straightforward — but note `apply_cursor_capture_()` (`engine.h:317-323`) hides and unbounds the cursor by default via the active `CameraController`, and there is currently no in-app way to release it (`:313-315`). A picking demo needs `capture_cursor: false` set on the scene's `CameraController` or an added release path.

**Verification**: point `default_scene` at `assets/scenes/physics_test/scene.yaml` (item 4 above), then `cbuild --vulkan` and `MAX_FRAMES=n cplay` in `/home/coopa/git/toyengine` (README `:42-62`) — `box_a`, `box_b` and `sphere_a` must all fall along **−Z**, land on `ground`, settle without jitter, and sleep; `box_a`/`box_b` should end up close together (they're dropped offset but near each other) without interpenetrating. Enable the debug visualizer and confirm wireframes align with rendered meshes (catches scale/center-offset errors fastest — `box_a`/`box_b`'s corner-origin `center` offset is exactly the kind of mistake this catches). Compare resting positions at `MAX_FRAMES=300` under `FIXED_DT=1/30` vs `FIXED_DT=1/144` — must agree to within a millimetre. `ctest --test-dir build` runs `toyengine_render` against `pixel_demo`, untouched by any of this; add a physics case to `toyengine/test.cpp` following its existing style (a hand-rolled `expect(bool, const std::string&)` at `:25-32`, plain `void test_<name>()` functions listed in `main()` at `:503-527` — no macros, unlike physxcoopa's own `test.cpp`) that loads `physics_test` specifically. Config `save_on_exit` is on by default (`assets/config.yaml:170-175`) — do not kill the process, let it exit normally. Re-run libcoopa's own `test.cpp` after the Phase 7 `Transform` edit and re-render `pixel_demo` to confirm non-physics output is unchanged.

---

## Design rationale

- **Split-impulse solver, not Baumgarte-in-the-velocity-solver.** Pairing a positional bias inside the velocity solver with a separate position-correction pass double-corrects — the bias injects energy that the position pass then has to absorb, visible as stacks that gently "breathe." Phase 3 uses split impulses instead: zero positional bias in the velocity solver ever, all positional correction in the position pass. Restitution is likewise kept out of the main velocity iterations (which always target 0) and applied as a small number of iterations *after* that base solve has converged, not before it and not interleaved with it — a target-0 pass run after a restitution pass cannot distinguish "genuinely bouncing" from "should be resting" and silently cancels the bounce it was supposed to produce, which is what a same-order implementation actually does in practice, not just in theory.
- **No CCD in v1.** A margin-inflated speculative-contact scheme cannot work with this velocity-solver bias term: `max(0, penetration - linear_slop) * beta / h` is identically zero for a separated pair, so an inflated broadphase margin produces a manifold with zero penetration, contributing nothing to the solve — the body passes through regardless. Real speculative contacts need a different constraint formulation (`v_n >= -separation/h`, not a penetration-based bias), and the margin has to scale with `|v|*h` or slow bodies generate enormous spurious manifolds. Ship a `max_linear_velocity` clamp (`config.h`) and a plain README caveat instead; defer to v1.1.
- **No GJK/EPA/QuickHull in v1.** Capsule–box has an exact analytic solution (segment-vs-OBB clipping, Phase 5) that is cheaper, has no iteration cap to fail, and produces stable feature ids for free. EPA produces no stable feature id at all (its output face depends on polytope-expansion order and jitters step to step on a resting contact), so warm-starting would silently fail to engage on exactly the shape pair — a character capsule resting on a floor — where it matters most. Every v1 shape (sphere/box/capsule/mesh) has an analytic or SAT solution; removing GJK/EPA/hull removes the two most numerically delicate files in the project and the only feature with a randomized-convergence test that could flake in CI.
- **Feature-id generation needs a scheme per pair, not just an assertion that ids match.** Specified in Phase 4: SAT pairs pack a combinatorial id from face/vertex selection; anything without a natural id (segment-based pairs) falls back to position-based matching within ~2 cm in the manifold frame.
- **Sleeping's wake conditions include the kinematic-platform case.** "New contact, applied force, or teleport" alone omits a sleeping body on a *stationary* kinematic platform that starts moving — none of those three fire, so the body would hover while the platform slides away. Phase 3/9 add: a kinematic body's derived velocity going non-zero wakes every island it touches.
- **`restitution_threshold = 0.5` m/s, not the more common 1.0.** At 1.0, in a Z-up scene at 60 Hz, a 5 cm drop impacts at ~1 m/s — right at the threshold, so runs would flip unpredictably between bouncy and dead.
- **`BodyId` mirrors `JobHandle`'s generational design** (see Phase 2) — both are `{index, generation}`, which prevents a recycled body/job slot from being addressed by a stale handle. `BodyId`'s version can be simpler than `JobHandle`'s, since it only needs a serial, non-atomic generation bump: v1's body array is never touched from more than one thread.
- **Teleport detection for dynamic bodies**: `PhysicsSystem`'s sync-in pass compares each dynamic body's current `Transform` against `Body::last_written_position/orientation` (Phase 2) to notice a script-driven `set_position()` distinct from physics' own last write.

## Determinism and parallelism

**v1 ships fully serial — zero job-dispatch code.** `FrameContext::jobs` is `nullptr` in both toyengine and blendy (neither calls `Scene::set_job_engine()`), so any v1 job code would be dead on arrival in both consumers. The real argument against parallelizing now isn't thread-safety of `Transform`'s matrix cache (transform write-back is a separate serial pass regardless of solver threading) — it's that the substep loop runs 1–8× per frame, so naive per-phase fork/join would mean up to 24 barrier syncs per frame, which swamps the actual work below a few hundred bodies.

When a job engine is eventually wired up, `coopa::job::JobEngine::parallel_for_blocking()` (`engine.h:309-316`) is the primitive to use — it manages job-handle lifetime internally (no manual `close()` bookkeeping) and is the exact idiom `coopa::anim::AnimationSystem::evaluate_parallel_()` and `coopa::scene::systems::TransformSystem::execute()` both already use for structurally identical fan-out-over-independent-units work. A shared `JobEngine` needs no coordination between subsystems — `begin_frame()`/`end_frame()` are diagnostics-only no-ops (`engine.h:147, 154`), so physxcoopa never needs to own or contend for a frame boundary.

Three rules adopted now so a later parallel pass is additive, not a rewrite:

1. **Narrowphase is the only correct first target**, and its layout must already support it: manifolds are written **by index into a pre-sized `std::vector<ContactManifold>` with a `valid` flag, then compacted in a separate serial pass — never `push_back` from a worker.** Each pair reads two immutable poses and writes one manifold with no cross-pair communication, so this layout is bit-identical to serial by construction, with nothing to prove later. (Island solving is the wrong target — Gauss-Seidel is order-dependent by definition. Broadphase refit is the wrong target — inherently serial tree mutation, ~2% of frame.)
2. **`world_state_hash()` ships in Phase 2**, not as an afterthought — FNV-1a over every body's position/orientation/velocity bits. It is the determinism harness for every phase from that point on, and becomes the bit-identical assertion the moment parallel narrowphase lands, the same way `AnimationSystem::set_parallel_threshold()` lets a test force serial-vs-parallel and compare.
3. **No pointer-keyed hash map may be iterated inside `step_fixed`.** `unordered_map` iteration order over pointer keys varies run to run under ASLR. The pair cache and manifold store are sorted vectors keyed on `(proxy_a, proxy_b)` with `proxy_a < proxy_b` (Phase 6); the body array is iterated by index throughout. The one exception is the `Collider*` → `BodyId` reconcile map (Phase 7), which is fine specifically because it is touched only during reconcile, never during the solve.

The determinism test needs to be stronger than "same scene, 600 steps, run twice ⇒ bit-identical" — that passes trivially even with a hash map in the solve loop, since it's the same process with the same allocator state both times. The real test: `world_state_hash()` must be invariant to spawning and destroying a distant, non-interacting body mid-run — that's what actually catches pair-ordering nondeterminism.

---

## v1.1 — deferred

Collected here rather than scattered through the phases, so the v1/v1.1 boundary is legible in one place:

- GJK, EPA, and QuickHull convex hulls (dynamic convex colliders beyond box/sphere/capsule).
- CCD via a correctly-formulated speculative-contact constraint (`v_n >= -separation/h`), replacing the v1 `max_linear_velocity` clamp.
- `Interpolation::Extrapolate` (v1 ships `None`/`Interpolate` only).
- Job-engine parallelism for narrowphase, via `JobEngine::parallel_for_blocking()` over a pre-sized, index-written manifold vector (contingent on a consumer actually installing a `JobEngine` via `Scene::set_job_engine()` — neither toyengine nor blendy does today).
- Routing any worker-dispatched collision callback that needs to spawn/destroy a `SceneObject` (e.g. a shatter-on-impact prefab) through `coopa::scene::SceneCommandBuffer` (`FrameContext::commands`) rather than a bespoke mechanism — not needed in v1, where the event queue drains on the owner thread inside `PhysicsSystem::execute()` and can call ordinary `Scene` mutation methods directly.
- Blender-exporter rigid-body round-trip: emitting `type: BoxCollider` / `type: Rigidbody` from `python/blendy/bpy/scene.py` (or a toyengine-side equivalent, since toyengine has no Blender exporter of its own today).

---

## Testing and Validation

### Headless suite — `physxcoopa/test.cpp`

Hand-rolled macros copied from `/home/coopa/git/uicoopa/test.cpp` (`RUN_TEST` `:68-82`, `ASSERT_TRUE` `:84-90`, `ASSERT_NEAR` `:92-100`), plus a new `ASSERT_VEC3_NEAR` built the same way as uicoopa's existing `ASSERT_VEC2_NEAR` (`:102-106`). Built around **closed-form answers**, organized by the phase that introduces each case:

| Area | Assertion |
|---|---|
| Geometry (P1) | Ray–AABB against hand-computed `t`; `closest_points_segment_segment` on the parallel-segment degenerate case; capsule inertia matches the analytic cylinder+hemispheres value |
| Free fall / determinism (P2) | After 1 s at `h=1/60`, `z ≈ -0.5·9.81` within semi-implicit Euler's known error bound; 600 `step_fixed` calls run twice ⇒ bit-identical `world_state_hash()`; hash is invariant to spawning/destroying a distant non-interacting body |
| Resting contact & restitution (P3) | A dropped sphere converges to `penetration < linear_slop` and `|v| < sleep_linear`, sleeps within 1 s; `restitution = 1.0` returns to ≥95% of drop height, `0.0` never bounces |
| Energy (P3) | A frictionless, restitution-0 box dropped on a plane never gains kinetic energy over 1000 steps — the cheapest detector for a double-correction bug in the solver |
| SAT box–box (P4) | Known overlaps with hand-derived normal and penetration; face-face contact yields exactly 4 points, edge-edge exactly 1 |
| Stacking & friction (P4) | 10-box tower stable for 1000 steps, no drift > 1 cm laterally, all asleep by the end; 30° slope: `μ=0.8` static after 5 s, `μ=0.3` slides at the expected acceleration |
| Mass ratio (P4) | A 100:1 mass stack does not explode — standard smoke test for iteration counts |
| Capsules (P5) | Capsule rests stably on a box; capsule pile settles without interpenetration |
| BVH / broadphase (P6) | Tree query returns the **identical pair set** as brute-force O(n²) over 1000 randomized AABBs; proxy move/refit preserves equivalence; every P3-P5 test's hash is unchanged with the tree enabled |
| Layers | `set_layer_collision(a, b, false)` ⇒ pair never reaches narrowphase (assert via a counter) |
| Kinematic (P3/P9) | Kinematic platform at 2 m/s imparts the correct impulse to a resting box; static-static and kinematic-kinematic pairs generate zero narrowphase calls |
| Kinematic wake (P9) | A body asleep on a *stationary* kinematic platform wakes when the platform starts moving |
| Mesh collider (P8) | Box slides across a two-triangle floor with no velocity discontinuity at the shared edge |
| Queries (P9) | Raycast at a unit box from a known origin returns exact `t`, point, face normal; `raycast_all` sorted by distance |
| Triggers (P9) | Enter fires exactly once, stay every step inside, exit exactly once on leaving |
| Perf | 500 dynamic boxes step in < 16 ms single-threaded (reported, not asserted — avoids a flaky CI gate) |

```bash
cbuild && ctest --test-dir build --output-on-failure     # or ./build/physxcoopa
```

### Integration validation in toyengine

Per the project's build convention, use `cbuild --vulkan` and `MAX_FRAMES=n cplay` in `/home/coopa/git/toyengine` — **never kill the process**, since `save_on_exit` (`assets/config.yaml:170-175`) writes on normal exit only.

1. Point `default_scene` at `assets/scenes/physics_test/scene.yaml`, then `cbuild --vulkan` and `MAX_FRAMES=300 cplay`: `box_a`, `box_b` and `sphere_a` must all fall along **−Z**, land on `ground`, settle without jitter, and sleep.
2. Enable the debug visualizer (post-upscale pass) and confirm collider wireframes align exactly with rendered meshes — fastest way to catch a scale or `center` offset error, especially given `box_a`/`box_b`'s corner-origin `cube.000` mesh.
3. Frame-rate independence: compare resting positions at `MAX_FRAMES=300` under `FIXED_DT=1/30` vs `FIXED_DT=1/144` — must agree to within a millimetre.
4. Add a physics case to `toyengine/test.cpp` in its native style (`expect(bool, const std::string&)`, `:25-32`; a plain `void test_*()` listed in `main()`, `:503-527` — no macros, unlike physxcoopa's own suite) that loads `physics_test` and asserts the three dynamic bodies come to rest above the ground plane.
5. Regression: re-run libcoopa's own `test.cpp` after the Phase 7 `Transform` edit, and re-render `pixel_demo` (`toyengine_render` ctest, unaffected by any of this) to confirm non-physics output is unchanged.
6. `coopadocs build` reports no warnings; `coopadocs show` renders every module.
