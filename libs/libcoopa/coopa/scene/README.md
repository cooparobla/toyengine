# Scene Module (`coopa::scene`)

The `scene` module provides the scene graph hierarchy, component-based entity
model, scene lifecycle management, YAML scene loading, and an ordered
per-frame system pipeline (see "Update Phases" below). It is
**dependency-free with respect to other repos**: nothing in this module
includes gfxcoopa, caml, uicoopa, or any other sibling repo — only libcoopa's
own vendored `fkYAML` and `glm`, plus libcoopa's own `coopa/event` (the scene
event bus) and `coopa/job` (`Scene::set_job_engine()`; see below). Every renderer-specific or UI-specific component (mesh
renderers, cameras, lights, GI/reflection probes, RectTransform, ...) lives
outside libcoopa and is parsed via `SceneLoader::register_component_parser()`,
never by this module directly. Registrants include gfxcoopa's
`engine::components::register_render_components()`, uicoopa's
`register_ui_components()`, physxcoopa's `register_physics_components()` and
sfxcoopa's `register_sfx_components()`.

## Update Phases

`Scene::update(dt)` and `Scene::late_update(dt)` run an ordered list of
`ISceneSystem` instances (`coopa/scene/scene_system.h`) rather than walking the
`Component` tree themselves. With nothing registered beyond the two built-ins
(`BehaviourSystem` at `UpdatePhase::Behaviour`, `LateBehaviourSystem` at
`UpdatePhase::LateBehaviour`, auto-installed by every `Scene`), the two entry
points are equivalent to a direct recursive walk of the `Component` tree.

```text
UpdatePhase::Physics          = 100   // reserved; no implementation ships in libcoopa
UpdatePhase::Behaviour        = 200   // built-in: the recursive Component::update() walk
UpdatePhase::Routine          = 250   // coopa::routine::RoutineSystem (coopa/routine)
UpdatePhase::Animation        = 300   // coopa::anim::AnimationSystem (coopa/animation)
UpdatePhase::TransformResolve = 350   // coopa::scene::TransformSystem (systems/transform_system.h)
UpdatePhase::LateBehaviour    = 400   // built-in: the recursive Component::late_update() walk
```

`Scene::update(dt)` runs every system with `order < LateBehaviour`;
`Scene::late_update(dt)` runs every system with `order >= LateBehaviour`. This
split — not a split at `Behaviour` — is deliberate: it is the only cut
consistent with every existing caller, including consumers that call only
`update()` and never `late_update()` (so `Animation` still runs for them) and
a contract test elsewhere in the tree asserting `update()` does not trigger
`late_update()` work. Only the two Behaviour walks are installed by default;
Routine, Animation and TransformResolve systems are added with `add_system()`.
Any integer order works, so a consumer can insert a system between built-ins
(e.g. IK at 320) without renumbering anything.

`Scene::set_job_engine(JobEngine*)` installs a non-owning `JobEngine` that
every system sees as `FrameContext::jobs`. `Scene` never calls the engine's
`begin_frame()`/`end_frame()`: job handles are reclaimed individually, so
those calls only reset diagnostic counters, and the same engine can be shared
by several scenes, `coopa::asset::AssetManager` and anything else.
`SceneManager::begin_frame()`/`end_frame()` forward to the engine for apps
that want the diagnostics. With no engine installed (the default), every
system runs inline and no threads are spawned.

---

