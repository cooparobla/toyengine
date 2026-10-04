# toyengine

**A modern C++20 / Vulkan 3D game engine with a Blender-style editor.**

toyengine pairs a full-resolution deferred PBR renderer with rigid-body and cloth physics,
dynamic water, streamed procedural terrain, an interactive UI toolkit, and a native editor
that writes exactly the files the game loads. It runs on Linux and macOS (Apple Silicon,
via MoltenVK).

![Lake with Gerstner waves, shoreline foam, a flowing river and buoyant crates](docs/images/water_test.jpg)

<table>
  <tr>
    <td><img src="docs/images/terrain_test.jpg" alt="Streamed procedural tile terrain"></td>
    <td><img src="docs/images/underwater_test.jpg" alt="Underwater fog, caustics and Snell's window"></td>
  </tr>
  <tr>
    <td align="center"><sub><b>terrain_test</b>: procedural world streamed in chunks around the camera</sub></td>
    <td align="center"><sub><b>underwater_test</b>: absorption, caustics, the surface seen from below</sub></td>
  </tr>
  <tr>
    <td><img src="docs/images/pixel_demo.jpg" alt="PBR materials, glass, emissive and water"></td>
    <td><img src="docs/images/physics_test.jpg" alt="Rigid-body physics arena"></td>
  </tr>
  <tr>
    <td align="center"><sub><b>pixel_demo</b>: PBR, refraction, SDFs, bloom, SSR</sub></td>
    <td align="center"><sub><b>physics_test</b>: bounciness, friction, stacking, hinges, triggers</sub></td>
  </tr>
</table>

## The editor

![toyengine_editor with the water_test scene open](docs/images/editor.jpg)

`toyengine_editor` embeds the real engine, so its **Full Render** viewport *is* the game's
renderer. It looks and feels like Blender (keymaps, gizmos, modes, outliner) and uses a
Unity-style component model:

- **Scenes and object assets.** Hierarchy, inspector, gizmos, play / pause / step in place.
  Prefab-like object assets are instanced with `prefab:`.
- **Mesh modelling.** Edit Mode has extrude, inset, bevel, loop cut and slide, bridge,
  subdivide (including Catmull-Clark), mirror and UVs. Sculpt Mode has draw, smooth, inflate,
  grab and flatten brushes, with symmetry.
- **Material lookdev.** A shader ball, studio lighting and a turntable.
- **Render and project settings,** applied live.
- **Build > Package** turns a project into compact `.caml` binaries.
- Snapshot undo, hot-reloading themes and a console.

Everything it saves is plain YAML in your project's `assets/` folder, so you can hand-edit,
diff and merge it. See [editor/README.md](editor/README.md) for the full tour.

## Features

### Rendering
- **Deferred PBR pipeline.** G-buffer plus Cook-Torrance lighting with directional, point and
  spot lights, an environment and sky, and texture maps (albedo, normal, roughness, metallic, AO).
- **Shadows.** Cascaded directional and point/spot shadows, soft PCF, optional PCSS contact
  hardening and screen-space contact shadows.
- **Screen-space effects.** Hi-Z SSR with temporal accumulation, traced SSGI colour bleed and
  temporally stable SSAO.
- **Transparency and refraction.** A forward pass for blended materials, with screen-space
  refraction and Fresnel.
- **SDF raymarching.** Signed-distance-field shapes that cast shadows and mix freely with meshes.
- **Volumetrics and fog.** Raymarched local volumes with shadowed light shafts, plus global
  height fog.
- **Post-processing.** Bloom, auto exposure, colour-grading LUTs, physically based depth of
  field, tilt-shift, and anti-aliasing (TAA, SMAA or FXAA).
- **Quality presets.** Per-feature tiers in `assets/config.yaml`. Every feature has a master
  switch, and "off" really costs nothing.
- **GPU and CPU profiler.** `PROFILE=1` writes per-feature timings to CSV.
- **Optional stylisation.** Banded cel lighting, outlines, ordered dithering and palette
  quantisation are available when a project wants them.

