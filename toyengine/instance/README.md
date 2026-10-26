# toyengine/instance

**Painted instances**: a bag of placed copies of an object asset, drawn as instanced meshes and
collided on demand. The hand-authored counterpart to the procedural foliage scatter
(`toyengine/particles/terrain_scatter.h`).

A scatter is *tuned* -- density, slope limits, a seed -- and a painter is *placed*: the editor's
Instance Paint mode brushes instances onto whatever surface is under the cursor, and the painter
stores exactly what was put down. Use a scatter for grass over a whole landscape; use a painter
for the trees, rocks, fences and clutter that make somewhere a place.

Demo: `./build/toyengine instance_paint_demo`
([assets/scenes/terrain/instance_paint_demo](../../assets/scenes/terrain/instance_paint_demo/scene.yaml),
written by [`tools/gen_instance_paint_demo.py`](../../tools/gen_instance_paint_demo.py)).

| File | Purpose |
|---|---|
| [`instance_set.h`](instance_set.h) | `InstanceRecord` (48 bytes), `InstanceSetData` (a sparse grid of chunks), `InstanceSetStore`, and the `.instances` file. Pure CPU; unit-tested directly. |
| [`instance_painter.h`](instance_painter.h) | `InstancePrototype`, the `InstancePainter` component, and `resolve_prototype()`. |
| [`instance_cache.h`](instance_cache.h) | `InstanceRenderCache` -- a chunk's records turned into world matrices, grouped by the (prototype, mesh part) they draw as. |
| [`instance_painter_system.h`](instance_painter_system.h) | `InstancePainterSystem` (order **62**): builds the chunks, hands the renderer their batches, streams the colliders. |
| `instance_yaml.cpp` | The `"InstancePainter"` scene parser. |

The editor side is [`editor/instance/`](../../editor/instance/README.md) (the brush and the
selection, as pure functions) and
[`editor/app/ui/instance_paint.h`](../../editor/app/ui/instance_paint.h) (the mode: Paint, Erase,
Place, Cluster and Select).

## Authoring

```yaml
- type: InstancePainter
  data: instances/forest.instances   # default instances/<object>.instances, beside the scene
  chunk_size: 32                     # metres per chunk: the draw, cull, collide and file unit
  collision_radius: 64               # metres from the camera colliders exist within; 0 = none
  cast_shadows: true
  prototypes:
    - object: objects/props/pine     # an object asset -- its meshes and bounds are read from it
      collision: capsule             # none | auto | box | sphere | capsule | mesh
      collision_radius: 0.45
      scale_range: [0.85, 1.3]
      yaw_range: [0, 360]            # degrees
      align_to_normal: 0.3           # 0 world up .. 1 fully along the surface normal
      sink: 0.05                     # metres pushed into the surface
      draw_distance: 0               # 0 = automatic, from the prototype's height
      fade_start: 0.6                # thinning begins at this fraction of the draw distance
    - mesh: rock_small               # ...or a bare mesh plus a material
      material: { albedo: { r: 0.5, g: 0.5, b: 0.52 }, roughness: 0.9 }
```

An `object:` reference is spelled the way `prefab:` is -- `objects/props/pine`, asset folder
included. Every other key is per prototype, because variation is a property of the *thing*, not
of the brush that placed it: a painter's pines all vary the same way whoever paints them.

## Why records and not objects

An instance is a 48-byte `InstanceRecord`, not a `SceneObject`. A world covered in them costs no
transforms, no component walk and no growth in the scene file -- which is the difference between
a few hundred props and a few hundred thousand. What it gives up is per-instance components: an
instance that needs to be scripted is promoted to a real `prefab:` object by the editor's
**Convert to Object**.

The records live in an `InstanceSetData` shared through the `InstanceSetStore`, keyed by the
`.instances` file -- **not** in the component. That is load bearing: the editor re-parses a
component from YAML on every inspector keystroke, so anything held on the component would be
lost on each one. `world::TileMapStore` exists for the same reason and is the model this follows.

## How it fits together

```
InstancePainterSystem::execute()              (order 62: after terrain 60/61, before Physics 100)
├─ resolve_prototype() once per prototype     (SceneLoader::instantiate -> walk for MeshRenderers)
├─ InstanceRenderCache::update()              (drop what moved or went out of range; build <= 2)
│     build_chunk(): records -> world matrices per (prototype, part), shuffled, with bounds
└─ update_collision_()                        (one runtime child per collidable instance in range)

Engine::sync_particle_render_state_()          (after late_update, before render)
└─ collect_render(): frustum-cull each chunk, thin by distance, append MeshInstanceBatches

ToyRenderPipeline::gather_meshes_()
└─ each batch is one item with its own persistent GPU instance buffer (static_key), its own
   culling bounds and its own LOD -- the same path the terrain scatter takes
```

