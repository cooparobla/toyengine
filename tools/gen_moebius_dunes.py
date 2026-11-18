"""Generates the moebius_demo desert: a dune-field heightmap plus its splat map.

Writes, under assets/scenes/rendering/moebius_demo/terrain/:
    height.png   16-bit greyscale, 513x513  -- white == the terrain's `height` in metres
    splat0.png   RGBA, 512x512              -- layers 0-3 = sand, dirt, rock, cliff

Run manually whenever the landscape needs regenerating:

    python3 tools/gen_moebius_dunes.py

## The vertical layout, and why the scene's Transform.z is what it is

A heightmap PNG cannot go below zero, so the part of the world the props stand on cannot be at
the bottom of the range -- the lake has to get BELOW it. So the normalised field is built around
a reference plane:

    PLAYA = 0.25   the flat pan the props sit on
    dunes rise from PLAYA toward 1.0, the lake basin drops from PLAYA toward 0.0

The scene then offsets the terrain by `-PLAYA * height` metres in Z, which puts the pan at world
z = 0 -- so every prop already placed at z = 0 still sits exactly on the ground, and the lake
bed is genuinely below it. Change PLAYA or `height` here and that Transform must move with it,
or the props hover or sink.

## The dunes

Transverse dunes: ridges across the wind, gentle windward slope and a short steep lee face --
an asymmetric sawtooth, not a sine, because a sine reads as corrugation rather than sand. Their
crest line wanders (a low-frequency warp of the phase) and their height is modulated by a second
low-frequency field, so the field does not read as a single repeated profile.

Dunes are faded out over the pan and over the lake, so neither gets a ridge through it.

Standard library plus numpy/Pillow (both present in this checkout, unlike the stdlib-only
palette/LUT generators -- a 513x513 field is not worth hand-rolling).
"""

import pathlib

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "scenes" / "rendering" / "moebius_demo" / "terrain"

SIZE_M = 256.0       # must match the scene's HeightmapTerrain.size
RES = 513            # must match resolution (2^n + 1)
SPLAT_RES = 512      # must match splat_resolution
HEIGHT_M = 24.0      # must match `height`
PLAYA = 0.25         # see the module docstring -- the scene's Transform.z depends on this

PLAYA_CENTRE = (0.0, 0.0)    # world metres; where the props stand
PLAYA_RADIUS = 22.0
PLAYA_FEATHER = 16.0

# Straight ahead of the default camera and far enough out that the lake's near shore clears
# the prop pan: the pan reaches y = PLAYA_RADIUS, the lake starts at LAKE_CENTRE[1] -
# LAKE_RADIUS, and anything less than ~18 m of dry sand between them stands the props in
# the water.
LAKE_CENTRE = (0.0, 64.0)
LAKE_RADIUS = 24.0
LAKE_FEATHER = 16.0
LAKE_DEPTH_M = 5.0           # bed below the pan at the deepest point

DUNE_WAVELENGTH = 46.0       # metres between crests
DUNE_HEIGHT_M = 11.0
SEED = 20


def fractal_noise(shape, rng, octaves=5, persistence=0.55):
    """Value noise: upsampled random lattices summed at halving amplitude. Returns 0..1."""
    out = np.zeros(shape, dtype=np.float64)
    amp, total, res = 1.0, 0.0, 4
    for _ in range(octaves):
        lattice = rng.random((res + 1, res + 1))
        img = Image.fromarray((lattice * 255).astype(np.uint8)).resize(
            (shape[1], shape[0]), Image.BICUBIC)
        out += amp * (np.asarray(img, dtype=np.float64) / 255.0)
        total += amp
        amp *= persistence
        res *= 2
    return out / total


def smooth_disc(xx, yy, centre, radius, feather):
    """1 inside `radius`, falling smoothly to 0 by `radius + feather`."""
    d = np.hypot(xx - centre[0], yy - centre[1])
    t = np.clip((d - radius) / max(feather, 1e-6), 0.0, 1.0)
    return 1.0 - (t * t * (3.0 - 2.0 * t))      # smoothstep


def smooth_capsule(xx, yy, a, b, radius, feather):
    """smooth_disc, but along the segment a->b: a flat corridor rather than a round clearing."""
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    denom = max(dx * dx + dy * dy, 1e-6)
    t = np.clip(((xx - ax) * dx + (yy - ay) * dy) / denom, 0.0, 1.0)
    d = np.hypot(xx - (ax + t * dx), yy - (ay + t * dy))
    u = np.clip((d - radius) / max(feather, 1e-6), 0.0, 1.0)
    return 1.0 - (u * u * (3.0 - 2.0 * u))


def dune_profile(phase):
    """Asymmetric sawtooth in 0..1: long windward rise, short steep lee face."""
    p = np.mod(phase, 1.0)
    windward = 0.78
    up = p / windward
    down = (1.0 - p) / (1.0 - windward)
    prof = np.where(p < windward, up, down)
    return prof * prof * (3.0 - 2.0 * prof)     # round the crest; a raw peak aliases


