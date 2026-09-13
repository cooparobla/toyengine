# toyengine

A basic pixel-art game engine, shaped like [blendy](../blendy) and built on
[gfxcoopa](libs/gfxcoopa) (Vulkan) and [libcoopa](libs/libcoopa) (scene graph,
assets, utilities). Its defining trait: the whole 3D scene renders into a
small offscreen buffer, then upscales to the window with nearest-neighbour
filtering and a centred, aspect-preserving best fit (`upscale_mode: fit`, the
default) or an integer-scale letterbox (`upscale_mode: integer`), for a crisp
pixelated look.

## Features

- **Low-resolution deferred renderer** — G-buffer + banded-PBR lighting at a
  fixed internal resolution (480×270 by default), independent of window size.
  Built on gfxcoopa's shared `DeferredLightingPass`/`SsrPass`/`TransparentPass`
  (see [gfxcoopa](libs/gfxcoopa)'s shared shader library and `ExtraSets`
  decoupling), not a private fork of them.
- **Cel-shaded or soft direct lighting** (`soft_lighting`) — default is
  banded/ramped: N·L quantized into discrete bands (`light_bands`), specular
  a hard-thresholded highlight (`spec_threshold`). `soft_lighting` switches
  both to a smooth, continuous Cook-Torrance falloff instead.
- **Screen-space reflections + SSGI** (`ssr_enabled`) — Hi-Z raymarched
  specular reflections, plus an optional diffuse colour-bleed bounce term
  (`ssgi_intensity`) reusing the same trace.
- **SSAO** (`ssao_enabled`) — hemisphere-sampled ambient occlusion with
  temporal accumulation.
- **Hard shadows** — directional (AABB-fit orthographic, texel-snapped) and
  one point-light cubemap, both a single hardware depth compare.
- **Forward transparency** (`transparency_enabled`) — BLEND-material meshes,
  depth-tested against the opaque G-buffer, drawn after SSR compositing.
- **Pixel-art post stack** — depth/normal outline, exposure, 8×8 Bayer ordered
  dithering, and palette quantization to an arbitrary Nx1 palette PNG.
- **Fit or integer-scale upscale** (`upscale_mode`) — default `fit` fills the
  window as closely as the render aspect allows, letterboxing only the one
  mismatched axis; `integer` snaps to a whole scale factor so every texel is
  an exact N×N block of screen pixels, at the cost of more letterbox bars.
- **World-space UI canvases** (`world_ui_enabled`) — [uicoopa](libs/uicoopa)'s entire widget
  library (panels, text, buttons, sliders, progress bars, layout groups, masks) placed on a
  quad in 3D, either billboarded to face the camera or honouring its own 3D Transform, and
  optionally occluded by scene geometry. Rendered at the internal resolution and
  nearest-upscaled, so it sits on the same pixel grid as the scene. Genuinely interactive: the
  mouse ray is intersected with the canvas plane, so a `Button` on a world canvas is clickable.
- **Screen-space UI overlay** (`screen_ui_enabled`) — the same widget library as a flat HUD,
  drawn last at full **window** resolution, so text stays crisp instead of being quantised to
  the render grid.