### Simulation
- **Rigid-body physics** ([physxcoopa](libs/physxcoopa)). Box, sphere, capsule and mesh
  colliders, physics materials, hinge joints, triggers, collision layers, and kinematic movers and controllers.
- **Cloth.** XPBD cloth that collides with the world and renders as a shaded, shadow-casting
  mesh.
- **Water** ([toyengine/water](toyengine/water/README.md)):
  - Lakes and oceans with Gerstner waves. The same wave math runs in the shader and in a CPU
    surface query, so gameplay and visuals agree.
  - Rivers whose current comes from the mesh's own slope and bends around obstacles.
  - Shoreline and contact foam, and ripple rings from anything crossing the surface.
  - `Buoyancy` makes any rigid body float, bob on waves and drift with the current.
  - Underwater, you get fog, absorption, caustics and Snell's window.
- **Procedural terrain** ([toyengine/world](toyengine/world/README.md)):
  - A seeded [mapcoopa](libs/mapcoopa) world with elevation, biomes and rivers.
  - Meshed in chunks on the job system and streamed around the camera.
  - Tile shapes are mesh assets, so the look changes with one line of YAML.

### UI and scenes
- **UI toolkit** ([uicoopa](libs/uicoopa)). Panels, text, buttons, sliders, progress bars,
  layouts and masks. Use it as a crisp screen-space HUD, or on world-space canvases that
  billboard or sit in 3D and stay clickable.
- **Data-driven scenes.** YAML scenes, object assets and shared material assets. Every loader
  also reads binary `.caml`, so a packaged project needs no path rewriting.
- **Components.** Cameras (orbit, fly, tracking), movers, skinned meshes, a multithreaded job
  system and named input actions.

## Getting started

### 1. Clone

The coopa libraries are pinned submodules under [`libs/`](libs/). Clone them recursively,
because gfxcoopa has a nested submodule of its own:

```bash
git clone --recurse-submodules git@github.com:cooparobla/toyengine.git
# already cloned?
git submodule update --init --recursive
```

### 2. Build

**Linux.** You need the Vulkan SDK (with `glslc`), GLFW, CMake and a C++20 compiler:

```bash
cmake -B build && cmake --build build -j
```

**macOS (Apple Silicon).** A one-time script installs MoltenVK, the Vulkan loader, validation
layers, `glslc` and GLFW through Homebrew:

```bash
tools/setup_macos.sh
cmake -B build && cmake --build build -j
```

### 3. Run a demo

```bash
./build/toyengine                  # the default scene from assets/config.yaml
./build/toyengine water_test       # or any scene under assets/scenes/ by name
./build/toyengine path/to/scene.yaml
```

| Scene | What it shows |
|---|---|
| `pixel_demo` | Materials showcase: PBR, glass and refraction, SDFs, emissive bloom, water |
| `water_test` | Lake, river, foam, ripples, buoyant crates, a raft and a circling boat |
| `underwater_test` | Underwater fog, caustics, Snell's window; scroll out to break the surface |
| `terrain_test` | Streamed procedural world. WASD moves the focus, Tab/Shift change height |
| `physics_test` | Restitution, friction, stacking, joints, triggers, kinematic platforms |
| `cloth_test` | XPBD cloth draped over a moving ball |
| `world_canvas_test` | World-space and screen-space UI, with a clickable health bar |
| `material_maps_test` | Texture-mapped vs. flat materials (`scene_mapped.yaml` / `scene_flat.yaml`) |

Default controls: the mouse orbits the camera (the cursor is captured), the scroll wheel zooms, and Esc quits.

### 4. Open the editor

```bash
./build/toyengine_editor                       # most recent project, else this repo's assets/
./build/toyengine_editor path/to/project       # any folder containing assets/
./build/toyengine_editor --new-project ~/game  # scaffold a new project with a starter scene
```

### 5. Write a scene by hand

Scenes are a tree of objects with components. The engine is **Z-up**, in metres.

