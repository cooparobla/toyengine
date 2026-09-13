# toyengine/scene

toyengine-specific scene components, registered as `coopa::scene::SceneLoader` parsers
alongside gfxcoopa's (`register_render_components()`).

| File | Purpose |
|---|---|
| [`camera_controller.h`](camera_controller.h) | `CameraController` — Orbit (spherical yaw/pitch/distance around a fixed `target` or a named `tracker` object followed with `follow_smoothing`) or Fly (WASD/E/Q along the world matrix's basis vectors, arrow keys to look). |
| [`kinematic_mover.h`](kinematic_mover.h) | `KinematicMover` — scripted motion for a kinematic Rigidbody: `pingpong`, `orbit` or `spin`. Only ever writes the owner's Transform; physxcoopa derives the body's velocity from the frame-to-frame delta. |
| [`kinematic_controller.h`](kinematic_controller.h) | `KinematicController` — the input-driven counterpart to `KinematicMover`: moves the owner across the world XY plane from a pushed-in `move_input`, holding its authored height. Pairs with `Rigidbody { is_kinematic: true, use_gravity: false }`. Velocity is smoothed exponentially (`1 - exp(-k*dt)`), not because of feel but because PhysicsSystem derives the body's velocity from the Transform delta — an instant start/stop jolts everything resting on or pinned to it. |
| [`cloth_renderer.h`](cloth_renderer.h) | `ClothRenderer` — draws a sibling `Cloth` (physxcoopa) as a shaded, shadow-casting mesh, rewriting a dynamic GPU vertex buffer from the solver's particles each frame. One vertex per particle; normals are area-weighted, tangents follow the grid's +U. Needs a sibling `MeshRenderer` with **no `mesh_path`** and ideally `cull_backfaces: false`. |
| [`health_driver.h`](health_driver.h) | `HealthDriver` — demo component owning a `coopa::stat::Resource` and binding it to a `coopa::ui::ProgressBar` in its own subtree. |
| [`kinematic_control_system.h`](kinematic_control_system.h) | `KinematicControlSystem` + `install_kinematic_control_system()` — runs `KinematicController` and `KinematicMover` at order **50**, ahead of `UpdatePhase::Physics` (100). Not stylistic: a component's `update()` runs at `Behaviour` (200), *after* `PhysicsSystem` has already read its Transform, so a kinematic body driven there is always one frame stale to the physics that consumes it. Harmless for rigid contacts; it was what made the ball clip through the cloth in `cloth_test`. |
| [`register.h`](register.h) | `register_scene_components()` — registers every parser that needs no dependencies; the `(Device&, Allocator&, AssetManager&, frames_in_flight)` overload is a strict superset that additionally registers `ClothRenderer`. Call one of them once at startup. |

Two conventions hold across all three:

- **Per-frame input is pushed in, not pulled.** `Engine::tick()` writes
  `CameraController`'s `mouse_delta`/`scroll_input`/`move_input`/`look_input` before
  `Scene::update()` consumes them, so the component is testable with no live GLFW window.
- **Child lookup happens in `start()`, never in a YAML parser.** `SceneLoader` parses an
  object's components before its children exist, so a parser can only record a name —
  see `HealthDriver::find_bar_()`.

## Conventions

Two rules every component here follows, both worth knowing before adding another:

1. **Per-frame input is PUSHED in, never pulled.** `CameraController` and `KinematicController` both
   expose plain public fields (`mouse_delta`, `move_input`, …) that `Engine::tick()` writes before
   `Scene::update()` consumes them. A component that never touches `coopa::input::Input` stays
   testable with no live GLFW window, and `NO_INPUT=1` can zero the whole thing for a reproducible
   headless capture. Note the consequence for tests: the Engine overwrites those fields at the top
   of every tick, so poking a value in from outside a running Engine cannot survive to `update()`.
2. **A component that drives a kinematic body's Transform writes it in `advance()`, not
   `update()`**, and is driven by `KinematicControlSystem` ahead of the physics phase — see that
   file's doc for the frame-order trace. `update()` on those components is deliberately empty, so a
   scene that forgets to install the system fails loudly (nothing moves) rather than subtly.
3. **Child/descendant lookup happens in `start()`, never in a YAML parser** — the parser runs before
   children exist (see `HealthDriver::find_bar_()`). `ClothRenderer` goes one step further and
   defers to its first `upload()`, because the thing it depends on (the simulated cloth) is not
   created until `PhysicsSystem`'s first reconcile pass, which is itself after `Scene::start()`.
