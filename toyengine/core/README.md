# toyengine/core

Application lifetime and configuration.

| File | Purpose |
|---|---|
| [`engine.h`](engine.h) | `Engine` — owns a `coopa::job::JobEngine`, then a `gfx::app::Context` (Window → Instance → Surface → Device → Allocator → Swapchain → CommandPool → RenderPass → Renderer, plus frame timing and resize handling), then `PixelRenderPipeline`, `AssetManager` and `SceneManager` on top. `run()`/`tick()` drive the loop. |
| [`config.h`](config.h) | `AppConfig::load(path)` — parses `assets/config.yaml` via fkYAML directly. Every field has an in-class default; missing or malformed keys fall back silently. Also layers a scene's `settings:` overrides (`with_scene_settings()`). |
| [`runtime_paths.h`](runtime_paths.h) | Where a running game finds its files — the source checkout or a relocated package — including the shader search roots (project → toyengine → gfxcoopa → uicoopa) and the per-user data directory. |
| [`module.h`](module.h) | `TOY_MODULE()` — how a game project's `src/` code registers components with every `Engine`. |
| [`caml_codec.h`](caml_codec.h) | Plugs caml's compressed + encrypted `.caml` documents into `coopa::yaml`. |
| [`runtime_log.h`](runtime_log.h) | A packaged game's log file and crash reports. |
| [`user_settings.h`](user_settings.h) | Per-player settings that outlive a run (audio volumes), in the per-user data directory. |
| [`branding.h`](branding.h) | toyengine's name, version and logo shapes. |

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
| `CAPTURE_RING=N` | Keeps the last N frames in RAM while playing and writes them to `output/seq/` on exit. |
| `CURSOR_POS="<x>,<y>"` | Pins the pointer to a fixed window-pixel position every frame — the only way to exercise world-space UI hit testing headlessly. |
| `PROFILE=1\|<path>.csv` | Per-frame CPU phase and per-feature GPU timings to a CSV (`output/profile.csv` for `1`), with averages printed on exit. |
| `TOY_PROJECT_DIR=<dir>` | Overrides the project root the binary was built for. |
| `TOY_VALIDATION=0\|1` | Forces Vulkan validation layers off or on. |
| `HEADLESS=1`, `CONFIG=<path>`, `TOY_LOG_FILE=1` | Read by `main.cpp` before the `Engine` exists: a never-mapped window with vsync off, a different config file, and the packaged-game log file from a source build. |

None of these are honoured in a `TOY_SHIPPING` build.

**A capture is only reproducible if `output.save_low_res` is `true`.** With it `false`,
`save_screenshot()` writes the swapchain-sized image, and the window manager does not
necessarily grant the client area `window.width`/`window.height` asks for — so the saved
PNG's dimensions can differ between runs on the same machine. The low-res buffer is
`render_width`×`render_height` and is unaffected.
