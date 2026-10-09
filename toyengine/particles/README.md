# toyengine/particles

Realtime particle effects: Unity's module stack (main, emission, shape, velocity, forces, noise,
colour/size/rotation over life, collision, sub emitters, renderer) combined with Blender's mesh
emitter, which spawns on a mesh's faces, vertices or edges, oriented to the surface. It also has
Blender's hair-style **scatter**, which places static instances over a surface. Simulation runs on
the frame's `JobEngine`, and rendering is one instanced draw per system inside the forward
transparent pass.

The look matches the water's mix of real and stylized. Lighting and depth are real: sun, sky and
nearby point lights, and soft intersections with geometry. Sprite shapes are stylized: crisp
edges broken up by noise, and toon-banded cores. Every sprite is procedural, so an effect needs no
texture authoring. `sprite: texture` takes an albedo map or a flipbook atlas instead.

Demo scene: `./build/toyengine particles_test`
([assets/scenes/tests/effects/particles_test](../../assets/scenes/tests/effects/particles_test/scene.yaml), meshes from
[`tools/gen_particle_test_assets.py`](../../tools/gen_particle_test_assets.py)). It shows:
- a campfire (`prefab: objects/campfire`, see
  [assets/objects/props/campfire.yaml](../../assets/objects/props/campfire.yaml)) with flames, glow, coals,
  embers, smoke and a flickering light;
- a torch whose world-space flames and smoke trail behind it as it circles;
- mushrooms, pebbles and fallen leaves scattered over a mound;
- sparkles and rune rings emitted from a spinning torus's faces and vertices;
- a fountain whose droplets die on the ground and spawn splash rings.

| File | Purpose |
|---|---|
| [`particle_math.h`](particle_math.h) | `Rng` (PCG32), `Range` (constant or random between two), `FloatCurve` / `Gradient` (key lists baked to 64-entry tables), `TurbulenceField` (curl noise), quaternion helpers. |
| [`particle_shape.h`](particle_shape.h) | Emission shapes. `MeshSurface` samples a mesh by area, vertex or edge length, and carries the normal and tangent. |
| [`particle_system.h`](particle_system.h) | `ParticleSystem`: the component, its `ParticleSettings`, the SoA pool, `step()` and `prepare_render()`. |
| [`particle_system_runner.h`](particle_system_runner.h) | `ParticleSimulationSystem` (order 360). Steps every system on the job workers, forwards sub-emitter deaths, and collects draw batches. |
| [`particle_yaml.h`](particle_yaml.h) | Parsers for "ParticleSystem" and "LightFlicker", plus the shared emitter-mesh cache. |
| [`light_flicker.h`](light_flicker.h) | `LightFlicker`: layered-noise intensity, colour and position flicker on a sibling `PointLight`. |

Rendering code outside this directory:
- [`render/particle_types.h`](../render/particle_types.h): the plain-data contract (`ParticleInstance`, `ParticleLook`, batches).
- [`render/passes/particle_pass.h`](../render/passes/particle_pass.h) with
  [`particle.vert`](../../assets/shaders/particle.vert) and [`particle.frag`](../../assets/shaders/particle.frag).
- `Engine::sync_particle_render_state_()`, the per-frame bridge.

## Authoring

```yaml
- type: ParticleSystem
  # Main
  shape: cone            # point | sphere | hemisphere | cone | box | circle | edge | mesh
  radius: 0.3
  angle: 8               # cone half-angle, degrees
  rate: 40               # per second; also rate_over_distance, bursts
  start_lifetime: [0.5, 0.9]       # a constant, [min, max], {min, max} or {x, y}
  start_speed: [0.7, 1.2]
  start_size: [0.4, 0.8]
  gravity: -0.25         # x 9.81 m/s^2 downward; negative rises
  noise_strength: 0.9    # curl-noise turbulence, m/s^2 RMS
  size_over_life:  [{t: 0, value: 0.6}, {t: 0.25, value: 1.0}, {t: 1, value: 0.2}]
  color_over_life: [{t: 0, color: {r: 1, g: 0.86, b: 0.36, a: 1}}, {t: 1, color: {r: 0.3, g: 0.03, b: 0, a: 0}}]
  # Renderer
  render_mode: vertical  # billboard | stretched | horizontal | vertical | aligned | mesh | none
  sprite: flame          # soft | circle | puff | flame | spark | ring | star | leaf | texture
  additive: 0.55         # 0 alpha blend .. 1 additive (or blend: alpha | additive)
  emissive: 1.9          # HDR multiplier; bloom catches > ~1.4
  toon_bands: 4          # banded lighting and flame cores; 0 = smooth
  lit: 0.0               # 0 unlit (fire) .. 1 lit by sun, sky and point lights (smoke)
```

