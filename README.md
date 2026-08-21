# toyengine

A basic pixel-art game engine, shaped like [blendy](../blendy) and built on
[gfxcoopa](../gfxcoopa) (Vulkan) and [libcoopa](../libcoopa) (scene graph,
assets, utilities). Its defining trait: the whole 3D scene renders into a
small offscreen buffer, then upscales to the window with nearest-neighbour
filtering and a centred integer-scale letterbox, for a crisp pixelated look.

## Features

- **Low-resolution deferred renderer** — G-buffer + banded-PBR lighting at a
  fixed internal resolution (480×270 by default), independent of window size.
- **Cel-shaded lighting** — directional + point lights, N·L quantized into
  discrete bands, hard-thresholded specular highlights.
- **Hard-edged shadows** — directional (AABB-fit orthographic, texel-snapped)
  and one point-light cubemap, both single-sample (no PCF) for crisp edges.
- **Pixel-art post stack** — depth/normal outline, exposure, 8×8 Bayer ordered
  dithering, and palette quantization to an arbitrary Nx1 palette PNG.
- **Integer-scale upscale** — every low-res texel becomes an exact N×N block
  of screen pixels; the window can be any size, letterboxed with black bars.
- **Orbit/fly camera controller**, YAML scene format (shared with blendy),
  nearest-filtered texture loading, headless `ONESHOT`/`MAX_FRAMES` capture.

Explicitly **not** included: SSR, reflection probes, GI probes, MSAA, and any
temporal AA (FXAA/SMAA/TAA) — temporal jitter would destroy the pixel-grid
stability this engine exists to produce.

## Build & run

```bash
cbuild --vulkan   # compiles assets/shaders/*.{vert,frag} via glslc, then cmake
cplay             # runs ./build/toyengine
```

Fallback (no `cbuild`/`cplay`):

```bash
cmake -B build && cmake --build build
./build/toyengine
```

Headless verification:

```bash
ONESHOT=1 ./build/toyengine        # render exactly one frame, save output/frame.png, exit
MAX_FRAMES=30 ./build/toyengine    # render 30 frames then exit
ctest --test-dir build             # pure-math + full headless-render integration tests
```

## Layout

```
toyengine/
├── core/       Engine, AppConfig, Time
├── input/      InputMap (named actions over GLFW keys)
├── loaders/    PixelTextureLoader (NEAREST-filtered texture asset loader)
├── scene/      CameraController (orbit/fly) + its SceneLoader registration
├── render/     PixelRenderPipeline, PixelRenderConfig, pixel_math, PaletteLut,
│               InstanceStream, and every pass in render/passes/
└── util/       screenshot.h (Vulkan image -> PNG)
```

See each subdirectory's own README for details on that module.
