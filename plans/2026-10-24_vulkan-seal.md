# Sealing toyengine off from raw Vulkan

> **Status: diagnosed, foundations landed, bulk outstanding.** `editor/` and `hub/` are
> **clean (0 leaks)**. `toyengine/` is at **504**, all but 1 of them in `toyengine/render/`.

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
| `vkCmdPipelineBarrier` | `CommandBuffer::transition()` / the semantic `*_barrier()` helpers |
| `vkCmd*` anything | a `CommandBuffer` method |

`tools/check_no_vulkan.sh` enforces it. A genuinely unavoidable reference opts out with a
trailing `// gfx-allow-vulkan`; keep that budget near zero.

## Why the number was never believable

The gate was wired as `check_no_vulkan.sh ${TOYENGINE_DIR}` — the **repo root**, which
includes `libs/`. gfxcoopa's own sources name Vulkan by definition, so the scan reported
~2,950 hits, ~2,440 of them from gfxcoopa itself. The script's own header says never to run it
over gfxcoopa. Fixed: the `toyengine_leak_check` target now runs it once per first-party root.

## Landed

- **Gate rescoped** to `toyengine/`, `editor/`, `hub/`; `toyengine_leak_check` is now always
  available (not only when `GFX_LEAK_CHECK=ON`), so the count is checkable any time.
- **gfxcoopa gaps filled** where a sealed sibling was simply missing:
  `CubemapTarget::cubemap_view_typed()` / `mip0_cube_view_typed()`,
  `ShadowMapTarget::cube_shadow_view_typed()`, `Swapchain::extent_typed()`,
  `OffscreenTarget::begin(ClearColor)`, and `Renderer::draw_frame`/`begin_frame` overloads
  taking `ClearColor`.
- **toyengine migrated** off `VkClearColorValue` and `VkExtent2D`; the two `VK_*` strings in
  `runtime_paths.cpp` are loader *environment variable names*, now marked `// gfx-allow-vulkan`.

## Outstanding: five gfxcoopa abstractions, then a mechanical migration

The 504 are not 504 separate problems. They are five missing abstractions:

| # | Missing from gfxcoopa | Unblocks | Lines |
|---|---|---|---|
| 1 | **Image barrier with aspect + sub-range.** `CommandBuffer::transition(Image&, TextureUsage)` exists but cannot express depth aspect, a mip/layer sub-range, or an image the caller holds only as a handle. | `dir_shadow_cache`, `local_shadow_atlas`, `cloud_noise_texture`, `contact_shadow_pass`, `motion_blur_pass`, `terrain_gpu`, `pipeline_record` | ~250 |
| 2 | **A `Framebuffer` RAII wrapper — none exists at all**, plus a `RenderPass` constructor taking `Format`/`TextureUsage` instead of `VkFormat`/`VkImageLayout`. | `local_shadow_atlas`, `dir_shadow_cache` (both hand-roll `vkCreateRenderPass` + `vkCreateFramebuffer` + their `vkDestroy*`) | ~120 |
| 3 | **`CommandBuffer::copy_image(src, dst, ImageRegion)`.** `copy_buffer*` and `blit` exist; image→image does not. | `motion_blur_pass`, `contact_shadow_pass`, `dir_shadow_cache`, `pipeline_record` | ~40 |
| 4 | **A query-pool / timestamp wrapper.** | `gpu_profiler` (`vkCmdWriteTimestamp`) | ~19 |
| 5 | **`TextureView` adoption.** The sealed accessors now all exist; toyengine's own pass constructors and members still take `VkImageView` / `VkRenderPass`. | `cloud_*`, `sky_cloud`, `particle_pass`, `terrain_gpu`, `pipeline_build`, `pipeline_sky_fog`, `toy_render_pipeline.h` | ~75 |

Items 1-3 are the real work; 5 is mechanical once 1 is in (most `VkImageView` members exist
only to be fed to a barrier or a render-pass construction).

## Finish line

Drive each root to zero, then set `GFX_LEAK_CHECK` **ON** by default in `CMakeLists.txt` so it
can never regress. `uicoopa` and `pixengine` already build with it on; toyengine should too.

## Verification

```bash
cmake --build build --target toyengine_leak_check    # per-root counts, exits 1 while any remain
ctest --test-dir build -L gpu -R "render|shadow|terrain|cloud" -j2
```

Barrier and layout work is exactly the class of change that compiles clean and breaks the
image, so the GPU render suites — not just a build — are the gate for items 1-3.
