"""Generates the dissolve alpha mask ViewBlockerFade walls erode through.

Standalone generator, not part of the C++ build -- run it manually
(`python3 tools/gen_dissolve_noise.py`) whenever
assets/textures/masks/dissolve_noise.png needs regenerating. Requires numpy and Pillow.

Separate from tools/gen_noise_mask.py, which makes the blobby foliage cutout mask, because a
dissolve wants the opposite character: fine, high-frequency speckle that eats a wall evenly as
the cutoff rises, rather than a few large blobs that would punch three big holes and look like
damage. Both store continuous coverage and leave the threshold to the material's alpha_cutoff.

**The alpha floor is the point of this file.** PBRMaterial::gpu_alpha_cutoff() clamps a CUTOUT
material's cutoff to a 0.0001 minimum rather than 0, so a texel at alpha 0 discards even at a
"solid" setting -- a wall at rest would carry a scatter of pinholes through it. Keeping the
minimum at ALPHA_FLOOR well above that clamp is what makes cutoff 0 mean genuinely solid.
"""

import pathlib

import numpy as np
from PIL import Image

SIZE = 128
"""Output texture width/height in pixels. Larger than the foliage mask's 64 because this one is
stretched over a whole wall rather than a leaf card, and a dissolve reads best when the
individual speckles are near pixel-sized at playing distance."""

ALPHA_FLOOR = 0.06
"""Lowest alpha any texel gets. Must stay clear of gpu_alpha_cutoff()'s 0.0001 clamp (see the
module doc) with enough margin that 8-bit quantization cannot land a texel under it."""

OUTPUT_PATH = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures" / "masks" / "dissolve_noise.png"
"""Where the generated PNG is written; resolved relative to this repo, not the caller's cwd."""


def _white_noise(size: int, seed: int) -> np.ndarray:
    """Generates uniform per-texel noise.

    Args:
        size: Output resolution (size x size).
        seed: RNG seed, so the asset is reproducible.

    Returns:
        np.ndarray: A (size, size) float32 array in [0, 1].
    """
    rng = np.random.default_rng(seed)
    return rng.random((size, size), dtype=np.float32)


def _value_noise(size: int, cells: int, seed: int) -> np.ndarray:
    """Generates one octave of smooth value noise by upsampling a coarse random grid.

    Args:
        size: Output resolution (size x size).
        cells: Random grid cells per axis before upsampling; lower means blobbier.
        seed: RNG seed.

    Returns:
        np.ndarray: A (size, size) float32 array in [0, 1], wrapping at the edges so the mask
            tiles without a visible seam.
    """
    rng = np.random.default_rng(seed)
    grid = rng.random((cells, cells), dtype=np.float32)
    # Wrap by sampling the grid modulo its size, so texel size-1 interpolates back toward 0.
    t = (np.arange(size, dtype=np.float32) + 0.5) * cells / size
    i0 = np.floor(t).astype(np.int32) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    f = f * f * (3.0 - 2.0 * f)   # smoothstep, so the octave has no grid-aligned creases

    rows = grid[i0][:, i0] * (1 - f)[None, :] + grid[i0][:, i1] * f[None, :]
    rows1 = grid[i1][:, i0] * (1 - f)[None, :] + grid[i1][:, i1] * f[None, :]
    return rows * (1 - f)[:, None] + rows1 * f[:, None]


def generate_dissolve_mask() -> np.ndarray:
    """Builds the dissolve coverage field.

    Mostly white noise, so the dissolve is an even erosion that reaches every part of the wall
    at roughly the same cutoff, plus one coarse value-noise octave so the holes clump a little
    and the wall comes apart in patches rather than as uniform static.

    Returns:
        np.ndarray: A (SIZE, SIZE) float32 array of coverage values in [ALPHA_FLOOR, 1].
    """
    noise = 0.65 * _white_noise(SIZE, seed=11) + 0.35 * _value_noise(SIZE, cells=8, seed=12)
    noise -= noise.min()
    noise /= max(noise.max(), 1e-6)
    return ALPHA_FLOOR + (1.0 - ALPHA_FLOOR) * noise


def main() -> None:
    """Generates the dissolve mask and writes it to OUTPUT_PATH as an 8-bit RGBA PNG."""
    mask = generate_dissolve_mask()
    texel = np.clip(mask * 255.0, 0, 255).astype(np.uint8)

    # RGB mirrors alpha so the file is inspectable as a plain greyscale image; only alpha is
    # ever sampled (see PBRMaterial::texture_alpha_mask's doc).
    rgba = np.dstack([texel, texel, texel, texel])

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(OUTPUT_PATH)
    print(f"Wrote {OUTPUT_PATH} ({SIZE}x{SIZE}, alpha {texel.min()}..{texel.max()})")


if __name__ == "__main__":
    main()
