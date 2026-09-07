"""Generates the blobby value-noise alpha mask used by the pixel_demo scene's CUTOUT sphere.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_noise_mask.py`) whenever assets/scenes/pixel_demo/textures/noise_mask.png needs
regenerating. Requires numpy and Pillow.

The output is deliberately NOT pre-thresholded to a binary 0/1 mask: it stores continuous
coverage values in RGB and A alike (a greyscale image), and it's the CUTOUT material's own
alpha_cutoff (see gfxcoopa's PBRMaterial::gpu_alpha_cutoff()) that turns it into a hard
silhouette test at render time. Storing it pre-thresholded would bake one specific cutoff
into the asset instead of leaving it tunable from scene YAML.
"""

import pathlib

import numpy as np
from PIL import Image

SIZE = 64
"""Output texture width/height in pixels. Small and NEAREST-sampled (see
toy::loaders::PixelTextureLoader) on purpose -- this is a pixel-art engine, and a handful of
chunky noise texels reads as a clean silhouette test; a smoothly filtered high-res mask would
blur the cutout edge, defeating the point of AlphaMode::Mask's hard discard.
"""

OUTPUT_PATH = pathlib.Path(__file__).resolve().parent.parent / "assets" / "scenes" / "pixel_demo" / "textures" / "noise_mask.png"
"""Where the generated PNG is written; resolved relative to this repo, not the caller's cwd."""


def _value_noise_octave(size: int, cells: int, seed: int) -> np.ndarray:
    """Generates one octave of smooth value noise by upsampling a coarse random grid.

    Args:
        size: Output resolution (size x size).
        cells: Number of random grid cells per axis before upsampling -- lower means blobbier.
        seed: RNG seed, so each octave is reproducible but distinct from the others.

    Returns:
        np.ndarray: A (size, size) float32 array in [0, 1], smoothly interpolated between
            `cells` x `cells` random control points.
    """
    rng = np.random.default_rng(seed)
    grid = rng.random((cells + 1, cells + 1), dtype=np.float64)

    coords = np.linspace(0.0, cells, size, endpoint=False)
    x0 = np.floor(coords).astype(int)
    frac = coords - x0

    # Smoothstep (3t^2 - 2t^3) interpolation, not linear -- avoids the visible grid-aligned
    # creases plain bilinear upsampling leaves at cell boundaries.
    smooth = frac * frac * (3.0 - 2.0 * frac)

    row_lo = grid[x0][:, x0]
    row_hi = grid[x0][:, x0 + 1]
    col_lo = row_lo + (row_hi - row_lo) * smooth[np.newaxis, :]

    row_lo2 = grid[x0 + 1][:, x0]
    row_hi2 = grid[x0 + 1][:, x0 + 1]
    col_hi = row_lo2 + (row_hi2 - row_lo2) * smooth[np.newaxis, :]

    return (col_lo + (col_hi - col_lo) * smooth[:, np.newaxis]).astype(np.float32)


def generate_noise_mask() -> np.ndarray:
    """Builds the multi-octave value-noise field used as the alpha mask.

    Three octaves (4, 8, 16 cells per axis) are summed with halving persistence, then
    renormalized to [0, 1] -- the standard fractal-value-noise recipe, tuned here for a coarse
    base octave so the result reads as a handful of legible blobs/holes rather than fine static.

    Returns:
        np.ndarray: A (SIZE, SIZE) float32 array of coverage values in [0, 1].
    """
    octaves = [(4, 0.6, 1), (8, 0.3, 2), (16, 0.1, 3)]
    noise = np.zeros((SIZE, SIZE), dtype=np.float32)
    for cells, weight, seed in octaves:
        noise += weight * _value_noise_octave(SIZE, cells, seed)

    noise -= noise.min()
    noise /= max(noise.max(), 1e-6)
    return noise


def main() -> None:
    """Generates the noise mask and writes it to OUTPUT_PATH as an 8-bit RGBA PNG."""
    noise = generate_noise_mask()
    texel = np.clip(noise * 255.0, 0, 255).astype(np.uint8)

    # RGB mirrors alpha so the file is visually inspectable as a plain greyscale image; only
    # alpha is ever sampled by gbuffer.frag/shadow_depth.frag/shadow_cube.frag (see
    # PBRMaterial::texture_alpha_mask's doc).
    rgba = np.dstack([texel, texel, texel, texel])

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(OUTPUT_PATH)
    print(f"Wrote {OUTPUT_PATH} ({SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
