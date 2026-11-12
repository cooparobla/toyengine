# Pipeline Submodule (`coopa::gfx::pipeline`)

The `coopa::gfx::pipeline` submodule provides RAII wrappers and builders for Vulkan shaders, render passes, descriptor pools/layouts/sets, and graphics pipeline state configuration, plus the shader-name resolver and surface-shader registry the engine passes use.

---

## Pipeline Architecture Graph

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                            PIPELINE CREATION & LAYOUT FLOW                               │
└──────────────────────────────────────────────────────────────────────────────────────────┘

       ┌───────────────────────┐                        ┌───────────────────────┐
       │        Shader         │                        │  DescriptorSetLayout  │
       └───────────┬───────────┘                        └───────────┬───────────┘
                   │                                                │
                   ▼                                                ▼
       ┌───────────────────────┐                        ┌───────────────────────┐
       │     PipelineDesc      ├───────────────────────►│   VkPipelineLayout    │
       └───────────┬───────────┘                        └───────────┬───────────┘
                   │                                                │
                   ▼                                                ▼
       ┌───────────────────────┐                        ┌───────────────────────┐
       │      RenderPass       ├───────────────────────►│       Pipeline        │
       └───────────────────────┘                        └───────────────────────┘
```

---

## File Breakdown

### [descriptor.h](descriptor.h)
- **Role**: RAII abstractions for Vulkan descriptor sets, layouts, and pools.
- **Key Classes / Structs**: `DescriptorSetLayout`, `DescriptorPool`, `DescriptorSet`, `DescriptorLayoutBuilder`, `DescriptorPoolBuilder`.
- **Details**: `DescriptorLayoutBuilder` adds bindings by type (`uniform_buffer`, `storage_buffer`, `combined_sampler`, `storage_image`) and builds a `DescriptorSetLayout`; `DescriptorPoolBuilder` sizes a pool from the layouts it will serve (`add_sets(layout, count)`); `DescriptorSet` is allocated from a pool and updated with `bind_buffer`, `bind_storage_buffer` and `bind_image`.

### [pipeline.h](pipeline.h)
- **Role**: RAII encapsulation of a graphics pipeline and its layout (`VkPipeline`, `VkPipelineLayout`).
- **Key Classes / Structs**: `Pipeline`, `PipelineDesc`, `RasterState`, `DepthState`, `BlendState`, `PushConstantRange`, `BlendMode`, `PipelineConfig`.
- **Details**: The sealed constructor takes a `PipelineDesc` (shaders, `VertexLayout`, descriptor layouts, push-constant ranges, raster/depth/blend state); a blend `color_attachment_count` of 0 takes the count from the `RenderPass`. Raw-typed constructors taking `PipelineConfig` and Vk structs remain for internal use. Viewport and scissor are always dynamic.

### [render_pass.h](render_pass.h)
- **Role**: RAII wrapper for `VkRenderPass`.
- **Key Classes / Structs**: `RenderPass`.
- **Details**: A single-subpass pass with an optional color attachment, optional depth attachment and optional MSAA, with configurable final layouts (present vs. shader-read). Exposes `samples()` and `color_attachment_count()` for the sealed `Pipeline` constructor.

### [shader.h](shader.h)
- **Role**: Encapsulates SPIR-V shader module loading (`VkShaderModule`).
- **Key Classes / Structs**: `Shader`.
- **Details**: Loads a compiled `.spv` file for one stage (`ShaderStage` or `VkShaderStageFlagBits`) and builds its `VkPipelineShaderStageCreateInfo`.

### [shader_library.h](shader_library.h)
- **Role**: Resolves a logical shader name (e.g. `"ssr_composite.frag"`) to a compiled `.spv` path.
- **Key Classes / Structs**: `ShaderLibrary`.
- **Details**: Searches an ordered list of directories and returns the first match, caching results. A plain directory string converts implicitly to a one-entry library; `app_over_base(app_dir, base_dir)` builds the two-tier "app first, then gfxcoopa's `assets/shaders/`" search path.

### [surface_shader.h](surface_shader.h)
- **Role**: Registry of derived surface shaders (custom vertex/fragment entry points layered on the G-buffer, shadow and transparent backbones).
- **Key Classes / Structs**: `SurfaceShaderDesc`, `SurfaceShaderDomain`, `SurfaceShaderRegistry`.
- **Details**: Each description names its logical entry points (`vert`, `frag`, `shadow_vert`, `shadow_frag`, `shadow_cube_vert`, `shadow_cube_frag`) plus a cull mode; the caller resolves them through its `ShaderLibrary` and hands the `.spv` paths to a pass's `add_variant()`. `add()` and `require()` throw on empty, duplicate or unknown names.

---

## Usage Example

```cpp
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>

using namespace coopa::gfx;

// 1. Create render pass (color only, presented to the swapchain)
pipeline::RenderPass render_pass(device, swapchain.image_format(), VK_FORMAT_UNDEFINED);

// 2. Load SPIR-V shaders
pipeline::Shader vert_shader(device, "assets/shaders/test.vert.spv", ShaderStage::Vertex);
pipeline::Shader frag_shader(device, "assets/shaders/test.frag.spv", ShaderStage::Fragment);

// 3. Build descriptor set layout
pipeline::DescriptorSetLayout layout = pipeline::DescriptorLayoutBuilder()
    .uniform_buffer(0, ShaderStage::Vertex)
    .build(device);

// 4. Describe and create the graphics pipeline
pipeline::PipelineDesc desc;
desc.shaders            = {&vert_shader, &frag_shader};
desc.descriptor_layouts = {&layout};
desc.depth.test         = false;  // no depth attachment
desc.depth.write        = false;

pipeline::Pipeline triangle(device, render_pass, desc);
```
