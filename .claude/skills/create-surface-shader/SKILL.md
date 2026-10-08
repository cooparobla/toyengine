---
name: create-surface-shader
description: Create a toyengine derived surface shader -- custom GLSL vertex/fragment hooks (wind sway, waves, triplanar, procedural patterns, dissolve, custom transparent looks) layered on the engine's G-buffer, shadow and transparent backbones, registered by name so a material can use `shader: <name>`. Use when asked to write a custom shader, material shader, vertex animation/displacement, procedural surface, or a new look a plain PBR material can't express.
---

# Create a surface shader

A surface shader is not a whole shader: it is a **hook** (`gfx_surface_vertex` and/or
`gfx_surface_fragment`) compiled into an engine **backbone** that owns the vertex layout,
G-buffer/forward write-out, lighting, shadows and SSR. A material opts in with
`shader: <name>` and passes up to 4 floats as `shader_params` (create-material).

Study these first -- they are complete, small, and documented:

| Example | Domain | Shows |
|---|---|---|
| `foliage.vert`, `foliage_shadow.vert`, `foliage_shadow_cube.vert`, `foliage_surface.glsl` | Opaque | vertex displacement with matching shadow entry points, `shader_params` layout doc |
| `triplanar.vert`, `triplanar.frag` | Opaque | custom varyings (locations 6-10), redefined sample macros, replaced normal |
| `terrain.frag` | Opaque | sample-macro-only shader, no hook |
| `water.vert`, `water.frag`, `water_surface.glsl` | Transparent | one file for both stages, `GFX_SURFACE_CUSTOM_VARYING`, `shader_params_ext`, forward globals |

All in `assets/shaders/`; the backbones are in `assets/shaders/gfx/surface/`. Shared lighting
headers (`gfx/brdf.glsl`, `gfx/ibl.glsl`, `gfx/sky.glsl`, `gfx/spot_light.glsl`) come from
`libs/gfxcoopa/assets/shaders/gfx/`.

## The two domains

| | Opaque (alpha_mode opaque or CUTOUT) | Transparent (alpha_mode BLEND) |
|---|---|---|
| Vertex backbone | `gfx/surface/gbuffer_vs.glsl` (+ `shadow_vs.glsl`, `shadow_cube_vs.glsl`) | `gfx/surface/transparent_vs.glsl` |
| Fragment backbone | `gfx/surface/gbuffer_fs.glsl` | `gfx/surface/transparent_fs.glsl` |
| Vertex hook | `void gfx_surface_vertex(inout GfxSurfaceVertex v)` -- edit `position_ws`, `normal_ws`, `tangent_ws` (also has `_os` inputs, `uv`, `model`, `normal_matrix`) | same, plus `vec4 v.custom` to pass to the fragment |
| Fragment hook | `void gfx_surface_fragment(inout GfxSurface s)` -- `albedo, metallic, roughness, ao, emissive, normal_ws, position_ws, uv, tbn`; runs after map sampling and the cutout test | `void gfx_surface_fragment(inout GfxTransparentSurface s)` -- `normal_ws, position_ws, uv, custom, albedo, alpha, roughness, thickness, ior` |
| Inputs | `gfx_params` (the 4 `shader_params`), `gfx_time` = (time, dt, frame, water clock) | also `gfx_params_ext0/1` (8 `shader_params_ext`), `forward_globals`, `camera`, `lights` |
| Stock fallbacks | `gbuffer.vert`, `gbuffer.frag`, `shadow_depth.vert/.frag`, `shadow_cube.vert/.frag` | `transparent.vert`, `transparent.frag` |

Optional fragment macros (define before the include): `GFX_SURFACE_SAMPLE(tex, uv)` wraps every
material-map fetch, `GFX_SURFACE_SAMPLE_NORMAL(tex, uv)` the normal fetch. The stock
`gbuffer.frag` adds texel anti-aliasing through `GFX_SURFACE_SAMPLE`; a custom opaque frag
should re-add it (`#include <gfx/texel_aa.glsl>`, see `gbuffer.frag`) or textures will alias.

Custom varyings (opaque): locations 0-5 and 11 belong to the backbone; use 6-10, declared
identically in vert and frag. Transparent: define `GFX_SURFACE_CUSTOM_VARYING` in both stages and
use `v.custom` / `s.custom`.

## Entry-point files

Each stage is a tiny top-level file that the build compiles:

```glsl
#version 450
// <name>.vert -- <what it does>; see <name>_surface.glsl.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>      // or shadow_vs / shadow_cube_vs / transparent_vs
#include "<name>_surface.glsl"               // defines the hook
```

