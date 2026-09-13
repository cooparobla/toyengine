# toyengine/core

Application lifetime and configuration.

| File | Purpose |
|---|---|
| [`engine.h`](engine.h) | `Engine` — owns a `coopa::job::JobEngine`, then a `gfx::app::Context` (Window → Instance → Surface → Device → Allocator → Swapchain → CommandPool → RenderPass → Renderer, plus frame timing and resize handling), then `PixelRenderPipeline`, `AssetManager` and `SceneManager` on top. `run()`/`tick()` drive the loop. |
| [`config.h`](config.h) | `AppConfig::load(path)` — parses `assets/config.yaml` via fkYAML directly. Every field has an in-class default; missing or malformed keys fall back silently. |

Frame timing lives in `gfx::app::Context`, not here.

## Usage

```cpp
toy::core::AppConfig config = toy::core::AppConfig::load(ROOT_DIR "/assets/config.yaml");
toy::core::Engine engine(std::move(config));
engine.run();
```

## Environment overrides

`Engine` honours a family of env vars so a scripted or one-off run never has to edit
version-controlled config:

| Var | Effect |
|---|---|
| `SCENE=<name\|path>` | Loads a different scene than `scene.default_scene`. Read by the constructor. |
| `ONESHOT=1` | Renders exactly one frame, then exits cleanly. |
| `MAX_FRAMES=N` | Renders N frames, then exits cleanly. |
| `FIXED_DT=<seconds>` | Overrides the per-tick delta fed to asset and scene updates, so animation advances by an exact, repeatable amount per frame. |
| `CAPTURE_FRAMES=N` | Dumps one PNG per tick to `output/seq/frame_%04d.png` for N frames. |
| `NO_INPUT=1` | Zeroes all camera-controller input, so a capture on a live desktop is not perturbed by real mouse/keyboard activity. |
| `CURSOR_POS="<x>,<y>"` | Pins the pointer to a fixed window-pixel position every frame — the only way to exercise world-space UI hit testing headlessly. |

**A capture is only reproducible if `output.save_low_res` is `true`.** With it `false`,
`save_screenshot()` writes the swapchain-sized image, and the window manager does not
necessarily grant the client area `window.width`/`window.height` asks for — so the saved
PNG's dimensions can differ between runs on the same machine. The low-res buffer is
`render_width`×`render_height` and is unaffected.
