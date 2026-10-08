"""Generates the displacement (height) maps under assets/textures/displacement/.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_displacement_maps.py`) whenever those files need regenerating. Requires numpy and
Pillow.

A displacement map is read by the tessellation evaluation stage (PBRMaterial::texture_displacement,
see gfx/surface/gbuffer_tes.glsl): the red channel, 0..1, pushes each generated vertex along its
normal by that times the material's displacement_scale. It is only meaningful on a renderer with
`tessellation:` enabled -- an untessellated mesh has no vertices between its authored ones to move.

  dunes_height.png -- 256x256, tileable: broad wind-blown dunes (fbm value noise) with fine
      ripples on top, for tess_test's sand station. Smooth on purpose: the map is sampled LINEAR,
      and a smooth height gives the tessellator something it can follow without stair-stepping.
"""

import pathlib

import numpy as np
from PIL import Image

SIZE = 256
SEED = 7


def _value_noise(size: int, cells: int, rng: np.random.Generator) -> np.ndarray:
    """Tileable smoothstep-interpolated value noise with `cells` lattice cells a side."""
    lattice = rng.random((cells, cells)).astype(np.float32)
    t = np.arange(size, dtype=np.float32) * cells / size
    i0 = np.floor(t).astype(int) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    f = f * f * (3.0 - 2.0 * f)
    a = lattice[np.ix_(i0, i0)]
    b = lattice[np.ix_(i0, i1)]
    c = lattice[np.ix_(i1, i0)]
    d = lattice[np.ix_(i1, i1)]
    fy = f[:, None]
    fx = f[None, :]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def dunes_height() -> np.ndarray:
    rng = np.random.default_rng(SEED)
    h = np.zeros((SIZE, SIZE), dtype=np.float32)
    amp, total = 1.0, 0.0
    for cells in (4, 8, 16, 32):
        h += _value_noise(SIZE, cells, rng) * amp
        total += amp
        amp *= 0.45
    h /= total
    # Wind ripples: a tileable diagonal sine (whole periods across the tile), bent by the dunes.
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32) / SIZE
    ripple = np.sin(2.0 * np.pi * (24.0 * xx + 8.0 * yy) + h * 6.0) * 0.5 + 0.5
    h = h * 0.96 + ripple * 0.04
    h = (h - h.min()) / (h.max() - h.min())
    return h


def main() -> None:
    out = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures" / "displacement"
    out.mkdir(parents=True, exist_ok=True)
    h = dunes_height()
    img = np.clip(h * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(np.stack([img, img, img], axis=-1), mode="RGB").save(out / "dunes_height.png")
    print(f"wrote {out / 'dunes_height.png'}")


if __name__ == "__main__":
    main()