- **Chunks** are the unit of three things at once: the instanced draw (one batch per chunk, so it
  frustum-culls alone), collision streaming, and the file's compression and dirty tracking. They
  are *sparse*, because a painter covers a world rather than a bounded grid.
- **Shuffled storage in the cache.** A chunk's matrices are shuffled deterministically on build,
  so drawing the first `n` of them is an even subset. That is what makes distance thinning a
  prefix draw rather than a rebuild.
- **Revisions.** Every edit bumps the set's revision and its chunk's. Each consumer (the render
  cache, the collider streamer, the editor's hover info) keeps its own seen revision and rebuilds
  only what moved; consumers never drain each other's changes.
- **Determinism.** A record stores its own seed, so a rebuild reproduces the same instance. The
  brush hashes every random number from `(seed, cell)`, so a dab is reproducible whatever order
  its candidates are consumed in.

## Collision

Per prototype, opt-in, and **streamed**: only instances within the painter's `collision_radius` of
the camera have a collider at all, one runtime child object each (`toy::scene::RuntimeObject`,
so the editor lists them under "Runtime" and never saves them). `collision_radius: 0` turns the
whole thing off whatever the prototypes ask for.

Shapes are **derived from the prototype's bounds**, not cloned from its own collider components:

| | |
|---|---|
| `none` | the default. Grass, flowers, pebbles: no physics cost at all |
| `auto` | a capsule when the prototype is taller than wide, else a box |
| `box` / `sphere` / `capsule` | fitted to the bounds, or to `collision_size` / `collision_radius` |
| `mesh` | a triangle collider from `collision_mesh`, or the render mesh when that is empty |

**`auto` fits the WHOLE prototype**, silhouette included. Right for a crate, a rock, a barrel.
Wrong for a tree: a pine fitted that way gets a trunk as wide as its canopy, and you bump into
the branches a couple of metres before you reach the wood. Author `collision_radius` there --
the demo's pine does.

## Things worth knowing

1. **A prototype resolves once, lazily, and asynchronously.** `resolve_prototype()` instantiates
   an object asset through the ordinary prefab path, so nested prefabs, material slots and LODs
   all come along -- but its meshes load in the background. `InstancePrototype::ready()` is the
   separate question of whether a batch can draw yet, and bounds are only known once it is true.
2. **An object asset carrying behaviour components warns and draws anyway.** An `Animator`, a
   `ParticleSystem`, a `CharacterController` cannot run on a record, because there is no object
   for them to run on. Place those with `prefab:` instances, or paint and then Convert to Object.
3. **The editor previews live.** The system `runs_in_edit_mode()`, which is what makes the brush
   usable at all: instances appear, cull, light and collide as they are painted.
4. **Records are saved with the scene, not in it.** Ctrl S writes every dirty `.instances` file,
   exactly as it writes the terrain's height and splat PNGs.
5. **A static batch that did not change reports that it did not move.** Otherwise a painted world
   would hold the renderer's temporal freeze open on every frame it is on screen -- see the
   `moved[]` assignment in `render/pipeline_gather.cpp`.
6. **Nothing about a painter survives the update -> render gap.** The editor applies a component
   edit in its `post_late_update` hook, which runs **between** `execute()` (order 62) and the
   renderer's hand-off. That rebuild destroys the `InstancePainter`, so `collect_render()` takes
   the `Scene` and looks the live components up again rather than reusing anything remembered.
   Caching a painter pointer across that gap segfaults on the first frame of any inspector drag
   (`tests/editor/gpu/instance_paint_test.cpp` reproduces it).
7. **Identity is the instance set's uid, not the component.** The editor destroys and recreates
   the component on every keystroke, routinely at the same address. Keying the derived state on
   the set -- which outlives every rebuild, because it lives in the store -- is what keeps a
   drag from handing the new painter the old one's cache, and keeps the renderer's persistent
   buffers from being retired and re-uploaded once per keystroke.
8. **A rebuild re-requests the prototype's mesh asynchronously**, so it is briefly not `ready()`.
   The chunk cache deliberately does NOT clear on that: it clears when a prototype *becomes*
   ready (it was skipped while loading), never when one stops being ready. Clearing both ways
   empties the cache on every frame of a slider drag, and the painted world blinks out while you
   adjust it.

## Not here yet

- Surface filters on the brush: slope, world height, terrain splat layer. The density model has a
  minimum spacing, but a stroke across a cliff face will plant on it.
- Per-instance tint or material variation (the `seed` is stored for it, nothing reads it yet).
- Painting onto a *moving* surface; records are world-space and do not follow anything.
- A chunk-wide welded trimesh collider, which would beat one body per instance for dense
  collidable sets (`world::build_chunk_collision` is the precedent if it is ever needed).
