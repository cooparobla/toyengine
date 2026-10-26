# environment_demo: editor-style collapsible panel, a puddle and a snow pile

> **Status: LANDED 2026-10-26.** Full suite 182/182, unchanged from the baseline taken before
> the work, so nothing regressed. Eleven new assertions across `env_field` and `env_render`.
>
> [`2026-10-26_fire.md`](2026-10-26_fire.md) is next, and inherits the wetness plumbing below
> (rain suppresses fire) plus the `ext.z` bit layout.
>
> **Difficulty: moderate** — as estimated. Three independent pieces: a UI rewrite plus a
> one-line uicoopa fix, an opt-in material key through the opaque G-buffer backbone, and a new
> local snow channel through the env field. No new system, no new area.

## Why

The environment layer (`toyengine/env/`) and its demo scene work: an `EnvField` of local
influence sources folded over the global weather, sampled by foliage, particles, cloth,
rigidbodies, fog drift, water and lying snow; a forest-clearing demo with six presets switched
from a UI bar by `EnvPresetSwitcher`.

Two things are wanted on top:

1. **The panel should read as a tool, not a game HUD** — collapsible, anchored top-left, with
   editor chrome, so the clearing is not obscured.
2. **Two new presets — a puddle and a local snow pile.** Both exercise the environment *surface*
   channels, which is the half of the system the demo does not yet show.

Neither works today, for the same underlying reason: the surface channels are plumbed but
barely consumed.

- **Wetness** reaches the GPU and `gfx_wet_apply()` exists, but its only callers are
  `assets/shaders/heightmap_terrain_surface.glsl:126` and `assets/shaders/terrain_styled.glsl:326`.
  The demo's ground is a plain `MeshRenderer`, so a puddle is invisible on it.
- **Snow** has only a *subtractive* local channel (`snow_clear`, which stamps the trench field).
  There is no way to *add* snow in one spot, and global cover 0 short-circuits the cover layer,
  the deep-snow height and the trench upload — so a pile needs a term that raises `cover` itself,
  not one that adds a separate amount.

Intended outcome: eight presets in a foldable editor-styled panel, including "Puddle" and
"Snow Pile" that work with the weather switched off entirely.

## Decisions (confirmed with the user)

| | Choice |
|---|---|
| Puddle | Wetness on the opaque backbone, **gated on a new opt-in material key `wet: true`**. Existing scenes render byte-identical. |
| Snow pile | A **real local snow channel** on `EnvSource`, `max()`-combined into cover. Works with no weather. |
| Panel theme | `assets/ui/themes/blender.yaml` — generated from `editor/themes/blender_dark.yaml`, so it is literally the editor palette. |

**A cost correction worth recording.** The snow pile's GPU cost was initially called "trivial" —
true of the UBO (2352 → 2480 bytes), but it understated the shader work. Doing it *fully* also
means reworking the analytic gradients in `assets/shaders/snow_surface.glsl:44-67`, where
`cover` is currently hoisted out of the derivative as a constant. This plan takes the
cheap-but-real version instead: **force the soft deep-snow branch whenever local sources
exist**, which sidesteps that rework entirely and costs one extra condition. Hard-edged snow
patches and a local pile therefore do not combine; everything else does.

---

## 1. The panel: collapsible, top-left, editor-themed