## Class Hierarchy & Ownership Model

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                              SCENE GRAPH CLASS HIERARCHY                                 │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                                ┌───────────────────────┐
                                │     SceneManager      │
                                ├───────────────────────┤
                                │ unique_ptr<Scene>     │
                                └───────────┬───────────┘
                                            │ owns active_scene_
                                            ▼
                                ┌───────────────────────┐
                                │        Scene          │
                                ├───────────────────────┤
                                │ vector<SceneObject*>  │
                                └───────────┬───────────┘
                                            │ owns root_objects_
                                            ▼
                                ┌───────────────────────┐
                                │      SceneObject      │ ◄───┐
                                ├───────────────────────┤     │ owns
                                │ vector<Component*>    │ ────┘ children_
                                │ vector<SceneObject*>  │
                                └───────────┬───────────┘
                                            │ owns components_
                                            ▼
                                ┌───────────────────────┐
                                │       Component       │
                                ├───────────────────────┤
                                │ Transform              │  (defined here)
                                │ MeshRenderer, Camera,  │  (defined by gfxcoopa)
                                │ RectTransform, Canvas  │  (defined by uicoopa)
                                │ Animator               │  (defined by coopa::anim)
                                └───────────────────────┘
```

---

## Scene Loading Sequence

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                                 SCENE LOADING LIFECYCLE                                  │
└──────────────────────────────────────────────────────────────────────────────────────────┘

  Application       SceneManager      SceneLoader      SceneInheritance      fkYAML
      │                   │                │                   │               │
      │── register_component_parser(...) for every non-Transform component (incl. Animator)
      │                   │                │                   │               │
      │── load_scene(path) ──►│            │                   │               │
      │                   │── load(path) ─►│                   │               │
      │                   │                │── resolve(path) ─►│               │
      │                   │                │                   │── deserialize()/inherit_from ──►│
      │                   │                │                   │◄─ fkyaml::node ──────────────────│
      │                   │                │                   │── expand every inherit_from
      │                   │                │                   │   (object-level, then scene-level)
      │                   │                │◄── merged node ───│
      │                   │                │
      │                   │                │── Build Scene Tree
      │                   │                │── Dispatch each component
      │                   │                │   node to its registered
      │                   │                │   parser by tag/type name
      │                   │◄── Scene Instance ─│
      │◄── Scene Ready ───│
```

---

## Structural Specifications & Ownership Matrix

| Class | Primary Owner | Contained Members | Responsibility |
|---|---|---|---|
| `SceneManager` | Main Engine Application | `std::unique_ptr<Scene>` | High-level orchestrator managing active scene swapping and lifecycle |
| `Scene` | `SceneManager` | `std::vector<std::unique_ptr<SceneObject>>` | Root scene node holding top-level objects; generic component queries |
| `SceneObject` | `Scene` or parent `SceneObject` | `std::vector<std::unique_ptr<Component>>`, `children` | Entity node holding component instances and recursive child hierarchy |
| `Component` | `SceneObject` | Non-owning raw `owner` pointer | Abstract base class for all component behaviors |
| `SceneLoader` | Static/stateless | Registry of external `ComponentParser`s | Parses `.yaml` scene descriptions, dispatching non-built-in components externally |
| `SceneInheritance` | Static/stateless | None (pure node transform) | Expands every `inherit_from` into one merged `fkyaml::node` before `SceneLoader` parses it |

---

## File Breakdown

### [`component.h`](component.h)

Abstract base class for all scene components:
- `owner`: Non-owning raw pointer to the host `SceneObject`.
- `virtual void start()`: Invoked once after scene instantiation.
- `virtual void update(float delta_time)`: Invoked every frame during scene update.
- `virtual std::string type_name() const = 0`: Returns human-readable component type name.

### [`scene.h`](scene.h)

Root container for a loaded scene. Owns all root-level `SceneObject` instances
via `std::unique_ptr`, and knows nothing about any specific component type —
callers use these generic templated queries instead:
- `get_components<T>()`: Flat vector of every active `T` in the hierarchy.
- `find_objects_with<T>()`: Flat vector of every active `SceneObject` carrying a `T`.
- `find_first_component<T>()`: The first active `T` found, pre-order.
- `find_object(name)`: Depth-first search for an object by name.

