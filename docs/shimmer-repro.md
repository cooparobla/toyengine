# Reproducing the terrain shimmer capture

`output/ring_capture_lossless.mp4` is the reference recording of the temporal
shimmer artifact on `terrain_test` — Coopa's own live play, captured engine-side
as lossless PNGs (no screen recorder, no video codec in the capture chain) and
encoded losslessly afterward. **That clip shows the issue**; this document is the
recipe to regenerate an equivalent capture, the measurements that characterise
it, and the state of the investigation so far.

## TL;DR — regenerate the capture

```bash
# 1. Config: assets/config.yaml must have the "crisp" settings below.
# 2. Build.
cbuild            # or: cmake -B build && cmake --build build

# 3. Play with the rolling lossless capture armed (keeps the LAST 300 frames
#    in RAM, ~2.5 GB; writes them to output/seq/ only when you quit):
SCENE=terrain_test CAPTURE_RING=300 ./build/toyengine

# 4. Reproduce the gesture (below), QUIT RIGHT AFTER the shimmer was visible —
#    the ring holds the final ~5 s. Confirm output/seq/ has 300 fresh PNGs
#    (rm -rf output/seq first if in doubt: stale frames from an earlier run
#    otherwise survive when a new run writes fewer files).

# 5. Encode losslessly. LOSSY ENCODES HIDE THIS ARTIFACT — crf 14 provably
#    erased it in earlier rounds; only -qp 0 is trustworthy:
FF=$(python3 -c "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())")
$FF -y -framerate 60 -i output/seq/frame_%04d.png \
    -c:v libx264 -qp 0 -pix_fmt yuv444p output/ring_capture_lossless.mp4
```

## Config state that produced the reference clip

`assets/config.yaml`, `render:` section — the load-bearing values (the reference
clip was recorded 2026-09-22 with exactly these):

```yaml
aa_mode: off                  # deliberately off: nothing spatially filters the artifact
dof_enabled: false            # deliberately off: no lens blur masking it
tilt_shift_enabled: false     # deliberately off
ssao_enabled: true
ssr_enabled: true             # also enables the SSGI bounce (ssgi_intensity: 0.6)
shadows_enabled: true
soft_shadows: true            # Vogel-disk PCF, shadow_softness 0.15
soft_lighting: true
bloom_enabled: true
volumetrics_enabled: true
resolution_mode: fixed        # 1920x1080 internal == display, 1:1 pixels
render_width: 1920
render_height: 1080
ssao_quality: high            # slices 2 / steps 16 / max_radius_px 80 / temporal_frames 32
ssao_radius: 2.0
ssao_power: 1.0
ssao_direct_lighting_strength: 0.25
ssr_temporal_blend: 0.85
```