- **Both UI layers composite after every post effect.** Depth of field, FXAA/SMAA/TAA and tilt
  shift are filters over the finished image; UI is not part of what they filter, so it lands in
  a stage of its own at the end of the frame (see [toyengine/render/README.md](toyengine/render/README.md#ui-layers)).
  `assets/scenes/world_canvas_test/` is the demo — a cube with a health bar floating above it,
  a rotated wall plate, and a screen-space HUD.
- **Orbit/fly camera controller**, YAML scene format (shared with blendy),
  nearest-filtered texture loading, headless `ONESHOT`/`MAX_FRAMES` capture.
- **Anti-aliasing** (`aa_mode`, default `off`) — FXAA 3.11, SMAA 1x, or TAA,
  ported from [blendy](../blendy)'s PbrRenderPipeline and run at the internal
  low resolution, before the upscale (see `PixelRenderConfig::aa_mode`).
  `off` is a true no-op: no extra target is allocated and the frame is
  byte-identical to a build with no AA support at all. `fxaa`/`smaa` are
  single-frame spatial filters; `taa` additionally jitters the camera
  projection every frame (an 8-frame Halton sequence, matching blendy's own),
  which is in genuine tension with `camera_pixel_snap`'s whole-texel
  snapping — at this engine's default internal resolution TAA will visibly
  soften the pixel grid it exists to keep crisp. It's included anyway
  because a future higher-internal-resolution mode is exactly where TAA
  earns its keep; until then, treat it as the mode you reach for on that
  future mode, not the low-res default. Ported as-is, warts included:
  blendy's TAA has no motion vectors or history reprojection (history is
  sampled at the current frame's UV and clamped to a YCoCg 3×3 AABB), so it
  ghosts under camera motion.

Explicitly **not** included: GI probes, reflection probes, and MSAA (blendy's
own `msaa_4x` is parsed but never read by its render pipeline, so there was
no working implementation to port). [blendy](../blendy) shares this same
gfxcoopa backbone with every feature (including this engine's own
pixel-art stack) exposed as an option.

## Build & run

The coopa libraries are vendored as pinned git submodules under [`libs/`](libs/),
so a clone must bring them down first. `--recursive` is required, not optional:
gfxcoopa has its own nested submodule (`includes/volk`), and without it the build
fails at `volk/volk.h: No such file or directory`.

```bash
git clone --recurse-submodules git@github.com:cooparobla/toyengine.git
# or, in an existing clone:
git submodule update --init --recursive
```

```bash
cbuild --vulkan   # compiles assets/shaders/*.{vert,frag} via glslc, then cmake
cplay             # runs ./build/toyengine on config.yaml's scene (pixel_demo)
cplay physics_test  # ...or on any other scene under assets/scenes, by name
```

Fallback (no `cbuild`/`cplay`):

```bash
cmake -B build && cmake --build build
./build/toyengine [scene]
```

The optional scene argument takes a bare name under `assets/scenes` (expanded to
`assets/scenes/<name>/scene.yaml`) or an explicit path to a `.yaml`. It overrides
`scene.default_scene` in `assets/config.yaml` for that run only; the `SCENE` env var
below still takes precedence over both.

Headless verification:

```bash
ONESHOT=1 ./build/toyengine        # render exactly one frame, save output/frame.png, exit
MAX_FRAMES=30 ./build/toyengine    # render 30 frames then exit
SCENE=world_canvas_test ./build/toyengine   # load a different scene than config.yaml's
```

Tests:

```bash
ctest --test-dir build -j4            # everything; -j4 overlaps the render groups
ctest --test-dir build -R toyengine_math   # instant, no Vulkan device
./build/toyengine_tests --list        # every test and its group
./build/toyengine_tests --group scene # one group
./build/toyengine_tests cloth -v      # name substring, every assertion printed
```

The groups are `math`, `config` and `scene` (pure CPU, milliseconds) plus `render_pixel`,
`render_ui`, `render_material` and `render_cloth`, each of which brings up a real Vulkan
device. Every render test runs with `window.visible: false` and `NO_INPUT=1`, so no window
appears, nothing takes focus and the pointer is never grabbed -- a full run is invisible on
a desktop you are still using. Nothing is written to `output/`; a frame is only dumped, to
a temp directory whose path is printed, when an assertion fails.

## Layout

```
toyengine/
├── core/       Engine, AppConfig, Time
├── input/      InputMap (named actions over GLFW keys)
├── loaders/    PixelTextureLoader (NEAREST-filtered texture asset loader)
├── scene/      CameraController (orbit/fly) + its SceneLoader registration
├── render/     PixelRenderPipeline, PixelRenderConfig, pixel_math,
│               InstanceStream, and every pass in render/passes/
└── util/       screenshot.h (Vulkan image -> PNG)

libs/           pinned submodules: libcoopa, gfxcoopa, physxcoopa,
                sfxcoopa, caml, uicoopa
```

See each subdirectory's own README for details on that module.

### Working in `libs/`

Each library under `libs/` is **still independently buildable in place** — the
repos are vendored unmodified, so they keep their usual contract: a repo builds
standalone as long as the repos it needs sit beside it under a shared parent.
`libs/` satisfies that exactly as `~/git/` does, since it holds the same set.

```bash
cd libs/gfxcoopa && cbuild     # works, resolving peers to libs/libcoopa
cd libs/uicoopa  && cplay      # ditto, demos included
```

Two things to know when building in there:

- `git submodule add` leaves a submodule on `main`, but `git submodule update`
  (and a fresh `--recurse-submodules` clone) checks out a **detached HEAD**.
  Commits made from that state are easy to lose — run
  `git submodule foreach git checkout main` first, or keep doing library work in
  your standalone `~/git/<repo>` checkouts.
- Building writes `.spv`/`.spv.d` next to the shader sources. Most repos gitignore
  those, but `uicoopa` tracks three depfiles, so building it in place shows up as
  a modified submodule in `git status`. `git -C libs/uicoopa checkout -- .` clears it.

`libs/uicoopa` **is** part of toyengine's build now, linked as `coopa::ui`. It used to be
excluded because it used `CMAKE_SOURCE_DIR` for its own include/shader/config paths and
exposed no consumable target; both are fixed in uicoopa itself, backward-compatibly, so
building it standalone as above still works exactly as before. Its `uicoopa_shaders` target
compiles `ui*.vert`/`.frag` into `.spv` next to the sources, and
[`engine.h`](toyengine/core/engine.h)'s `ShaderLibrary` searches that directory as a third
root after toyengine's own and gfxcoopa's.

## Documentation

[`docs/`](docs/) holds hand-written guides to specific engine behaviour (e.g.
[ambient lighting](docs/ambient-lighting.md)). This is separate from `.docs/`,
the generated HTML API reference built from in-source docstrings via
`coopadocs build`.
