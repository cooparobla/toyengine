# toyengine/procgraph

A **node graph → scene graph converter**. A graph reads values and objects from the surrounding
scene, is evaluated over the job system, and bakes into a real object subtree with components.

Geometry nodes are the first and largest thing you can build with it — a Blender-style set ships
here — but they are a *category of nodes*, not the identity of the system. A graph that emits no
geometry at all (lights along a curve, props at authored markers, audio emitters at spawn points)
is an equally valid use and needs no new machinery.

The graph machinery itself is generic and lives in libcoopa:
[`coopa/nodegraph/`](../../libs/libcoopa/coopa/nodegraph/README.md). This directory is the engine's
*domain* — its socket types, its node library, and the two halves that turn a graph into something
on screen.

Demo: `./build/toyengine procgraph_demo`
([scene](../../assets/scenes/procedural/procgraph_demo/scene.yaml),
written by `tools/gen_procgraph_demo.py`).

| File | Purpose |
|---|---|
| [`proc_types.h`](./proc_types.h) / `.cpp` | This domain's socket types — `Float`, `Vector`, `Geometry`, `Object`, `ObjectSet`, `Component`, `ObjectSpec` — their YAML hooks, their hashes and the implicit conversions between them. |
| [`geometry_set.h`](./geometry_set.h) / `.cpp` | **`GeometrySet`**: a refcounted, copy-on-write bundle of mesh / points / instances. 16 bytes, so a socket copy is a refcount bump. |
| [`scene_input.h`](./scene_input.h) / `.cpp` | **The binding layer**: `InputBinding`, the extensible `register_input_resolver()` registry, the variable table, and `resolve_inputs()` — the snapshot seam. |
| [`bake_context.h`](./bake_context.h) | What a node can learn about its bake: the seed, and a snapshot of the owning object. |
| [`bake_result.h`](./bake_result.h) | **`BakeResult`**: the object tree a graph produces. Pure data. |
| [`proc_bake.h`](./proc_bake.h) / `.cpp` | The pure bake: compiled graph + resolved inputs → `BakeResult`. No Vulkan, no `Scene`, no `AssetManager`. |
| [`proc_publish.h`](./proc_publish.h) / `.cpp` | The only engine-touching half: `BakeResult` → real objects, uploaded meshes, colliders, instance sets. Main thread. |
| [`nodes.h`](./nodes.h), `nodes_*.cpp` | The node library, one translation unit per category, all behind `register_procgraph_nodes()`. |
| [`procedural_graph.h`](./procedural_graph.h) / `.cpp` | The `ProceduralGraph` component and its YAML parser. |
| [`procedural_graph_system.h`](./procedural_graph_system.h) / `.cpp` | Order **63**: resolve → bake → publish, plus `rebake()` and the variable table. |

## Authoring

```yaml
- name: Railing
  components:
    ProceduralGraph:
      graph: graphs/railing            # assets/graphs/railing.yaml
      seed: 7
      bake: on_change                  # on_load | on_change | manual
      refresh_on_asset_change: true    # re-bake when the graph file changes on disk
      inputs:
        posts: 8                       # a literal
        start:   { object: RailStart }         # a scene binding
        markers: { children_of: Spawns }
        season:  { var: world.season }
```

**Four keys and no more.** There is deliberately no `collider:`, no `output:` and no `emit:`.
What a graph produces is decided by the nodes in it, because a graph may be doing something with
nothing to do with meshes. A collider is what a `Make Collider` node emits; so is a light, a
rigidbody, or a game project's own component.

| Author | For |
|---|---|
| `graph` | Which graph asset to bake. |
| `seed` | Seeds every random node, so a bake is repeatable. Change it for a different result. |
| `bake` | `on_load` (once, during warm-up), `on_change` (whenever a bound input moves), `manual`. |
| `inputs` | A literal or a binding per graph interface input. Validated against the graph, so its parameters become inspector parameters with no schema edit per graph. |

## The two decisions that shape everything

### 1. The output is a scene subtree, not a mesh

`BakeResult` is a tree of objects, each with components expressed as **a registered type name plus
the keys a scene file would write**. Publishing is one call to
`coopa::scene::SceneLoader::build_object()`, which dispatches every component through the
process-wide `register_component_parser` table.

So a graph can emit a `Light`, a `Rigidbody`, an `AudioSource`, a `ParticleSystem` or a game
project's own `TOY_MODULE()` component **with no new plumbing per component type** — nothing in
this module knows what a `Light` is. `AddComponent` is that generality in one node;
`MakeCollider` and `MakeInstancer` are convenience wrappers over it.

The payloads YAML cannot carry — a built mesh, a collision shape, instance transforms — ride
alongside the tree as pointers and are attached in a second pass, keyed by the object's path.

### 2. The graph reads the scene, and the bake never touches it

