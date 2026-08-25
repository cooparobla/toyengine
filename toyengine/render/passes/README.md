# toyengine/render/passes

Individual render passes making up the pixel-art frame graph (see
`../README.md` for the toggle list and frame graph). Each follows gfxcoopa's
pass convention: ctor takes `(Device&, RenderPass&, layouts..., shader
paths...)`, `set_*_image()`/`update_descriptors()` binders (called once, at
construction — `DescriptorSet::bind_image()` updates the descriptor set
immediately, which is unsafe to redo from inside the per-frame render loop
without either a per-frame wait or double-buffered descriptor sets; see
`pixel_render_pipeline.h`'s `device_.wait_idle()` call site for the one place
that rule gets bent, and why), and a `draw(cmd, ...)`/`execute(cmd, ...)`.

| File | Reads | Writes | Notes |
|---|---|---|---|
| [`pixel_lighting_pass.h`](pixel_lighting_pass.h) | camera, light, shadow, G-buffer, SSAO | `offscreen_target_` | A fork, not a reuse, of gfxcoopa's `DeferredLightingPass` — that class has a latent descriptor-set-index bug when built without GI (see its file doc). Banded/masked-specular direct lighting is fixed (see `pixel_lighting.frag`'s file doc); the SSAO binding and the sky-based indirect term are always present, toggle or not; shadows are always a single hard depth compare. |
| [`ssr_pass.h`](ssr_pass.h) | camera, G-buffer, Hi-Z, scene-colour mips, SSAO | own composite target | Hi-Z raymarch → temporal resolve → composite (specular swap + SSGI diffuse bounce). A structural port of gfxcoopa's `SsrPass` with the reflection-probe/GI descriptor set and `half_res` removed — see its file doc. Only constructed when `ssr_enabled` is set. |
| [`upscale_pass.h`](upscale_pass.h) | `post_target_` (NEAREST) | swapchain | Adds the x/y letterbox offset gfxcoopa's `PresentPass` doesn't support. |
| [`gbuffer_visualize_pass.h`](gbuffer_visualize_pass.h) | G-buffer albedo | — | Unused diagnostic, kept from the Phase 3 milestone (unlit G-buffer visualization) for debugging. |

Reused directly from gfxcoopa (not forked): `GBufferPipeline`, `ShadowPipeline`,
`SkyboxPass` (always); `SsaoPass` (always constructed, only its per-frame
`execute()` is gated on `ssao_enabled`); `HiZPass`, `SceneColorMipPass`
(only constructed when `ssr_enabled` is set — see `../README.md` for the
synchronization caveat that comes with reusing these two under this
pipeline's frame-overlap model); `PixelStylizePass` and `PaletteLut`
(`gfxcoopa/engine/passes/pixel_stylize_pass.h` / `gfxcoopa/engine/data/palette_lut.h`,
shared with blendy) — exposure → ACES tonemap (gated on `PushConstants::exposure`,
since blendy already tonemapped by the time it reaches this pass but toyengine
hasn't) → outline (alpha-blended over the tonemapped color, not a hard replace)
→ Bayer dither → palette quantize, one fullscreen shader, always run the same
way regardless of which toggles are set (each stage no-ops on its own when its
config disables it).
