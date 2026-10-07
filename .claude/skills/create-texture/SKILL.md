---
name: create-texture
description: Create texture maps for toyengine materials -- albedo, tangent-space normal, metallic-roughness and alpha-mask PNGs in assets/textures/ -- usually with a small Python generator (procedural bricks, noise masks, grids, atlases), and wire them into a material. Use when asked to make, generate, paint or fix a texture, texture map, normal map, mask, atlas or pattern.
---

# Create a texture

Textures are PNGs in `assets/textures/` (scene-only ones in `assets/scenes/<scene>/textures/`),
loaded as RGBA8 by stb_image. They are referenced from a material's `texture_*` keys
(create-material) as `textures/<file>.png`.

Generators to copy from: `tools/gen_material_maps.py` (brick albedo + normal + metallic-
roughness from one height field), `tools/gen_noise_mask.py` (value-noise alpha mask),
`tools/gen_prototype_grid.py` (standard-library-only PNG writer), `tools/gen_terrain_atlas.py`
(atlas).

## What the renderer expects

| Map | Material key | Colour space | Content |
|---|---|---|---|
| Albedo | `texture_albedo` | sRGB | colour; multiplied by the material `albedo` (set it white to show the map as painted); alpha is used by CUTOUT and multiplies `alpha` in BLEND |
| Normal | `texture_normal` | linear | tangent space, OpenGL convention (green = +V / up), encoded `n * 0.5 + 0.5`; flat = (128, 128, 255) |
| Metallic-roughness | `texture_metallic_roughness` | linear | glTF packing: **G = roughness, B = metallic**, R and A unused (write 255). Multiplies the material's `roughness`/`metallic` scalars |
| Alpha mask | `texture_alpha_mask` | linear | `.a` only, CUTOUT only; store continuous coverage, the material's `alpha_cutoff` thresholds it |

There is no AO / ORM channel (AO is the material's `ao` scalar).

**Sampling**: clamp-to-edge, no mipmaps; LINEAR with `render.texel_aa` on (default), else
NEAREST. UVs outside 0..1 do not repeat, so a texture covers its mesh once -- for tiling use
`shader: triplanar` with a `tiling` param. The shipped textures are small (32-128 px) and
pixel-art scaled; there's no need for large maps, and large maps without mipmaps shimmer at a
distance.

If two materials declare the same file with different colour spaces, the first wins with a
warning; fix it with `texture_color_space: { textures/x.png: linear }`.

## Procedure

1. **Prefer a generator script** (`tools/gen_<name>.py`) over a hand-made binary: it is
   reviewable and re-runnable. Write fixed output paths relative to the repo root (resolve from
   `__file__`), like the existing tools; no command-line arguments needed.
2. **Python environment**: system `python3` has no numpy/Pillow. Either write PNGs with the
   standard library (zlib + struct, as `gen_prototype_grid.py` does) or create a scratch venv:
   ```sh
   python3 -m venv "$TMPDIR/texvenv" && "$TMPDIR/texvenv/bin/pip" install numpy pillow
   "$TMPDIR/texvenv/bin/python" tools/gen_<name>.py
   ```
   Never install packages into the system python.
3. **Make the set consistent**: derive normal and roughness from the same height/mask field as
   the albedo (see `gen_material_maps.py`) so details line up. Normal from height: Sobel/central
   differences, `n = normalize(-dh/dx * s, -dh/dy * s, 1)`; flip the green channel's sign if the
   bumps light from below.
4. **Tileable?** If it will be used with triplanar, make it seamless (wrap the noise/pattern).
5. **Wire it up** in a material (create-material) and, for a shared texture, mention it in
   `assets/README.md` where the material is listed.

## Verify

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene using the material>
.claude/skills/create-scene/scripts/render_scene.sh <scene> debug_view=normals save_low_res=true
```

Look at the PNG itself (Read tool) and the rendered result. A wrong path can fall back
silently to white / flat normal, so compare against `debug_view=albedo` and check the `.log` for
load warnings. Lighting that looks
inverted on bumps means the normal map's green channel is flipped.
