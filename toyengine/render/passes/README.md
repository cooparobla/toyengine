# toyengine/render/passes

Individual render passes making up the pixel-art frame graph. Each follows
gfxcoopa's pass convention: ctor takes `(Device&, RenderPass&, layouts...,
shader paths...)`, `set_*_image()` binders (called once, before
`begin_frame()` — `DescriptorSet::bind_image()` updates the descriptor set
immediately), and a `draw(cmd, ...)`.

| File | Reads | Writes | Notes |
|---|---|---|---|
| [`pixel_lighting_pass.h`](pixel_lighting_pass.h) | camera, light, shadow, G-buffer | `offscreen_target_` | A fork, not a reuse, of gfxcoopa's `DeferredLightingPass` — that class has a latent descriptor-set-index bug when built without GI (see its file doc). |
| [`pixel_post_pass.h`](pixel_post_pass.h) | scene color, G-buffer depth/normal, palette LUT | `post_target_` | Outline → exposure → Bayer dither → palette quantize, one fullscreen shader. |
| [`upscale_pass.h`](upscale_pass.h) | `post_target_` (NEAREST) | swapchain | Adds the x/y letterbox offset gfxcoopa's `PresentPass` doesn't support. |
| [`gbuffer_visualize_pass.h`](gbuffer_visualize_pass.h) | G-buffer albedo | — | Unused diagnostic, kept from the Phase 3 milestone (unlit G-buffer visualization) for debugging. |

Reused directly from gfxcoopa (not forked): `GBufferPipeline`, `ShadowPipeline`, `SkyboxPass`.
