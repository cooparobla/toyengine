# GfxCoopa Module (`coopa::gfx`)

`gfxcoopa` is a modern, header-only C++20 Vulkan rendering library designed to bring clean RAII ergonomics, low-overhead resource management, and state-of-the-art graphics feature sets to Vulkan applications. Built around dynamic API function loading via Volk and GPU memory management via Vulkan Memory Allocator (VMA), `gfxcoopa` hides low-level Vulkan boilerplate without sacrificing performance or direct hardware control.

The `engine` module provides the building blocks of a deferred Physically-Based Rendering (PBR) renderer: a G-buffer, Cook-Torrance deferred lighting, 2nd-order (9-coefficient) Spherical Harmonics (SH) probe volumes and reflection probes, cascaded directional / point / spot shadow maps, raymarched SDF shapes, Hi-Z Screen-Space Reflections (SSR) with an optional SSGI bounce, GTAO-style SSAO, local and froxel volumetrics, fog, bloom, auto exposure, depth of field, tilt-shift, and anti-aliasing (Jimenez SMAA 1x, TAA, FXAA 3.11). The application assembles these passes into its own frame.

**Shaders belong to the caller.** A pass class owns its pipelines, targets and descriptor sets and defines the push-constant layout its shader must match, but takes the compiled `.spv` paths as constructor arguments. gfxcoopa's own `assets/shaders/` holds only the shaders its own code loads (the GI bake: `brdf_lut`, `env_prefilter`, `probe_capture`, `probe_sky_background`, `pbr.vert`; `SmaaPass`: `smaa_*`), the `test.vert`/`test.frag` pair, and the shared `gfx/` headers (`brdf`, `ibl`, `sky`, `spot_light`, `surface2d/quad_vs`) that every consumer's shaders reach through `-I`. Shader names mentioned in pass docs (`ssr.frag`, `fog.frag`, ...) are the caller's shaders; toyengine ships one set.

---

## GfxCoopa Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                                GFXCOOPA LAYER ARCHITECTURE                               │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                                ┌───────────────────────┐
                                │   Application Code    │
                                └───────────┬───────────┘
                                            │
                                            ▼
                                ┌───────────────────────┐
                                │          app          │  (Context: bring-up + frame loop)
                                └───────────┬───────────┘
                                            │
                                            ▼
                                ┌───────────────────────┐
                                │     presentation      │  (Window, Renderer)
                                └───────────┬───────────┘
                                            │
                    ┌───────────────────────┼───────────────────────┐
                    ▼                       ▼                       ▼
        ┌───────────────────────┐ ┌───────────────────┐ ┌───────────────────────┐
        │        engine         │ │     pipeline      │ │        command        │
        │(Passes, Targets, Data)│ │(Shaders, Pipeline)│ │(CmdPool, CmdBuffer)   │
        └───────────┬───────────┘ └─────────┬─────────┘ └───────────┬───────────┘
                    │                       │                       │
                    └───────────────────────┼───────────────────────┘
                                            │
                    ┌───────────────────────┴───────────────────────┐
                    ▼                                               ▼
        ┌───────────────────────┐                       ┌───────────────────────┐
        │        memory         │                       │         core          │
        │(Allocator, VMA, Image)│                       │(Instance, Device, Swap)│
        └───────────┬───────────┘                       └───────────┬───────────┘
                    │                                               │
                    └───────────────────────┬───────────────────────┘
                                            │
                                            ▼
                                ┌───────────────────────┐
                                │         util          │  (Volk, Error, Debug, Readback)
                                └───────────┬───────────┘
                                            │
                                            ▼
                                ┌───────────────────────┐
                                │      Vulkan API       │
                                └───────────────────────┘

   types/ (Format, SamplerDesc, VertexLayout, TextureView, ...) is Vulkan-free and usable from every layer.

┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                     TYPICAL DEFERRED FRAME (assembled by the application)                │
└──────────────────────────────────────────────────────────────────────────────────────────┘

  ShadowMapTarget        ShadowPipeline + SdfShadowPass   (directional cascades, point cubes, spot)
        │
  GBufferTarget          GBufferPipeline + SdfGBufferPass
        │                G0 albedo+AO · G1 normal+metallic · G2 position+roughness
        │                G3 emissive · G4 velocity · D32 depth
        │
  HiZPass / SsaoPass     depth pyramid, GTAO + temporal resolve + bilateral blur
        │
  DeferredLightingPass   Cook-Torrance direct light + SH / reflection-probe indirect ──► HDR target
        │
  TemporalHistoryPass    shared history-validity / sample-count buffer
  SceneColorMipPass      prefiltered HDR mip chain for glossy cone tracing
  SsrPass                Hi-Z trace, resolve, blur, (SSGI), composite
        │
  TransparentPass        forward BLEND meshes; SdfForwardPass for BLEND SDFs
  FogPass / VolumetricsPass / FroxelVolumetricsPass
        │
  ExposurePass · BloomPass · DofPass · TiltShiftPass
        │
  TaaPass · SmaaPass · FxaaPass · StylizePass
        │
  PresentPass            final LDR image ──► swapchain
