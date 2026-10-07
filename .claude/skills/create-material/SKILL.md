---
name: create-material
description: Create or edit a PBR render material for toyengine -- a shared assets/materials/<name>.yaml, a scene-local one, or an inline `material:` map on a MeshRenderer/SdfRenderer (opaque, CUTOUT, BLEND glass/refraction, emissive, textured, or on a surface shader like triplanar/foliage/water). Use when asked to make, add, tweak or fix a material, give an object a look (metal, glass, glowing, grid...), or wire texture maps to a mesh.
---

# Create a material

A material is the PBR description a `MeshRenderer` or `SdfRenderer` draws with. The parser
(`parse_pbr_material_()` in `libs/gfxcoopa/gfxcoopa/engine/components/register.h`) **silently
ignores unknown keys and unknown enum values**, so a misspelt key renders as the default with no
error. Every key below is the exact spelling; check against this list, never guess.

Read `assets/README.md` (Materials section) first: it lists the shared materials, and reusing or
overriding one (`base:`) beats creating a near-duplicate.

## Where it goes

| Want | Do |
|---|---|
| Reusable across scenes | `assets/materials/<name>.yaml`, referenced as `material: materials/<name>` |
| Only one scene uses it | `assets/scenes/<scene>/materials/<name>.yaml` (paths resolve against the scene's folder first, then `assets/`) |
| A one-off tweak of a shared one | `material: { base: materials/<name>, roughness: 0.3 }` on the renderer |
| A one-off | inline map: `material: { albedo: { r: 1, g: 0, b: 0 }, roughness: 0.4 }` |

Names are lowercase snake_case. `.yaml` is appended automatically; a missing file throws
`Material '...' not found` at load.

## Keys

| key | type / default | notes |
|---|---|---|
| `albedo` | `{ r, g, b }` / 0.8 grey | all three components required. For metals this is the F0 (reflectance) colour |
| `metallic` | 0..1 / 0 | |
| `roughness` | 0..1 / 0.5 | clamped to >= 0.045 when drawn |
| `ao` | 0..1 / 1 | the ONLY ambient-occlusion input (no AO texture channel exists) |
| `emissive` | `{ r, g, b }` / 0 | linear colour, opaque path only; does not light other surfaces (only via SSR/SSGI/bloom) |
| `emissive_strength` | float / 1 | HDR multiplier on `emissive` |
| `alpha` | 0..1 / 1 | straight alpha; BLEND only |
| `alpha_mode` | `BLEND` \| `CUTOUT` (`MASK`/`CLIP` too) / opaque | UPPER-CASE. Anything else -- `OPAQUE`, `blend`, typos -- means opaque |
| `alpha_cutoff` | float / 0.5 | CUTOUT threshold |
| `cull_backfaces` | bool / true | `false` = double-sided. Stock opaque/CUTOUT only; a `shader:` uses its own cull, BLEND is always back-culled |
| `refraction` | bool / false | BLEND only; needs `render.transparency_enabled` |
| `ior`, `refraction_thickness` | float / -1 | < 0 = use the config's `refraction_*` value. Glass ~1.45, water ~1.33 |
| `refraction_tint` | `{ r, g, b }` / -1 | Beer-Lambert tint; < 0 = config value |
| `texture_albedo` | path | sRGB; multiplies `albedo` (use white albedo to show it as authored) |
| `texture_normal` | path | linear, tangent-space, OpenGL green-up |
| `texture_metallic_roughness` | path | linear; G = roughness, B = metallic (glTF). MULTIPLIES the scalars, so set the scalars to the maximum you want |
| `texture_alpha_mask` | path | linear, `.a` only, CUTOUT only |
| `texture_color_space` | `{ <path>: srgb \| linear }` | per-file override when two materials disagree |
| `shader` | name | a registered surface shader (`triplanar`, `foliage`, `water`, `terrain`, `terrain_styled`, ...) -- see the create-surface-shader skill |
| `shader_params` | up to 4 floats | the shader's `gfx_params`; meaning is per shader (documented in its .glsl header) |
| `shader_params_ext` | up to 8 floats | BLEND surface shaders only (`gfx_params_ext0/1`) |
| `base` | material path | inline maps only: load that file first, then apply this map on top |

Keys that do NOT exist (and are silently ignored): `albedo_map`, `normal_map`, `double_sided`,
`cull`, `color`, `metalness`, `opacity`, `transparent`.

Textures do not tile: samplers clamp and have no mipmaps, and UVs are used as-is in 0..1. For a
repeating pattern on big or UV-less surfaces use `shader: triplanar` (`shader_params: [tiling,
sharpness, space, normal_strength]`, space 0 = world, 1 = object).

## Procedure

1. **Check for an existing material** in `assets/materials/` (and the scene's folder). Prefer
   `base:` + overrides over a copy.
2. **Pick the mode**: opaque (default), `CUTOUT` for leaves/fences with an alpha mask or albedo
   alpha, `BLEND` for glass/water/ghosts. BLEND materials with `alpha < 1` cast no shadow; BLEND
   needs `render.transparency_enabled` (on in the shipped config).
3. **Write the file** with a one-line `#` comment saying what it is (every shipped material has
   one), then only the keys that differ from the defaults. Template:
   ```yaml
   # Brushed steel: a rough metal for machinery.
   albedo: { r: 0.56, g: 0.57, b: 0.58 }
   metallic: 1.0
   roughness: 0.35
   ao: 1.0
   ```
   Physically plausible ranges: dielectric albedo 0.04-0.9 (avoid pure black/white), metal
   albedo = its reflectance colour (gold 1.0/0.77/0.34, copper 0.95/0.64/0.54), roughness rarely
   below 0.05 except mirrors.
4. **Textures**: put maps in `assets/textures/` (create-texture skill for generating them) and
   reference them as `textures/<file>.png`. Missing maps fall back to white / flat normal.
5. **Surface shader**: if `shader:` is set, its domain must match `alpha_mode` -- an Opaque
   shader needs opaque/CUTOUT, a Transparent one needs BLEND. A mismatch silently renders the
   stock shader. An unregistered name throws at scene load
   ("material references unregistered shader").
6. **Editor**: the editor's material field list is `material_fields()` in
   `editor/schema/component_schema.h`; a shared material written with only the keys above opens
   there unchanged.
7. **Document**: add a row to the Materials table in `assets/README.md` for a new shared
   material.

## Verify

Put it on an object in a scene (or a scratch copy of one) and render headless:

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>          # finished frame
.claude/skills/create-scene/scripts/render_scene.sh <scene> debug_view=albedo   # also: normals, roughness, metallic, emissive, material_ao
./build/toyengine_tests caml_roundtrips_every_asset                       # every YAML under assets/ must round-trip
```

Look at the image (Read tool). Check the `.log` next to it for `not found` warnings (a bad
texture path) and for colour-space conflict warnings. Valid `debug_view` values are listed in
the comment block above `debug_view:` in `assets/config.yaml`.
