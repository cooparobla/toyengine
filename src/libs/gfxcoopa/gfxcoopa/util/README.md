# Utility Submodule (`coopa::gfx::util`)

The `coopa::gfx::util` submodule provides debug validation messengers, format helpers, Volk dynamic loader initialization, error checking macros, and GPU image readback.

---

## Utility Component Graph

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                             UTILITY COMPONENT HIERARCHY                                  │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                                ┌───────────────────────┐
                                │       volk_init       │  (volkInitialize)
                                └───────────┬───────────┘
                                            │
       ┌────────────────────────────────────┼────────────────────────────────────┐
       ▼                                    ▼                                    ▼
┌───────────────┐                  ┌─────────────────┐                  ┌────────────────┐
│DebugMessenger │                  │  GFX_VK_CHECK   │                  │ Format Helpers │
└───────────────┘                  └─────────────────┘                  └────────────────┘
```

---

## File Breakdown

### [debug_messenger.h](debug_messenger.h)
- **Role**: Vulkan validation layer debug messenger (`VkDebugUtilsMessengerEXT`).
- **Key Classes / Structs**: `DebugMessenger`.
- **Details**: Captures Vulkan API warning/error logs and routes formatted debug reports to stderr. `make_create_info()` is also chained into instance creation so validation covers `vkCreateInstance` itself.

### [error.h](error.h)
- **Role**: Error checking macros and string translation utilities.
- **Key Classes / Structs**: `GFX_VK_CHECK()`, `vk_check()`, `vk_result_string()`.
- **Details**: `GFX_VK_CHECK(expr)` calls `vk_check()` with the stringified call, which throws `std::runtime_error` naming the call and the `VkResult` when it is not `VK_SUCCESS`.

### [format.h](format.h)
- **Role**: `VkFormat` helpers.
- **Key Classes / Structs**: `format_from_channels()`, `format_byte_size()`, `format_has_depth()`, `format_has_stencil()`.
- **Details**: Picks an 8-bit UNORM/SRGB format for a channel count, returns a format's texel size, and classifies depth/stencil formats.

### [image_readback.h](image_readback.h)
- **Role**: GPU image readback, the mirror of `memory/image_upload.h`.
- **Key Classes / Structs**: `ImageData`, `read_image()`, `save_image_png()`.
- **Details**: Copies an image into a staging buffer with a one-shot command buffer and returns tightly packed pixels, or writes them straight to a PNG (creating parent directories).

### [volk_init.h](volk_init.h)
- **Role**: Dynamic Vulkan loader initialization wrapper.
- **Key Classes / Structs**: `ensure_volk_initialized()`.
- **Details**: Initializes volk exactly once, so Vulkan entry points load at runtime without linking the loader. `Instance`'s constructor calls it. On macOS it prefers a loader bundled in the app's `Contents/Frameworks`, then `$VULKAN_SDK/lib`, `/opt/homebrew/lib` and `/usr/local/lib`, and hands the loader to GLFW.

---

## Usage Example

```cpp
#include <gfxcoopa/util/volk_init.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/util/format.h>
#include <gfxcoopa/util/image_readback.h>

// 1. Initialize Volk dynamic loader (Instance does this for you)
coopa::gfx::util::ensure_volk_initialized();

// 2. Check a Vulkan call
GFX_VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

// 3. Pick a texture format for a 4-channel sRGB image
VkFormat format = coopa::gfx::util::format_from_channels(4, /*srgb=*/true);

// 4. Save a rendered image to disk
coopa::gfx::util::save_image_png(device, allocator, cmd_pool, image, "out/frame.png");
```