```yaml
format: blender
scene:
  scene_name: Hello
  root_objects:
    - name: camera
      components:
        - type: Transform
          position: { x: 0.0, y: -10.0, z: 6.0 }
          rotation: { x: 68.0, y: 0.0, z: 0.0 }
        - type: Camera
          main: true
          fov: 50.0
    - name: sun
      components:
        - type: Transform
        - type: DirectionalLight
          direction: { x: -0.35, y: -0.45, z: -0.82 }
          cast_shadows: true
    - name: ball
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 4.0 }
          scale: { x: 0.5, y: 0.5, z: 0.5 }
        - type: MeshRenderer
          mesh_path: sphere.000
          material: { albedo: { r: 0.8, g: 0.3, b: 0.2 }, roughness: 0.4 }
        - type: SphereCollider
          radius: 1.0
        - type: Rigidbody
          mass: 1.0
```

The scenes in [`assets/scenes/`](assets/scenes/) are heavily commented and are the best
reference for every component. Rendering is configured in
[`assets/config.yaml`](assets/config.yaml), with a comment on every key.

## Testing and headless runs

```bash
ctest --test-dir build -j4                         # engine + editor suites
./build/toyengine_tests --list                     # tests and groups
./build/toyengine_tests --group scene              # one group
./build/toyengine_editor_tests                     # editor suite
```

Render tests use a never-mapped window, so a full run is invisible on your desktop. You can
also script the engine:

```bash
HEADLESS=1 MAX_FRAMES=600 ./build/toyengine terrain_test   # benchmark, then save output/frame.png
ONESHOT=1 ./build/toyengine                                # render one frame and exit
HEADLESS=1 PROFILE=1 MAX_FRAMES=600 ./build/toyengine      # per-feature timings -> output/profile.csv
```

`FIXED_DT`, `NO_INPUT`, `CAPTURE_FRAMES`, `SCENE` and `CONFIG` make captures reproducible. See
the docs on `Engine::run()` in [toyengine/core/engine.h](toyengine/core/engine.h).

## Project layout

```
toyengine/
├── core/       Engine, AppConfig, caml codec, branding
├── render/     the render pipeline, its config, profiler, and passes/
├── scene/      gameplay components: camera controller, movers, cloth & skinned renderers
├── world/      streamed procedural tile terrain
└── water/      WaterBody, Buoyancy, WaterSystem
editor/         toyengine_editor: app, documents & undo, schemas, mesh modelling, viewport, packager
assets/         config.yaml, scenes, meshes, materials, textures, shaders, fonts
docs/           hand-written guides (e.g. ambient lighting); docs/images holds these screenshots
libs/           pinned submodules: libcoopa (scene graph, assets, jobs), gfxcoopa (Vulkan),
                physxcoopa (physics), sfxcoopa, uicoopa (UI), mapcoopa (world gen), caml
```

Each module has its own README with the details.

## Platform notes

- **macOS.** Use the plain CMake path; `cbuild`/`cplay` are Linux-only workspace tools.
  - Plain `cmake -B build` configures a Release build here. Pass `-DCMAKE_BUILD_TYPE=Debug`
    to get validation layers.
  - MoltenVK has no MAILBOX present mode, so presentation is always FIFO.
  - Seeded worlds are byte-identical to Linux. mapcoopa ships its own libstdc++-exact random
    and sort functions and builds with `-ffp-contract=off`.
- **SMAA lookup textures** are fetched at configure time unless `SMAA_TEXTURES_DIR` points at
  a checkout.
- **Working inside `libs/`.** Each library still builds standalone in place. Submodules check
  out a detached HEAD, so run `git submodule foreach git checkout main` before committing to
  one. Building `uicoopa` in place touches three tracked depfiles;
  `git -C libs/uicoopa checkout -- .` clears them.

## Documentation

- Per-module READMEs: [core](toyengine/core/README.md), [render](toyengine/render/README.md),
  [scene](toyengine/scene/README.md), [world](toyengine/world/README.md),
  [water](toyengine/water/README.md), [editor](editor/README.md).
- [`docs/`](docs/) holds hand-written guides to specific engine behaviour.
- `.docs/` is the generated HTML API reference, built from in-source docstrings with
  `coopadocs build`.
