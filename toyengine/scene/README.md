# toyengine/scene

toyengine-specific scene components, registered as `coopa::scene::SceneLoader`
parsers alongside gfxcoopa's (`register_render_components()`).

| File | Purpose |
|---|---|
| [`camera_controller.h`](camera_controller.h) | `CameraController` — Orbit (true spherical yaw/pitch/distance around a fixed `target` point or a named `tracker` GameObject followed with `follow_smoothing`; mouse motion drives yaw/pitch continuously, pitch clamped to `[min_pitch_deg, max_pitch_deg]`, scroll wheel zooms `distance` between `min_distance`/`max_distance`; `movement_smoothing` (0..1) adds drag to how quickly the camera's own pose chases that input, separate from `follow_smoothing`'s target-tracking lag) or Fly (WASD/E/Q along the current world matrix's basis vectors + look via `rotation.x`/`rotation.z`, arrow keys). Per-frame input is pushed in by `toy::core::Engine::tick()` before `Scene::update()`, not read from a `Window` directly — keeps this headlessly testable. |
| [`register.h`](register.h) | `register_scene_components()` — registers `"CameraController"` with `SceneLoader`. Call once at startup. |

No camera controller exists anywhere else in the workspace — blendy's camera
only ever moves via an orbit `AnimationComponent` on a *different* object.
