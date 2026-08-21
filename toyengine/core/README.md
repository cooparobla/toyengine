# toyengine/core

Application lifetime and configuration.

| File | Purpose |
|---|---|
| [`engine.h`](engine.h) | `Engine`: owns Window → Instance → Surface → Device → Allocator → Swapchain → CommandPool → RenderPass → Renderer → `PixelRenderPipeline`, plus `AssetManager`/`SceneManager`. `run()`/`tick()` drive the loop; honors `ONESHOT=1`/`MAX_FRAMES=N` env vars for headless capture. |
| [`config.h`](config.h) | `AppConfig::load(path)` — parses `assets/config.yaml` via fkYAML directly (no `caml`, unlike blendy — toyengine has no need for encrypted/compressed config). Every field has an in-class default; missing/malformed keys fall back silently. |
| [`time.h`](time.h) | `Time` — frame delta/elapsed/count on `std::chrono::high_resolution_clock`, ported from `blendy/core/time.h`. |

## Usage

```cpp
toy::core::AppConfig config = toy::core::AppConfig::load(ROOT_DIR "/assets/config.yaml");
toy::core::Engine engine(std::move(config));
engine.run();
```
