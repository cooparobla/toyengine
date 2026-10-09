# toyengine/render

The deferred frame graph. One pipeline, not a choice between tracks: everything is an
independent toggle on top of the same G-buffer/deferred base. The frame renders at an
internal resolution (`resolution_mode` / `render_width` / `render_height` /
`scale_divisor`; this repo's `assets/config.yaml` uses 1920×1080, the struct defaults
480×270 for a pixel-art look) and is nearest-neighbour upscaled to the window
(`upscale_mode`: `fit`, the default, or `integer`).

| File | Purpose |
|---|---|
| [`pixel_render_pipeline.h`](pixel_render_pipeline.h) | `PixelRenderPipeline` — owns every target, UBO, descriptor set and pass; `render()` records the whole frame into one `Renderer::begin_frame()` call. **Read its file doc first**: two rules (descriptors bound once at construction; per-frame-in-flight data needs per-slot buffers) explain most of the design. |
| [`pixel_render_config.h`](pixel_render_config.h) | `PixelRenderConfig` — feature toggles first, then resolution, lighting, shadow, outline, palette, dither, SSAO, SSR/SSGI, refraction, fog, volumetrics, SDF, bloom, DOF, tilt-shift and AA tunables, grouped the same way as `assets/config.yaml`. |
| [`pixel_render_types.h`](pixel_render_types.h) | The three push-constant blocks this engine appends to gfxcoopa's passes, and `SdfDrawItem`. |
| [`pixel_math.h`](pixel_math.h) | Pure-CPU, Vulkan-free math: render extent, letterbox/fit rects, pixel-snap density, SDF clip rects, view-space depth, exponential smoothing, and the directional-shadow frustum fit. Exercised directly by `toyengine_tests` with no device needed. |
| [`instance_stream.h`](instance_stream.h) | `InstanceStream` — per-frame-in-flight instance transform buffer. |
| [`forward_globals.h`](forward_globals.h) | `ForwardGlobalsData` — per-frame-in-flight UBO for the forward transparent pass's lighting/indirect/SSR/refraction tuning. |
| [`particle_types.h`](particle_types.h) | The plain-data contract with `toyengine/particles/`: `ParticleInstance` (the 80-byte GPU instance), `ParticleLook`, and the per-frame quad and mesh batches handed over by `set_particle_state()`. |
| [`local_shadow_atlas.h`](local_shadow_atlas.h) | `LocalShadowAtlas` — the shared depth atlas for shadowed point and spot lights. |
| [`visibility.h`](visibility.h) | CPU visibility math for the mesh draw path: frustum planes, world bounds, projected size and LOD selection. |
| [`frame_profile.h`](frame_profile.h), [`gpu_profiler.h`](gpu_profiler.h) | The `PROFILE` mode's CPU phase timings and per-feature GPU timestamp scopes. |
| [`passes/`](passes/) | The passes toyengine defines itself; everything else is a gfxcoopa pass class. Every shader any of them loads, apart from SMAA's, is in this repo's `assets/shaders/`. |

## Startup-fixed vs runtime toggles

A toggle that selects **which image a pass reads** is baked into a descriptor when the
pipeline is built, and cannot change at runtime — `DescriptorSet::bind_image()` updates
immediately, and the frame loop overlaps command buffers with no wait. A toggle that only
changes push-constant contents is free to flip every frame.

| Startup-fixed | Runtime |
|---|---|
| `ssr_enabled`, `ssgi_traced`, `ssao_enabled`, `transparency_enabled`, `refraction_enabled`, `fog_enabled`, `volumetrics_enabled`, `bloom_enabled`, `dof_enabled`, `tilt_shift_enabled`, `auto_exposure_enabled`, `grading_lut_path`, `aa_mode`, `skinning`, `world_ui_enabled`, `screen_ui_enabled`, and every resolution/capacity field | `sdf_enabled`, `shadows_enabled`, `sdf_shadows_enabled`, `shadow_pcss_enabled`, `contact_shadows_enabled`, `volumetrics_shadows_enabled`, `grading_enabled`, `ssr_reflect_transparent`, `debug_view`, and every numeric tunable |

The list lives in one place, the `TOY_STARTUP_FIXED_FIELDS` X-macro at the top of
`pixel_render_pipeline.h`. `apply_live_config()` enforces the split: it restores any
startup-fixed field the caller tried to change and names it in a warning, rather than
accepting an edit that would silently do nothing.