Put the hook(s) in `<name>_surface.glsl`, guarded with `#ifdef GFX_SURFACE_VERTEX` /
`#ifdef GFX_SURFACE_FRAGMENT` when one file serves both stages, and document the meaning of each
`gfx_params` slot in its header.

**If the vertex hook moves vertices, also make `<name>_shadow.vert` and
`<name>_shadow_cube.vert`** including the same surface file, or shadows stay on the undisplaced
mesh (the shadow backbones give real position/uv only; normals are zero there).

**Tessellation.** A renderer with `tessellation: true` draws through the tessellated twin of its
shader, whose vertex hook runs per GENERATED vertex in an evaluation-stage backbone. A shader with
its own vertex stage needs `.tese` entry points too, or it draws untessellated (warning once):

```glsl
#version 450
// <name>.tese -- <name>'s G-buffer evaluation stage.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_tes.glsl>     // or shadow_tes (+ #define GFX_SHADOW_CUBE) / transparent_tes
#include "<name>_surface.glsl"
```

The hook must be a function of POSITION only (world xy, time, the world set) for anything it adds
along a direction -- two patches share an edge's vertices but not their normals, so normal-based
displacement cracks. The surface world set (`gfx/surface/world.glsl`: snow, the precipitation
"open sky" map, the trench field) is readable in every opaque stage; shadow vertex entry points
include it themselves (`#define GFX_WORLD_SET 1` before the include). Opaque fragment shaders get
the snow cover layer automatically; define `GFX_SURFACE_NO_SNOW` before the backbone to opt out.

## Registration (C++)

1. `toyengine/core/engine.h`, `make_render_config_()`: add an entry next to the others
   (`foliage`, `terrain`, `triplanar`, `water`...):
   ```cpp
   rc.surface_shaders.add({
       /* name  */ "<name>",
       /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,   // or Transparent
       /* vert  */ "<name>.vert",            // "" = stock
       /* frag  */ "<name>.frag",            // "" = stock
       /* shadow_vert */ "<name>_shadow.vert",
       /* shadow_frag */ "",
       /* shadow_cube_vert */ "<name>_shadow_cube.vert",
       /* shadow_cube_frag */ "",
       /* cull */ coopa::gfx::CullMode::Back,
       /* tesc */ "",                              // "" = stock control stage
       /* tese */ "<name>.tese",                   // "" = untessellated (unless vert is stock)
       /* shadow_tese */ "<name>_shadow.tese",
       /* shadow_cube_tese */ "<name>_shadow_cube.tese",
   });
   ```
   Logical file names, no `.spv`, no directory. A non-empty `shadow_vert` marks its shadows as
   animated (never cached).
2. Editor catalogue: add a `SurfaceShaderInfo` (name, label, transparent flag matching the
   domain, note, up to 4 `ShaderParamDesc{label, default, min, max, speed}`) in
   `surface_shaders()` and the name to the `f_enum("shader", {...})` list in
   `editor/schema/component_schema.h`. The editor test `editor_material_shader_catalogue`
   fails if the engine and editor lists disagree.
3. Compilation is automatic: CMake globs top-level `assets/shaders/*.vert|*.frag|*.tesc|*.tese` into `.spv`
   in the build tree, `build/shaders/toyengine_shaders/` (a project's own shaders:
   `build/shaders/project_shaders/`); includes resolve `assets/shaders` then
   `libs/gfxcoopa/assets/shaders`. No CMake edit; `.glsl` files are only included.

## Procedure

1. Read the closest example end to end. Decide the domain from the material's alpha mode.
2. Write the surface `.glsl`, the entry points, register in engine.h and the editor schema.
3. Add a material that uses it (`shader: <name>`, `shader_params: [...]`) -- shared in
   `assets/materials/` if it is a reusable look -- and put it on an object in a scene.
4. Document: a section in `assets/README.md` like "Triplanar surface shader" (params, domain,
   caveats).

Gotchas: a material whose alpha mode doesn't match the shader's domain silently renders the
stock shader; an unregistered name throws at scene load; opaque/shadow shaders get only 4
params (push-constant budget).

## Verify

```sh
cmake --build build -j10 2>&1 | grep -iE "error|glslc"          # shader compile errors show here
ls -la build/shaders/toyengine_shaders/<name>*.spv               # each entry point compiled now
.claude/skills/create-scene/scripts/render_scene.sh <scene>
./build/toyengine_tests caml_roundtrips_every_asset
./build/toyengine_editor_tests editor_material_shader_catalogue
```

A failed compile leaves the OLD `.spv` in place, so check the timestamps, not just that the
file exists. Render the scene and look: displaced shapes should cast matching shadows; compare
with `debug_view=albedo` / `normals` for fragment work. Run the full `ctest` if you touched the
backbones.
