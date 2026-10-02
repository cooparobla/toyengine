# toyengine/render/passes

The render passes toyengine defines itself. Everything else in the frame graph is reused
from gfxcoopa — see the list at the bottom.

Each follows gfxcoopa's pass convention: the constructor takes
`(Device&, RenderPass&, layouts…, shader paths…)`, images are bound through
`set_*_image()` / `update_descriptors()`, and drawing goes through `draw(cmd, …)` or
`execute(cmd, …)`.

**Binders are called once, at construction.** `DescriptorSet::bind_image()` issues
`vkUpdateDescriptorSets` immediately, and `PixelRenderPipeline` overlaps
`MAX_FRAMES_IN_FLIGHT` command buffers with no per-frame wait — so rebinding from inside
the frame loop would update a set a still-pending submission references. See rule 1 in
`pixel_render_pipeline.h`'s file doc.

| File | Reads | Writes | Notes |
|---|---|---|---|
| [`upscale_pass.h`](upscale_pass.h) | `overlay_target_` (NEAREST) | swapchain | A 1:1 blit. Adds the x/y destination-rect offset gfxcoopa's `PresentPass` does not support; the rect comes from `pixel_math::compute_display_rect()`. The actual pixel-art upscale happens one stage earlier, in `ui_composite_pass.h`. |
| [`ui_composite_pass.h`](ui_composite_pass.h) | post/AA or tilt-shift result, world-UI layer | `overlay_target_` | Composites the world-space UI over the finished frame, *after* AA and tilt shift, and performs the nearest upscale when tilt shift is off. Structurally a two-source `UpscalePass`. |
| [`debug_line_pass.h`](debug_line_pass.h) | — | `post_target_` (as a guest) | Physics collider wireframes and contact normals, `LineList`, depth test off. Deliberately physxcoopa-free: `Engine::tick()` converts `PhysicsWorld::debug_draw()` output into the neutral `toy::render::DebugLine`. |
| [`contact_shadow_pass.h`](contact_shadow_pass.h) | camera + light UBOs, G-buffer, scene depth, the shared accumulation count | its own resolved occlusion buffer | The screen-space contact march (`contact_shadow.frag`, including the same `contact_shadow_body.glsl` the lighting shader used to call inline), then a temporal resolve that reuses gfxcoopa's `ssr_resolve.frag` verbatim -- a scalar occlusion in `.r` is a vec4 whose other channels are zero, so there is no third copy of the accumulation logic. `pixel_lighting.frag` and `debug_view.frag` sample the result through an `ExtraSets` set instead of marching. Always constructed and always executed; `contact_shadows_enabled` is runtime, and with `contact_params.x` at 0 the march early-outs and the buffer reads 0. |
| [`fullscreen_blit_pass.h`](fullscreen_blit_pass.h) | one sampled texture | whatever target it is built against | Minimal unlit fullscreen pass, for a "show me this one texture" diagnostic. Not currently instantiated by `PixelRenderPipeline` -- `debug_view`'s channels are drawn by gfxcoopa's `DeferredLightingPass` instead (see `debug_view.frag`), since most of them need the camera/light/shadow sets this pass doesn't declare. |

## Reused directly from gfxcoopa

Constructed **unconditionally** and gated per frame at their record site:
`GBufferPipeline`, `ShadowPipeline`, `DeferredLightingPass` (with this
engine's banded-cel `pixel_lighting.frag`, which also draws the procedural sky at background
pixels -- gfxcoopa's `SkyboxPass` is not used), `SsaoPass`, `HiZPass`, `SceneColorMipPass`,
`SsrPass`, `TemporalHistoryPass`, `TransparentPass`, `TransparentCapturePass`, `FogPass`, `VolumetricsPass` /
`FroxelVolumetricsPass` (per `volumetrics_mode`),
`DofPass`, `BloomPass`, `TiltShiftPass`, `PixelStylizePass`, and the four SDF passes
(`SdfGBufferPass`, `SdfForwardPass`, `SdfShadowPass`, `SdfCapturePass`).

`FxaaPass`, `SmaaPass` and `TaaPass` are the exception: all three are built together only
when `aa_mode != "off"`, because that toggle changes which descriptor downstream passes
are bound to.

`PaletteLut` and `PixelStylizePass` are shared with blendy — exposure → ACES tonemap →
outline (alpha-blended over the tonemapped colour) → Bayer dither → palette quantize, one
fullscreen shader, always run the same way; each stage no-ops when its config disables it.

## The one synchronization caveat

`HiZPass`, `SceneColorMipPass` and `TransparentCapturePass` rebind their own descriptors
inside every `execute()`. That is safe only under a per-frame wait, which this pipeline
otherwise avoids — so a frame that runs any of them pays for a single `device_.wait_idle()`
before recording. See `FrameContext::need_ssr_trace_inputs`.