```

---

## Submodule Architecture Map

| Submodule | Namespace | Description |
|---|---|---|
| `app` | `coopa::gfx::app` | `Context`: window-to-swapchain bring-up, resize handling, frame timing and the main loop. |
| [`command`](command/README.md) | `coopa::gfx::command` | Command buffers, command pools, and CPU/GPU sync primitives (Fences, Semaphores). |
| [`core`](core/README.md) | `coopa::gfx::core` | Vulkan instance initialization, GLFW surface integration, physical/logical device selection, and swapchain management. |
| `detail` | `coopa::gfx::detail` | Internal Vk/GLFW conversions for the sealed types. Not for consumers. |
| [`engine`](engine/README.md) | `coopa::gfx::engine` | Render passes, targets, GPU data blocks, scene components, asset loaders, GI, and engine utilities. |
| [`memory`](memory/README.md) | `coopa::gfx::memory` | Vulkan Memory Allocator (VMA) RAII integration, GPU buffers, images, and staged image upload. |
| [`pipeline`](pipeline/README.md) | `coopa::gfx::pipeline` | Shaders, shader search paths, surface-shader registry, render passes, descriptors, and graphics pipelines. |
| [`presentation`](presentation/README.md) | `coopa::gfx::presentation` | GLFW windowing wrapper and multi-buffered frame orchestration (`Renderer::draw_frame()`). |
| `types` | `coopa::gfx` | Vulkan-free vocabulary (`Format`, `SamplerDesc`, `VertexLayout`, `TextureView`, enums, clear values). |
| [`util`](util/README.md) | `coopa::gfx::util` | Debug messenger, error checks, format helpers, Volk initialization, and GPU image readback. |

---

## Descriptor Set Conventions

The lit passes (`DeferredLightingPass`, `TransparentPass`, `SdfForwardPass`) share a leading set order; the application builds and owns these sets and passes their layouts in:

| Set | Owner / Source | Contents |
|---|---|---|
| **Set 0** | `CameraUBO` | `CameraData`: view, projection (TAA-jittered), eye position, and the previous view/projection plus jitter for reprojection |
| **Set 1** | `LightData` | `LightUBO`: directional light and cascades, up to 16 point and 8 spot lights, sky colours, local-shadow records |
| **Set 2** | application | Shadow map samplers from `ShadowMapTarget` (layout defined by the caller's shaders) |
| **Set 3+** | `ExtraSets` | Optional caller-supplied sets, e.g. `GiSystem::layout()`: GI uniforms (0), SH probe SSBO (1), BRDF LUT (2), reflection cubemaps (3-6), reflection probe UBO (7) |
| **Last** | the pass | The pass's own set (e.g. the G-buffer + SSAO images for `DeferredLightingPass`) |

---

## Compute

Compute runs **in-line on the graphics queue**, recorded into the frame's command buffer outside any render pass and before the passes that consume it. There is no async compute queue. Gate every compute path on `Device::supports_compute()`. On MoltenVK the graphics family supports compute, storage buffers can be up to 4 GiB, a stage can bind 31 of them, and a work group can have up to 1024 invocations.

```cpp
auto layout = DescriptorLayoutBuilder().storage_buffer(0, ShaderStage::Compute).build(device);
ComputePipeline sim(device, library.resolve("particles_sim.comp"), {&layout},
                    {{ShaderStage::Compute, 0, sizeof(SimPush)}});
auto particles = memory::make_storage_buffer(device, allocator, bytes, BufferUsage::Vertex);
auto args      = memory::make_storage_buffer(device, allocator, 16, BufferUsage::Indirect);

