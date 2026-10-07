---
name: create-palette-or-lut
description: Create a toyengine colour palette (PNG strip the image is quantized to) or a colour-grading LUT (N*N x N PNG strip applied after tonemapping), and point the config or a scene at it. Use when asked for a colour grade, LUT, film look, colour palette, limited-colour / retro look, or to change the overall colour mood of the image.
---

# Create a palette or grading LUT

Both are applied at the end of the post chain to display-referred (tonemapped, sRGB-encoded)
colour: grading first, then palette quantization. Both paths are **startup-fixed** config keys
(`render.palette`, `render.grading_lut`), so set them in `assets/config.yaml` (or a scene's
settings for a scene loaded at startup) and restart; the enable flags `palette_enabled` and
`grading_enabled` are runtime toggles. The engine positions itself as a full-resolution engine:
the pixel-art palette look is legacy -- don't make it a scene's default without being asked.

## Palette

`assets/palettes/<name>.png` -- convention: a horizontal N x 1 strip of the colours
(`pico8.png` 8x1, `aap64.png`, `duel_1x.png` 256x1). Any size works: the loader reads pixels in
row order, keeps unique opaque colours in first-seen order, skips alpha == 0, caps at 256.
Matching is nearest colour by RGB distance in sRGB. Loader:
`libs/gfxcoopa/gfxcoopa/engine/data/palette_lut.h`.

```yaml
render:
  palette_enabled: true
  palette: "assets/palettes/<name>.png"
```

## Grading LUT

A PNG strip **N*N wide by N high** (e.g. 1024 x 32 for N = 32; anything else is rejected with a
warning and grading turns off). Unity/Photoshop strip layout: blue selects the N x N slice left
to right, red increases across each slice, green increases down (row 0 = g 0). Identity:

    pixel(x, y) = ( r = (x mod N) / (N-1),  g = y / (N-1),  b = floor(x / N) / (N-1) )

Sampled linearly with clamp. Loader: `libs/gfxcoopa/gfxcoopa/engine/data/grading_lut.h`;
shader `assets/shaders/gfx/grading.glsl`.

```yaml
render:
  grading_enabled: true
  grading_lut: "assets/luts/<name>.png"
```

## Procedure

1. Write a generator `tools/gen_<name>_lut.py` / `gen_<name>_palette.py`: start from the
   identity LUT above and apply the grade as a function of (r, g, b) (lift/gamma/gain, split
   toning, saturation, a curve), so it is reviewable and re-runnable. Standard library only
   (zlib + struct PNG writer, see `tools/gen_prototype_grid.py`) -- system python has no
   numpy/Pillow; or use a scratch venv.
2. Store LUTs in `assets/luts/` (new folder -- add it to the layout table in
   `assets/README.md`) and palettes in `assets/palettes/`.
3. Sanity check: an identity LUT must render the same as `grading_enabled: false`.

## Verify

```sh
.claude/skills/create-scene/scripts/render_scene.sh pixel_demo "$TMPDIR/grade_off.png" grading_enabled=false
.claude/skills/create-scene/scripts/render_scene.sh pixel_demo "$TMPDIR/grade_on.png"  grading_enabled=true grading_lut='"assets/luts/<name>.png"'
```

Compare the two frames; check the `.log` for a rejected-LUT warning (wrong dimensions).