Every key, by module:

- **Main:** `mode` (emitter, scatter), `duration`, `looping`, `prewarm`, `start_delay`,
  `play_on_start`, `start_lifetime`, `start_speed`, `start_size`, `start_rotation` (degrees),
  `start_color`, `start_color_b` (each particle takes a random mix), `gravity`,
  `simulation_space` (world, local), `max_particles`, `seed` (0 derives one from the object
  name), `time_scale`.
- **Emission:** `rate`, `rate_over_distance`, `bursts` (a list of `{time, count, cycles,
  interval, probability}`; `cycles: 0` repeats forever), and `count` for scatter mode.
- **Shape:** `shape`, `radius`, `radius_thickness` (0 = shell only, 1 = whole volume), `angle`,
  `arc`, `box`, `length`, `shape_offset`, `random_direction`.
  - Mesh: `mesh_path`, `emit_from` (faces, vertices, edges), `distribution` (random, even),
    `normal_offset`.
  - Orientation: `align_to_normal`, `random_spin`, `inherit_velocity`.
- **Motion:** `velocity`, `force` (world acceleration: wind, buoyancy), `drag`, `orbital`
  (rad/s about the emitter's +Z), `radial`, `tumble` (deg/s about a random axis),
  `angular_velocity`, `noise_strength`, `noise_frequency`, `noise_scroll`, `noise_octaves`.
- **Over life:** `color_over_life`, `size_over_life`, `alpha_over_life`.
- **Collision:** `collide` (a world-Z ground plane), `ground_height`, `bounce`,
  `collision_friction`, `kill_on_collide`. From code, `ParticleSystem::ground_field` (a
  `GroundField` height map) replaces the plane; the weather's rain and snow land on roofs and
  terrain this way. A particle born below the field never spawns.
- **Wrap:** `wrap_box` (full extents of a box centred on the emitter; world space) and `wrap_fade`.
  A particle that leaves the box comes back in on the opposite side, so a volume that follows the
  camera is always full -- precipitation that keeps up however fast the camera moves or turns.
  Alpha fades toward the box's sides (`wrap_fade` of its half width) so a wrap is never seen.
- **Sub emitters:** `on_death` (a list of `{target: <object name>, count, inherit_velocity}`).
  `count` is rounded per death, so `[0, 1.2]` fires on about 60% of them.
  `on_death_collision_only` fires them only for deaths by collision (splashes where rain lands,
  none when a drop simply ages out in mid-air).
- **Renderer:**
  - Mode and look: `render_mode`, `sprite`, `blend` / `additive`, `lit`, `toon_bands`, `emissive`,
    `softness` (0 = crisp cel edge, 1 = feathered), `distortion`, `opacity`.
  - Light (`lit` > 0): sky ambient, the sun, and every point and spot light, each with a
    half-Lambert wrap. `receive_shadows` (default on): one hard tap of the sun's cascade atlas,
    and the local-light atlas for shadowed lamps. `scatter` / `scatter_anisotropy`:
    Henyey-Greenstein forward scattering of every light toward the eye, so rain glints around a
    street lamp or against a low sun, and backlit smoke glows.
  - TAA: `reactive` (0..1). A particle has no motion vector, so TAA's history smears a fast thin
    one into a dashed streak. Reactive batches draw their coverage again into an R8 mask, and
    the resolve trusts the current frame there and restarts the pixel's accumulation. Use 1 for
    rain, snow and sparks. The rest of the image keeps full TAA.
  - Fades: `soft_distance` (fade where the particle meets geometry), `camera_fade`.
  - Shape of the quad: `aspect` (width / height), `pivot`, `stretch_speed`, `stretch_length`.
  - Texture: `texture`, `flipbook: {x: cols, y: rows}`, `flipbook_mode` (lifetime, random, fps),
    `flipbook_fps`, `flipbook_cycles`.
  - Draw control: `sort` (distance, none, oldest, youngest), `max_draw_distance`.
  - Mesh mode: `render_mesh`, `material`.

An unknown enum value is a scene-load error, not a silent default.

### Spawning on a mesh (Blender's emitter)

```yaml
- type: ParticleSystem
  shape: mesh
  mesh_path: mossy_mound   # omit to use this object's own MeshRenderer mesh
  emit_from: faces         # faces (by area) | vertices | edges (by length)
  distribution: even       # random | even (stratified: no clumps, no bald patches)
  start_speed: [0.1, 0.3]  # along each face's normal
  align_to_normal: true    # orient to the surface (aligned sprites, instanced meshes)
  random_spin: true        # random twist about the normal
```

- **Faces.** A triangle is chosen with probability proportional to its area, then a uniform point
  inside it, so density is even however the mesh is tessellated. The normal is the interpolated
  vertex normal. The tangent comes from the mesh's UVs, so an aligned particle has a consistent
  twist across a face unless `random_spin` is on.
- **Vertices** are the mesh's distinct positions, welded. **Edges** are chosen by length.
- **`even`** stratifies the area CDF (the i-th of N takes `(i + jitter) / N`). This is Blender's
  "Jittered", and is what you want for a scatter.

### Scatter (Blender's hair, rendered as instances)

`mode: scatter` places `count` particles over the shape once. They never age or move, and they
simulate in the object's local space, so they follow it.
- **Mesh instances.** `render_mode: mesh` with a `render_mesh` and `material` draws them as an
  instanced mesh through the opaque G-buffer. They cast and receive shadows and batch with any
  other renderer that uses the same mesh and material. Mesh mode is opaque or cutout only.
- **Surface cards.** `render_mode: aligned` lays sprites in the surface plane: fallen leaves,
  decals, moss.
- **Variety.** Over-life curves are sampled at a per-instance random position instead of age. A
  `color_over_life` gradient becomes a palette that each instance picks from, and
  `size_over_life` becomes a size distribution.

```yaml
- type: ParticleSystem
  mode: scatter
  shape: mesh
  mesh_path: mossy_mound
  distribution: even
  count: 70
  align_to_normal: true
  start_size: [0.12, 0.32]
  render_mode: mesh
  render_mesh: mushroom
  material: { albedo: { r: 0.82, g: 0.36, b: 0.24 }, roughness: 0.6 }
```

### Playback from code

```cpp
auto* ps = obj->get_component<toy::particles::ParticleSystem>();
ps->play();             // also restart(), pause(true), stop(), stop(/*clear=*/true)
ps->emit(30);           // a burst right now
ps->emit_at(pos, vel);  // one particle at a world position (what sub emitters use)
ps->settings.rate = 0;  // settings are live; call apply_settings() after changing noise
```

## How it fits together

```
ParticleSimulationSystem::execute()         (order 360: after TransformResolve, in edit mode too)
├─ serial: init new systems, resolve sub-emitter targets, read world matrices
├─ big systems (>= 2048 live): step() on this thread, the per-particle update split over workers
├─ small systems: one job each, in parallel
│     step(): update (parallel) -> deaths (serial swap-remove) -> births (serial, seeded RNG)
│             -> bounds (parallel reduce)
└─ serial: sub-emitter deaths -> target->emit_at() (spawned next frame)

Engine::sync_particle_render_state_()       (after late_update, before render)
└─ collect_render(): frustum-cull each system's bounds, prepare_render() the visible ones
     (sort back to front, build ParticleInstance / mesh matrices; parallel like the step)
     -> PixelRenderPipeline::set_particle_state()

PixelRenderPipeline::render()
├─ gather_meshes_(): each mesh-mode batch joins the opaque G-buffer and shadow batching
│                    (one item, N instances)
├─ ParticlePass::upload(): every quad batch into this frame slot's instance buffer
└─ record_transparent_(): quad batches join the back-to-front list with BLEND meshes and SDFs
                          (one bracket), one instanced draw (6 vertices x N) per batch
```

- **SoA pool.** Position, velocity, age, life, size, rotation, spin, colour, orientation and seed
  each live in their own array. Deaths swap-remove.
- **Births.** Particles born in one frame are spread along the emitter's path over that frame, and
  each is pre-aged by how far back in the frame it was born. A fast emitter draws a continuous
  ribbon rather than per-frame clumps.
- **Turbulence** is the analytic curl of six plane-wave potentials. It is divergence-free, so
  smoke swirls instead of collecting in sinks. It is normalized to unit RMS, so `noise_strength`
  is a typical acceleration.
- **One pipeline, every blend.** The quads use premultiplied blending, and the shader scales
  alpha by `1 - additive`. Fire (mostly additive) and smoke (alpha) share one pipeline and
  depth-sort together.
- **Soft particles** compare against Hi-Z mip 0, which is a copy of the opaque depth. The depth
  attachment can't be sampled because it is bound to this same render pass.

**Determinism.** Spawning reads the system's own seeded RNG, serially. The update uses no RNG:
anything random after birth hashes the particle's seed. Every parallel range writes only its own
indices. Results are bit-identical on any worker count; `particles_parallel_matches_serial` checks
positions, velocities, ages and the sorted render instances.

## Performance

Measured headless at 1920x1080 on an 8-worker Apple Silicon machine (`PROFILE=1`, mean of the
steady-state frames):

| Scene | Simulation (`scene_update`, with everything else) | Render prep | GPU draw (`transparent`) |
|---|---|---|---|
| `particles_test` (16 systems, ~1k live particles) | 0.33 ms | 0.14 ms | 0.46 ms |
| One 20k-particle system, 1 worker | 1.75 ms | 0.59 ms | 0.34 ms |
| One 20k-particle system, 8 workers | 0.82 ms | 0.60 ms | 0.35 ms |

The render prep column includes back-to-front sorting, which is a linear-time radix sort. A
`std::sort` through the key indirection cost about 1.6 ms at 20k particles.

- **CPU, simulation.** Per-particle work is a handful of vec3 operations, and six sin/cos pairs
  when noise is on. A system with 2,048 or more live particles splits its own update across
  all workers; smaller systems run one per job, side by side.
- **CPU, render prep.** Only for systems whose bounds are on screen. Sorting is a radix sort on
  one key per particle. Use `sort: none` for additive effects, which don't need it.
- **GPU.** One draw per system. Cost is fill-rate: big, overlapping smoke quads are the expensive
  part, as in any engine.
  - Unlit sprites (`lit: 0`) skip the lighting loop entirely.
  - Lit ones loop over the sun, up to 16 point lights and 8 spot lights per pixel. Shadows add one
    sun tap, plus a PCF lookup per shadowed lamp in range.
  - A `reactive` batch draws a second time into the TAA mask. That pass is coverage only, with no
    lighting, and runs only with `aa_mode: taa`.
- **Culling.** Systems outside the camera frustum, or past `max_draw_distance`, are not prepared or
  drawn. They still simulate, so they are in the right state when they come back into view.

## GPU simulation (`simulation: gpu`)

For 100k+ particle effects (spark showers, dust fields), a system can opt in with
`simulation: gpu`. The default `cpu` path is unchanged. Emission timing (rate, distance, bursts)
stays on the CPU; birth, simulation, compaction and sorting run in compute shaders
(`assets/shaders/particles_*.comp`, recorded by
[gpu_particle_pass.h](../render/passes/gpu_particle_pass.h) before the shadow pass).

- **Same look.** The compute passes port `spawn_()`, `update_()` and `prepare_render()`: every
  analytic shape and mesh faces (an area-CDF triangle buffer), gravity and force, drag, the same
  curl-noise field, orbital and radial motion, the ground plane (bounce or kill), the wrap box,
  and colour, size and alpha over life (64-sample LUTs baked from the curves). Quads are drawn by
  the normal particle pipeline from the GPU buffer through `draw_indirect`, so soft fade, HDR glow,
  lighting, shadows, the atlas and the TAA reactive mask all work unchanged.
- **Buffers.** `max_particles` sets a fixed pool per system, which uses 164 bytes per particle
  (about 250 when sorted). Dead slots go back to a free list. Live particles are appended to an alive list, and
  its atomic counter is the indirect draw's instance count.
- **Sorting.** Blended systems are bitonic-sorted on the GPU (by distance, or oldest/youngest
  first). Additive systems (`additive: 1`) and `sort: none` systems skip the sort.
- **Fallback.** A system that asks for `gpu` but uses something the GPU path lacks falls back to
  the CPU and logs one warning. That covers sub emitters (either end), scatter mode, mesh render
  mode, `prewarm`, a runtime `ground_field`, and mesh emission from vertices or edges.
  `particles.gpu_enabled: false` in `config.yaml`, or a device without compute, runs every
  system on the CPU.
- **What the CPU sees.** `particle_count()` is the alive count read back from the GPU, a couple of
  frames late. Culling bounds are a conservative estimate made from the settings: the fastest
  start speed, acceleration and noise over one lifetime, around the shape and the emitter's
  recent path.
- **Determinism.** Births use per-particle hashes, so the set of particles is the same every run
  under `FIXED_DT`. The alive-list order comes from atomics, though, so unsorted additive systems
  can differ by blend rounding.

`particles_gpu_stress` (three 60k-spark fountains and a 40k sorted dust field) compares the two
paths with `PROFILE=1` (flip `particles.gpu_enabled`). On an 8-worker Apple Silicon machine at
1920x1080, ~150k live particles:

| Path | CPU `scene_update` | CPU `dynamic_meshes` (render prep) | GPU `particles.sim` | GPU `transparent` |
|---|---|---|---|---|
| CPU | 12.8 ms | 5.0 ms | n/a | 1.67 ms |
| GPU | 0.15 ms | 0.04 ms | 1.33 ms | 1.76 ms |

## Things worth knowing

1. **Quads need `transparency_enabled`.** They draw in the forward transparent pass. It is on in
   `assets/config.yaml`. With it off, the engine warns once and skips quads; mesh-mode particles
   still draw.
2. **Emitter meshes are read straight from the file.** They do not go through the AssetManager,
   which caches one asset type per path, and the same mesh is usually also a MeshRenderer's GPU
   mesh. Systems that use the same mesh share one `MeshSurface`.
3. **Ground collision is a plane at `ground_height` (or a `ground_field` height map), for
   world-space systems only.** Particles never raycast themselves; a height map probed once per
   cell (as the weather does, toyengine/weather/ground_probe.h) is what lets many land on roofs.
4. **The editor previews live.** The system runs in edit mode. Selecting an object with a
   ParticleSystem draws its emission shape and particle bounds as gizmos. **Add > Particle
   System** creates one.
5. **Generated meshes need real tangents.** A mesh whose tangent is parallel to its normal (a
   constant +X tangent on an X-facing cap) turns into NaN in the G-buffer's TBN, and bloom and TAA
   smear that into white and black blocks. `tools/mesh_yaml.py`'s `perpendicular_tangents()`
   avoids it.

## Not here yet

- Trails and ribbons, and sub emitters on birth or collision (only on death).
- Collision with physics colliders, terrain and water surfaces.
- GPU depth-buffer collision, and the GPU features listed under "GPU simulation" as falling back.
- Particles casting shadows or writing motion vectors. TAA reprojects them by camera motion only;
  `reactive` is the remedy for fast ones.
- Particle-specific quality tiers in `config.yaml`.