cmd.fill_buffer(args, 0);                         // reset a counter / indirect args
cmd.transfer_to_compute_barrier();
cmd.bind_pipeline(sim);                           // sealed bind/push now target the compute bind point
cmd.bind_descriptor_set(set);
cmd.push_constants(ShaderStage::Compute, push);
cmd.dispatch(ComputePipeline::groups_for(count, 64));
cmd.compute_to_draw_barrier();                    // vertex attributes + index + indirect args
// ... render pass ...
cmd.draw_indirect(args);                          // VkDrawIndirectCommand written by the shader
```

- **Recording**: `bind_pipeline(const ComputePipeline&)`, `dispatch(x, y, z)`, `dispatch_indirect(buffer, offset)` (three uint32 group counts), `fill_buffer()`, and `draw_indirect()` / `draw_indexed_indirect()` for draws whose arguments compute wrote.
- **Barriers**: `buffer_barrier(BufferAccess from, BufferAccess to)` records one global memory barrier. Each `BufferAccess` bit implies its stage. The named shortcuts are `compute_to_draw_barrier()`, `compute_to_compute_barrier()` (which also covers indirect args for a following `dispatch_indirect()`), `compute_to_graphics_read_barrier()` (vertex/fragment storage reads) and `transfer_to_compute_barrier()`.
- **Compute-written vertices**: `Mesh::from_arrays(..., buffer_count, /*compute_writable=*/true)` adds Storage usage to a dynamic mesh's per-slot vertex buffers. A compute pass binds `vertex_buffer(slot)` as an SSBO (a `Vertex` is 12 floats: position, normal, uv, tangent, tightly packed), and `mark_gpu_written(slot, bounds_min, bounds_max)` replaces `update_vertices()`.
- **Shaders**: `cmake/GfxShaders.cmake` compiles `*.comp` next to `*.vert`/`*.frag`, flat per shader directory.

---

## File Breakdown

### App Submodule (`coopa::gfx::app`)

#### [context.h](app/context.h)
- **Role**: One-object Vulkan bring-up and main loop.
- **Key Classes / Structs**: `Context`, `ContextConfig`, `FrameCallbacks`.
- **Details**: Owns Window → Instance → Surface → Device → Allocator → Swapchain → CommandPool → RenderPass → Renderer in order. Installs the full resize sequence, reads `ONESHOT`/`MAX_FRAMES` via `ContextConfig::from_env()`, and exposes `poll()`, `frame()`, `run()` and `submit_once()`.

---

### Command Submodule (`coopa::gfx::command`)

#### [command_buffer.h](command/command_buffer.h)
- **Role**: Non-owning wrapper around `VkCommandBuffer` with named recording methods.
- **Key Classes / Structs**: `CommandBuffer`.
- **Details**: Render pass begin/end, pipeline and descriptor-set binding (graphics or compute), vertex/index buffers, viewport/scissor, typed push constants, draws and indirect draws, compute `dispatch()`/`dispatch_indirect()`, `fill_buffer()`, buffer barriers, copies, and `transition()` for `memory::Image` layout changes. See [Compute](#compute).

#### [command_pool.h](command/command_pool.h)
- **Role**: RAII manager for `VkCommandPool`.
- **Key Classes / Structs**: `CommandPool`.
- **Details**: `allocate()` for per-frame command buffers, `begin_single_use()`/`end_single_use()`, and `submit_once()`, which records a lambda into a one-shot buffer and waits for it.

#### [sync.h](command/sync.h)
- **Role**: RAII wrappers for Vulkan synchronization primitives.
- **Key Classes / Structs**: `Fence`, `Semaphore`.
- **Details**: `Fence` provides `wait()` and `reset()` for CPU-GPU synchronization; `Semaphore` handles GPU-GPU signaling for swapchain acquire and present.

---

### Core Submodule (`coopa::gfx::core`)

#### [device.h](core/device.h)
- **Role**: Physical GPU selection and logical `VkDevice` creation.
- **Key Classes / Structs**: `Device`, `QueueFamilyIndices`.
- **Details**: Scores physical devices (discrete GPUs preferred), finds graphics and present queue families, enables `VK_KHR_swapchain` (and `VK_KHR_portability_subset` on MoltenVK), and waits idle on teardown. `supports_compute()` / `compute_caps()` (`ComputeCaps`) report whether the graphics queue runs compute and the storage-buffer and work-group limits, logged once per process.

#### [instance.h](core/instance.h)
- **Role**: Vulkan instance lifecycle (`VkInstance`).
- **Key Classes / Structs**: `Instance`.
- **Details**: Initializes volk, enables the GLFW-required extensions plus `VK_KHR_portability_enumeration` when the loader offers it (MoltenVK), and, when validation is requested, enables `VK_LAYER_KHRONOS_validation` and `VK_EXT_debug_utils` with a `DebugMessenger`.

#### [surface.h](core/surface.h)
- **Role**: GLFW window surface (`VkSurfaceKHR`).
- **Key Classes / Structs**: `Surface`, `SwapchainSupportDetails`.
- **Details**: Creates the surface with `glfwCreateWindowSurface` and queries capabilities, formats and present modes.

#### [swapchain.h](core/swapchain.h)
- **Role**: `VkSwapchainKHR` creation, image views, and recreation.
- **Key Classes / Structs**: `Swapchain`.
- **Details**: Prefers `B8G8R8A8_SRGB` / `SRGB_NONLINEAR`; uses FIFO when `vsync` is true, otherwise prefers MAILBOX and falls back to FIFO. `recreate()` rebuilds on resize.

---

### Memory Submodule (`coopa::gfx::memory`)

#### [allocator.h](memory/allocator.h)
- **Role**: RAII wrapper around `VmaAllocator`.
- **Key Classes / Structs**: `Allocator`.
- **Details**: Initializes VMA from the instance and device handles.

#### [buffer.h](memory/buffer.h)
- **Role**: RAII GPU buffer (`VkBuffer` + `VmaAllocation`).
- **Key Classes / Structs**: `Buffer`.
- **Details**: Factories `vertex()`, `index()`, `uniform()`, `staging()` and `storage()`; `upload()` and `download()` copy through a mapping (or a temporary staging buffer for device-local memory).

#### [storage_buffer.h](memory/storage_buffer.h)
- **Role**: Storage buffers for compute.
- **Key Classes / Structs**: `make_storage_buffer()`, `StorageBufferRing`.
- **Details**: `make_storage_buffer()` is Storage + transfer usage plus optional Vertex/Index/Indirect usage, so compute output can be drawn from directly. `StorageBufferRing` keeps one buffer per frame in flight (`current(slot)`, `previous(slot)`, `upload(slot, ...)`).

#### [image.h](memory/image.h)
- **Role**: RAII 2D GPU image (`VkImage`, `VkImageView`, `VmaAllocation`).
- **Key Classes / Structs**: `Image`.
- **Details**: Tracks the `TextureUsage` it was last transitioned to (`current_usage()`); `view_typed()` returns a sealed `TextureView`. Layout transitions are recorded by `command::CommandBuffer::transition()`.

#### [image_upload.h](memory/image_upload.h)
- **Role**: Staged pixel upload.
- **Key Classes / Structs**: `upload_image_2d()`.
- **Details**: Creates a 2D `Image` and fills it from tightly packed host pixels through a staging buffer.

---

### Pipeline Submodule (`coopa::gfx::pipeline`)

#### [descriptor.h](pipeline/descriptor.h)
- **Role**: Descriptor sets, layouts, pools, and their builders.
- **Key Classes / Structs**: `DescriptorBinding`, `DescriptorSetLayout`, `DescriptorPool`, `DescriptorSet`, `DescriptorLayoutBuilder`, `DescriptorPoolBuilder`.
- **Details**: Builders declare uniform/storage buffers, combined samplers and storage images per binding; `DescriptorSet::bind_buffer()`, `bind_storage_buffer()` and `bind_image()` write descriptors in the style of `glBindBufferBase`.

#### [pipeline.h](pipeline/pipeline.h)
- **Role**: RAII graphics pipeline (`VkPipeline`, `VkPipelineLayout`).
- **Key Classes / Structs**: `Pipeline`, `PipelineDesc`, `RasterState`, `DepthState`, `BlendState`, `BlendMode`, `PushConstantRange`, `PipelineConfig`.
- **Details**: `PipelineDesc` is the sealed description (shaders, `VertexLayout`, raster/depth/blend state, descriptor layouts, push constants) with GL-like defaults; `PipelineConfig` is the Vk-typed rasterization config the raw-binding constructors take. Viewport and scissor are always dynamic.

#### [compute_pipeline.h](pipeline/compute_pipeline.h)
- **Role**: RAII compute pipeline (`VkPipeline`, `VkPipelineLayout`).
- **Key Classes / Structs**: `ComputePipeline`, `ComputePipelineDesc`.
- **Details**: Built from one compute `Shader` (or a `.spv` path), descriptor set layouts and `PushConstantRange`s. `groups_for(count, local_size)` is the ceiling divide for `dispatch()`.

#### [render_pass.h](pipeline/render_pass.h)
- **Role**: RAII wrapper for `VkRenderPass`.
- **Key Classes / Structs**: `RenderPass`.
- **Details**: One color attachment plus an optional depth attachment, with configurable final layouts and sample count.

#### [shader.h](pipeline/shader.h)
- **Role**: SPIR-V shader module loading (`VkShaderModule`).
- **Key Classes / Structs**: `Shader`.
- **Details**: Loads a `.spv` file for one `ShaderStage` and provides `stage_info()` for pipeline creation.

#### [shader_library.h](pipeline/shader_library.h)
- **Role**: Logical shader name → `.spv` path resolver.
- **Key Classes / Structs**: `ShaderLibrary`.
- **Details**: Searches an ordered list of directories and returns the first match, so an app's shader directory can shadow another's (`app_over_base()`). Implicitly constructible from a single directory string.

#### [surface_shader.h](pipeline/surface_shader.h)
- **Role**: Registry of derived surface shaders that materials select by name.
- **Key Classes / Structs**: `SurfaceShaderDomain`, `SurfaceShaderDesc`, `SurfaceShaderRegistry`.
- **Details**: Each entry names its G-buffer/transparent and shadow entry points and a cull mode; passes such as `GBufferPipeline`, `ShadowPipeline` and `TransparentPass` build one pipeline variant per entry.

---

### Presentation Submodule (`coopa::gfx::presentation`)

#### [renderer.h](presentation/renderer.h)
- **Role**: Multi-buffered frame orchestration (`acquire` → `record` → `submit` → `present`).
- **Key Classes / Structs**: `Renderer`.
- **Details**: Owns `MAX_FRAMES_IN_FLIGHT` (2) sets of fences, semaphores and command buffers plus the swapchain framebuffers. `draw_frame()` takes a record callback, an optional pre-pass callback and a resize callback; `set_resize_handler()` replaces the built-in resize path.

#### [window.h](presentation/window.h)
- **Role**: GLFW window wrapper and input backend.
- **Key Classes / Structs**: `Window`.
- **Details**: Creates a `GLFW_NO_API` window (optionally hidden), tracks framebuffer resizes, owns the `coopa::input::Input` fed by GLFW callbacks, and sets the window/Dock icon.

---

### Types (`coopa::gfx`, `gfxcoopa/types/`)

Vulkan-free headers that compile against the `coopa::gfx_pure` target; `types.h` includes them all.

- **[clear.h](types/clear.h)**: `Extent2D`, `ClearColor`, `ClearDepthStencil`, `ClearValues`, `ImageRegion`.
- **[enums.h](types/enums.h)**: `ShaderStage`, `Filter`, `AddressMode`, `MipmapMode`, `CompareOp`, `Topology`, `PolygonMode`, `CullMode`, `FrontFace`, `SampleCount`, `IndexType`, and related state enums.
- **[format.h](types/format.h)**: `Format` and helpers (`format_from_channels()`, `is_depth()`, `is_srgb()`, ...).
- **[sampler_desc.h](types/sampler_desc.h)**: `SamplerDesc` with presets (`pixel_art()`, `pixel_art_smooth()`, `shadow()`).
- **[texture_view.h](types/texture_view.h)**: `TextureView`, an opaque, hashable texture identity handle.
- **[vertex_layout.h](types/vertex_layout.h)**: `VertexBinding`, `VertexAttribute`, `VertexLayout`.

---

### Utility Submodule (`coopa::gfx::util`)

#### [debug_messenger.h](util/debug_messenger.h)
- **Role**: Validation-layer debug messenger (`VkDebugUtilsMessengerEXT`).
- **Key Classes / Structs**: `DebugMessenger`.
- **Details**: Routes validation output to stderr.

#### [error.h](util/error.h)
- **Role**: Vulkan result checking.
- **Key Classes / Structs**: `GFX_VK_CHECK()`, `vk_check()`, `vk_result_string()`.
- **Details**: Throws `std::runtime_error` with context when a Vulkan call does not return `VK_SUCCESS`.

#### [format.h](util/format.h)
- **Role**: OpenGL-style `VkFormat` helpers.
- **Key Classes / Structs**: `format_from_channels()`, `format_byte_size()`, `format_has_depth()`, `format_has_stencil()`.
- **Details**: Maps channel counts to formats and answers size/aspect questions about a `VkFormat`.

#### [image_readback.h](util/image_readback.h)
- **Role**: GPU image download, the mirror of `memory/image_upload.h`.
- **Key Classes / Structs**: `ImageData`, `read_image()`, `save_image_png()`.
- **Details**: Copies an image to host memory through a staging buffer and optionally writes it as a PNG.

#### [volk_init.h](util/volk_init.h)
- **Role**: One-time volk initialization.
- **Key Classes / Structs**: `ensure_volk_initialized()`.
- **Details**: Loads the Vulkan loader at runtime (searching `$VULKAN_SDK/lib` and Homebrew paths on macOS); called by `Instance`'s constructor.

---

### Engine Submodule (`coopa::gfx::engine`)

#### [render_features.h](engine/render_features.h)
- **Role**: Cross-pass invariants shared by every consumer's render config.
- **Key Classes / Structs**: `IndirectParams`.
- **Details**: The indirect-lighting terms the lighting pass adds and the SSR composite subtracts, held in one struct so both passes are fed the same values.

#### Components (`engine/components`)

- **Role**: libcoopa scene components and their YAML parsers.
- **Key Classes / Structs**: `MeshRenderer` (+ `PBRMaterial`, `AlphaMode`), `CameraComponent`, `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent`, `EnvironmentLightComponent`, `ReflectionProbeComponent`, `GiProbeVolumeComponent`, `VolumeComponent`, `SdfRenderer`, `SdfShape`, `RenderableRef`.
- **Details**: `register_render_components()` ([register.h](engine/components/register.h)) adds a parser for each to libcoopa's scene loader; `gather_renderables()` resolves each renderable object's `MeshRenderer` and `Transform` once.

#### Data Layer (`engine/data`)

##### [camera_ubo.h](engine/data/camera_ubo.h)
- **Role**: Per-frame camera uniform buffer.
- **Key Classes / Structs**: `CameraData`, `CameraUBO`.
- **Details**: 288-byte std140 block: `view`, `proj`, `view_pos`, then `prev_view`, `prev_proj` and `jitter_ndc` for the G-buffer velocity attachment (`set_reprojection()`).

##### [light_data.h](engine/data/light_data.h)
- **Role**: Per-frame light uniform buffer.
- **Key Classes / Structs**: `LightUBO`, `PointLightGPU`, `SpotLightGPU`, `LocalShadowGPU`, `LocalShadowBlock`, `LightData`.
- **Details**: One directional light with up to 4 shadow cascades, up to 16 point and 8 spot lights, sky gradient colours, the local-light shadow atlas records, and the global fog block (`fog_color` .. `fog_water`).

##### [volumetrics_data.h](engine/data/volumetrics_data.h)
- **Role**: Uniform buffer for the local-volume passes.
- **Key Classes / Structs**: `VolumeGPU`, `ScatterLightGPU`, `VolumetricsUBO`, `VolumetricsData`.
- **Details**: Up to 8 volumes and 4 in-scattering point/spot lights per frame.

##### [sdf_data.h](engine/data/sdf_data.h)
- **Role**: Per-frame-in-flight buffers describing every `SdfRenderer` and its `SdfShape`s.
- **Key Classes / Structs**: `SdfData`, `SdfGpuShapeType`, `SdfGpuOp`.

##### [mesh.h](engine/data/mesh.h)
- **Role**: GPU-resident mesh.
- **Key Classes / Structs**: `Vertex`, `InstanceData`, `MeshPart`, `MeshLod`, `MeshCpuData`, `Mesh`.
- **Details**: `Vertex` is position, normal, uv and tangent; `InstanceData` streams per-instance model matrices. Meshes come from the Blender-exported mesh YAML (`from_node()`), from in-memory arrays, or from CPU data with LODs; a multi-buffered mesh can be rewritten each frame (`update_vertices()`).

##### [model_ubo.h](engine/data/model_ubo.h)
- **Role**: Per-object push constant block.
- **Key Classes / Structs**: `ModelPushConstants`.
- **Details**: 128 bytes: `model` and `normal_matrix = transpose(inverse(model))`.

##### [texture.h](engine/data/texture.h)
- **Role**: GPU-resident 2D texture.
- **Key Classes / Structs**: `Texture`.
- **Details**: An RGBA8 `Image` plus its own `Sampler`, decoded from an image file.

##### [grading_lut.h](engine/data/grading_lut.h) / [palette_lut.h](engine/data/palette_lut.h)
- **Role**: Colour-grading strip LUT and palette lookup textures.
- **Key Classes / Structs**: `GradingLut`, `PaletteLut`.
- **Details**: `GradingLut` loads an N·N × N strip PNG with bilinear filtering; `PaletteLut` loads a palette PNG into an N × 1 nearest-filtered texture for quantization.

##### [skinned_mesh_source.h](engine/data/skinned_mesh_source.h)
- **Role**: CPU-only bind-pose mesh with joint indices and weights.
- **Key Classes / Structs**: `SkinnedMeshSource`.

#### Asset Loaders (`engine/loaders`)

- **Role**: `coopa::asset` loaders.
- **Key Classes / Structs**: `MeshLoader`, `TextureLoader` (+ `DecodedImage`), `SkinnedMeshSourceLoader`.
- **Details**: Decode on a worker thread, finalize GPU resources on the main thread. `TextureLoader` decodes with stb_image and honours per-path colour-space declarations.

#### Global Illumination Layer (`engine/gi`, namespace `coopa::gfx::engine::gi`)

##### [brdf_lut.h](engine/gi/brdf_lut.h)
- **Role**: Split-sum BRDF integration look-up table.
- **Key Classes / Structs**: `BRDFLUT`.
- **Details**: Renders a 512×512 `R16G16_SFLOAT` table once, using gfxcoopa's own `brdf_lut.vert`/`.frag`.

##### [gi_baker.h](engine/gi/gi_baker.h)
- **Role**: CPU SH probe baker.
- **Key Classes / Structs**: `GiBaker`, `SceneBox`, `HitInfo`.
- **Details**: Traces 128 rays per probe against the scene's renderables (as boxes) and projects the radiance onto 9 L2 SH coefficients, baking probes in parallel.

##### [gi_data.h](engine/gi/gi_data.h)
- **Role**: GPU buffers for probe volumes and reflection probes.
- **Key Classes / Structs**: `SHProbe`, `GiUniforms`, `ReflectionProbeUniforms`, `GiData`.

##### [gi_system.h](engine/gi/gi_system.h)
- **Role**: Global Illumination orchestrator.
- **Key Classes / Structs**: `GiSystem`.
- **Details**: Owns the probe SSBO, GI uniforms, BRDF LUT and up to 4 reflection-probe cubemaps; `bake()` fills the probe grid and captures each reflection probe (`ProbeCapturePass` + `EnvPrefilterPass`). Exposes one descriptor set (see the table above), usually passed to lit passes as an `ExtraSets` entry.

#### Render Targets (`engine/targets`)

##### [cubemap_target.h](engine/targets/cubemap_target.h)
- **Role**: Mipmapped HDR colour cubemap render target.
- **Key Classes / Structs**: `CubemapTarget`.
- **Details**: Per-face and per-mip views and render passes for reflection-probe capture and prefiltering.

##### [gbuffer_target.h](engine/targets/gbuffer_target.h)
- **Role**: The deferred G-buffer.
- **Key Classes / Structs**: `GBufferTarget`.
- **Details**: G0 `R8G8B8A8_UNORM` albedo + AO, G1 `R16G16B16A16_SFLOAT` normal + metallic, G2 `R16G16B16A16_SFLOAT` position + roughness, G3 `R16G16B16A16_SFLOAT` emissive, G4 `R16G16B16A16_SFLOAT` velocity and linear depths, and `D32_SFLOAT` depth left readable after the pass.

##### [offscreen_target.h](engine/targets/offscreen_target.h)
- **Role**: Colour (+ optional depth) render target.
- **Key Classes / Structs**: `OffscreenTarget`, `ColorOnlyTag`.
- **Details**: Configurable size, colour `Format` (default `RGBA8_Unorm`) and sample count; the render pass leaves the colour image shader-readable for later passes.

##### [shadow_map_target.h](engine/targets/shadow_map_target.h)
- **Role**: Shadow depth targets.
- **Key Classes / Structs**: `ShadowMapTarget`.
- **Details**: A directional atlas of 1–4 cascade tiles (default 2048² per tile), a point-light cubemap (default 512² per face) and a spot-light map (default 1024²), with `begin_directional_pass()`, `set_cascade_viewport()`, `begin_cube_face_pass()` and `begin_spot_pass()`.

#### Render Passes & Pipelines (`engine/passes`)

Every pass takes its `.spv` paths from the caller and documents the push-constant layout that shader must match, except `SmaaPass`, `ProbeCapturePass` and the GI bake, which load gfxcoopa's own shaders through a `ShaderLibrary`.

##### Shared building blocks
- **[fullscreen_stage.h](engine/passes/fullscreen_stage.h)**: `FullscreenStage`, `FullscreenStageDesc` — one fullscreen-triangle stage (two shaders, its own descriptor sets, a pipeline with no vertex input or depth test). Most post-process passes own one or more.
- **[extra_sets.h](engine/passes/extra_sets.h)**: `ExtraSets` — optional app-supplied descriptor-set layouts and their binder, appended after a pass's own sets and validated together.

##### Geometry and lighting
- **[gbuffer_pipeline.h](engine/passes/gbuffer_pipeline.h)**: `GBufferPipeline` — opaque and alpha-masked mesh pipelines into the G-buffer, with a no-cull sibling and one variant per registered surface shader.
- **[shadow_pipeline.h](engine/passes/shadow_pipeline.h)**: `ShadowPipeline` — depth-only directional and cube-face pipelines, with depth bias and per-surface-shader variants.
- **[sdf_gbuffer_pass.h](engine/passes/sdf_gbuffer_pass.h)** / **[sdf_shadow_pass.h](engine/passes/sdf_shadow_pass.h)** / **[sdf_forward_pass.h](engine/passes/sdf_forward_pass.h)**: `SdfGBufferPass`, `SdfShadowPass`, `SdfForwardPass` — raymarch OPAQUE/MASK SDFs into the G-buffer, into shadow maps, and BLEND SDFs into the HDR frame alongside `TransparentPass`.
- **[deferred_lighting_pass.h](engine/passes/deferred_lighting_pass.h)**: `DeferredLightingPass` — shades the G-buffer into an HDR target in one fullscreen draw (camera, light, shadow, extras, then its own G-buffer/SSAO set).
- **[transparent_pass.h](engine/passes/transparent_pass.h)**: `TransparentPass` — forward alpha-blended geometry over the lit frame, depth-tested against the opaque depth, with per-surface-shader variants.
- **[probe_capture_pass.h](engine/passes/probe_capture_pass.h)**: `ProbeCapturePass` — draws the analytic sky, then forward-lit scene geometry, into one reflection-probe cubemap face.
- **[env_prefilter_pass.h](engine/passes/env_prefilter_pass.h)**: `EnvPrefilterPass` — GGX-prefilters mips 1..N-1 of a captured cubemap from its mip 0.

##### Screen-space effects
- **[hiz_pass.h](engine/passes/hiz_pass.h)**: `HiZPass` — an `R32_SFLOAT` depth pyramid; the reduction is whatever the caller's fragment shader computes (min for SSR, a depth-aware average for SSAO).
- **[ssao_pass.h](engine/passes/ssao_pass.h)**: `SsaoPass` — GTAO-style horizon AO over a prefiltered depth pyramid, temporal resolve, bilateral blur, optional half resolution with a depth/normal-aware upsample.
- **[temporal_history_pass.h](engine/passes/temporal_history_pass.h)**: `TemporalHistoryPass` — shared per-pixel history-validity and sample-count buffer (RG16F, ping-ponged) for the temporal accumulators.
- **[scene_color_mip_pass.h](engine/passes/scene_color_mip_pass.h)**: `SceneColorMipPass` — up to 7 mips of the lit HDR colour (`R16G16B16A16_SFLOAT`, 2×2 box filter) for SSR cone tracing.
- **[ssr_pass.h](engine/passes/ssr_pass.h)**: `SsrPass` — Hi-Z raymarch (optionally half resolution), ping-ponged temporal resolve, bilateral blur, optional traced SSGI bounce, and a BRDF composite into the HDR frame.

##### Volumetrics and fog
- **[fog_pass.h](engine/passes/fog_pass.h)**: `FogPass` — global height fog blended in place over the opaque scene (premultiplied, inside a caller's LOAD render pass); parameters come from the light UBO's fog block.
- **[volumetrics_pass.h](engine/passes/volumetrics_pass.h)**: `VolumetricsPass` — reduced-resolution raymarch of local volumes, then a full-resolution composite.
- **[froxel_volumetrics_pass.h](engine/passes/froxel_volumetrics_pass.h)**: `FroxelVolumetricsPass` — the froxel-grid alternative: inject, two-pass integrate, then a per-pixel apply, all as fragment passes over a 2D atlas.

##### Post-processing and anti-aliasing
- **[exposure_pass.h](engine/passes/exposure_pass.h)**: `ExposurePass` — auto-exposure metering into a 1×1 target that adapts over time.
- **[bloom_pass.h](engine/passes/bloom_pass.h)**: `BloomPass` — threshold prefilter, downsample pyramid, additive upsample.
- **[dof_pass.h](engine/passes/dof_pass.h)**: `DofPass` — thin-lens circle of confusion, half-resolution bokeh gather, full-resolution composite.
- **[tilt_shift_pass.h](engine/passes/tilt_shift_pass.h)**: `TiltShiftPass` — separable screen-space focus-band blur with the upscale folded in.
- **[fxaa_pass.h](engine/passes/fxaa_pass.h)**: `FxaaPass` — standalone FXAA 3.11 over an already-tonemapped image.
- **[taa_pass.h](engine/passes/taa_pass.h)**: `TaaPass` — ping-ponged RGBA16F accumulation, reprojected by the G-buffer velocity (or depth and camera motion), copied into an RGBA8 output.
- **[smaa_pass.h](engine/passes/smaa_pass.h)**: `SmaaPass` — Jimenez SMAA 1x: edge detection, blend weights, neighbourhood blend, using gfxcoopa's `smaa_*` shaders and the reference area/search textures.
- **[stylize_pass.h](engine/passes/stylize_pass.h)**: `StylizePass` — optional bloom, tonemap, depth/normal outlines, ordered dither and palette quantization in one draw.
- **[present_pass.h](engine/passes/present_pass.h)**: `PresentPass` — writes the final LDR image to the swapchain unmodified.

##### 2D
- **[textured_quad_2d_pass.h](engine/passes/textured_quad_2d_pass.h)**: `TexturedQuad2DPass`, `TexturedQuad2DDesc` — shared 2D textured-quad machinery (descriptor cache, streamed geometry, named pipeline variants) for UI and sprite passes; pairs with `gfx/surface2d/quad_vs.glsl`.

#### Engine Utilities (`engine/util`)

##### [sampler.h](engine/util/sampler.h)
- **Role**: RAII `VkSampler`.
- **Key Classes / Structs**: `Sampler`.
- **Details**: Built from a `SamplerDesc`, or via `nearest()`, `linear()` and `shadow()` (depth comparison).

##### [instance_batcher.h](engine/util/instance_batcher.h)
- **Role**: Instanced draw batching.
- **Key Classes / Structs**: `InstanceBatcher`.
- **Details**: Groups per-frame draw items into contiguous batches and streams their transforms through one shared instance vertex buffer.

##### [material_texture_cache.h](engine/util/material_texture_cache.h)
- **Role**: Shared material descriptor sets.
- **Key Classes / Structs**: `MaterialTextureCache`.
- **Details**: One set per distinct (alpha mask, albedo, normal, metallic-roughness) combination, with neutral fallbacks for unused slots, shared by the G-buffer, shadow, transparent and probe-capture paths.

##### [sh_math.h](engine/util/sh_math.h)
- **Role**: 2nd-order Spherical Harmonics math.
- **Key Classes / Structs**: `sh_basis()`, `sh_project_sample()`, `sh_evaluate_irradiance()`.
- **Details**: The 9 real SH basis functions, sample projection, and cosine-lobe irradiance evaluation.

##### [smaa_textures.h](engine/util/smaa_textures.h)
- **Role**: SMAA lookup textures.
- **Key Classes / Structs**: `SmaaTextures`.
- **Details**: Uploads the Jimenez reference area and search textures from `AreaTex.h`/`SearchTex.h` (found via `SMAA_TEXTURES_DIR`; not vendored).

---

## Usage Example

`app::Context` performs the whole bring-up; see the [top-level README](../README.md) for a complete triangle.

```cpp
#include <gfxcoopa/app/context.h>

using namespace coopa::gfx;

int main() {
    app::ContextConfig config;
    config.title = "gfxcoopa demo";
    app::Context ctx(app::ContextConfig::from_env(config));  // reads ONESHOT / MAX_FRAMES

    app::FrameCallbacks frame;
    frame.clear  = ClearColor{0.1f, 0.1f, 0.12f, 1.0f};
    frame.record = [&](command::CommandBuffer& cmd) {
        // Record draw commands into the swapchain render pass here.
    };

    ctx.run(nullptr, frame);  // poll, update, draw, until the window closes
}
```

To drive the loop yourself, call `ctx.poll()` and `ctx.frame(frame)` while `!ctx.should_close()`. The individual objects (`presentation::Window`, `core::Instance`, `core::Device`, `core::Swapchain`, `presentation::Renderer`, ...) remain available for applications that build the chain by hand.