Files: `assets/scenes/effects/environment_demo/ui/presets.yaml` (rewrite),
`libs/uicoopa/uicoopa/builder/ui_composites_yaml.h` (one key),
`editor/schema/ui_schema.cpp` (that key's schema row).

- **Theme**: swap `ui/themes/default.yaml` → `ui/themes/blender.yaml` on the root's `Theme`.
  That alone gives 30 px rows, 17 px body text, 40 px `card_header_height`, 1 px borders,
  `#4772b3` selection and a populated `collapsible:` block.
- **Anchor**: `anchor_min`/`anchor_max` `{0, 1}`, `pivot` `{0, 1}`,
  `anchored_position: {x: 24, y: -24}`. **Y = 1 is the top**, so a top-anchored panel moves
  down with a *negative* y (`assets/ui/hud/hud.yaml:24-30` is the shipped example).
- **Structure**: `Collapsible { title: "Environment", start_expanded: true }` wrapping a
  vertical `MenuList` of the eight presets, with the blurb `ThemedText` beneath it. The bottom
  `ActionBar` row goes away — eight entries want a column.

**One engine fix this needs.** `CollapsibleComposite::expand()` leaves
`CollapsibleOptions::expanded_extent` at `-1`, and `CollapsiblePanel::apply_()` deliberately
never writes a negative extent — so inside a layout group the first collapse writes
`preferred_size.y = header_height` onto the authored object and the re-expand writes nothing.
The panel would stay header-height forever after one fold. The C++ builder path works around
this with `CollapsibleHandle::fit()`; the YAML path has no equivalent.

Fix it at the source rather than in the demo: add an `expanded_extent` key to the composite
(one line, `opts.expanded_extent = num("expanded_extent", -1.0f);`) plus its
`editor/schema/ui_schema.cpp:202` row. **Nothing in the repo uses `Collapsible` from YAML yet**
— a grep for `type: Collapsible` returns nothing — so this is the first real exercise of it,
and worth leaving working for the next caller.

Put the `Collapsible` in a `VerticalLayoutGroup` + `ContentSizeFitter` column so the framed box
shrinks with it, rather than leaving an empty `boxed` frame under the header.

## 2. The puddle: opt-in wetness on opaque materials

| File | Change |
|---|---|
| `libs/gfxcoopa/gfxcoopa/engine/components/mesh_renderer.h:137` | `bool wet = false;` on `PBRMaterial`, next to `bool snow = true;` |
| `libs/gfxcoopa/gfxcoopa/engine/components/register.h:212` | parse the `wet:` key beside `snow:` |
| `toyengine/render/pipeline_sky_fog.cpp:458` | `if (m.wet) ext.z \|= 2u;` — bit 1, uncontested (bit 0 is `snow: false`; `ext.z` has exactly one writer and one reader today) |
| `assets/shaders/gfx/surface/gbuffer_fs.glsl` | include `wetness.glsl` **after** the `GfxSurface` struct (:94-106), hoist `geo_n` above both layers, and call `gfx_wet_apply` between `gfx_surface_fragment(s)` (:175) and the snow block (:177) |
| `assets/shaders/heightmap_terrain.frag`, `terrain_styled.frag`, `terrain_styled_planet.frag` | `#define GFX_SURFACE_NO_WET` — belt and braces against a double-apply if someone sets `wet: true` on a terrain material |
| `editor/schema/component_schema.cpp:266` | the `wet` row beside `snow`, and its `sec("Weather","World")` key list at :311 |

Three details that decide whether this works rather than merely compiles:

- **Include position matters.** `gfx_snow_apply` is a *macro*, so it survives being defined
  before the struct; `gfx_wet_apply` is a real function taking `inout GfxSurface`, so its
  include must come after it.
- **The puddle normal must be gated on uprightness.** `gfx_wet_apply` forces `s.normal_ws`
  toward world `+Z` by the `puddle` argument, which is only right on up-facing geometry — pass
  `puddle = smoothstep(0.96, 0.995, geo_n.z)`, as both existing callers do. Without it, walls
  get mirror-flat upward normals.
- **The gate cannot copy the snow one.** `gfx_world.snow.z > 0.0` (global wetness) would skip a
  local puddle in clear weather — exactly our case. Gate on
  `(gfx_world.snow.z > 0.0 || gfx_world.terrain_count.y > 0)`.

Known limitation to document, not fix: `gfx_wet_apply` multiplies by `gfx_world_open_sky()`, so
a local puddle *under a roof* is multiplied to zero wherever the precipitation map is valid.
Correct for weather, wrong for a burst pipe indoors.

**Why opt-in rather than on by default.** The stock `rain`/`storm`/`overcast` conditions carry
wetness 0.8/1.0/0.25, so an unconditional backbone call would change every wet scene and shift
`render_pipeline_test`'s strict pixel-diff and pico-8 palette assertions. The `wet:` key leaves
flipping the default a one-line decision for later.

## 3. The snow pile: a local additive snow channel

The channel is the mirror of `snow_clear`, and must raise `cover` rather than add an amount — at
cover 0 the patch threshold is unreachable (`edge = 1 - cover*1.25`), so a separate additive
term would still mask to nothing.

| File | Change |
|---|---|
| `toyengine/env/env_source.h` | `float snow = 0.0f;` beside `snow_clear`, and in `scale_channels_` |
| `toyengine/env/env_field.h` | `snow_at()` via the existing `env_fold_scalar_` |
| `toyengine/env/env_influence.cpp` | parse `snow:`; include it in `contributes()` |
| `toyengine/env/env_system.cpp` | the pulse-envelope scaling list |
| `toyengine/render/surface_world.h` | **a 4th `vec4` on `EnvSourceGpu`** (surface channels: wetness moves here, snow joins it, two spare) and both `static_assert`s |
| `toyengine/render/surface_world.cpp` | pack it |
| `assets/shaders/gfx/surface/world.glsl` | mirror the struct; add `gfx_env_snow_at(vec3)` cloned from `gfx_env_wetness_at` |
| `assets/shaders/gfx/surface/snow.glsl:100` | the `gfx_snow_apply` macro's cover → `max(gfx_world.snow.x, gfx_env_snow_at((s).position_ws))` |
| `assets/shaders/gfx/surface/snow_height.glsl:21` | same `max()` in `gfx_snow_lying_height` |
| `assets/shaders/gfx/surface/gbuffer_fs.glsl:179` | call-site gate widened as for wetness |
| `assets/shaders/snow_surface.glsl:32, :39` | early-out widened, **and force the soft branch when `terrain_count.y > 0`** — the simplification above |
| `toyengine/world/snow_field.h:298` | `deep_snow_depth()` gains the local term so `SnowSystem::depth_at()` keeps mirroring the shader |
| `toyengine/world/snow_system.cpp:50` | the `cover_ <= 0` early-out must not skip a scene whose snow is purely local |

`SnowField` cannot be reused for this: it is **unorm16, strictly non-negative**, `stamp()`
rejects `depth <= 0` outright and max-combines upward, and it is read *subtractively* on both
CPU and GPU. It represents trenches only.

Keep `env_field.h`, `world.glsl` and `surface_world.cpp` in step — the three-way CPU/GPU mirror
the existing tests already pin.

## 4. The two new presets

In `assets/scenes/effects/environment_demo/scene.yaml`, as subtrees beside the existing five:

| Preset | Content |
|---|---|
| `preset_puddle` | An `EnvInfluence { shape: sphere, radius: 4.5, falloff: 0.7, wetness: 1.0 }` over the clearing centre, a drip `ParticleSystem`, and a leaking standpipe prop. The ground material gains `wet: true`. Condition `still`. |
| `preset_snowpile` | An `EnvInfluence { shape: sphere, radius: 4, falloff: 0.6, snow: 0.95 }`, a few boulders to catch the cover, and `snow_max_depth: 0.35` in the scene's weather block so the deep-snow term has something to scale. Condition `still` — the point is that it works with no snowfall. |

Both get a `button:` in the panel's `MenuList` and a blurb. `EnvPresetSwitcher` needs no change
— it already activates one subtree, replays it and blends the weather.

## Build and test sequence

`toyengine/render/surface_world.h` is one of the headers that invalidates nearly every TU.
**Finish every header edit before the first build.**

```bash
# Stage A -- uicoopa Collapsible key + schema (editor-side only)
cmake --build build --target toyengine_editor_tests -j4
ctest --test-dir build -L unit -j2

# Stage B -- the env/render/shader batch. ALL header edits first, then one build.
cmake --build build --target toyengine_shaders            # after any .glsl edit
cmake --build build --target toyengine toyengine_tests -j4
./build/tests/toyengine_tests --suite env_field
./build/tests/toyengine_tests --suite env_system

# Stage C -- the scene and panel
HEADLESS=1 NO_INPUT=1 MAX_FRAMES=90 ./build/toyengine environment_demo
./build/tests/toyengine_tests --suite env_render

# Stage D -- the suites most at risk from a G-buffer change
ctest --test-dir build -L gpu -R "render_pipeline|heightmap_terrain_render|snow_render|weather_scene|terrain_render|material_maps" -j2

# Stage E -- editor binary (a stale one silently drops new component keys)
cmake --build build --target toyengine_editor -j4

# Stage F -- the full suite, ONCE, at the end
ctest --test-dir build -j2
```

Per the project build rules: `-j4` for builds, `-j2` for ctest, one heavy job at a time, and
`run_in_background` rather than detaching. Note the guard picks its cap by scanning the WHOLE
command for `\bctest\b`, so chaining a build and a test run on one line applies the test cap of
2 to the build as well -- keep them in separate invocations.

## What the implementation changed about this plan

- **`ext.z` bit 2 is now the next free one** (0 = `snow: false`, 1 = `wet: true`). Fire's
  `burns: false` takes 4u, as its plan already assumes.
- **`SnowSystem` needed more than the plan said.** `depth_at()` is a *const* query callable
  between frames, so the env field had to be latched per frame into the system rather than
  sampled on demand; and a purely local pile in a scene whose weather sets no `snow_max_depth`
  had nothing to scale, so there is now a `kLocalSnowDepth` fallback.
- **The `geo_n` hoist needs a guard.** `snow.frag` and `editor_paint.frag` compile both surface
  layers out, so an unguarded hoist leaves them a dead `normalize`.
- **Two test traps worth remembering**, both of which look like product bugs at first: a
  full-width `band_mean` includes the panel, whose highlighted button moves between captures;
  and a tolerance tighter than the renderer's frame-to-frame dither (SSAO rotation, SSR jitter)
  fails on noise. The fixture documents the dither; trust it over a tight number.

## Verification

**Unit** — extend `tests/engine/unit/env_field_test.cpp`: the `snow` channel folds and falls off
like the other scalars; `snow_clear` and `snow` are independent.

**GPU** — extend `tests/engine/gpu/env_render_test.cpp`:
- a local wetness source darkens a `wet: true` opaque material and leaves a `wet: false` one
  untouched (the opt-in contract, and the regression guard for every existing scene);
- a local snow source raises cover where global cover is 0 — read back through
  `SnowSystem::depth_at()`, which must agree with the shader's `gfx_snow_lying_height`;
- the CPU/GPU fold agreement test gains the new channel;
- the preset walk gains `puddle` and `snowpile`, asserting each puts its own channel in the
  field and leaves none behind when switched away.

**Panel behaviour** — the thing most likely to break silently. Assert, in the existing click
test: the header Button exists by name; collapsing hides the body; **expanding again restores
the body's full height** (the `expanded_extent` bug); and all eight preset buttons are still
reachable after a collapse/expand cycle.

**Visual** — capture `puddle` and `snowpile` with `CAPTURE_RING=1`, and one capture of the panel
collapsed and expanded. A screenshot is the only thing that proves the editor styling and the
fold actually read; an earlier round of this work shipped two permanently-orange buttons that
every passing test agreed with.
