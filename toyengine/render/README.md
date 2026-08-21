# toyengine/render

The low-resolution deferred pixel-art frame graph.

| File | Purpose |
|---|---|
| [`pixel_render_pipeline.h`](pixel_render_pipeline.h) | `PixelRenderPipeline` — owns every target, UBO, descriptor set, and pass; `render(renderer, scene)` records the whole frame into one `Renderer::begin_frame()` call (no per-frame `vkQueueWaitIdle`, unlike blendy). |
| [`pixel_render_config.h`](pixel_render_config.h) | `PixelRenderConfig` — resolution, lighting, shadow, and post-process tunables. |
| [`pixel_math.h`](pixel_math.h) | Pure-CPU, dependency-light math: `compute_render_extent()`, `compute_letterbox()` (integer-scale upscale rect), `compute_pixel_density()` (camera pixel-snap). Exercised directly by `toyengine_tests` with no Vulkan device needed. |
| [`instance_stream.h`](instance_stream.h) | `InstanceStream` — per-frame-in-flight instance transform buffer; a from-scratch equivalent of gfxcoopa's `InstanceBatcher`, needed because that class is documented safe only under a per-frame `vkQueueWaitIdle`, which this pipeline doesn't do. |
| [`palette_lut.h`](palette_lut.h) | `PaletteLut::load()` — loads a palette PNG into an Nx1 NEAREST-filtered texture; extraction rule matches coopixel's `extract_palette_from_image()`. |
| [`passes/`](passes/) | Every render pass: `PixelLightingPass`, `PixelPostPass`, `UpscalePass`, `GBufferVisualizePass` (unused diagnostic). |

## Frame graph

1. Directional shadow depth (`ShadowMapTarget`, reused from gfxcoopa)
2. Point-light cubemap shadow, 6 faces (first shadow-caster only)
3. G-buffer geometry (`GBufferTarget` + gfxcoopa's `GBufferPipeline`)
4. Banded lighting + skybox → `offscreen_target_` (reused from gfxcoopa's `SkyboxPass`)
5. Outline + exposure + dither + palette → `post_target_`
6. Integer-scale letterboxed upscale → swapchain

Steps 1–5 record into `Renderer::begin_frame()`'s `pre_pass_fn`; step 6 is the
`record_fn`. See each pass's own file doc for descriptor set layout and
which blendy shader (if any) it was derived from.
