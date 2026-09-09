"""Generates a demo albedo/normal/metallic-roughness texture set for the pixel_demo cube.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_material_maps.py`) whenever the assets/scenes/pixel_demo/textures/crate_*.png files
need regenerating. Requires numpy and Pillow.

Produces a simple running-bond brick pattern -- chosen because a brick's mortar grooves give the
normal map an unambiguous, easy-to-eyeball bump (flat brick faces, recessed mortar lines) rather
than something that could pass for noise or a lighting bug. Three files, glTF-conventioned:

  crate_albedo.png -- base color (RGB), authored in sRGB (see PBRMaterial's texture_albedo
      doc / TextureLoader::declare_color_space()'s default slot mapping for why: this is the
      one map of the three that actually needs gamma decoding).
  crate_normal.png -- tangent-space normal map (RGB, green-up/OpenGL convention -- matches
      gfx/surface/gbuffer_vs.glsl's Gram-Schmidt TBN, which needs no further sign flip), linear.
  crate_mr.png -- metallic-roughness (metallic in B, roughness in G, R/A unused), linear.

All three are NEAREST-sampled (see toy::loaders::PixelTextureLoader / SamplerDesc::pixel_art())
and small on purpose -- this is a pixel-art engine, and a chunky, blocky brick texture is more in
keeping with it than a smoothly filtered high-res one.
"""

import pathlib

import numpy as np
from PIL import Image

SIZE = 32
"""Output texture width/height in pixels for all three maps."""

BRICK_W = 8
"""Brick width in texels, before the running-bond half-brick offset on alternating rows."""

BRICK_H = 8
"""Brick height (course height) in texels."""

MORTAR = 1
"""Mortar groove thickness in texels, on both the vertical and horizontal joints."""

BUMP_STRENGTH = 2.5
"""How far the mortar grooves tip the normal map away from straight-up. Purely a visual
tuning constant -- see _height_to_normal()'s central-difference gradient, which this scales.
"""

OUTPUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "assets" / "scenes" / "pixel_demo" / "textures"
"""Directory the three crate_*.png files are written into; resolved relative to this repo, not
the caller's cwd.
"""


def _brick_height_field() -> np.ndarray:
    """Builds a running-bond brick height field: 1.0 on brick faces, 0.0 in mortar grooves.

    Returns:
        np.ndarray: A (SIZE, SIZE) float32 array in {0.0, 1.0} -- 0.0 wherever a texel falls
            in a mortar groove (vertical or horizontal joint), 1.0 elsewhere. Alternating brick
            courses are offset by half a brick width (running bond), matching real brickwork
            and avoiding the long unbroken vertical joints a plain grid pattern would have.
    """
    y, x = np.mgrid[0:SIZE, 0:SIZE]

    course = y // BRICK_H
    # Running bond: odd courses shift half a brick to the right before wrapping into columns,
    # so vertical joints never line up between adjacent courses.
    shifted_x = (x + (BRICK_W // 2) * (course % 2)) % SIZE

    in_horizontal_joint = (y % BRICK_H) < MORTAR
    in_vertical_joint = (shifted_x % BRICK_W) < MORTAR

    height = np.ones((SIZE, SIZE), dtype=np.float32)
    height[in_horizontal_joint | in_vertical_joint] = 0.0
    return height


def _height_to_normal(height: np.ndarray) -> np.ndarray:
    """Converts a height field into an encoded tangent-space normal map via a central-difference
    gradient.

    Args:
        height: A (SIZE, SIZE) float32 array of relative surface height.

    Returns:
        np.ndarray: A (SIZE, SIZE, 3) uint8 array, RGB-encoded per gbuffer_fs.glsl's decode
            (texel * 2 - 1 recovers the unit tangent-space normal). Wraps at the texture edges
            (np.roll, not a clamped/zero-padded gradient) so the map tiles seamlessly if the
            crate is ever applied to a UV-repeating surface.
    """
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5

    nx = -dx * BUMP_STRENGTH
    ny = -dy * BUMP_STRENGTH
    nz = np.ones_like(height)

    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx / length, ny / length, nz / length

    encoded = np.dstack([nx, ny, nz]) * 0.5 + 0.5
    return np.clip(encoded * 255.0, 0, 255).astype(np.uint8)


def _brick_albedo(height: np.ndarray) -> np.ndarray:
    """Colors the brick height field: warm red-orange brick faces, cool grey mortar grooves.

    Args:
        height: A (SIZE, SIZE) float32 array as returned by _brick_height_field() -- 1.0 on
            brick faces, 0.0 in mortar grooves.

    Returns:
        np.ndarray: A (SIZE, SIZE, 3) uint8 RGB array.
    """
    brick_color = np.array([176.0, 82.0, 58.0])
    mortar_color = np.array([150.0, 143.0, 132.0])

    # Per-brick brightness jitter so faces read as individual bricks rather than one flat
    # rectangle -- seeded for reproducibility across regenerations.
    rng = np.random.default_rng(7)
    y, x = np.mgrid[0:SIZE, 0:SIZE]
    course = y // BRICK_H
    shifted_x = (x + (BRICK_W // 2) * (course % 2)) % SIZE
    brick_id = course * (SIZE // BRICK_W + 2) + shifted_x // BRICK_W
    jitter_table = rng.uniform(0.85, 1.15, size=int(brick_id.max()) + 1).astype(np.float32)
    jitter = jitter_table[brick_id]

    mask = (height > 0.5)[:, :, np.newaxis]
    rgb = np.where(mask, brick_color[np.newaxis, np.newaxis, :] * jitter[:, :, np.newaxis],
                   mortar_color[np.newaxis, np.newaxis, :])
    return np.clip(rgb, 0, 255).astype(np.uint8)


def _brick_metallic_roughness(height: np.ndarray) -> np.ndarray:
    """Builds the metallic-roughness map: non-metal throughout, mortar slightly rougher.

    Args:
        height: A (SIZE, SIZE) float32 array as returned by _brick_height_field().

    Returns:
        np.ndarray: A (SIZE, SIZE, 4) uint8 RGBA array. glTF packing -- G = roughness,
            B = metallic; R and A are unused/reserved (see gbuffer_fs.glsl's `.bg` swizzle) and
            filled with 255 so the file also previews sensibly as a plain image.
    """
    roughness = np.where(height > 0.5, 0.75, 0.9).astype(np.float32)
    metallic = np.zeros_like(height)  # brick/mortar are both fully non-metal

    r = np.full_like(height, 255.0)
    g = roughness * 255.0
    b = metallic * 255.0
    a = np.full_like(height, 255.0)
    return np.clip(np.dstack([r, g, b, a]), 0, 255).astype(np.uint8)


def main() -> None:
    """Generates all three crate_*.png maps and writes them to OUTPUT_DIR."""
    height = _brick_height_field()

    albedo = _brick_albedo(height)
    normal = _height_to_normal(height)
    mr = _brick_metallic_roughness(height)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    albedo_path = OUTPUT_DIR / "crate_albedo.png"
    normal_path = OUTPUT_DIR / "crate_normal.png"
    mr_path = OUTPUT_DIR / "crate_mr.png"

    Image.fromarray(albedo, mode="RGB").save(albedo_path)
    Image.fromarray(normal, mode="RGB").save(normal_path)
    Image.fromarray(mr, mode="RGBA").save(mr_path)

    print(f"Wrote {albedo_path} ({SIZE}x{SIZE})")
    print(f"Wrote {normal_path} ({SIZE}x{SIZE})")
    print(f"Wrote {mr_path} ({SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
