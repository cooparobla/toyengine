# Structural cleanup — engine, editor, hub, docs

> **Status: planned.** Nothing here is implemented yet. Behaviour-preserving with **one
> deliberate exception**: the default internal render resolution changes from 480×270 to
> 1920×1080 (Stage 2d, category D). Everything else leaves the render, editor and gameplay
> behaviour untouched, with `ctest` green after every stage.

## Introduction

toyengine has grown to ~102k lines of first-party C++ ([`toyengine/`](../toyengine) 55k,
[`editor/`](../editor) 38k, [`hub/`](../hub) 0.8k) plus 183 test files. The code itself is
careful and unusually well commented, but the *structure* has drifted in four specific ways
that now cost real time on every change.

**1. Three god objects.** `ToyRenderPipeline` (5,167-line `.cpp`, 1,937-line header, ~60
methods), `Engine` (2,663-line `.cpp`, 1,408-line header, ~80 methods) and `EditorApp` (541
declarations, ~264 data members, 180 method bodies in `editor_app.cpp` plus 25 `ui/*.inl`
fragments included into its class body). Each is the single place almost every change has to
land, and each one's header is a build-time hub.

**2. Header weight is the build cost.** [`.claude/CLAUDE.md`](../.claude/CLAUDE.md) already
records the symptom — "a header `engine.h` / `editor_app.h` pulls in → ~2 min (~40 files)".
The cause is measurable: `engine.h` is included by 49 TUs and carries ~45 project includes, of
which a verified set (`world/snow_system.h`, `world/terrain_system.h`, `scene/register.h`,
`save/register.h`, `coopa/animation/animation_system.h`, and ~10 more) are referenced by
**zero** declarations in the header — they are `engine.cpp`'s needs, paid for by all 49.

**3. Dead weight and copy-paste.** Root `test.cpp` (11,624 lines) and
`editor/editor_test.cpp` (8,583 lines) are retired monolithic test files excluded from the
build but still in the tree; a 0-byte `draw` file and an unreferenced
`includes/stb_image_write.h` sit beside them. And the pattern
`if (node.contains(k)) out = node.at(k).get_value<T>();` appears ~726 times behind ~20
separately re-declared local helpers (`auto f`/`auto b`/`auto str` lambdas re-defined 15× in
[`toyengine/scene/register.cpp`](../toyengine/scene/register.cpp) alone,
`get_f`/`get_i`/`get_b`/`get_s` in `heightmap_terrain.cpp`, `num` in three more files).

