# Engine Submodule (`coopa::gfx::engine`)

The `coopa::gfx::engine` module provides high-level rendering engine abstractions built on top of `gfxcoopa` lower-level Vulkan modules. It is structured into 7 specialized submodules plus `render_features.h`:

- **`coopa::gfx::engine::passes`** ([`passes`](passes)): Render pass execution and graphics pipelines (`FullscreenStage` — the shared scaffold most post-processing passes are built from — plus `DeferredLightingPass`, `GBufferPipeline`, `ShadowPipeline`, `TransparentPass`, `SsaoPass`, `SsrPass`, `HiZPass`, `SceneColorMipPass`, `TemporalHistoryPass`, `TaaPass`, `SmaaPass`, `FxaaPass`, `ExposurePass`, `PresentPass`, `FogPass`, `VolumetricsPass`, `FroxelVolumetricsPass`, `BloomPass`, `DofPass`, `TiltShiftPass`, `StylizePass`, `EnvPrefilterPass`, `ProbeCapturePass`, `TexturedQuad2DPass`, the `Sdf*` passes).
- **`coopa::gfx::engine::targets`** ([`targets`](targets)): Framebuffer and render target resource managers (`OffscreenTarget`, `GBufferTarget`, `ShadowMapTarget`, `CubemapTarget`).
- **`coopa::gfx::engine::gi`** ([`gi`](gi)): Global Illumination system, CPU probe baking, and SH data structures (`GiSystem`, `GiBaker`, `GiData`, `BRDFLUT`).
- **`coopa::gfx::engine::data`** ([`data`](data)): Mesh, texture and LUT data plus per-frame GPU buffers (`Mesh`, `Vertex`, `InstanceData`, `Texture`, `GradingLut`, `PaletteLut`, `CameraUBO`, `LightData`, `VolumetricsData`, `SdfData`, `SkinnedMeshSource`).
- **`coopa::gfx::engine::components`** ([`components`](components)): Scene components and `register_render_components()`, which adds their YAML parsers to libcoopa's `SceneLoader`.
- **`coopa::gfx::engine::loaders`** ([`loaders`](loaders)): `AssetManager` loaders (`MeshLoader`, `TextureLoader`, `SkinnedMeshSourceLoader`).
- **`coopa::gfx::engine::util`** ([`util`](util)): Rendering utility wrappers (`Sampler`, `MaterialTextureCache`, `InstanceBatcher`, `SmaaTextures`, `sh_math`).

### Shaders

A pass class owns its descriptor layouts, push-constant structs and pipelines, but not its GLSL: its constructor takes the `.spv` paths (`vert_spv`, `frag_spv`, …) from the caller, and the push-constant structs document the block the caller's shader must declare (toyengine's `assets/shaders/` holds one full set). gfxcoopa's own `assets/shaders/` ships only what its code loads itself — `brdf_lut`, `env_prefilter`, `probe_capture`, `probe_sky_background` and `pbr.vert` (for `BRDFLUT`, `GiSystem` and `ProbeCapturePass`), and `smaa_*` with `SMAA.hlsl` (for `SmaaPass`) — plus the shared `gfx/brdf.glsl`, `gfx/ibl.glsl`, `gfx/sky.glsl`, `gfx/spot_light.glsl` and `gfx/surface2d/quad_vs.glsl` headers that consumers include through `-I`.

---

## Render Target & Shading Flow Graph

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                          ENGINE RENDER TARGET & SHADING FLOW                             │
└──────────────────────────────────────────────────────────────────────────────────────────┘

  CameraUBO / LightUBO / ModelPushConstants / GiSystemData
                            │
                            ▼
                ┌───────────────────────┐
                │    Mesh / Geometry    │
                └───────────┬───────────┘
                            │
       ┌────────────────────┴────────────────────┐
       ▼                                         ▼
┌───────────────────────┐             ┌───────────────────────┐
│   ShadowMapTarget     │             │    OffscreenTarget    │
├───────────────────────┤             ├───────────────────────┤
│ Directional & Point   │             │ Color Attachment      │
│ Depth Shadow Passes   │             │ Depth Attachment      │
└───────────┬───────────┘             └───────────┬───────────┘
            │                                     │ transitions color to
            │ shadow depth map                    │ SHADER_READ_ONLY_OPTIMAL
            └───────────────────┬─────────────────┘
                                │
                                ▼
                    ┌───────────────────────┐
                    │   FullscreenStage     │  (Sampler::nearest / linear)
                    ├───────────────────────┤
                    │ Post-processing /     │
                    │ Upscale Composite Pass│
                    └───────────────────────┘
