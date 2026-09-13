# toyengine/scene

toyengine-specific scene components, registered as `coopa::scene::SceneLoader` parsers
alongside gfxcoopa's (`register_render_components()`).

| File | Purpose |
|---|---|
| [`camera_controller.h`](camera_controller.h) | `CameraController` — Orbit (spherical yaw/pitch/distance around a fixed `target` or a named `tracker` object followed with `follow_smoothing`) or Fly (WASD/E/Q along the world matrix's basis vectors, arrow keys to look). |
| [`kinematic_mover.h`](kinematic_mover.h) | `KinematicMover` — scripted motion for a kinematic Rigidbody: `pingpong`, `orbit` or `spin`. Only ever writes the owner's Transform; physxcoopa derives the body's velocity from the frame-to-frame delta. |
| [`health_driver.h`](health_driver.h) | `HealthDriver` — demo component owning a `coopa::stat::Resource` and binding it to a `coopa::ui::ProgressBar` in its own subtree. |
| [`register.h`](register.h) | `register_scene_components()` — registers all three parsers. Call once at startup. |

Two conventions hold across all three:

- **Per-frame input is pushed in, not pulled.** `Engine::tick()` writes
  `CameraController`'s `mouse_delta`/`scroll_input`/`move_input`/`look_input` before
  `Scene::update()` consumes them, so the component is testable with no live GLFW window.
- **Child lookup happens in `start()`, never in a YAML parser.** `SceneLoader` parses an
  object's components before its children exist, so a parser can only record a name —
  see `HealthDriver::find_bar_()`.