**4. Legacy pixel-art framing.** The engine began as a low-resolution pixel-art renderer and
has long since grown past it, but 29 places still describe it that way — including the file
docs of `toy_render_pipeline.h` ("the low-resolution deferred pixel-art frame graph"),
`toy_render_types.h`, `upscale_pass.h`, the opening paragraph of
[`toyengine/render/README.md`](../toyengine/render/README.md), and two `tools/` generators that
state outright "this is a pixel-art engine". The root `README.md` is already correct ("a modern
C++20 / Vulkan 3D game engine", "full-resolution deferred PBR renderer"); the stale framing
survives inside the render module, where it is most likely to be read as current. The low-res
render target and its nearest-neighbour upscale **stay as features** — only the claim that they
are what the engine is *for* goes.

Documentation is in better shape than the code: 10 module READMEs, a 539-line root README, a
12-page editor user manual. The gaps are specific — four `toyengine/` subdirs and all of
`editor/`'s subdirs, `hub/` and `tests/` have no README; the root README's "Project layout"
tree omits `weather/`, `save/`, `debug/`, `destruct/`, `tests/` and `tools/`; and
`.claude/CLAUDE.md` is 103 lines of *build and test* guidance with no "where do I change X"
map at all, which is the thing it is most often needed for.

**Outcome.** Each subsystem has an obvious home, a touched header invalidates tens of TUs
instead of hundreds, and `.claude/CLAUDE.md` answers "where does this change go" for in-game,
engine, shader, render, editor (tools / presentation / inspectors / panels) and hub work.

### Working-tree note

The tree has uncommitted work in `toyengine/render/` (modified `toy_render_pipeline.cpp/h`,
`frame_profile.*`, `toy_render_config.h`, plus new untracked `dir_shadow_cache.*` and
`caster_grid.h`) and new tests under `tests/engine/`. **Stage 5 (render pipeline split) is
sequenced last among the code stages for exactly this reason** — it is the stage most likely
to collide with that work.

---

## Stage 0 — Baseline

Nothing is restructured until there is a net to catch it.

- `ccache -z`, then record a clean-ish build time for `toyengine_tests` and
  `toyengine_editor_tests` (`-j4`, per CLAUDE.md), plus `ccache -s`.
- Record the per-header blast radius for the four hub headers, so the Stage 3 win is a number
  and not a claim: for each of `core/engine.h`, `render/toy_render_config.h`,
  `render/toy_render_math.h`, `editor/app/editor_app.h`, count the TUs that transitively
  include it. Exact counts come from `build/**/*.d` depfiles, which are already there.
- Full `ctest --test-dir build -j4`. The expected baseline is **157/157 green**;
  `assets/config.yaml` has `debug_view: "off"`, so the render suite is measuring the real
  image and is a valid regression net. If anything is red at baseline, it is recorded as
  pre-existing *before* any edit lands.

**Done when:** a baseline note exists with build times, blast-radius counts and the ctest
result. Every later stage is measured against it.

---

## Stage 1 — Tree hygiene (delete dead weight)

| Path | Why |
|---|---|
| `test.cpp` | 11,624 lines; the retired monolithic suite, superseded by `tests/engine/**`. Not in any glob or target. |
| `editor/editor_test.cpp` | 8,583 lines; same, superseded by `tests/editor/**`. Explicitly filtered out at `CMakeLists.txt:219`. |
| `draw` | 0-byte stray at the repo root. |
| `includes/stb_image_write.h` | No include path points at root `includes/` and nothing includes it; three live copies exist under `libs/`. |

Before deleting, a coverage read-through: skim the retired files' test names against
`tests/engine/**` and `tests/editor/**` and **report any test that has no live counterpart**
rather than silently dropping it. Content stays recoverable from history.

Then the references that become stale:

- `CMakeLists.txt:216-219` — the comment and the `list(FILTER ...)` exclusion for
  `editor/editor_test.cpp` can both go; the regex reduces to `/editor/editor\.cpp$`.
- [`docs/shimmer-repro.md:146`](../docs/shimmer-repro.md) references `test.cpp`; it gets a
  pointer to the live suite. `plans/2026-09-29_terrain-smooth-tiles.md` also references it but
  is a historical design record and stays as-is.

**Verify:** `cmake -B build -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` reconfigures clean,
`cmake --build build --target toyengine_tests toyengine_editor_tests -j4` links,
`ctest --test-dir build -j4` still 157/157.

---

## Stage 2 — Documentation and the `.claude/` change map

Done early: it is zero-risk, and the map is what the later stages navigate by. Each later
stage then **updates the doc section it invalidates as part of its own definition of done** —
docs never go stale behind the refactor.

### 2a. `.claude/CLAUDE.md` — add a "Where does this change go?" part

Keep the existing build/test content verbatim (it is accurate and hard-won). Add a second
part, each entry naming the files to touch, the order to touch them in, and the narrowest
build + test command:

- **In-game / content changes** (scenes, materials, meshes, prefabs, UI, particles, terrain,
  water, animation) → these are *asset* changes and already have skills. A table mapping the
  ask to its skill (`create-scene`, `create-material`, `create-mesh`, `create-object`,
  `create-ui`, `create-particle-effect`, `create-terrain`, `create-heightmap-terrain`,
  `create-water-body`, `create-animation`, `create-fog-volume`, `create-surface-shader`,
  `create-texture`, `create-palette-or-lut`, `create-physics-material`, `create-ui-theme`),
  with the one rule that spans them: no C++ rebuild needed, but a new *component key* needs
  the engine chain below.
- **Engine changes — a new component.** The full chain, currently undocumented and
  rediscovered each time: component header + `.cpp` in `toyengine/<area>/` → parser in
  `toyengine/scene/register.cpp` (or the area's own `*_yaml.cpp`) → system update order →
  inspector schema in `editor/schema/component_schema.cpp` → the `create-scene` skill's
  `reference/components.md` key table → a unit test in `tests/engine/unit/`. Plus: a
  *project*'s own component goes through `TOY_MODULE()`
  ([`toyengine/core/module.h`](../toyengine/core/module.h)) instead of `register.cpp`.
- **Render changes.** Which file for which kind of change: a new config key
  (`render/toy_render_config.h` + `.cpp` → `core/config.cpp` →
  `editor/schema/settings_schema.cpp` → the quality-preset table, and the trap that *writing a
  preset-covered key in `config.yaml` makes its `*_quality` tier inert*); a new pass
  (`render/passes/`, the `build_*`/`record_*` seam — Stage 5 makes this a named file); a new
  debug channel (the `debug_view` string, `debug_view.frag`, and the `ExtraSets` set-index
  hazard). Links to [`toyengine/render/README.md`](../toyengine/render/README.md) and
  [`render/passes/README.md`](../toyengine/render/passes/README.md) rather than restating them.
- **Shader changes.** The two traps, written down where they will be read: `assets/shaders/`
  is what loads (the `libs/gfxcoopa/assets/shaders/` copy is a silent no-op for engine
  shaders), and `.glsl` edits do nothing until
  `cmake --build build --target toyengine_shaders`. Plus the `assets/shaders/gfx/` shared-body
  layout and which `*_body.glsl` includes feed which pass.
- **Editor changes**, split by kind:
  - *Viewport tools* — `editor/viewport/` (`gizmo.h`, `modal_transform.h`, `rect_gizmo.h`,
    `editor_camera.h`) and `editor/mesh/` (`mesh_ops`, `sculpt`, `mesh_loops`,
    `mesh_subdivide`, `auto_weights`, `mesh_bvh`); keymaps and the Blender-modal conventions.
  - *Visual presentation* — `editor/app/editor_theme.h`, `editor/ui/ui_palette.h`,
    `editor/themes/`, and where uicoopa's immediate-mode widgets end and the editor's own
    drawing begins.
  - *Component inspectors* — `editor/schema/component_schema.h`'s
    `FieldKind` / `FieldDesc` / `ComponentSchema`, `register_component_schema()`,
    `editor/schema/inspector.cpp`'s row rendering, and the "no reflection in the engine,
    schemas only describe" contract from `component_schema.h`'s own file comment. The
    most-requested editor change, with no entry point documented anywhere today.
  - *Panels and layout* — `editor/app/ui/` (post-Stage-7: one panel = one class = one header +
    one `.cpp`), `layout.cpp` for the Blender-style region split.
  - *Settings screens* — `editor/schema/settings_schema.cpp` (render + project settings rows).
  - *Build / packaging* — `editor/build/` (`build_pipeline`, `packager`, `bundle_linux`,
    `bundle_macos`, `deps`).
  - *Editor user docs* — the `editor-user-docs` skill and `tests/editor/docs/docs_test.cpp`
    screenshot tool.
  - The narrow build/test commands per area (`--target toyengine_editor_lib`, `ctest -L unit`,
    `ctest -L gpu -R <area>`), and the standing rule: **rebuild `toyengine_editor` after
    engine changes** — scenes are played in the editor, and a stale binary silently drops new
    components.
- **Hub changes** — `hub/hub_app.h` (pages, UI), `hub/hub_core.h` (project list, settings,
  `~/.toyengine/`), `tools/toyhub` (the script the hub is a front end over: the hub must never
  duplicate project logic), `templates/project/` (what `toyhub new` creates). Build
  `--target toyengine_hub`; smoke-run
  `HEADLESS=1 MAX_FRAMES=30 HUB_SCREENSHOT=out.png HUB_PAGE=projects ./build/toyengine_hub`.

### 2b. READMEs

New module READMEs, each written from the source in the style of the existing ones (what the
module owns, the data flow in, the contracts a caller must respect — not a file listing):

- `toyengine/audio/`, `toyengine/debug/`, `toyengine/destruct/`, `toyengine/ui/`
- [`editor/README.md`](../editor/README.md) gains a short per-subdir map; new READMEs for
  `editor/schema/`, `editor/mesh/`, `editor/viewport/`, `editor/build/`, `editor/core/`, and
  `editor/app/` (the last is where the EditorApp/panel architecture gets written down —
  authored at Stage 7, stubbed here).
- `hub/README.md` — the hub / `toyhub` division of labour, pages, `~/.toyengine/` files.
- `tests/README.md` — the tier system (`unit` / `gpu` / `extended`), the
  suite-name-is-the-file-stem rule, `SLOT gpu:2`, and how to add a suite with no CMake edit.
  Most of this exists only as comments in `tests/CMakeLists.txt`.

Audit the existing READMEs against the code (`render/` 283 lines, `world/` 626, `scene/` 89,
`core/`, `water/`, `weather/`, `particles/`, `save/`, `render/passes/`) and correct drift found
while reading those modules in the later stages.

### 2c. Root `README.md`

- Fix the "Project layout" tree: add `weather/`, `save/`, `debug/`, `destruct/`,
  `render/passes/`, and the top-level `tests/`, `tools/`, `plans/`, `configuration/`.
- The claim "Each header declares, and its `.cpp` beside it holds the function bodies" has
  three real exceptions — `hub/` is header-only, `editor/app/ui/*.inl` are class-body fragments
  (removed by Stage 7), and the hot math headers are inline by design. State the rule and its
  exceptions rather than a rule the tree contradicts.
- Point the "Documentation" section at the new per-subdir READMEs.

### 2d. Retire the pixel-art framing

The engine is a full-featured 3D engine; it is not a pixel-art engine. The low-resolution
render target and its nearest-neighbour upscale are a **retained capability** for anyone who
wants that look, and the outline / palette / dither / texel-AA stylisation stays exactly as it
is. What goes is the framing — every claim that this is what the engine is *for*, or that the
frame graph exists to serve it.

Of 29 occurrences across the first-party tree (`PixelRender` / `pixel_render` / `PixelArt`
appear **nowhere** — if you remember those names, they are already gone), three categories:

**A. Framing to delete — the engine described as a pixel-art engine.**

| File | Current wording |
|---|---|
| `tools/gen_material_maps.py:19` | "this is a pixel-art engine, and a chunky, blocky brick texture is more in …" |
| `tools/gen_noise_mask.py:21` | "this is a pixel-art engine, and a handful of chunky noise texels …" |
| `toyengine/render/toy_render_pipeline.h:3` | "Orchestrates the low-resolution deferred pixel-art frame graph." |
| `toyengine/render/toy_render_pipeline.h:187` | "Renders a scene through the low-resolution deferred pixel-art frame graph" |
| `toyengine/render/toy_render_types.h:3` | "GPU-layout structs shared across the pixel-art frame graph" |
| `toyengine/render/README.md:7` | "the struct defaults 480×270 for a pixel-art look" |
| `toyengine/render/passes/upscale_pass.h:5` | "its pixel-art look" |
| `editor/viewport/gizmo.h:6` | "the (low-resolution, pixel-art) scene image" |
| `hub/hub_app.h:113` | "pixel-art toggles" |
| `toyengine/render/toy_render_config.h:131` | "the pixel-art post-process stack" |

The frame graph is a **deferred PBR frame graph that renders at a configurable internal
resolution**. That is both accurate and a strict superset of what the old wording claimed.

**B. Reword to name the mechanism, not the genre.** These describe real, retained features
whose only problem is being introduced by genre:

- *Texel-AA sampling* — `assets/shaders/gfx/texel_aa.glsl:4`, `gbuffer.frag:4`,
  `gfx/surface/gbuffer_fs.glsl:122`, `foliage.frag:4`, `toyengine/core/engine.cpp:88`,
  `toy_render_config.h:781`. This is *anti-aliased point sampling for hard-edged texture
  atlases* — say that. It is why tile atlases stay crisp under a free 3D camera, which has
  nothing to do with the game's art style.
- *The low-res render target and its upscale* — `assets/shaders/upscale.frag:4`,
  `ui_composite.frag:13`, `render/README.md:193,226`, `passes/README.md:20`,
  `toy_render_config.h:1119,1291`, `toy_render_pipeline.cpp:1180,2315`,
  `tests/engine/extended/temporal_stability_test.cpp:997`. "the pixel-art upscale" → "the
  low-resolution upscale"; "part of the pixel-art image" → "part of the upscaled scene image".
- *Stylisation toggles* — `editor/app/ui/viewport_chrome.cpp:960-961` ("the banded pixel-art
  look", "the pixel-art edge outline") → "the banded stylised look", "the stylised edge
  outline". `tests/editor/docs/docs_test.cpp:41` ("no pixel-art post effects") → "no
  stylisation post effects".
- `.claude/skills/create-texture/SKILL.md:31` ("pixel-art scaled") → describe the sampling.

**C. Leave as-is.** `.claude/skills/create-palette-or-lut/SKILL.md:13` already says the palette
look "is legacy -- don't make it a scene's default without being asked", and
`.claude/skills/editor-user-docs/SKILL.md:69` already forbids showing the stylisation effects
in manual screenshots — both state the intended position and only get terminology alignment.
[`docs/shimmer-repro.md`](../docs/shimmer-repro.md) is a historical investigation record.
`CMakeLists.txt:273` names the sibling repo `pixengine` in a factual comparison about the
Vulkan leak gate — a real external repo, not framing; flagged in case you want it gone too.
Feature names that describe a mechanism (`camera_pixel_snap`, `render_width` in pixels) are
kept: they are not genre claims.

Also removed for free: `pixel_demo` survives only inside the two dead test files Stage 1
deletes — the scene itself is already gone (`assets/scenes/` is now organised by area:
`rendering/`, `terrain/`, `water/`, …), so no live reference to it remains after Stage 1.

**D. The default internal resolution — the one behaviour change in this plan.**

`ToyRenderConfig`'s struct defaults are still `resolution_mode: "fixed"`, `render_width: 480`,
`render_height: 270` — the last place where pixel-art is the engine's *default*. They change to
**`"fixed"`, `1920`, `1080`**.

Scope of the change is narrow, which is why it is safe: `assets/config.yaml` already sets
1920×1080 explicitly (lines 110-112), and `Project::default_config()`
(`editor/app/project.cpp:475`) copies that file into every new project — so neither this repo
nor a hub-created project is affected. Only a run with *no* config at all currently gets
480×270. Resolution is not covered by `apply_quality_presets()` (checked
`toy_render_config.cpp:55`), so no `*_quality` tier desyncs. Every test suite that cares sets
`resolution_mode` / `render_width` / `render_height` explicitly (`display_rect_test.cpp`,
`sky_render_test.cpp`), and `app_config_test.cpp`'s default-value assertions cover
`dither_strength` and `exposure`, not resolution.

I checked `"divisor"` with `scale_divisor: 1` first, since "native resolution, follows the
window" is the better-sounding default — **it does not work today.** `render_extent_` is
computed once in `ToyRenderPipeline`'s constructor (`toy_render_pipeline.cpp:49`) and
`handle_resize_()` only rebuilds the overlay and display layers; the *only* mode that re-derives
the extent on a window resize is `"fill"`, via `Engine::update_fill_extent_()`
(`engine.cpp:1778`), which early-returns on anything that is not `"fill"`. A `divisor` default
would therefore freeze the render extent at the startup window size and silently scale after
any resize. `"fixed" 1920×1080` keeps exactly the resize semantics the engine has today and
changes nothing but the number.

Four places carry the default and **must move together** — a mismatch here is the same class of
silent desync as the quality-preset trap:

| File | Change |
|---|---|
| `toyengine/render/toy_render_config.h:284-285` | `render_width = 1920`, `render_height = 1080` |
| `toyengine/render/toy_render_math.h:45-46` | `RenderExtent`'s own member defaults, to match (these move to `render/render_extent.h` in Stage 3b) |
| `editor/schema/settings_schema.cpp:81-82` | `f_int("render_width", 1920, 16, 7680)`, `f_int("render_height", 1080, 16, 4320)` — this is the default the editor's Render Settings displays and resets to |
| `toyengine/render/README.md:7` | the sentence that cites the old default (already in table A) |

`scale_divisor` stays at 4: it is only read in `divisor` mode, and changing it would be churn
without a reason.

`assets/config.yaml:110-112` becomes redundant but is **kept** — the engine repo's capture and
test workflow wants the resolution stated outright rather than inherited. If a clarifying
comment is added there it must be a **single unwrapped line**: a wrapped comment in
`config.yaml` silently voids the rest of the render block.

**Verify:**

- `grep -ri "pixel.art\|pixengine\|pixelart"` over
  `toyengine editor hub tests assets docs templates tools cmake README.md .claude` returns only
  the Category C lines.
- `grep -rn "\b480\b\|\b270\b"` over `toyengine/ editor/ hub/` returns no resolution defaults
  (the remaining hits are an unrelated UI layout width in `editor/ui/ui_palette.cpp:208` and a
  gizmo angle in `viewport_chrome.cpp:625`).
- The A/B/C wording edits are comment-only, including the two shader files — confirm the edits
  sit above the first `#version`/code line, and then no `toyengine_shaders` rebuild is needed
  and no rebuild at all is needed for the rest (per CLAUDE.md: never rebuild after a doc-only
  change).
- Category D is a real code change: `cmake --build build --target toyengine_tests
  toyengine_editor_tests -j4`, then `ctest -L unit -j4` with particular attention to
  `app_config` and `display_rect`, then
  `ctest -L gpu -R "render_pipeline|display|settings" -j4`.
- A config-less sanity run — `HEADLESS=1 MAX_FRAMES=5` against a scene with the render block
  removed — should now produce a 1920×1080 image rather than 480×270.

**Interaction with Stage 5:** Stage 5 rewrites `toy_render_pipeline.h`'s banners and file doc.
Doing 2d first means Stage 5 starts from the corrected wording; Stage 5's definition of done
includes *not* reintroducing it.

---

## Stage 3 — Header diet (largest build-time win, lowest risk)

### 3a. `toyengine/core/engine.h`

Audit all ~45 project includes against what the header's *declarations* actually name; move
the rest into `engine.cpp`. Verified unused-by-declaration so far: `world/snow_system.h`,
`world/terrain_system.h`, `scene/register.h`, `save/register.h`,
`coopa/animation/animation_system.h`. Another ~13 appear exactly once and are almost certainly
doc-comment mentions only (`weather/weather_reactor.h`, `coopa/animation/ik_system.h`,
`uicoopa/audio/sound_library.h`, `gfxcoopa/engine/loaders/{mesh,texture}_loader.h`,
`gfxcoopa/util/image_readback.h`, `particles/particle_yaml.h`, `render/visibility.h`,
`scene/free_mover.h`, `scene/kinematic_control_system.h`, `scene/scene_link.h`,
`uicoopa/ui_yaml.h`, `physxcoopa/physx_yaml.h`, `sfxcoopa/sfx_yaml.h`) — each checked by hand,
not by count.

Removing an include from a 49-consumer header breaks consumers that were relying on it
transitively. That is the point, and the fix is per-consumer: each TU adds the include it
actually needs. Expect a first build with many errors and a short, mechanical fix pass.

While in the file: `engine.h` interleaves `private:` / `public:` / `private:` / `public:` four
times (around lines 476, 484, 498, 500). Consolidate into one public then one private section,
keeping the existing doc comments and the **load-bearing member declaration order** in the
private data block (`jobs_` before `ctx_` before `boot_splash_` before `pipeline_`; `audio_`
before `scene_mgr_`) — that ordering is commented as destruction-order-critical and must not
move.

### 3b. Split `toyengine/render/toy_render_math.h` (831 lines, 4 unrelated topics)

Included by `engine.h`, so all 49 TUs pay for all of it. It is pure `inline` functions over
plain data — ideal to split, and already independently tested
(`tests/engine/unit/shadow_fit_test.cpp`):

| New header | Contents | Who needs it |
|---|---|---|
| `render/render_extent.h` | `LetterboxRect`, `RenderExtent`, `compute_render_extent` / `letterbox` / `fit` / `display_rect` / `pixel_density` | `engine.h` — its only need is `LetterboxRect` and `RenderExtent` |
| `render/shadow_fit.h` | `ShadowFitCamera`, `DirShadowFit`, `CascadeSplits`, `CascadeTile`, the cascade / clipmap / sphere / slice fits, `compute_shadow_normal_bias`, `snap_light_direction` | the pipeline and its shadow tests only |
| `render/sdf_clip.h` | `SdfClipRect`, `PixelRect`, `compute_sdf_clip_rect`, `sdf_clip_rect_to_pixels` | the pipeline's SDF gather only |
| `render/view_math.h` | `view_space_depth`, `view_depth_half_extent`, `world_bounds_center`, `exp_smooth_toward` | several |

`toy_render_math.h` stays as a header that includes all four, so no existing include breaks;
`engine.h` and the pipeline then narrow to what they use. Each function's doc comment travels
with it verbatim.

**Verify:** blast-radius counts re-measured against Stage 0 (the headline number for this
stage), `ctest -j4` green, and `ccache -s` to confirm the win is real compile avoidance rather
than cache luck.

---

## Stage 4 — One YAML read helper

The pattern `if (node.contains(k)) out = node.at(k).get_value<T>();` occurs ~726 times behind
~20 independently re-declared helpers. Add **`toyengine/core/yaml_read.h`** (header-only,
`inline`, no new dependency — fkYAML and glm are already everywhere):

```cpp
namespace toy::core::yaml {
/// Assigns node[key] to out if the key is present; leaves out untouched otherwise.
template <class T> bool read(const fkyaml::node& n, const char* key, T& out);
bool read(const fkyaml::node& n, const char* key, glm::vec3& out);   // {x,y,z} or [a,b,c]
bool read(const fkyaml::node& n, const char* key, glm::vec2& out);
bool read_rgb(const fkyaml::node& n, const char* key, glm::vec3& out);
bool read_degrees(const fkyaml::node& n, const char* key, float& out_radians);
template <class E> bool read_enum(const fkyaml::node&, const char* key, E& out,
                                  std::initializer_list<std::pair<std::string_view, E>>);
float num(const fkyaml::node& n, float def = 0.0f);   // int-node-tolerant float
}
```

The `float`-accepts-`int` tolerance (`particle_yaml.cpp`'s `num`, `editor/core/yaml_util.h`'s
`as_float`) and the `{x,y,z}`-or-sequence vector spelling both already exist in three places
each; this becomes the one place they live.

Call sites to convert, largest first: `toyengine/scene/register.cpp` (871 lines, 15 duplicate
lambda definitions — the single biggest readability win in the engine),
`toyengine/core/config.cpp` (556), `toyengine/world/heightmap_terrain.cpp`,
`toyengine/particles/particle_yaml.cpp`, `toyengine/weather/weather_profile.cpp`,
`toyengine/world/snow_system.cpp`, `toyengine/audio/audio_components.cpp`,
`toyengine/core/runtime_paths.cpp`, `toyengine/save/save_game.cpp`,
`toyengine/world/terrain_generator.cpp`.

Then `editor/core/yaml_util.h` keeps its editor-facing API (`get_float`, `as_vec3`, `tidy`,
`make_vec3`, `erase_key`, `keys_of` — the editor also *writes* nodes, which the engine never
does) but delegates its readers to `yaml_read.h`, so the two can no longer disagree about what
`{x, y, z}` means.

**Verify:** scene / config / particle / weather parsing is covered by `ctest -L unit` (~4 s) —
run it after each file. Then
`ctest -L gpu -R "scene|config|particle|weather|terrain|water"`. A converted parser that
silently stops reading a key is the risk; `-L unit` plus the GPU scene suites are the net.

---

## Stage 5 — Split `ToyRenderPipeline`

5,167-line `.cpp`, 1,937-line header, ~60 methods. The methods already fall into clean
families by prefix, and the header is only included by `engine.cpp`, `editor_app.cpp` and the
GPU tests — so this is a readability and navigability change, not a build-time one, and it can
be done as pure file moves.

Split `toy_render_pipeline.cpp` into sibling TUs, each holding the same member functions
unchanged:

| New TU | Methods (existing prefixes) | ~lines |
|---|---|---|
| `toy_render_pipeline.cpp` | ctor, `render()`, config / live-apply, accessors, resize | ~900 |
| `pipeline_build.cpp` | `build_frame_descriptors_`, `build_geometry_passes_`, `build_sdf_passes_`, `build_lighting_passes_`, `build_ssr_passes_`, `build_transparency_passes_`, `build_post_chain_`, `build_world_ui_descriptor_`, `rebuild_*` | ~1,100 |
| `pipeline_gather.cpp` | `gather_meshes_`, `gather_ui_canvases_`, `gather_sdf_`, `snapshot_scene_`, `tick_static_instances_` | ~900 |
| `pipeline_shadows.cpp` | `update_dir_shadow_matrix_`, `record_directional_shadow_`, `update_local_shadows_`, `record_local_shadows_`, `update_lights_` | ~850 |
| `pipeline_sky_fog.cpp` | `record_physical_sky_`, `record_clouds_`, `record_cloud_overlay_`, `fill_fog_block_`, `record_fog_`, `update_volumetrics_data_`, the `*_quality_steps_` | ~700 |
| `pipeline_record.cpp` | `record_scene_`, `record_gbuffer_`, `record_transparent_`, `record_transparent_preview_`, `record_post_chain_`, `record_overlay_`, `record_reactive_mask_`, `record_focus_probe_`, the layout transitions | ~1,200 |

The header's `private:` section gets matching `// --- <family> (pipeline_<file>.cpp) ---`
banners so a declaration points at its body, in the style `editor/app/editor_app.h` already
uses for its panels. **No signature, no member and no ordering changes** — every `.cpp`
includes `toy_render_pipeline.h` and nothing else moves. The header's interleaved
`public:` / `private:` runs (lines 193, 476, 584, 588, 1300, 1308) get consolidated the same
way `engine.h` does in Stage 3.

The one judgement call: the header is 1,937 lines mostly because its private member block is
richly documented. That documentation is the file's main asset and stays. If the header still
reads as a wall afterwards, the follow-up is a private `struct Impl` for the pass-object
members — **not** in this plan, flagged for a decision later.

Stage 2d rewrote this header's file doc and `render()`'s brief; the rewritten banners and
section comments must not reintroduce the pixel-art framing.

**Why last among the code stages:** `toy_render_pipeline.cpp/h` are modified in the working
tree, and `dir_shadow_cache.*` / `caster_grid.h` are new and unintegrated. Doing this stage
last keeps that work out of the blast radius.

**Verify:** `--target toyengine_tests -j4` (the heaviest TU, `toy_render_pipeline.cpp`, peaks
at 2.4 GB — `-j4`, per CLAUDE.md), then `ctest -L gpu -j4` in full, since this stage touches
every render path. Byte-identical captures are implied by the GPU suites' own pixel assertions.

---

## Stage 6 — Thin `Engine`

~80 methods across concerns that do not belong together. Two extractions, both
behaviour-preserving (same call order from `tick()`, same state):

1. **`toyengine/core/scene_render_bridge.{h,cpp}`** — the `sync_*_render_state_` family that
   translates scene state into render config: `sync_weather_render_state_`,
   `sync_sky_render_state_`, `sync_cloud_state_`, `sync_surface_state_`,
   `sync_water_render_state_`, `sync_particle_render_state_`, `restore_sky_colours_`,
   `restore_weather_atmosphere_`, `water_settings_for_`, `weather_node_`, plus the state they
   own (`sky_atmosphere_`, `sky_base_`, `sky_applied_`, `cloud_offset_`, `cloud_flat_phase_`,
   `sky_time_`, `weather_base_`, `weather_applied_`, `k_sky_gain`, `k_moon_ratio`,
   `particle_frame_`). ~600 lines out of `engine.cpp`, and ~15 of `engine.h`'s includes
   (`weather/*`, `water/water_system.h`, `world/snow_system.h`, `render/particle_types.h`)
   follow it out. This is the biggest single reason `engine.h` is heavy, so it compounds
   Stage 3.
2. **The async scene-load state machine** — `update_scene_load_`, `apply_warmup_budgets_`,
   `poll_scene_warmup_`, `activate_scene_load_`, `apply_spawn_point_`, `cancel_scene_load_`,
   `finish_transition_`, `consume_scene_link_requests_` move into the existing
   [`toyengine/core/scene_loading.{h,cpp}`](../toyengine/core/scene_loading.h), which already
   owns `SceneLoadState`, `SceneLoadOptions` and `SceneLoadHandle` — the logic is simply in the
   wrong file today. ~350 lines.

`Engine` keeps what is genuinely its own: the frame loop, the Vulkan context and pipeline
lifecycle, input, scene management, hooks, capture, the config-rebuild debounce, and the
documented member *declaration order* in its private block (destruction-order-critical —
anything moved out must not perturb it).

The public `Engine` API does not change: the extracted types are held as members and called
from the same points in `tick()`.

**Verify:** `ctest -L unit`, then
`ctest -L gpu -R "engine|scene|weather|sky|water|snow|particles"`, then a headless run of a
weather scene and a water scene (`HEADLESS=1 MAX_FRAMES=120 ./build/toyengine <scene>`) since
the sync family is per-frame and order-sensitive. Finally the full `ctest -j4`.

---

## Stage 7 — `EditorApp` panel extraction (phased)

Today [`editor/app/editor_app.h`](../editor/app/editor_app.h) `#include`s 25 `ui/*.inl` files
*inside the class body*, so every panel's declarations are in one class and every panel `.cpp`
includes `editor_app.h` — touching any panel's `.inl` recompiles all 25 panel TUs plus
`editor_app.cpp`.

### What makes this tractable

Of ~1,000 identifiers declared across `editor_app.h` + the `.inl` files, only **15 are used by
6 or more panels**: `engine_`, `project_`, `doc_`, `et_` (theme), `apply_`, `inspector_env_`,
`active_type_`, `asset_view_`, `mesh_`, `tool_`, `prop_tab_`, `pending_modal_`, `test_rects_`,
`effective_*`, `sync_*`. Everything else is either panel-private or shared by a small cluster.
Per-panel coupling (distinct `EditorApp` identifiers touched):

```
browser 3   particle_panel 6   tile_terrain_panel 11   project_settings 18   theme_editor 25
build 27    layout 28   statusbar 28   outliner 35   world_generator 36   weights 37
tile_mode 45   terrain 46   instances 49   weather 53   sculpt 54   mesh_tools 57
topbar 60    paint 68    timeline 89    ui_canvas 102   assets 121   viewport_chrome 133
properties 149
```

### Target shape

```cpp
// editor/app/editor_context.h  — small, stable, the only header every panel includes
class EditorContext {           // the ~15 genuinely shared things, by reference/accessor
    Engine& engine(); Project& project(); SceneDocument& doc(); const EditorTheme& theme();
    void apply(Change); InspectorEnv inspector_env(); AssetType active_type(); ...
};

// editor/app/ui/properties.h   — forward-declares EditorContext only
class PropertiesPanel {
public:
    explicit PropertiesPanel(EditorContext& ed);
    void draw(imm::Context&, const imm::Box& area);
private:
    EditorContext& ed_;
    /* the 47 members only this panel touches */
};
```

`editor_app.h` holds panels as `std::unique_ptr<PropertiesPanel>` behind **forward
declarations only** — it includes no panel header at all. A panel header is then included by
its own `.cpp` and `editor_app.cpp`, so editing `properties.h` rebuilds two files instead of
twenty-six.

Clusters that share state get a state object rather than 25 isolated classes (this is what the
coupling data shows; forcing flat panels would just push the state back into `EditorContext`):

- `MeshModeState` — `mesh_tools`, `sculpt`, `paint`, `weights`, `instances`
- `TerrainEditState` — `terrain`, `tile_mode`, `tile_terrain_panel`, `world_generator`
- `UiDesignerState` — `ui_canvas`, `theme_editor`
- `TimelineState` — `timeline`

### Phasing (each phase independently green)

1. **`EditorContext`** extracted and threaded, `.inl` pattern untouched. No panel moves yet;
   this is the enabling step and must leave behaviour identical.
2. **Low-coupling panels** (3-28 identifiers): `browser`, `particle_panel`,
   `tile_terrain_panel`, `project_settings`, `theme_editor`, `build`, `layout`, `statusbar`.
   Eight panels prove the pattern cheaply. **`editor/app/README.md` is written here**, while
   the design is fresh, and CLAUDE.md's editor section updated to the new shape.
3. **Mid-coupling** (35-60): `outliner`, `world_generator`, `weights`, `tile_mode`, `terrain`,
   `instances`, `weather`, `sculpt`, `mesh_tools`, `topbar` — introducing `MeshModeState` and
   `TerrainEditState` as their clusters land.
4. **High-coupling** (68-149): `paint`, `timeline`, `ui_canvas`, `assets`, `viewport_chrome`,
   `properties`. Here the shared surface is real; expect `EditorContext` to grow a little, and
   justify each addition.
5. **Cleanup**: `editor_app.h` loses all 25 `.inl` includes; `editor_app.cpp` (currently 180
   method bodies, 3,772 lines) keeps only app-level concerns — document lifecycle,
   open / save / package, play / stop, undo / redo, picking, the frame hooks. Oversized panel
   `.cpp`s still large after extraction (`assets.cpp` 1,487, `ui_canvas.cpp` 1,332,
   `timeline.cpp` 1,160, `properties.cpp` 1,111) get split by sub-concern *then*, when their
   boundaries are visible.

Risk control: one panel per build/test cycle; `--target toyengine_editor_lib -j4` to
compile-check, then `ctest -L unit` and the editor GPU suites for that panel's area. The editor
GPU suites are slow (50-90 s each) so the full `-L gpu` sweep runs at phase boundaries, not per
panel. `--target toyengine_editor` is rebuilt before any manual check — a stale editor binary
silently drops behaviour.

### Hub

[`hub/`](../hub) is 752 lines of fully-inline `hub_app.h` plus `hub_core.h`, compiled into the
single `hub.cpp` TU. No build cost (one consumer) but it contradicts the root README's stated
convention, and it reuses `EditorTheme`, `LogView`, `Logo` and `EditorCamera` from the editor.
Scope here is deliberately small: split the method bodies out into `hub_app.cpp` /
`hub_core.cpp` so the tree has one convention, and write `hub/README.md` (Stage 2). No
restructuring beyond that — it is small, it works, and it is not where the pain is.

---

## Verification

Per stage, narrowest-first (the ladder `.claude/CLAUDE.md` already prescribes):

1. Compile-check the touched TU: `make -C build toyengine/<path>.cpp.o`.
2. `cmake --build build --target toyengine_tests -j4` (or `toyengine_editor_tests`). Never a
   bare `--build build`; never more than `-j4` outside a memory-capped cgroup; never two heavy
   builds at once; never a header edit while a build is in flight (the ODR-mismatch segfault in
   an unrelated test is documented in CLAUDE.md and is extremely hard to diagnose).
3. `ctest --test-dir build -L unit -j4` (~4 s) after every edit batch.
4. `ctest --test-dir build -L gpu -R <touched system> -j4` — filtered, because unfiltered
   `-L gpu` sweeps in the slow editor UI suites (~20 min).
5. At each stage boundary: full `ctest --test-dir build -j4`, expected **157/157** (plus the
   new `tests/engine/` suites present in the working tree).

End-to-end, beyond tests:

- `HEADLESS=1 MAX_FRAMES=120 ./build/toyengine <scene>` for a weather scene, a water scene, a
  tile-terrain scene and a heightmap scene after Stages 4-6 — the stages that touch per-frame
  state translation. Headless only; never a visible window.
- `--target toyengine_editor` and a manual pass over the Scene / Object / Mesh / Material / UI
  tabs after each Stage 7 phase, since editor behaviour is not fully test-covered. Any render
  artifact check ends with the camera stopping and holding still 1-2 s.
- `--target toyengine_hub` plus
  `HEADLESS=1 MAX_FRAMES=30 HUB_SCREENSHOT=/tmp/hub.png ./build/toyengine_hub` after the hub
  split.
- `coopadocs build` once at the end: the new and split headers must parse with no warnings,
  since every public declaration here is Doxygen-documented.
- Build-time re-measurement against the Stage 0 baseline, with `ccache -s`. The Stage 3 and
  Stage 7 blast-radius numbers are the headline results of this whole effort and get reported
  before/after.

## Not in this plan

Flagged, deliberately out of scope, for a separate decision:

- A private `Impl` struct for `ToyRenderPipeline`'s pass members (revisit after Stage 5).
- `toyengine/render/toy_render_config.h` (1,361 lines) — a flat config struct whose
  per-feature split would churn every consumer for little gain; Stage 3's header diet addresses
  its cost from the consumer side instead.
- [`libs/`](../libs) — all seven are pinned submodules with their own standalone build
  contract; nothing here touches them.
- **`resolution_mode: "divisor"` does not follow a window resize.** Found while choosing the
  new default (Stage 2d, category D): `render_extent_` is computed once in
  `ToyRenderPipeline`'s constructor, and only `"fill"` re-derives it on resize
  (`Engine::update_fill_extent_()` early-returns on every other mode). So a `divisor` config
  renders at the startup window size / divisor forever and scales after a resize, which is not
  what the mode's own documentation implies. Fixing it means extending `update_fill_extent_()`
  to cover `divisor` with the same debounce — a real behaviour change for anyone on that mode,
  so it is flagged here rather than folded in.
- Porting any test coverage found missing in Stage 1's read-through; that is reported as a list
  to prioritise separately.
