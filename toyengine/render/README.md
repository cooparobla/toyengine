# toyengine/render

The low-resolution deferred pixel-art frame graph. One pipeline, not a
choice between tracks: direct lighting is always the same banded-cel formula
(`light_bands`/`spec_threshold` in `assets/config.yaml`), and everything else
is independently toggleable on top of it —
`outline_enabled`/`palette_enabled`/`dither_enabled`/`camera_pixel_snap`
(pixel-art post/camera), `soft_shadows` (rotated-PCF instead of a single hard
compare), `ssao_enabled`, and `ssr_enabled` (also gates the SSGI diffuse-
bounce term, `ssgi_intensity`). Still renders at a low internal resolution
with an integer-scale upscale regardless of which toggles are set, so the
output always stays pixelated.

| File | Purpose |
|---|---|
| [`pixel_render_pipeline.h`](pixel_render_pipeline.h) | `PixelRenderPipeline` — owns every target, UBO, descriptor set, and pass; `render(renderer, scene)` records the whole frame into one `Renderer::begin_frame()` call (no per-frame `vkQueueWaitIdle` in general — see below for the `ssr_enabled` exception). |
| [`pixel_render_config.h`](pixel_render_config.h) | `PixelRenderConfig` — the seven feature toggles up front, then resolution, lighting, shadow, outline, palette, dither, SSAO, and SSR+SSGI tunables, grouped the same way as `assets/config.yaml`. |
| [`pixel_math.h`](pixel_math.h) | Pure-CPU, dependency-light math: `compute_render_extent()`, `compute_letterbox()` (integer-scale upscale rect), `compute_pixel_density()` (camera pixel-snap). Exercised directly by `toyengine_tests` with no Vulkan device needed. |
| [`instance_stream.h`](instance_stream.h) | `InstanceStream` — per-frame-in-flight instance transform buffer; a from-scratch equivalent of gfxcoopa's `InstanceBatcher`, needed because that class is documented safe only under a per-frame `vkQueueWaitIdle`, which this pipeline doesn't do. |
| [`palette_lut.h`](palette_lut.h) | `PaletteLut::load()` — loads a palette PNG into an Nx1 NEAREST-filtered texture; extraction rule matches coopixel's `extract_palette_from_image()`. |
| [`passes/`](passes/) | Every render pass: `PixelLightingPass`, `PixelPostPass`, `UpscalePass`, `SsrPass`, `GBufferVisualizePass` (unused diagnostic), plus `HiZPass`/`SceneColorMipPass`/`SsaoPass` reused directly from gfxcoopa. |

## Frame graph

1. Directional shadow depth (`ShadowMapTarget`, reused from gfxcoopa)
2. Point-light cubemap shadow, 6 faces (first shadow-caster only)
3. G-buffer geometry (`GBufferTarget` + gfxcoopa's `GBufferPipeline`)
4. If `ssr_enabled`: `HiZPass::execute()` (also performs the gbuffer-depth layout
   transition `pixel_post.frag`'s outline sampler needs, as a side effect). Otherwise: the
   manual transition (`transition_gbuffer_depth_to_shader_read_()`).
5. If `ssao_enabled`: `SsaoPass::execute()`. Otherwise: `SsaoPass::invalidate_history()`.
   `SsaoPass` itself is always constructed — `PixelLightingPass` (and `SsrPass`'s composite,
   when built) always have a `g_ssao` binding to fill, pointing at either `output_view()` or
   the pass's permanent neutral (fully-unoccluded) texture, decided once at construction from
   `ssao_enabled`.
6. `PixelLightingPass` + skybox → `offscreen_target_` (reused from gfxcoopa's `SkyboxPass`;
   always HDR `R16G16B16A16_SFLOAT`, since the sky-based indirect term can exceed 1.0
   regardless of which toggles are set)
7. If `ssr_enabled`: `SceneColorMipPass` (prefiltered scene-colour mip chain) → `SsrPass`
   (Hi-Z raymarch → temporal resolve → specular swap + SSGI diffuse bounce composite)
8. Exposure + ACES tonemap + outline + dither + palette → `post_target_`
9. Integer-scale letterboxed upscale → swapchain

Steps 1–8 record into `Renderer::begin_frame()`'s `pre_pass_fn`; step 9 is the
`record_fn`. See each pass's own file doc for descriptor set layout and
which blendy/gfxcoopa shader (if any) it was derived from.

**`ssr_enabled` synchronization exception:** `HiZPass`/`SceneColorMipPass`
(reused unmodified from gfxcoopa, only constructed when `ssr_enabled` is set)
rebind their own descriptors on every `execute()` call, which is only safe
under blendy's per-frame `vkQueueWaitIdle`. Since `PixelRenderPipeline`
otherwise overlaps `MAX_FRAMES_IN_FLIGHT` command buffers with no such wait,
`render()` calls `device_.wait_idle()` once per frame when `ssr_enabled` is
true — see that call site's comment for the full reasoning. Every other
toggle is unaffected.
