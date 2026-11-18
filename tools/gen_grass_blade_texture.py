"""Generates the grass-tuft cutout textures used by `meshes/nature/grass_tuft`.

Writes, under assets/textures/foliage/:
    grass_blade.png       RGBA, 256x256 -- greyscale blades, alpha = the cutout
    grass_blade_mask.png  RGBA, 256x256 -- the same alpha as an explicit mask texture

Run manually whenever the blade shape needs regenerating:

    python3 tools/gen_grass_blade_texture.py

## Why it is greyscale rather than green

`grass_tuft` is three crossed quads, and without an alpha cutout each one renders as a solid
card -- the "grass looks like flat planes" failure. The alpha is what the mesh actually needs;
the colour is the material's job. Every caller already sets its own `albedo` tint (a desert
scrub and a lakeside reed are different greens over the same blades), and a texture carrying
its own green would multiply against that and fight it. So the RGB here is a neutral
base-to-tip ramp -- shading only, mean near white -- and the tint comes through unchanged.

## The UV convention this matches

`grass_tuft.yaml` maps each quad's full 0..1 square, with **v = 0 at the blade tips** and
v = 1 at the base (vertex 0 is z = 0 at uv (0,1); vertex 3 is z = 0.6 at uv (0,0)). That is
the same orientation as image rows, so row 0 of these PNGs is the tip end and the last row is
where the blades meet the ground. Flipping them vertically plants the tuft upside down.

Blades fan from the bottom centre, each curving outward and tapering to a point, with the
whole tuft drawn at 4x and box-filtered down so the alpha edge is antialiased rather than
stair-stepped -- `alpha_cutoff` then bites into a soft edge and the silhouette stays smooth
when the card is close to the camera.

Standard library plus numpy/Pillow, like tools/gen_moebius_dunes.py.
"""

import pathlib

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "textures" / "foliage"

RES = 256          # final texture size
SS = 4             # supersampling factor for the alpha edge
BLADES = 9
SEED = 7

BASE_SHADE = 0.55  # RGB at the blade base -- darker, where a tuft self-shadows
TIP_SHADE = 1.0    # RGB at the tip


def build_tuft(res: int, rng: np.random.Generator) -> np.ndarray:
    """Rasterises the blades into an (res, res) coverage field in 0..1."""
    cover = np.zeros((res, res), dtype=np.float64)
    ys = np.arange(res, dtype=np.float64)
    # t = 0 at the base (last row) .. 1 at the tip (row 0), matching the mesh's v axis.
    t = 1.0 - (ys + 0.5) / res

    for i in range(BLADES):
        # Spread the fan across the card rather than letting rng clump it: an even base
        # position plus jitter keeps a blade in each part of the quad, so the tuft reads as
        # a clump instead of as a few blades to one side.
        frac = (i + 0.5) / BLADES
        x0 = 0.5 + (frac - 0.5) * 0.46 + rng.normal(0.0, 0.015)
        lean = (frac - 0.5) * 2.0                       # -1 (left) .. +1 (right)
        bend = lean * rng.uniform(0.18, 0.34) + rng.normal(0.0, 0.03)
        height = rng.uniform(0.62, 1.0)                 # not every blade reaches the top
        width = rng.uniform(0.014, 0.026)

        # Along the blade: centre curves with t^1.6 (slow near the root, faster near the tip,
        # the way a real blade arcs) and the half-width tapers to zero at the tip.
        live = t <= height
        tt = np.clip(t / max(height, 1e-6), 0.0, 1.0)
        cx = x0 + bend * np.power(tt, 1.6)
        half = width * (1.0 - tt) ** 0.7

        xs = (np.arange(res, dtype=np.float64) + 0.5) / res
        dist = np.abs(xs[None, :] - cx[:, None])
        blade = (dist <= half[:, None]) & live[:, None]
        cover = np.maximum(cover, blade.astype(np.float64))

    return cover


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)

    hi = build_tuft(RES * SS, rng)
    # Box-filter the supersampled coverage down to the final size: this is what turns a hard
    # 1-bit rasterisation into an antialiased alpha.
    alpha = hi.reshape(RES, SS, RES, SS).mean(axis=(1, 3))

    # RGB: a vertical shading ramp only (see the module docstring on why it is not green).
    t = 1.0 - (np.arange(RES, dtype=np.float64) + 0.5) / RES
    shade = BASE_SHADE + (TIP_SHADE - BASE_SHADE) * t
    rgb = np.repeat(shade[:, None], RES, axis=1)

    rgba = np.zeros((RES, RES, 4), dtype=np.uint8)
    for c in range(3):
        rgba[:, :, c] = np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
    rgba[:, :, 3] = np.clip(alpha * 255.0, 0, 255).astype(np.uint8)
    Image.fromarray(rgba, mode="RGBA").save(OUT / "grass_blade.png")

    mask = np.zeros((RES, RES, 4), dtype=np.uint8)
    a8 = np.clip(alpha * 255.0, 0, 255).astype(np.uint8)
    for c in range(3):
        mask[:, :, c] = a8
    mask[:, :, 3] = a8
    Image.fromarray(mask, mode="RGBA").save(OUT / "grass_blade_mask.png")

    print(f"wrote {OUT}/grass_blade.png and grass_blade_mask.png ({RES}x{RES})")
    print(f"  blade coverage {100.0 * (alpha > 0.5).mean():.1f}% of the card at cutoff 0.5")