The bake runs on a worker thread; `Scene` is single-owner and main-thread-only (component parsers
are not thread-safe, and the editor destroys and recreates components between `execute()` and the
render hand-off). A node reaching into `Scene` would be a data race surfacing as a rare crash
somewhere else entirely.

**So resolution and evaluation are separated by a snapshot.** `resolve_inputs()` runs on the main
thread and turns every binding into an immutable `Value`; the bake then reads nothing else. No
node type ever sees a `Scene*` or a `SceneObject*` — `ObjectRef` and `ObjectSet` carry a name, a
transform, bounds and optionally geometry, and **no pointers**. The absence of a pointer is what
makes the contract enforceable rather than a convention.

The snapshot is also what makes `bake: on_change` affordable: re-resolving is a handful of
transform reads, hashed, and the bake only re-runs when the hash moves.

### Binding kinds, and adding your own

Each kind is a registered function. A game project adds one with a single
`register_input_resolver()` call and no edit here.

| Key | Yields | Notes |
|---|---|---|
| `object` | `Object` | Its transform and world bounds. |
| `children_of` | `ObjectSet` | An object's direct children. |
| `name_like` | `ObjectSet` | Glob over object names (`"Rock_*"`), sorted by name. |
| `component` + `key` | a scalar / vector | One property off a component, via `coopa::anim::AnimatedPropertyRegistry`. |
| `var` | whatever it holds | The escape hatch. |

There is **no `tag` resolver**: this engine's `SceneObject` has no tag concept, so a tag binding
would need one invented first. `name_like` covers the same authoring need.

**Variables** are the general escape hatch for anything a scene query cannot express, fed from two
directions — `scene.settings.procgraph.variables` for authored constants, and
`ProceduralGraphSystem::set_variable()` for anything live. A graph reads one with
`{ var: world.season }`. That is how a gameplay value drives a graph without the component knowing
anything about gameplay.

## What it reads, and what it publishes

Order **63**, the first free slot after the terrain systems (60, 61) and the instance painter (62).
After terrain so a graph could sample ground height; before Physics (100) so a collider it emits is
stepped the same frame it appears.

- **Reads**: the scene (resolution only, main thread), the variable table, graph assets under the
  project's asset roots.
- **Publishes**: `SceneObject`s under the component's owner, all marked
  [`RuntimeObject`](../scene/runtime_object.h) so the editor groups them under "Runtime" and never
  writes them into the scene file; runtime meshes under `runtime/procgraph/<key>/...` synthetic
  asset ids; non-persistent `InstanceSetStore` entries that an emitted `InstancePainter` points at.
- **Renders nothing itself.** Instance output becomes an `InstancePainter` component, and the
  existing `InstancePainterSystem` does the instanced drawing, frustum culling, distance thinning
  and streamed collision.

`bake: on_load` graphs bake during **warm-up**, so the loading screen holds until they are done
(`warm_up_state()`, as `TerrainSystem` and `WaterSystem` use). That is also why being ordered after
the instance painter costs nothing: a load-time bake is complete before the first steady-state
frame. A runtime re-bake lands its instances on the next frame, which is invisible.

**Derived state does not live on the component.** The editor recreates a component on every
inspector keystroke, so everything the system remembers is keyed by `ProceduralGraph::state_key()`
— the owner's name plus the graph path — rather than by a component pointer.

## Limits

- **Only four vertex channels survive a bake.** `coopa::gfx::engine::data::Vertex` is
  `{position, normal, uv, tangent}` and is the renderer's only vertex format; there is no second
  stream and vertex colours are already ignored. An attribute can drive a graph and then it is
  gone. Changing that means a new vertex binding on all five mesh backbones.
- **No per-element fields yet.** Blender's fields (a value that is a function of per-element
  context) are the generic library's stage-3 work. `Set Position` therefore takes one uniform
  offset rather than a per-point field with a selection. When fields land, these nodes gain the
  extra sockets without changing shape.
- **`Switch` evaluates both branches.** The evaluator is eager and the library's lazy-input
  machinery is declared but not yet deferring. Wasted work, not wrong work.
- **A graph reads the scene; it never writes to it.** The only thing it produces is its own baked
  subtree. It cannot move, delete or edit an authored object — that would make the scene file and
  the bake fight over who owns an object's state, with no sane undo story.
- **`convex` colliders are trimeshes.** There is no convex-hull builder in the tree, so a convex
  request gets the full trimesh: heavier, but correct, rather than silently producing nothing.
- **`geometry: true` on an object binding yields nothing but a warning.** An uploaded mesh keeps no
  CPU-side vertex copy (the staging buffer is released after the upload), so there is nothing
  honest to hand back. Use a primitive node instead.
- **No cross-graph dependencies.** One graph cannot take another's output as an input; that would
  need a dependency order between components and re-bake cascades. A node group shares logic.
- **Inputs may be driven, but not animated.** Each change is a full re-bake, so a graph bound to a
  continuously-varying value re-bakes continuously. Per-frame varying geometry is what particles,
  water and cloth are for.