```

---

## File Breakdown

### Data Submodule (`engine/data`)

#### [camera_ubo.h](data/camera_ubo.h)
- **Role**: Host-visible uniform buffer storing camera matrices (`view`, `proj`, `view_pos`) and last frame's reprojection data.
- **Key Classes / Structs**: `CameraData`, `CameraUBO`.
- **Details**: `update()` optionally pixel-snaps the camera position for low-resolution rendering. The trailing `prev_view`/`prev_proj`/`jitter_ndc` block feeds G-buffer velocity and TAA.

#### [light_data.h](data/light_data.h)
- **Role**: The light UBO layout and its host-visible buffer.
- **Key Classes / Structs**: `PointLightGPU`, `SpotLightGPU`, `LocalShadowGPU`, `LocalShadowBlock`, `LightUBO`, `LightData`.
- **Details**: Uploads the directional light with up to 4 shadow cascades, point and spot lights, and the local-light shadow atlas records shared by shadowed point and spot lights.

#### [mesh.h](data/mesh.h)
- **Role**: GPU-resident mesh data loading and vertex/index buffer management.
- **Key Classes / Structs**: `Vertex`, `InstanceData`, `MeshPart`, `MeshLod`, `MeshCpuData`, `Mesh`.
- **Details**: `build_cpu()` parses the Blender-exported mesh YAML and welds, cache-optimises and builds LODs with meshoptimizer; `from_node()` uploads the result. `from_arrays()` builds a mesh from memory, and with more than one vertex buffer `update_vertices()` can rewrite it every frame (cloth, CPU skinning).

#### [model_ubo.h](data/model_ubo.h)
- **Role**: Push constant data structure for per-object matrix transformations.
- **Key Classes / Structs**: `ModelPushConstants`.
- **Details**: 128-byte layout containing model matrix (`mat4`) and normal matrix (`mat4 normal_matrix = transpose(inverse(model))`).

#### [texture.h](data/texture.h)
- **Role**: GPU-resident 2D texture decoded from an image file.
- **Key Classes / Structs**: `Texture`.
- **Details**: An `Image` plus its own `Sampler`; produced by `TextureLoader`.

#### [grading_lut.h](data/grading_lut.h) / [palette_lut.h](data/palette_lut.h)
- **Role**: Colour-grading strip LUT (linear-sampled) and palette strip (nearest-sampled) loaded from PNGs.
- **Key Classes / Structs**: `GradingLut`, `PaletteLut`.

#### [volumetrics_data.h](data/volumetrics_data.h)
- **Role**: Per-frame uniform buffer for scene-placed local volumes.
- **Key Classes / Structs**: `ScatterLightGPU`, `VolumeGPU`, `VolumetricsUBO`, `VolumetricsData`.
- **Details**: Consumed by `VolumetricsPass` and `FroxelVolumetricsPass`.

#### [sdf_data.h](data/sdf_data.h)
- **Role**: Per-frame-in-flight storage buffers describing every `SdfRenderer`/`SdfShape`.
- **Key Classes / Structs**: `SdfShapeGPU`, `SdfRendererGPU`, `SdfGlobals`, `SdfData`.

#### [skinned_mesh_source.h](data/skinned_mesh_source.h)
- **Role**: CPU-only bind-pose mesh plus joints, weights and inverse bind matrices, for CPU skinning into a dynamic `Mesh`.
- **Key Classes / Structs**: `SkinnedMeshSource`.

### Global Illumination Submodule (`engine/gi`)

#### [brdf_lut.h](gi/brdf_lut.h)
- **Role**: BRDF Integration Look-Up Table generator.
- **Key Classes / Structs**: `BRDFLUT`.
- **Details**: Renders a 512x512 R16G16_SFLOAT texture of the split-sum scale and bias terms with gfxcoopa's `brdf_lut.vert`/`.frag`.

#### [gi_baker.h](gi/gi_baker.h)
- **Role**: CPU probe baker for Spherical Harmonics indirect illumination.
- **Key Classes / Structs**: `GiBaker`, `SceneBox`.
- **Details**: `bake_cpu()` traces 128 rays per probe against each renderable's world-space box, in parallel across probes, projecting radiance into 9 2nd-order SH coefficients.

#### [gi_data.h](gi/gi_data.h)
- **Role**: Spherical Harmonics probe grid data structures and GPU buffers.
- **Key Classes / Structs**: `SHProbe`, `GiUniforms`, `ReflectionProbeUniforms`, `GiData`.
- **Details**: The probe SSBO, the GI uniform buffer, and per-reflection-probe bounds and parameters.

#### [gi_system.h](gi/gi_system.h)
- **Role**: Global Illumination subsystem orchestrator.
- **Key Classes / Structs**: `GiSystem`.
- **Details**: `bake()` runs once before the first frame: it fills the probe grid via `GiBaker`, captures each reflection probe with `ProbeCapturePass` and prefilters it with `EnvPrefilterPass`. Afterwards it exposes one descriptor set (probes, GI uniforms, BRDF LUT, reflection cubemaps). Loads its shaders from gfxcoopa's `assets/shaders/` through the `ShaderLibrary` it is given.

### Render Targets Submodule (`engine/targets`)

#### [cubemap_target.h](targets/cubemap_target.h)
- **Role**: Offscreen multi-face HDR color cubemap render target utility.
- **Key Classes / Structs**: `CubemapTarget`.
- **Details**: A 6-face color cubemap (256² per face and one mip by default) with per-face views and a depth-inclusive render pass, for reflection probe capture and prefiltering.

#### [gbuffer_target.h](targets/gbuffer_target.h)
- **Role**: Offscreen multi-attachment G-Buffer render target manager.
- **Key Classes / Structs**: `GBufferTarget`.
- **Details**: Manages deferred geometry pass attachments: G0 Albedo+AO, G1 Normal+Metallic, G2 Position+Roughness, G3 Emissive, G4 Velocity (screen motion plus last/current linear depth), and Depth.

#### [offscreen_target.h](targets/offscreen_target.h)
- **Role**: Custom-resolution offscreen render target.
- **Key Classes / Structs**: `OffscreenTarget`, `ColorOnlyTag`.
- **Details**: Owns a color attachment (`RGBA8_Unorm` by default; any `Format`, optionally MSAA) and a depth attachment, transitioning both to `SHADER_READ_ONLY_OPTIMAL` after the pass. Pass `kColorOnly` for a target with no depth attachment.

#### [shadow_map_target.h](targets/shadow_map_target.h)
- **Role**: Depth attachment targets for directional, point and spot light shadows.
- **Key Classes / Structs**: `ShadowMapTarget`.
- **Details**: The directional map is an atlas of up to 4 cascade tiles (2048² each by default), plus a 512² point light cubemap and a 1024² spot light depth map.

### Render Passes & Pipelines Submodule (`engine/passes`)

Every pass below takes its shaders from the caller unless noted.

#### [deferred_lighting_pass.h](passes/deferred_lighting_pass.h)
- **Role**: Deferred lighting evaluation pass.
- **Key Classes / Structs**: `DeferredLightingPass`.
- **Details**: Shades the G-buffer into an HDR target in one fullscreen draw. Owns the G-buffer/SSAO set; the caller binds the camera, light and shadow sets and any `ExtraSets`.

#### [env_prefilter_pass.h](passes/env_prefilter_pass.h)
- **Role**: GGX-prefiltered environment map generator.
- **Key Classes / Structs**: `EnvPrefilterPass`.
- **Details**: Writes the roughness mip chain of a reflection probe cubemap from its mip 0. Used by `GiSystem` with `env_prefilter.vert`/`.frag`.

#### [gbuffer_pipeline.h](passes/gbuffer_pipeline.h)
- **Role**: Graphics pipelines for geometry rasterization into G-Buffer attachments.
- **Key Classes / Structs**: `GBufferPipeline`.
- **Details**: The stock back-face-culled pipeline, a no-cull sibling, and one variant per surface shader registered with `add_variant()`; `bind()` selects by material shader name.

#### [hiz_pass.h](passes/hiz_pass.h)
- **Role**: Hierarchical Z-Buffer depth pyramid generator.
- **Key Classes / Structs**: `HiZPass`.
- **Details**: Downsamples scene depth into a mip pyramid; the caller's fragment shader picks the reduction (a min() pyramid for SSR, a depth-aware average for SSAO).

#### [present_pass.h](passes/present_pass.h)
- **Role**: Final blit pass to swapchain framebuffers.
- **Key Classes / Structs**: `PresentPass`.
- **Details**: Draws a fullscreen triangle sampling an already display-ready image into the swapchain.

#### [probe_capture_pass.h](passes/probe_capture_pass.h)
- **Role**: Scene capture pass into reflection probe cubemaps.
- **Key Classes / Structs**: `ProbeCapturePass`.
- **Details**: Per face, fills the analytic sky, then draws scene geometry on top. Loads gfxcoopa's `env_prefilter.vert`, `probe_sky_background.frag`, `pbr.vert` and `probe_capture.frag`.

#### [scene_color_mip_pass.h](passes/scene_color_mip_pass.h)
- **Role**: Downsampled color mip chain generator.
- **Key Classes / Structs**: `SceneColorMipPass`.
- **Details**: Builds a prefiltered HDR scene colour pyramid for SSR's glossy cone tracing.

#### [shadow_pipeline.h](passes/shadow_pipeline.h)
- **Role**: Depth-only shadow map pipelines.
- **Key Classes / Structs**: `ShadowPipeline`, `DirectionalShadowPushConstants`, `CubeShadowPushConstants`.
- **Details**: Directional and cube shadow pipelines, plus per-surface-shader variants (`add_variant()`). Given a material layout, the pipelines also bind the material set so alpha-masked casters can test their mask.

#### [smaa_pass.h](passes/smaa_pass.h)
- **Role**: Subpixel Morphological Anti-Aliasing (SMAA 1x) pass.
- **Key Classes / Structs**: `SmaaPass`.
- **Details**: Executes 3-pass SMAA post-processing (edge detection, blend weights, neighborhood blending) with gfxcoopa's own `smaa_*` shaders.

#### [ssr_pass.h](passes/ssr_pass.h)
- **Role**: Screen-Space Reflections (SSR) pass.
- **Key Classes / Structs**: `SsrPass`.
- **Details**: Hi-Z raymarch (optionally at half resolution), temporal resolve, optional bilateral blur, and a BRDF composite back into the HDR frame; an optional traced-SSGI stage adds a diffuse bounce.

#### [ssao_pass.h](passes/ssao_pass.h)
- **Role**: Screen-Space Ambient Occlusion.
- **Key Classes / Structs**: `SsaoPass`.
- **Details**: Horizon-based (GTAO) estimation over a prefiltered depth pyramid, temporal resolve, and a depth/normal-aware bilateral blur, optionally at half resolution.

#### [temporal_history_pass.h](passes/temporal_history_pass.h)
- **Role**: Shared per-pixel history-validity and sample-count buffer.
- **Key Classes / Structs**: `TemporalHistoryPass`.
- **Details**: Runs the disocclusion test once per frame so every temporally accumulated effect (SSR, SSGI, contact shadows) shares one answer.

#### [taa_pass.h](passes/taa_pass.h)
- **Role**: Temporal Anti-Aliasing (TAA) pass.
- **Key Classes / Structs**: `TaaPass`.
- **Details**: Reprojects a ping-ponged RGBA16F accumulation history (through G-buffer velocity when given, else camera-only) and blends it with the current frame.

#### [fxaa_pass.h](passes/fxaa_pass.h)
- **Role**: Standalone LDR FXAA 3.11 pass.
- **Key Classes / Structs**: `FxaaPass`.
- **Details**: Expects an already-tonemapped source; does no exposure or ACES work.

#### [exposure_pass.h](passes/exposure_pass.h)
- **Role**: Auto-exposure (eye adaptation).
- **Key Classes / Structs**: `ExposurePass`.
- **Details**: Meters an HDR image into a 1x1 R16F exposure target that a tonemap multiplies by.

#### [fog_pass.h](passes/fog_pass.h)
- **Role**: Global exponential height fog over the OPAQUE scene and sky, composited in place before translucency (forward shaders fog themselves).
- **Key Classes / Structs**: `FogPass`.
- **Details**: Reads the G-buffer's normal/world-position and the caller's light set (`LightUBO`'s fog block); outputs premultiplied (in-scatter, 1 - T) through a premultiplied blend, inside a caller-supplied raw render pass that loads the HDR image.

#### [fullscreen_stage.h](passes/fullscreen_stage.h)
- **Role**: The shared scaffold for a fullscreen-triangle render stage — its two shaders, its own descriptor set(s), and its pipeline.
- **Key Classes / Structs**: `FullscreenStageDesc`, `FullscreenStage`.
- **Details**: A pass *owns* one or more stages rather than deriving from anything, so its public constructor and `draw()` stay unchanged. `owned_sets` declares the sets the stage allocates, `leading_layouts`/`extra_layouts` the ones owned elsewhere, and `instances` the number of independent copies (one per pyramid level or blur axis). `rebuild_sets()` reallocates the sets without rebuilding the pipeline. Render targets, barriers and stage ordering deliberately stay in the pass.

#### [extra_sets.h](passes/extra_sets.h)
- **Role**: Optional app-supplied descriptor sets appended after a pass's own.
- **Key Classes / Structs**: `ExtraSets`.
- **Details**: Carries the layouts and the binder together so the two cannot drift apart; `validate()` turns a mismatch into a startup exception. Empty extras mean the set is not declared in the pipeline layout at all.

#### Post-processing passes
- [bloom_pass.h](passes/bloom_pass.h) (`BloomPass`) — bright-pass threshold, multi-tap downsample, tent-filter upsample+combine.
- [dof_pass.h](passes/dof_pass.h) (`DofPass`) — thin-lens circle-of-confusion depth of field with a half-resolution spiral bokeh gather.
- [tilt_shift_pass.h](passes/tilt_shift_pass.h) (`TiltShiftPass`) — separable horizontal/vertical band blur, folded with the upscale.
- [volumetrics_pass.h](passes/volumetrics_pass.h) (`VolumetricsPass`) — raymarched local volumes: reduced-resolution march + depth-aware full-resolution composite.
- [froxel_volumetrics_pass.h](passes/froxel_volumetrics_pass.h) (`FroxelVolumetricsPass`) — the froxel-grid alternative for local volumes: inject, integrate along each column, apply.
- [stylize_pass.h](passes/stylize_pass.h) (`StylizePass`) — optional tonemap plus outline, palette quantization, dither and bloom composite.

#### Geometry and transparency passes
- [transparent_pass.h](passes/transparent_pass.h) (`TransparentPass`) — forward BLEND pass, depth-tested against the G-buffer, with per-surface-shader variants.
- [textured_quad_2d_pass.h](passes/textured_quad_2d_pass.h) (`TexturedQuad2DPass`) — shared 2D textured-quad pass with a per-texture descriptor cache and streaming geometry; pairs with gfxcoopa's `gfx/surface2d/quad_vs.glsl`.
- [sdf_gbuffer_pass.h](passes/sdf_gbuffer_pass.h), [sdf_forward_pass.h](passes/sdf_forward_pass.h), [sdf_shadow_pass.h](passes/sdf_shadow_pass.h) — signed-distance-field shapes rendered into the G-buffer, the forward pass, and both shadow paths.

### Components Submodule (`engine/components`)

- [register.h](components/register.h) — `register_render_components()` adds YAML parsers for `MeshRenderer`, `Camera`, `DirectionalLight`, `PointLight`, `SpotLight`, `EnvironmentLight`, `ReflectionProbe`, `GiProbeVolume`, `Volume`, `SdfRenderer` and `SdfShape` to libcoopa's `SceneLoader`. Meshes and textures load through the caller's `AssetManager`, which must already have the loaders registered.
- [mesh_renderer.h](components/mesh_renderer.h) — `MeshRenderer` and its `PBRMaterial` (`AlphaMode` Opaque/Mask/Blend, texture maps, an optional named surface shader and its parameters).
- [camera_component.h](components/camera_component.h), [directional_light.h](components/directional_light.h), [point_light.h](components/point_light.h), [spot_light.h](components/spot_light.h), [environment_light.h](components/environment_light.h), [reflection_probe.h](components/reflection_probe.h), [gi_probe_volume.h](components/gi_probe_volume.h), [volume.h](components/volume.h), [sdf_renderer.h](components/sdf_renderer.h), [sdf_shape.h](components/sdf_shape.h) — the remaining scene components.
- [renderable_ref.h](components/renderable_ref.h) — `RenderableRef` and `gather_renderables()`, every `MeshRenderer` in a scene with its transform.

### Loaders Submodule (`engine/loaders`)

- [mesh_loader.h](loaders/mesh_loader.h) (`MeshLoader`) — parses mesh YAML (plus an optional `<mesh>.lod.yaml` sidecar) off-thread and uploads on the main thread.
- [texture_loader.h](loaders/texture_loader.h) (`TextureLoader`, `DecodedImage`) — decodes PNG/JPG with stb_image off-thread and uploads with an explicitly declared colour space.
- [skinned_mesh_source_loader.h](loaders/skinned_mesh_source_loader.h) (`SkinnedMeshSourceLoader`) — CPU-only, no GPU upload.

### Engine Utilities Submodule (`engine/util`)

#### [sampler.h](util/sampler.h)
- **Role**: RAII `VkSampler` wrapper and factory methods.
- **Key Classes / Structs**: `Sampler`.
- **Details**: Built from a `SamplerDesc`, or through the static factories `nearest()`, `linear()` and `shadow()` (depth comparison).

#### [material_texture_cache.h](util/material_texture_cache.h)
- **Role**: The shared "material" descriptor set (alpha mask, albedo, normal, metallic-roughness).
- **Key Classes / Structs**: `MaterialTextureCache`.
- **Details**: `set_for(material)` lazily allocates one set per distinct texture combination, binding neutral fallbacks for untextured slots, so G-buffer, shadow, transparent and probe-capture passes share sets.

#### [instance_batcher.h](util/instance_batcher.h)
- **Role**: Groups per-frame draw items into instanced batches.
- **Key Classes / Structs**: `InstanceBatcher`.
- **Details**: `begin()`/`add()`/`upload()`/`bind()` stream per-instance transforms (`InstanceData`) to a growing vertex buffer.

#### [sh_math.h](util/sh_math.h)
- **Role**: 2nd-order Spherical Harmonics math functions.
- **Key Classes / Structs**: `sh_basis()`, `sh_project_sample()`, `sh_evaluate_irradiance()`.
- **Details**: Computes 9 L2 SH basis functions, projects samples, and evaluates cosine-lobe irradiance.

#### [smaa_textures.h](util/smaa_textures.h)
- **Role**: SMAA area/search texture loader.
- **Key Classes / Structs**: `SmaaTextures`.
- **Details**: Uploads the Jimenez reference `AreaTex.h`/`SearchTex.h` arrays, found through the `SMAA_TEXTURES_DIR` CMake variable rather than vendored.

### [render_features.h](render_features.h)
- **Key Classes / Structs**: `IndirectParams`.
- **Details**: The indirect-lighting terms (ambient, sky and SSGI intensities, sky gradient colours) a lighting pass adds and the SSR composite subtracts, held in one struct so both are fed identical values.

---

## Usage Example

```cpp
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/targets/shadow_map_target.h>
#include <gfxcoopa/engine/util/sampler.h>

// Create an offscreen render target for retro low-resolution rendering
coopa::gfx::engine::targets::OffscreenTarget offscreen(device, allocator, 320, 240);

// Create shadow map targets: 2048² directional, 512² point cubes, 1024² spot maps
coopa::gfx::engine::targets::ShadowMapTarget shadow_target(device, allocator, 2048, 512, 1024);

// Create a nearest-neighbor sampler for pixel-art upscaling
coopa::gfx::engine::util::Sampler nearest_sampler = coopa::gfx::engine::util::Sampler::nearest(device);

// Record offscreen render pass
offscreen.begin(cmd);
// ... record geometry draw calls ...
offscreen.end(cmd); // Color attachment automatically transitions to SHADER_READ_ONLY_OPTIMAL
```
