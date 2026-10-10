# toyengine/core

Application lifetime and configuration.

| File | Purpose |
|---|---|
| [`engine.h`](engine.h) | `Engine` — owns a `coopa::job::JobEngine`, then a `gfx::app::Context` (Window → Instance → Surface → Device → Allocator → Swapchain → CommandPool → RenderPass → Renderer, plus frame timing and resize handling), then `ToyRenderPipeline`, `AssetManager` and `SceneManager` on top. `run()`/`tick()` drive the loop. |
| [`config.h`](config.h) | `AppConfig::load(path)` — parses `assets/config.yaml` via fkYAML directly. Every field has an in-class default; missing or malformed keys fall back silently. Also layers a scene's `settings:` overrides (`with_scene_settings()`). |
| [`scene_loading.h`](scene_loading.h) | Async scene loading: `SceneLoadOptions`, `SceneTransition`, `SceneLoadHandle` (what `Engine::load_scene_async()` takes and returns) and the transition layer it draws. |
| [`runtime_paths.h`](runtime_paths.h) | Where a running game finds its files — the source checkout or a relocated package — including the shader search roots (project → toyengine → gfxcoopa → uicoopa) and the per-user data directory. |
| [`module.h`](module.h) | `TOY_MODULE()` — how a game project's `src/` code registers components with every `Engine`. |
| [`caml_codec.h`](caml_codec.h) | Plugs caml's compressed + encrypted `.caml` documents into `coopa::yaml`. |
| [`runtime_log.h`](runtime_log.h) | A packaged game's log file and crash reports. |
| [`user_settings.h`](user_settings.h) | Per-player settings that outlive a run (audio volumes), in the per-user data directory. |
| [`branding.h`](branding.h) | toyengine's name, version and logo shapes. |

Frame timing lives in `gfx::app::Context`, not here.

## Overlay layers and the debug overlay

`set_overlay_scene()` is the host's single overlay (the editor UI). On top of it the engine
keeps its own ordered **overlay layers** — `add_overlay_layer(scene, order, in_display_rect)` /
`remove_overlay_layer()` — ticked like the overlay scene and drawn over everything else.
`in_display_rect` places a layer's canvases inside the scene's display rect (the letterboxed
game image, or the editor viewport).

The first layer is the stats HUD, [`toyengine/debug/debug_overlay.h`](../debug/debug_overlay.h).
**F3** (the `debug_overlay` input action) cycles it off → fps → full; `debug.overlay` in
`config.yaml` sets the startup mode, and the editor shows it with View > Stats Overlay.

- **fps** — FPS, frame ms (avg/min/max), the 1% low, and a 240-frame sparkline.
- **full** — adds CPU phase and GPU pass timings (a live `FrameProfile`, no CSV; GPU numbers
  arrive two frames late), draw stats, scene counts, physics, navigation and VMA heap budgets.
- **Game lines** — `toy::debug::watch("speed", v)` and `toy::debug::text("state: %s", s)`
  publish per-frame lines into a "Game" block; they are cleared every frame.

Off costs nothing: no layer is ticked or drawn, no profiler runs, and `watch()`/`text()`
return at once. A `TOY_SHIPPING` build compiles the overlay out unless it is configured with
`-DTOY_SHIPPING_DEBUG_OVERLAY=ON`.

## Async scene loading and transitions

`load_scene()` blocks: GPU wait, parse, start, drain every asset load. Tests and tools keep
using it. A running game changes level with `load_scene_async()`, which keeps the current
scene running and rendering while the next one loads:

```cpp
toy::core::SceneLoadOptions opts;
opts.transition = toy::core::SceneTransition::loading_screen_ui("ui/loading_screen");
opts.min_display_time = 1.0f;     // seconds; a fast load doesn't flash the screen
opts.spawn_point = "from_hub";    // move the player (first CharacterController) here
toy::core::SceneLoadHandle load = engine.load_scene_async("scenes/level_2", opts);
load.on_complete([](coopa::scene::Scene& s) { /* restore state before its first frame */ });
// load.progress() (0..1, monotonic), load.is_ready(), load.is_done(), load.failed()/error()
```

1. **Document** — read, decoded and its `inherit_from`/`prefab` references expanded on a
   worker (`SceneLoader::read_document_async()`).
2. **Build** — objects are built on the main thread a batch of root objects per frame
   (`build_budget_ms`, default 4); parsers start their asset loads, which decode on workers.
   Nothing is started yet (`SceneLoader::LoadOptions{.start = false}`).
3. **Assets** — the engine waits for the pending loads while `AssetManager::update(dt,
   finalize_budget_ms)` spreads the main-thread finalizes (GPU uploads) over frames.
4. **Activation** — once the transition covers the screen and `min_display_time` has passed:
   one `wait_idle()`, the old scenes are destroyed (unless `additive`, which adds the scene
   alongside and keeps the active one), the new scene is started, its systems installed, and it
   simulates. No asset drain. `on_complete` fires here, before its first update.

`progress()` weighs the document 5 %, the build 65 % and the assets 30 %. One load runs at a
time (a second request returns the in-flight handle); `load_scene()`/`set_scene()` cancel an
unfinished one, and a failed load fades back to the running scene and fires `on_failed`.

**Transitions** (`SceneTransition`) draw on an engine overlay layer (order 50, inside the
display rect): `fade(seconds, colour)`, `loading_screen_ui(ui_asset, fade, colour)` — the UI
asset is shown over the fade colour once it covers the screen, with a `progress` StatBar /
ProgressBar (0..1) and a `status` Text filled in by name (`assets/ui/menus/loading_screen.yaml`
is the template) — or `none()`.

**`SceneLink`** ([`toyengine/scene/scene_link.h`](../scene/scene_link.h)) changes level with no
code: a box (`size`) that loads `target_scene` with `transition` (`fade` / `loading_screen` /
`none`), `fade_time`, `color`, `loading_screen`, `min_display_time` and `spawn_point` when a
CharacterController enters it. Demo: `./build/toyengine loading_test` (two linked scenes, one
with over a thousand objects; generated by `tools/gen_loading_test_scene.py`). An engine
embedded in a tool (`EngineOptions::edit_mode`, the editor's play mode) ignores SceneLinks.

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