Renderer-facing code (blendy, gfxcoopa's GI system) builds these into a more
convenient shape via gfxcoopa's `engine::components::SceneView` adapter rather
than calling them directly.

### [`scene_object.h`](scene_object.h)

The core node in the scene tree (equivalent to Unity's `GameObject`):
- Holds object name, active state flag, non-owning parent pointer, list of owned components, and list of owned child objects.
- Component API: `add_component<T>(...)`, `get_component<T>()`, `attach_component(...)`.
- Convenient getter: `get_transform()` (the one component type this module defines).
- Hierarchy API: `add_child(...)`, `detach_child(...)`, `set_parent(...)`, `find_descendant(name)`, `for_each_recursive(fn)`.
- Life-cycle methods (`start()`, `update(dt)`) recurse through attached components and active children.

### [`scene_manager.h`](scene_manager.h)

High-level manager for scene lifecycle:
- Wraps scene loading via `SceneLoader`: `load_scene` (replaces every scene)
  and `load_scene_additive`; `add_scene`/`remove_scene` take already-built
  scenes. Default-constructible — carries no GPU handles of its own.
- Owns zero or more `Scene`s, one of which is "active" (`get_active_scene()`,
  and the target of `add_system()`); `set_scene_active()` pauses or resumes
  any of them.
- `update()`/`late_update()` run every active scene — one job per scene when a
  `JobEngine` is installed via `set_job_engine()` and more than one scene is
  active, serially otherwise.

### [`scene_system.h`](scene_system.h)

`UpdatePhase`, `FrameContext` (delta time, frame index, `jobs`, per-worker
`commands`), the `ISceneSystem` interface, and the built-in
`BehaviourSystem`/`LateBehaviourSystem` walks.

### [`scene_commands.h`](scene_commands.h)

`SceneCommandBuffer` — structural changes (add/remove objects and components)
recorded by a system's worker jobs and applied serially on the owning thread
by `Scene::flush_commands()`.

### [`systems/transform_system.h`](systems/transform_system.h)

`TransformSystem` — opt-in (`install_transform_system(scene)` or
`add_system()`), at `UpdatePhase::TransformResolve`: resolves dirty world
matrices top-down, in parallel across root subtrees when a `JobEngine` is
installed.

### [`config.h`](config.h)

`SceneConfig` — a default scene path for applications that want one.

### [`scene_loader.h`](scene_loader.h)

Parser for YAML scene files:
- Understands hierarchy plus the one component type this module defines:
  `!Transform` / `type: Transform`.
- Every other component name is dispatched to whatever parser was registered
  for it via `register_component_parser(name, fn)`; unregistered names are
  silently skipped. A leading `!` is stripped before matching, so a YAML tag
  and a `type:` key spelling reach the same registered parser.
- `set_document_loader(fn)` lets an application route parsing through
  something other than a plain file on disk — e.g. an encrypted/compressed
  container — without this module depending on whatever that requires.
- `ParseContext{scene_path, scene_dir, search_dirs, resolve()}` is passed to
  every parser so it can resolve asset paths against the file that actually
  declared them — see Scene Inheritance below.
- Before any of this runs, `SceneInheritance::resolve()` expands every
  `inherit_from` in the raw document into one merged node (see below).
- Async loading in pieces: `read_document(path)` is that document step on its
  own (pure fkYAML + file reads, worker-safe), `read_document_async(path, jobs)`
  runs it on a JobEngine job and returns a `DocumentRead` to poll, and
  `SceneLoader::Builder` builds the resolved document a few root objects per
  `step(budget_ms)` on the main thread. `LoadOptions{.start = false}` (also on
  `load(path, options)`) builds a scene without calling `start()`.

### [`scene_inherit.h`](scene_inherit.h)

Resolves `inherit_from` — a pure YAML-node merge pass with no knowledge of
`Scene`/`SceneObject`/`Component`, run once by `SceneLoader::load()` before
anything is parsed into live objects.

**Where it's allowed:**

| Level | Key location | Effect |
|---|---|---|
| Object | any object node (`root_objects` entry or `children` entry) | merges in another file's object as this object's base |
| Scene  | the top-level `scene:` block | merges in another file's `scene:` block (name, `auto_transform`, `root_objects`) as this scene's base |

A base reference is a string or list of strings: `inherit_from: path.yaml` or
`inherit_from: [a.yaml, b.yaml]` (later entries override earlier ones), and
may include an anchor for object-level references: `path.yaml#ObjectName`.
The referenced document supplies its base object via, in order: a top-level
`object:` key (the canonical shape for a reusable prefab file), a `#ObjectName`
lookup (depth-first) or first entry of a `scene: root_objects:` list, or
otherwise its bare top-level mapping.

**Merge rules:**

| Node | Match key | Behavior |
|---|---|---|
| Plain fields | — | override wins; two mappings merge recursively (`color: { a: 0.5 }` touches only alpha); everything else replaces wholesale |
| `components:` | normalized `type`/`!Tag`, plus optional `id:` to disambiguate repeats | matched entries deep-merge in place; unmatched append; an ambiguous match (repeated type, no `id`) appends with a warning instead of guessing |
| `children:` | `name` | matched entries merge in place, preserving the base's position (draw/traversal order matters); unmatched append |

Any component or child entry carrying `remove: true` deletes the matching
base entry instead of merging.

**Prefabs:** `prefab: objects/crate` is the same reference as `inherit_from`,
written for reusable object files (`objects/<name>.yaml`, shape `object: {...}`).
The extension may be omitted, and the instance's own root `Transform` (or, for a
UI prefab, `RectTransform`) replaces the prefab's instead of merging, so every
instance is placed absolutely. References resolve against the declaring file's
folder, the root scene's folder, then the search roots (`set_search_roots()`),
and finally by name through `coopa::asset::AssetIndex` — `objects/crate` also
finds `objects/props/crate.yaml`.

**Reserved keys** (read by the merge pass, otherwise inert to component
parsers): `inherit_from`, `id`, `remove`, and `__source_dirs` — a per-node
stamped list of declaring directories (nearest first), consumed by
`ParseContext::resolve()` so a component merged in from a prefab in another
directory keeps resolving its own relative asset paths (`font:`,
`animation_file:`, ...) against the file that actually wrote them.

Errors: a missing base file, an inheritance cycle, or a chain deeper than 32
files all throw `std::runtime_error`.

```yaml
# assets/prefabs/corner_panel.yaml — a reusable prefab
object:
  name: CornerPanel
  components:
    - type: RectTransform
      size_delta: { x: 220.0, y: 90.0 }
    - type: Text
      font: ../fonts/DejaVuSans.ttf   # resolves against assets/prefabs/, not the including scene
      font_size: 16

# assets/scenes/hud/scene.yaml — uses it, overriding just what differs
scene:
  root_objects:
    - name: TopLeft
      inherit_from: ../prefabs/corner_panel.yaml
      components:
        - type: Text
          text: "Top Left"   # only `text` changes; font/font_size/... survive
```

---

## Component Submodule

For detailed documentation on the two component types defined here, see the
[Components Submodule README](components/README.md).
Renderer components live in gfxcoopa's `engine/components/`; UI components
live in uicoopa's `uicoopa/`.

---

## Usage Example

```cpp
#include <coopa/scene/scene_manager.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/register.h>

// 1. Register every non-Transform component this application needs.
coopa::gfx::engine::components::register_render_components(device, allocator, cmd_pool);

// 2. Initialize scene manager and load a scene.
coopa::scene::SceneManager manager;
manager.load_scene("assets/scenes/gi_cornell_box/scene.yaml");

// 3. Obtain active scene.
auto& scene = manager.get_active_scene();

// 4. Update scene hierarchy per frame.
float delta_time = 0.016f; // 60 FPS frame delta
manager.update(delta_time);
```
