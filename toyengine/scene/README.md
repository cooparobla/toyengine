# toyengine/scene

toyengine-specific scene components, registered as `coopa::scene::SceneLoader`
parsers alongside gfxcoopa's (`register_render_components()`).

| File | Purpose |
|---|---|
| [`camera_controller.h`](camera_controller.h) | `CameraController` — orbit (rotates position+orientation together by an identical rotation about world Z, which preserves "looking at target" without needing to know the camera's local forward-axis convention) or fly (WASD along the current world matrix's basis vectors + look via `rotation.x`/`rotation.z`). Per-frame input is pushed in by `toy::core::Engine::tick()` before `Scene::update()`, not read from a `Window` directly — keeps this headlessly testable. |
| [`register.h`](register.h) | `register_scene_components()` — registers `"CameraController"` with `SceneLoader`. Call once at startup. |

No camera controller exists anywhere else in the workspace — blendy's camera
only ever moves via an orbit `AnimationComponent` on a *different* object.
