# toyengine/render

The low-resolution deferred pixel-art frame graph. One pipeline, not a
choice between tracks: direct lighting is always the same banded-cel formula
(`light_bands`/`spec_threshold` in `assets/config.yaml`), and everything else
is independently toggleable on top of it —
`outline_enabled`/`palette_enabled`/`dither_enabled`/`camera_pixel_snap`
(pixel-art post/camera), `ssao_enabled`, and `ssr_enabled` (also gates the
SSGI diffuse-bounce term, `ssgi_intensity`). Shadows are always a single hard
depth compare. Still renders at a low internal resolution
with a nearest-neighbour upscale (`upscale_mode`: `fit`, the default, or
`integer`) regardless of which toggles are set, so the output always stays
pixelated.

| File | Purpose |
|---|---|
| [`pixel_render_pipeline.h`](pixel_render_pipeline.h) | `PixelRenderPipeline` — owns every target, UBO, descriptor set, and pass; `render(renderer, scene)` records the whole frame into one `Renderer::begin_frame()` call (no per-frame `vkQueueWaitIdle` in general — see below for the `ssr_enabled` exception). |
| [`pixel_render_config.h`](pixel_render_config.h) | `PixelRenderConfig` — the six feature toggles up front, then resolution, lighting, shadow, outline, palette, dither, SSAO, and SSR+SSGI tunables, grouped the same way as `assets/config.yaml`. |
| [`pixel_math.h`](pixel_math.h) | Pure-CPU, dependency-light math: `compute_render_extent()`, `compute_display_rect()` (dispatches to `compute_fit()`'s aspect-preserving best fit or `compute_letterbox()`'s integer-scale rect, per `upscale_mode`), `compute_pixel_density()` (camera pixel-snap). Exercised directly by `toyengine_tests` with no Vulkan device needed. |
| [`instance_stream.h`](instance_stream.h) | `InstanceStream` — per-frame-in-flight instance transform buffer; a from-scratch equivalent of gfxcoopa's `InstanceBatcher`, needed because that class is documented safe only under a per-frame `vkQueueWaitIdle`, which this pipeline doesn't do. |
| [`passes/`](passes/) | Every toyengine-specific render pass: `PixelLightingPass`, `UpscalePass`, `SsrPass`, `GBufferVisualizePass` (unused diagnostic), plus `HiZPass`/`SceneColorMipPass`/`SsaoPass` reused directly from gfxcoopa. `PaletteLut` and `PixelStylizePass` (the outline/dither/palette overlay) live in gfxcoopa too, shared with blendy. |

## Frame graph

1. Directional shadow depth (`ShadowMapTarget`, reused from gfxcoopa)
2. Point-light cubemap shadow, 6 faces (first shadow-caster only)
3. G-buffer geometry (`GBufferTarget` + gfxcoopa's `GBufferPipeline`)
4. If `ssr_enabled`: `HiZPass::execute()` (also performs the gbuffer-depth layout
   transition `pixel_stylize.frag`'s outline sampler needs, as a side effect). Otherwise: the
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
7a. If `transparency_enabled` and `refraction_enabled` and
    `refraction_include_reflections`: `SceneColorMipPass` re-run against the SSR composite
    output (or the plain lit image if SSR is off), so the forward transparent pass below
    refracts a background that includes SSR reflections, not the pre-SSR image the chain
    otherwise still holds.
7b. If `transparency_enabled`: `TransparentPass` (+ `SdfForwardPass` for BLEND SDFs) —
    forward-shaded alpha-blended geometry, back-to-front sorted, drawn in place into the
    SSR composite (or `offscreen_target_` if SSR is off). BLEND *mesh* objects additionally
    apply screen-space refraction (bend/blur/tint/chromatic-aberration/Fresnel — see
    `assets/shaders/refraction.glsl`) when `refraction_enabled` and the object's own
    material opt in; BLEND SDF objects never refract (see the refraction plan).
8. Exposure + ACES tonemap + outline + dither + palette → `post_target_`
8a. If `aa_mode != "off"`: FXAA/SMAA/TAA (whichever `aa_mode` selects) → `aa_target_`,
    ported from blendy's `PbrRenderPipeline` (`FxaaPass`/`SmaaPass`/`TaaPass`, all reused
    from gfxcoopa) — see `PixelRenderConfig::aa_mode`'s own doc. Runs at the low internal
    resolution, after the pixel-art post stack and before the upscale, so it's the last
    stage that still sees individual low-res texels.
9. Nearest-neighbour upscale (fit or integer-scale letterboxed, per `upscale_mode`) → swapchain

Steps 1–8a record into `Renderer::begin_frame()`'s `pre_pass_fn`; step 9 is the
`record_fn`. See each pass's own file doc for descriptor set layout and
which blendy/gfxcoopa shader (if any) it was derived from.

**This list is not exhaustive** — it predates fog, bloom, tilt-shift, and the SDF
raymarching system, all of which also have their own frame-graph steps. See
`pixel_render_pipeline.h`'s own file doc (top of that file) and its `render()` method
for the accurate, up-to-date ordering.

**`ssr_enabled` synchronization exception:** `HiZPass`/`SceneColorMipPass`
(reused unmodified from gfxcoopa, only constructed when `ssr_enabled` is set)
rebind their own descriptors on every `execute()` call, which is only safe
under blendy's per-frame `vkQueueWaitIdle`. Since `PixelRenderPipeline`
otherwise overlaps `MAX_FRAMES_IN_FLIGHT` command buffers with no such wait,
`render()` calls `device_.wait_idle()` once per frame when `ssr_enabled` is
true — see that call site's comment for the full reasoning. Every other
toggle is unaffected.