Startup-fixed does not mean "needs an app restart". `PixelRenderPipeline::needs_rebuild()`
detects a change to one of these fields, and the Engine then rebuilds the pipeline in place
on the same device and swapchain (`Engine::rebuild_pipeline()`). It does this when a scene's
`settings.render` or an editor edit changes one: `apply_scene_settings_()` queues the rebuild,
and it runs at the top of the next tick, or after a short debounce for live editor edits.
`Engine::restart_renderer()` forces a rebuild. A rebuild only applies the fields whose
config-derived value changed, so code that sets `render_config()` at runtime keeps its
values.

## Frame graph

Recorded in this order inside `Renderer::begin_frame()`'s `pre_pass_fn`; the `record_fn`
is only the final 1:1 blit into the swapchain. `render()` delegates to three stages —
`record_scene_()`, `record_post_chain_()` and `record_overlay_()` — which map onto the
three groups below.

**Compute pre-pass** (top of the command buffer, before `record_scene_()`):

0. GPU skinning (`passes/skinning_pass.h`, GPU scope `skinning`), when `skinning: gpu` (the
   default) and the device has compute. Each `SkinnedMeshRenderer` keeps a static bind-pose
   SSBO (position, normal, tangent, UV, 4 joints, 4 weights) and a per-slot palette ring of
   `inverse(owner_world) * bone_world * inverse_bind`; `Engine::upload_dynamic_meshes_()`
   queues one `skin.comp` dispatch per mesh, and the pass uploads the palettes (after the slot's
   fence wait) and records every dispatch behind one `compute_to_draw_barrier()`. The output is
   the mesh's own per-slot dynamic vertex buffer, so shadows, the G-buffer, the local-shadow
   atlas and surface vertex hooks draw it unchanged. Culling bounds come from the palette: each
   bone's bind-pose box of its influenced vertices, transformed and unioned. Motion vectors stay
   per object (a skinned mesh's surface motion counts as scene motion, but limbs get no
   per-vertex velocity). `skinning: cpu` (or no compute) keeps the CPU loop + `update_vertices()`.

**Scene** (`record_scene_()`):

1. Directional shadow cascades, then the local-light shadow atlas: every `cast_shadows` point
   and spot light competing for a slot by screen importance (point lights as six guard-banded
   tiles), with static casters cached per light (`record_local_shadows_()`).
2. G-buffer geometry, meshes and opaque/masked SDFs.
3. Hi-Z pyramid, when SSR, transparency or SSAO is on. This also
   performs the G-buffer depth transition `pixel_stylize.frag`'s outline sampler needs; when
   it does not run, `transition_gbuffer_depth_to_shader_read_()` does it instead. Exactly one
   of the two must happen.
4. There is no separate transparent capture: reflections see transparent geometry, fog and
   other reflections through the previous frame's final colour (step 9).