def build_height():
    rng = np.random.default_rng(SEED)
    axis = np.linspace(-SIZE_M / 2.0, SIZE_M / 2.0, RES)
    xx, yy = np.meshgrid(axis, axis)

    warp = (fractal_noise((RES, RES), rng, octaves=3) - 0.5) * 2.0
    amp_field = fractal_noise((RES, RES), rng, octaves=3)
    grain = (fractal_noise((RES, RES), rng, octaves=6) - 0.5) * 2.0

    # Crests run roughly along +y and advance in +x; the warp bends them so they meander.
    phase = (xx + warp * 26.0 + yy * 0.18) / DUNE_WAVELENGTH
    dunes = dune_profile(phase) * (0.45 + 0.55 * amp_field)

    pan = smooth_disc(xx, yy, PLAYA_CENTRE, PLAYA_RADIUS, PLAYA_FEATHER)
    lake = smooth_disc(xx, yy, LAKE_CENTRE, LAKE_RADIUS, LAKE_FEATHER)
    # Without this the dune field closes over the gap between pan and lake, and since the lake
    # sits BELOW the pan a 10 m dune in between hides it completely from any low camera. The
    # corridor keeps that approach open so the water is actually part of the view.
    corridor = smooth_capsule(xx, yy, PLAYA_CENTRE, LAKE_CENTRE, 24.0, 20.0)
    clear = np.clip(1.0 - np.maximum(np.maximum(pan, lake), corridor), 0.0, 1.0)

    field = np.full((RES, RES), PLAYA, dtype=np.float64)
    field += dunes * clear * (DUNE_HEIGHT_M / HEIGHT_M)
    field -= lake * (LAKE_DEPTH_M / HEIGHT_M)
    field += grain * clear * 0.006                    # wind ripple, well under a dune
    field += (1.0 - clear) * grain * 0.0015           # the pan is flat, not dead flat
    return np.clip(field, 0.0, 1.0), xx, yy


def build_splat(field, xx, yy):
    """RGBA = sand, dirt, rock, cliff. Slope picks rock/cliff; the lake shore gets dirt."""
    gy, gx = np.gradient(field * HEIGHT_M, SIZE_M / (RES - 1))
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))

    rock = np.clip((slope - 20.0) / 14.0, 0.0, 1.0)
    cliff = np.clip((slope - 34.0) / 12.0, 0.0, 1.0)
    rock = np.clip(rock - cliff, 0.0, 1.0)

    # A damp band around the waterline reads as a shore rather than sand meeting water.
    water_norm = PLAYA - (1.5 / HEIGHT_M)            # must match the scene's lake level
    dirt = np.clip(1.0 - np.abs(field - water_norm) / (3.0 / HEIGHT_M), 0.0, 1.0)
    dirt = np.clip(dirt - cliff, 0.0, 1.0) * 0.85

    sand = np.clip(1.0 - rock - cliff - dirt, 0.0, 1.0)
    total = np.maximum(sand + dirt + rock + cliff, 1e-6)
    stack = [c / total for c in (sand, dirt, rock, cliff)]

    small = [np.asarray(Image.fromarray((c * 255).astype(np.uint8)).resize(
        (SPLAT_RES, SPLAT_RES), Image.BILINEAR)) for c in stack]
    return np.dstack(small).astype(np.uint8)


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    field, xx, yy = build_height()

    splat = build_splat(field, xx, yy)
    # Row 0 of the PNG is the terrain's local y = 0 -- its MINIMUM-y edge, which sits at world
    # y = Transform.y. That is the same order as the arrays built above, whose row 0 is
    # y = -SIZE_M/2, so the rows are written through unflipped: flipping them mirrors the whole
    # terrain in y and puts every feature placed by world coordinate on the opposite side of
    # the scene from where LAKE_CENTRE and PLAYA_CENTRE say it is.

    # 16-bit: an 8-bit heightmap terraces visibly over 24 m of range.
    Image.fromarray((field * 65535.0).astype(np.uint16), mode="I;16").save(OUT / "height.png")
    Image.fromarray(splat, mode="RGBA").save(OUT / "splat0.png")

    pan_z = -PLAYA * HEIGHT_M
    print(f"wrote {OUT}/height.png ({RES}x{RES}, 16-bit) and splat0.png ({SPLAT_RES}x{SPLAT_RES})")
    print(f"  scene Transform.z must be {pan_z:.1f}  (puts the pan at world z = 0)")
    print(f"  dune crests reach ~{(field.max() - PLAYA) * HEIGHT_M:.1f} m above the pan")
    print(f"  lake bed bottoms at ~{(field.min() - PLAYA) * HEIGHT_M:.1f} m")
