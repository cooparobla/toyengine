# Sealing toyengine off from raw Vulkan

> **Status: done.** Every first-party root -- `src/toyengine`, `src/editor`, `src/hub`,
> `src/tests` -- reports **0 leaks**, `GFX_LEAK_CHECK` defaults to **ON**, and
> `ctest -R no_vulkan` runs the gate as four tests. No `// gfx-allow-vulkan` marker exists
> under any of those roots except the two loader environment-variable names in
> `runtime_paths.cpp`, which are strings rather than API use.

## The rule

`libs/gfxcoopa` is the sole owner of Vulkan, VMA and GLFW. Every other first-party
directory — `toyengine/`, `editor/`, `hub/` — must speak only gfxcoopa's sealed API:

| Instead of | Use |
|---|---|
| `VkImageView` | `coopa::gfx::TextureView` (`types/texture_view.h`) |
| `VkImageLayout` | `coopa::gfx::TextureUsage` (`types/enums.h`) |
| `VkImageUsageFlags` | `coopa::gfx::ImageUsage` |
| `VkFormat` | `coopa::gfx::Format` |
| `VkClearColorValue` | `coopa::gfx::ClearColor` (`types/clear.h`) |
| `VkExtent2D` | `coopa::gfx::Extent2D` |
| `vkCmdPipelineBarrier` | `CommandBuffer::transition()` where the image tracks its own usage, `barrier()` / `barriers()` with an `ImageBarrier` where it does not |
| `VkRenderPass` | `coopa::gfx::RenderPassRef` (`types/render_pass_ref.h`) |
| `VkSampler` | `coopa::gfx::SamplerHandle` (`types/sampler_handle.h`) |
| `vmaCreateImage` for an array / volume / cube | `memory::Image`'s `ImageDesc` ctor, and `Image::make_view()` for a per-mip or per-layer view |
| `vkCmd*` anything | a `CommandBuffer` method |

`tools/check_no_vulkan.sh` enforces it. A genuinely unavoidable reference opts out with a
trailing `// gfx-allow-vulkan`; keep that budget near zero.

## Why the number was never believable

The gate was wired as `check_no_vulkan.sh ${TOYENGINE_DIR}` — the **repo root**, which
includes `libs/`. gfxcoopa's own sources name Vulkan by definition, so the scan reported
~2,950 hits, ~2,440 of them from gfxcoopa itself. The script's own header says never to run it
over gfxcoopa. Fixed: the `toyengine_leak_check` target now runs it once per first-party root.

## What it took

The leaks were never 504 separate problems. They were six missing gfxcoopa abstractions, and
filling them is what the work actually was:

| # | Added to gfxcoopa | What it unblocked |
|---|---|---|
| 1 | **Image barriers with aspect and sub-range** -- `types/barrier.h`'s `ImageAspect`, `SubresourceRange`, `ImageAccess` (a bitmask), `ImageBarrier`, and `CommandBuffer::barrier()` / `barriers()` for an image held only as an `ImageHandle` | every hand-rolled `vkCmdPipelineBarrier` in `render/` |
| 2 | **`pipeline::Framebuffer`** (there was no framebuffer type at all) and a `RenderPass` ctor taking a `RenderPassDesc` of sealed `Format`/`TextureUsage` | `local_shadow_atlas`, `dir_shadow_cache`, which each hand-rolled `vkCreateRenderPass` + `vkCreateFramebuffer` and their destroys |
| 3 | **`CommandBuffer::copy_image()`** in three shapes, plus multi-region `copy_buffer_to_image()` / `copy_image_to_buffer()` | `motion_blur_pass`, `contact_shadow_pass`, `dir_shadow_cache`, the focus probe |
| 4 | **`command::TimestampPool`**, absorbing the tick period and the `timestampValidBits` mask | `gpu_profiler` |
| 5 | **`RenderPassRef`** -- an opaque borrowed reference, with `Pipeline` / `FullscreenStage` ctors and `*_render_pass_ref()` accessors taking it | the five sky/cloud/particle passes that smuggled handles past the seal as `detail::RawRenderPass` |
| 6 | **`ImageDesc` / `memory::ImageView`** for 2D-array, 3D and cube images, and per-mip views | `terrain_gpu` (a 2D array) and `cloud_noise_texture` (a 3D volume with one storage view per mip) -- 107 leaks that **could not** have been sealed without it, and which the first version of this plan folded into item 1 by mistake |

Two sealed vocabulary additions came with them: `TextureUsage::Storage` (GENERAL, for a storage
image) and `TextureUsage::DepthReadOnly` (sampled while still depth-tested), plus
`SamplerHandle` for the samplers a pass hands another pass.

## Three places the behaviour had to be preserved exactly

Each compiles clean either way and renders *almost* right, which is why each is now stated at
its call site:

1. **Terrain's upload barrier keeps its vertex stage.** Tessellated displacement samples the
   splat atlas in the vertex shader, and `ShaderRead`'s derived mask is fragment-only, so
   `terrain_gpu` sets `VertexShaderRead | FragmentShaderRead` explicitly.
2. **`AllCommands` survives in `transition_gbuffer_depth_to_shader_read_()`.** A narrower
   hand-picked mask reintroduced a write-after-write hazard once transparency ran; the barrier
   sits downstream of a pass chain whose last writer the call site cannot determine.
3. **Render-pass compatibility did not move.** Both shadow classes' passes stay
   render-pass-compatible with `ShadowMapTarget`'s directional pass (one D32 attachment, one
   subpass), which is what lets every per-material shadow pipeline variant draw into either.
   `render/shadow_pass_desc.h` now holds that description once so the four cannot drift.

## Verification

```bash
cmake --build build --target toyengine_leak_check    # per-root counts; exits 1 if any appear
ctest --test-dir build -R no_vulkan                  # the same four, as tests
ctest --test-dir build -R 'terrain|water|cloud|sky|particle|transparent|ssr|ssao|shadow|fog|dof|taa|render' -j8
```

Barrier and layout work is exactly the class of change that compiles clean and breaks the
image, so the GPU render suites -- not just a build -- are the gate. They pass.
