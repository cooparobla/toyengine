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

## UI layers

UI is composited **after every post effect**, in a stage of its own at the end of the frame.
That is a hard rule here, not a default: DoF, FXAA/SMAA/TAA and tilt shift are all filters over
the finished image, and a canvas that rides along inside `post_target_` gets fed through them —
TAA ghosts a canvas moving under an orbiting camera, and tilt shift smears the whole UI along
with the scene.

There are two layers. **Neither is rasterized at the internal render resolution** — UI is not part
of the pixel-art image, it is drawn on top of the finished one:

| Layer | Pass | Resolution | Purpose |
| --- | --- | --- | --- |
| `world_ui_enabled` | uicoopa `UiWorldPass` → `ui_world_target_` | display (letterbox) rect | `render_mode: WorldSpace` canvases, as real 3D geometry |
| `screen_ui_enabled` | uicoopa `UiPass` → `overlay_target_` | full window | `ScreenSpaceOverlay` canvases — an HUD |

The chain:

```
                   render extent                                 window
[scene] → DoF → stylize + debug lines → AA → tilt shift ──┐
                                                           ├─→ ui_composite_pass_ ──┐
[world canvases] → ui_world_target_ (RGBA8, cleared a=0) ──┘         (1:1)          │
                        at the letterbox rect                                        ▼
                                                          [screen canvases] → overlay_target_
                                                                                    │
                                                                    upscale_pass_ (1:1) → swapchain
```

`ui_world_target_` is an `OffscreenTarget` at `upscaled_extent_` (the letterbox rect), cleared to a
fully **transparent** black — it is a coverage layer, not an image, and `ui_composite.frag` reads
its alpha as "how much UI is here". `UiWorldPass` draws it with `BlendMode::AlphaOver` rather than
`Alpha`: the two differ only in destination alpha, and `Alpha`'s `dstAlpha = ZERO` would leave the
layer's alpha equal to the *last* fragment's instead of accumulated coverage.

It used to live at `render_extent_`, on the theory that world UI belonged on the same pixel grid as
the scene. What that actually bought was a non-integer NEAREST upscale in the composite — ×1.18 at a
typical window — duplicating roughly every fifth row and column, which is what made world-canvas
text read as visibly stepped next to the HUD. At the letterbox size the composite samples it **1:1**
and nothing resamples the UI at all.

`ui_composite_pass_` draws the post-processed scene into the letterbox rect of `overlay_target_`
with that layer composited over it (a premultiplied "over"). The *base* is still nearest-sampled at
destination UVs, so when tilt shift is off this is where the scene's pixel-art upscale happens; the
*UI* is a straight texel-for-texel fetch. `overlay_target_` is sized to the whole swapchain, not the
letterbox rect, so the bars belong to it and a screen-space HUD can draw over them; `upscale_pass_`
is left as a 1:1 blit whose only remaining job is `upscale.frag`'s `srgb_decode()`.

Three consequences worth knowing:

- **`low_res_color_image()` contains no UI at all** — it is the scene plus debug lines. Use
  `final_color_image()` (what `save_screenshot(low_res=false)` and `save_low_res: false` read)
  for a capture with UI in it.
- **The window-sized chain is rebuilt when *either* the swapchain extent or the letterbox rect
  changes**, and they are not the same trigger: the swapchain sizes `overlay_target_`, the letterbox
  sizes `ui_world_target_` and `tilt_shift_pass_`, and under `upscale_mode: integer` the rect snaps
  to `render_extent_ * floor(scale)` so the window can change size with the rect unchanged.
  `rebuild_overlay_chain_()` moves the targets and **both** UI passes together, because
  `TexturedQuad2DPass` (behind both) stores the `RenderPass&` it was built against.
- **That rebuild runs first in `render()`, before the canvas gather, and must stay there.** A
  rebuilt UI pass has no registered textures, no text-atlas marks and no streaming-buffer capacity;
  the gather (`register_textures()` / `mark_text_atlases()` / `begin_frame()`) is what installs
  them. Rebuilding after it means that frame draws every texture as the 1×1 white fallback, every
  glyph atlas through the RGBA "quad" variant, and uploads geometry into a buffer never sized for it
  — `Buffer::upload` is an unchecked `memcpy`.

Three things are specific to the world-space pass and worth knowing before touching it:

- **Occlusion is a shader-side compare, not a depth test.** `ui_world_target_` has its own D32
  attachment, but it is cleared at `begin()` and only ever written by a fullscreen triangle, so
  it holds nothing about the scene. The real scene depth is `gbuffer_target_.depth_view()`,
  handed to the pass as a sampled texture at set 1 through gfxcoopa's `ExtraSets`, and compared
  per fragment. The descriptor is bound **once** (`bind_image()` issues `vkUpdateDescriptorSets`
  immediately), against the startup-fixed `gbuffer_target_` — which is why `world_ui_enabled` is
  startup-fixed even though the *pass* is now rebuilt on resize. Note that depth is at
  `render_extent_` while the layer is at the letterbox rect, so an occluded edge stair-steps on the
  render grid while the canvas's own edges are crisp — the one thing the display-resolution layer
  trades away.
- **The viewport must be re-set to negative height.** These vertices go through a real
  `view_proj`, so a positive-height viewport mirrors the canvas vertically — and the viewport
  in effect at this point is whatever `PixelStylizePass::draw()` last set, which is *positive*.
  Depending on inherited state here would make correctness hinge on unrelated feature flags.
  `UiWorldPass::draw()` sets it itself, exactly as `DebugLinePass::draw()` does.
- **Canvases are depth-sorted, then appended into one buffer.** World UI composites with the
  depth test off, so overlapping canvases resolve purely by draw order; `render()` stable-sorts
  them by `sort_order` first and view-space depth second (farthest first). They then all stream
  into a single vertex/index buffer pair per frame-in-flight, which is why
  `UiWorldPass::begin_frame()` sizes for the whole frame up front — growing mid-frame would
  reallocate out from under geometry earlier canvases already uploaded.

`Engine::drive_ui_canvases_()` is the other half: it seeds each canvas's default texture,
refreshes its world transform, and converts the cursor into a world ray for hit testing,
between `Scene::update()` and `Scene::late_update()`.

It sizes the two kinds of canvas differently, and that split is load-bearing rather than
cosmetic: `CanvasComponent::set_input()` divides the window-pixel cursor by the scale factor
`set_viewport()` derived, so a **world** canvas takes the render extent (with
`build_pointer_ray_()` mapping the cursor through the letterbox into that space) while a
**screen** canvas takes the window extent, where the raw cursor already lives. Sizing a screen
canvas to the render extent puts every hit test off by the upscale factor plus the letterbox
offset.
