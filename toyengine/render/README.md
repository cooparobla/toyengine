# toyengine/render

The low-resolution deferred pixel-art frame graph. One pipeline, not a choice between
tracks: everything is an independent toggle on top of the same G-buffer/deferred base, and
the frame always renders at a low internal resolution with a nearest-neighbour upscale
(`upscale_mode`: `fit`, the default, or `integer`) — so the output stays pixelated
whatever is switched on.

| File | Purpose |
|---|---|
| [`pixel_render_pipeline.h`](pixel_render_pipeline.h) | `PixelRenderPipeline` — owns every target, UBO, descriptor set and pass; `render()` records the whole frame into one `Renderer::begin_frame()` call. **Read its file doc first**: two rules (descriptors bound once at construction; per-frame-in-flight data needs per-slot buffers) explain most of the design. |
| [`pixel_render_config.h`](pixel_render_config.h) | `PixelRenderConfig` — feature toggles first, then resolution, lighting, shadow, outline, palette, dither, SSAO, SSR/SSGI, refraction, fog, volumetrics, SDF, bloom, DOF, tilt-shift and AA tunables, grouped the same way as `assets/config.yaml`. |
| [`pixel_render_types.h`](pixel_render_types.h) | The three push-constant blocks this engine appends to gfxcoopa's passes, and `SdfDrawItem`. |
| [`pixel_math.h`](pixel_math.h) | Pure-CPU, Vulkan-free math: render extent, letterbox/fit rects, pixel-snap density, SDF clip rects, view-space depth, exponential smoothing, and the directional-shadow frustum fit. Exercised directly by `toyengine_tests` with no device needed. |
| [`instance_stream.h`](instance_stream.h) | `InstanceStream` — per-frame-in-flight instance transform buffer. |
| [`forward_globals.h`](forward_globals.h) | `ForwardGlobalsData` — per-frame-in-flight UBO for the forward transparent pass's lighting/indirect/SSR/refraction tuning. |
| [`passes/`](passes/) | The passes toyengine defines itself; everything else is reused from gfxcoopa. |

## Startup-fixed vs runtime toggles

A toggle that selects **which image a pass reads** is baked into a descriptor when the
pipeline is built, and cannot change at runtime — `DescriptorSet::bind_image()` updates
immediately, and the frame loop overlaps command buffers with no wait. A toggle that only
changes push-constant contents is free to flip every frame.

| Startup-fixed | Runtime |
|---|---|
| `ssr_enabled`, `ssao_enabled`, `transparency_enabled`, `refraction_enabled`, `fog_enabled`, `volumetrics_enabled`, `bloom_enabled`, `dof_enabled`, `tilt_shift_enabled`, `aa_mode`, `world_ui_enabled`, `screen_ui_enabled`, and every resolution/capacity field | `sdf_enabled`, `shadows_enabled`, `sdf_shadows_enabled`, `ssr_reflect_transparent`, `debug_lines_enabled`, `ssao_debug_view`, `dof_debug_view`, `volumetrics_debug_view`, and every numeric tunable |

`apply_live_config()` enforces the split: it restores any startup-fixed field the caller
tried to change and names it in a warning, rather than accepting an edit that would
silently do nothing.

## Frame graph

Recorded in this order inside `Renderer::begin_frame()`'s `pre_pass_fn`; the `record_fn`
is only the final 1:1 blit into the swapchain. `render()` delegates to three stages —
`record_scene_()`, `record_post_chain_()` and `record_overlay_()` — which map onto the
three groups below.

**Scene** (`record_scene_()`):

1. Directional shadow depth, then the point-light cube map, 6 faces — first shadow-caster only.
2. G-buffer geometry, meshes and opaque/masked SDFs.
3. Hi-Z pyramid, when any of SSR / transparency / `ssr_reflect_transparent` is on. This also
   performs the G-buffer depth transition `pixel_stylize.frag`'s outline sampler needs; when
   it does not run, `transition_gbuffer_depth_to_shader_read_()` does it instead. Exactly one
   of the two must happen.
4. `ssr_reflect_transparent`: capture transparent geometry into its own target and build a
   second Hi-Z pyramid and scene-colour mip chain, so opaque surfaces can reflect it.
5. SSAO, or `invalidate_history()` when it is off.
6. Deferred lighting + skybox → `offscreen_target_` (HDR; the sky-based indirect term can
   exceed 1.0 whatever the toggles say). `ssao_debug_view` replaces both with a raw
   occlusion visualization.
7. Scene-colour mip chain, then SSR — Hi-Z raymarch → temporal resolve → specular swap plus
   SSGI diffuse bounce.
8. Refraction's own scene-colour chain, when refraction is on.
9. Forward transparent pass: BLEND meshes and BLEND SDFs merged into one back-to-front list,
   drawn in place into the SSR composite. BLEND *meshes* additionally refract; BLEND SDFs
   never do.

**Post** (`record_post_chain_()`):

10. Fog (analytic, global) → `fog_target_`.
11. Volumetrics (raymarched local `VolumeComponent`s) → `volumetrics_target_`.
12. Depth of field, at render resolution.
13. Bloom pyramid.
14. Exposure + ACES tonemap + outline + dither + palette → `post_target_`, with debug lines
    drawn as a guest in the same bracket.
15. World-space UI → `ui_world_target_`, at the display rect.
16. FXAA / SMAA / TAA → `aa_target_`, when `aa_mode != "off"`.
17. Tilt shift, at display resolution.

**Overlay** (`record_overlay_()`):

18. `ui_composite_pass_` draws the post-processed scene into the letterbox sub-rect of
    `overlay_target_` with the world-UI layer over it — performing the nearest upscale
    itself when tilt shift is off — then screen-space UI draws as a guest at full window
    resolution.
19. `record_fn`: `upscale_pass_` blits `overlay_target_` 1:1 into the swapchain.

Each stage is gated on its own toggle and skipped entirely when off.

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

Sizing it to the letterbox rect rather than `render_extent_` is what makes the composite sample it
**1:1**, so nothing resamples the UI at all. At `render_extent_` the composite needs a non-integer
NEAREST upscale — ×1.18 at a typical window — which duplicates roughly every fifth row and column
and makes world-canvas text read as visibly stepped next to the HUD.

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