5. `TemporalHistoryPass` — the shared per-pixel accumulation count every temporally averaged
   screen-space effect reads (Unity HDRP's `_HistoryValidityBuffer`). One depth-based
   disocclusion answer per frame, reprojected clip-to-clip through a double-composed matrix,
   published as a count that rises to the deepest cap any consumer asks for. What it buys the
   consumers is a *converging* running average (frame N at `1/N`) in place of a fixed-rate
   exponential blend, which cannot converge at all on a trace that re-jitters every frame.
   Reprojection goes through the G-buffer velocity (G4), so a moving object keeps its history.
6. Contact shadows: the screen-space march into its own buffer, then its temporal resolve
   (`contact_shadow_pass.h`). Runs before lighting, which samples the result.
7. SSAO, or `invalidate_history()` when it is off.
7b. Physical sky, only while `sky_model: physical` (`record_physical_sky_()`, GPU scopes `sky`
   and `clouds`): the transmittance (256×64) and multiple-scattering (32×32) tables when the
   atmosphere's media changed, the sky-view table (192×108) every frame from the sun and moon
   (Hillaire 2020, `passes/sky_atmosphere_pass.h`), and with `clouds` the low-resolution
   cloud-layer raymarch (`passes/sky_cloud_pass.h`: ~1/5 res on low, 1/4 on medium and high,
   1/2 on ultra, the fraction carried to the upsample in `LightUBO::sky_params.y`; its 2D shape
   map and 64³ tiling 3D noise atlas are baked once at startup). Every target is built and bound at
   construction, so both toggles are live; with the gradient sky nothing records. The engine's
   CPU twin of the atmosphere (`toyengine/weather/atmosphere_model.h`) writes `sky_zenith` /
   `sky_horizon` / `sky_ground` and tints the directional light (`SkyFrameState::light_tint`,
   applied wherever the renderer reads the light's colour), so every gradient consumer —
   ambient, SSR fallback, fog, forward-shaded water and glass, particles — matches the drawn sky.
8. Deferred lighting (`pixel_lighting.frag`, which also draws the sky at background pixels:
   the gradient, or with the physical sky `sky_physical.glsl`'s sky-view lookup, sun and moon
   discs, stars and the upsampled clouds) → `offscreen_target_` (HDR; the sky-based indirect term can exceed 1.0
   whatever the toggles say). Always drawn — `debug_view`'s channel views replace step 16's
   final draw instead, reading the G-buffer/lighting/SSAO/SSR sources directly rather than
   swapping out this step.
9. Scene-colour mip chain, then SSR — Hi-Z raymarch → temporal resolve → specular swap plus
   the SSGI diffuse bounce. As in Unreal, the chain's mip 0 is the PREVIOUS frame's final
   pre-DOF HDR image (copied at the end of the post chain, `GpuScope::SceneColorHistory`), and
   each hit samples it at the hit's reprojected position (`hit_uv - G4.xy`); only frame 0 draws
   mip 0 from this frame's lit opaque image. Rays are GGX visible-normal samples
   (`ssr_jitter` scales the lobe, `ssr_rays_per_pixel` per tier), the trace writes a hit
   distance alongside the colour, the resolve reprojects mirror-like surfaces by the reflected
   image's virtual point, and the denoise is roughness-aware (mirrors stay sharp). Under `ssgi_traced` the bounce is its own cosine-hemisphere Hi-Z
   trace (`ssgi.frag`) through a second resolve+denoise chain inside `SsrPass`, rather than the
   single normal-offset mip tap the composite falls back to. It runs at the SSR trace
   resolution divided by `ssgi_resolution_scale` (2 at Low/Medium `ssgi_quality`).
10. Global fog over the opaque scene and sky (`record_fog_()`): gfxcoopa's `FogPass` blends
   premultiplied fog in place, drawn inside `TransparentPass`'s render pass (it loads the HDR
   image). Exponential height fog (`gfx/fog.glsl`), its parameters in the light UBO's fog block.
   Translucency is fogged by the forward shaders themselves at their own distance (step 12), the
   Unreal/HDRP split, so water, glass and particles are never fogged at the distance of whatever
   lies behind them. With the camera under water only the part of each ray above the surface is
   fogged; the in-water part belongs to the underwater pass.
11. Refraction's own scene-colour chain, when refraction is on and a BLEND mesh is in view
   (`refraction_this_frame_`; nothing else samples it, so a frame without one skips the build).
12. Forward transparent pass: BLEND meshes, BLEND SDFs and particle batches merged into one
   back-to-front list, drawn in place into the SSR composite. BLEND *meshes* additionally
   refract; BLEND SDFs never do. Each particle batch is one instanced draw through
   `ParticlePass` (see `passes/particle_pass.h` and `toyengine/particles/`). Mesh-mode particles
   are not here: they join step 2's G-buffer batches and the shadow views as instanced meshes.

**Post** (`record_post_chain_()`):

13. The underwater look (`UnderwaterPass` → `underwater_target_`, a copy unless the camera is
    below a water surface), then volumetrics (raymarched local `VolumeComponent`s) →
    `volumetrics_target_`. The march
    shadows its sun in-scatter against the directional map (light shafts) and scatters the
    nearest point/spot lights into each volume.
14. Depth of field, at render resolution. `debug_view: dof` swaps its composite to the signed
    CoC field instead of the blurred image; `debug_view: volumetrics` does the same to step 13's
    volumetrics march (accumulated density, scene colour suppressed).
15. Bloom pyramid, and auto-exposure metering (its own 1×1 ping-pong target; the value it
    produces is consumed by the *next* frame's tonemap).
16. Exposure + ACES tonemap + colour-grading LUT + outline + dither + palette → `post_target_` —
    or, when `debug_view` names a channel, that raw intermediate buffer instead, with every
    other step in this line reduced to its no-op value. `debug_view: lines` draws the normal
    image here and adds physics collider/contact wireframes as a guest in the same bracket.
17. World-space UI → `ui_world_target_`, at the display rect.
18. FXAA / SMAA / TAA → `aa_target_`, when `aa_mode != "off"`.
19. Tilt shift, at display resolution.

**Overlay** (`record_overlay_()`):

20. `ui_composite_pass_` draws the post-processed scene into the letterbox sub-rect of
    `overlay_target_` with the world-UI layer over it — performing the nearest upscale
    itself when tilt shift is off — then screen-space UI draws as a guest at full window
    resolution.
21. `record_fn`: `upscale_pass_` blits `overlay_target_` 1:1 into the swapchain.

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
  startup-fixed even though the *pass* is rebuilt on resize. Note that depth is at
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