Window: 1920x1080, `vsync: true`, visible. Scene: `assets/scenes/terrain_test/`
(launch with `SCENE=terrain_test`; the config's `default_scene` is pixel_demo).

Build-state caveat: the reference clip predates `kFrozenSsrBlend`
(`pixel_render_pipeline.h`, added 2026-09-23), which shortens the post-stop
SSR/SSGI convergence wash from ~1 s to ~0.2 s. A regeneration on a current build
reproduces the rest of the clip's behaviour but with that one component
shortened; to reproduce the reference exactly, set `kFrozenSsrBlend = 0.85f`
temporarily (making the frozen path equal to `ssr_temporal_blend` again).

## The gesture

Real mouse/keyboard play — deliberately NOT a scripted camera, since several
mechanisms only exist under real input and real frame pacing:

- Camera close to the terrain and pitched steeply down (orbit distance roughly
  10–15, looking down at nearby block faces; the reference clip's framing).
- Repeated quick mouse flicks (fast yaw sweeps, a few hundred ms each) each
  followed by ~0.5–1 s of holding still, hand resting on the mouse. About ten
  flick-and-hold cycles over the 5 s window.
- The artifact reads as shimmer on close block faces **during the motion and in
  the first fraction of a second after each stop**.

## What the capture measures as (reference-clip numbers)

Per-frame mean |Δ| (levels/px, 0–255) over consecutive frames, computed on the
raw PNGs — the analysis loop every round of this investigation has used:

```python
from PIL import Image
import numpy as np
prev = None
for i in range(300):
    a = np.asarray(Image.open(f'output/seq/frame_{i:04d}.png').convert('L'), dtype=np.int16)
    if prev is not None:
        print(i, float(np.abs(a - prev).mean()))
    prev = a
```

- Motion phases: 9–16 levels/px of churn (block textures re-rasterising at 1:1
  with `aa_mode: off`, plus all shading following the sweep).
- After each stop: a settle tail of ~6–10 frames — 0.7 → 0.4 → 0.2 → 0.04 →
  0.01 — then **exact byte-static zero** (the freeze contract holds; there are
  no engine-side pops at rest; isolated full-frame "pops" seen in desktop
  screencasts of the same play do NOT exist in engine-side frames and are
  artifacts of the recording chain).
- The settle tail decomposes (measured by subsystem ablation at the same pose,
  `ssao_blip_probe` in test.cpp): SSAO reaches byte-zero within ~9 frames;
  the ~1 s remainder is the SSR/SSGI temporal EMA converging (now shortened by
  `kFrozenSsrBlend`); with `ssr_enabled: false` the tail vanishes entirely.

## Investigation state (2026-09-23)

Confirmed NOT the cause (all measured, see the memory ledger and
`test.cpp`'s gated probes `ssao_travel_probe` / `ssao_blip_probe`,
run via `SSAO_PROBE_DUMP=1 ./build/toyengine_tests <name>`):
estimator sample counts, blur plane-sigma, AO temporal depth, direct-lighting
AO, integer blur-spacing snaps (fixed anyway), freeze entry/exit transitions,
stillness-deadband semantics (fixed anyway), shadow PCF rotation at rest,
video-encoder keyframes (engine-side frames are codec-free).

**Root cause identified and fixed (2026-09-23, `texel_aa`)**: frame-level
inspection of the reference clip pinned the dominant visible shimmer as
NEAREST-sampled magnified texels snapping to the screen pixel grid. The block
textures are 16x16-texel atlas cells (`tile_types.h`); at close range each
texel spans ~6-10 screen px, and with point sampling every texel boundary
flips whole screen pixels each time the camera moves sub-pixel — with
`aa_mode: off` nothing softened it, so every close face crawled/sizzled in
motion. The fix is texel-AA ("anti-aliased point sampling"): material textures
bind with `SamplerDesc::pixel_art_smooth()` (LINEAR) and `gbuffer.frag` routes
every material fetch through `gfx_texel_aa_uv()` (`gfx/texel_aa.glsl`), which
confines the bilinear blend to a one-screen-pixel band at each texel boundary.
Texel interiors stay flat and hard (the pixel-art look is preserved at rest);
boundaries glide sub-pixel in motion instead of snapping. Toggle:
`texel_aa: true|false` in config.yaml (startup-fixed; false = raw NEAREST for
A/B). Evidence, on THIS document's reproduction (the `texel_aa_ring_repro` probe in
test.cpp scripts the reference gesture: same framing -- close blocks, camera pitched
steeply down -- five quick-flick-then-hold cycles over five seconds, 300 frames,
ending at rest; regenerate with
`SSAO_PROBE_DUMP=1 ./build/toyengine_tests texel_aa_ring_repro`):
- `output/ring_repro_texelaa_off_lossless.mp4` -- the fix off (the reference look)
- `output/ring_repro_texelaa_on_lossless.mp4`  -- the fix on
- `output/ring_repro_texelaa_sbs_lossless.mp4` -- centre crops side by side (left off,
  right on), frame-aligned.

Remaining smaller temporal terms, in order: geometric silhouette aliasing
(re-enable `aa_mode: smaa` if wanted — texel-AA does not touch mesh edges),
the SSR/SSGI post-stop wash (already shortened by `kFrozenSsrBlend`), and the
round-7 AO parity floor.
