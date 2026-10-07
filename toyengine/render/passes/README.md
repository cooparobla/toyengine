# toyengine/render/passes

The render passes toyengine defines itself. Everything else in the frame graph is a
gfxcoopa pass class, reused as-is — see the list at the bottom. The shaders all of them load
(gfxcoopa's included, apart from SMAA's) live in this repo's `assets/shaders/`.

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
| [`contact_shadow_pass.h`](contact_shadow_pass.h) | camera + light UBOs, G-buffer, scene depth, the shared accumulation count | its own resolved R16F occlusion buffer | The screen-space contact march (`contact_shadow.frag`, built on `contact_shadow_body.glsl`), then a temporal resolve (`contact_shadow_resolve.frag`), the scalar counterpart of `ssr_resolve.frag`: same push constants, bindings and converging `1/N` schedule, without the YCoCg/vec4 machinery. `pixel_lighting.frag` and `debug_view.frag` sample the result through an `ExtraSets` set instead of marching. Always constructed and always executed; `contact_shadows_enabled` is runtime, and with `contact_params.x` at 0 the march early-outs and the buffer reads 0. |
| [`particle_pass.h`](particle_pass.h) | camera, lights, Hi-Z mip 0 (soft particles), material set (sprite texture) | the HDR scene colour, inside `transparent_pass_`'s bracket | Instanced particle quads. There is no vertex buffer: five vec4s per instance, expanded to six vertices in `particle.vert` by render mode. It uses one premultiplied-blend pipeline for both alpha and additive (the shader scales alpha by `1 - additive`). Built against `TransparentPass::render_pass()` and drawn in `record_transparent_()`'s back-to-front list, the same arrangement `SdfForwardPass` has. Its per-frame-in-flight instance buffer doubles when it runs out of room. |
| [`underwater_pass.h`](underwater_pass.h) | the post chain's source image, G-buffer position/normal | its own target, which the rest of the post chain reads | The underwater look (`underwater.frag`): fog, colour absorption, caustics and shimmer along the in-water length of each view ray. First in the post chain, ahead of global fog. Built only when `underwater_enabled`; once built it runs every frame and is a plain copy while the camera is above water. All parameters ride in one 128-byte push-constant block. |
| [`transparent_preview_pass.h`](transparent_preview_pass.h) | camera, material set, G-buffer depth | `post_target_` (as a guest) | BLEND meshes for the editor's Solid / Material Preview / Wireframe shading modes, drawn back-to-front over the debug-view image with `editor_shading.glsl` maths (`transparent.vert` + `transparent_preview.frag`). Occlusion is a per-fragment compare against the sampled G-buffer depth. |
| [`fullscreen_blit_pass.h`](fullscreen_blit_pass.h) | one sampled texture | whatever target it is built against | Minimal unlit fullscreen pass, for a "show me this one texture" diagnostic. Not currently instantiated by `PixelRenderPipeline` -- `debug_view`'s channels are drawn by gfxcoopa's `DeferredLightingPass` instead (see `debug_view.frag`), since most of them need the camera/light/shadow sets this pass doesn't declare. |

## Reused directly from gfxcoopa

Constructed **unconditionally** and gated per frame at their record site:
`GBufferPipeline`, `ShadowPipeline`, `DeferredLightingPass` (with this
engine's banded-cel `pixel_lighting.frag`, which also draws the procedural sky at background
pixels), `SsaoPass`, `HiZPass`, `SceneColorMipPass`,
`SsrPass`, `TemporalHistoryPass`, `TransparentPass`, `FogPass`, `VolumetricsPass` /
`FroxelVolumetricsPass` (per `volumetrics_mode`),
`DofPass`, `BloomPass`, `TiltShiftPass`, `PixelStylizePass`, and the three SDF passes
(`SdfGBufferPass`, `SdfForwardPass`, `SdfShadowPass`).

The exceptions are built only when their startup switch is on, because the switch changes
which descriptor downstream passes are bound to: `FxaaPass`, `SmaaPass` and `TaaPass` (all
three together, when `aa_mode != "off"`), `ExposurePass` (`auto_exposure_enabled`), and
`UnderwaterPass` (`underwater_enabled`).

`PaletteLut` and `PixelStylizePass` are shared with blendy — exposure → ACES tonemap →
outline (alpha-blended over the tonemapped colour) → Bayer dither → palette quantize, one
fullscreen shader, always run the same way; each stage no-ops when its config disables it.

## The one synchronization caveat

`HiZPass` and `SceneColorMipPass` bind their own descriptors inside `execute()`. They bind
lazily and skip the write when the source view is unchanged, so only a frame that would
actually write one (the first) pays a `device_.wait_idle()` before recording. See
`trace_inputs_need_rebind_()` in `pixel_render_pipeline.h`.
