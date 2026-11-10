# Core Submodule (`coopa::gfx::core`)

The `coopa::gfx::core` submodule encapsulates core Vulkan initialization components: instance creation, window surface integration, physical/logical device selection, and swapchain management.

---

## Core Subsystem Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                            CORE SYSTEM DEPENDENCY HIERARCHY                              │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                                ┌───────────────────────┐
                                │      volkInitialize   │
                                └───────────┬───────────┘
                                            │
                                            ▼
                                ┌───────────────────────┐
                                │       Instance        │
                                └───────────┬───────────┘
                                            │
                    ┌───────────────────────┴───────────────────────┐
                    │                                               │
                    ▼                                               ▼
        ┌───────────────────────┐                       ┌───────────────────────┐
        │        Window         │                       │    DebugMessenger     │
        └───────────┬───────────┘                       └───────────────────────┘
                    │ creates VkSurfaceKHR
                    ▼
        ┌───────────────────────┐
        │        Surface        │
        └───────────┬───────────┘
                    │
                    ▼
        ┌───────────────────────┐
        │        Device         │  (Selects Physical GPU & creates VkDevice)
        └───────────┬───────────┘
                    │
                    ▼
        ┌───────────────────────┐
        │       Swapchain       │  (Manages VkSwapchainKHR & Swapchain Image Views)
        └───────────────────────┘
```

---

## File Breakdown

### [device.h](device.h)
- **Role**: Physical GPU selection (preferring discrete GPUs), logical `VkDevice` creation, queue family indexing (graphics and present queues), queue retrieval, and `wait_idle()`.
- **Key Classes / Structs**: `Device`, `QueueFamilyIndices`.
- **Details**: Automatically rates physical GPUs, queries queue families for graphics and present support, creates logical devices with required extensions (`VK_KHR_swapchain`, plus `VK_KHR_portability_subset` when the device reports it), and enforces idle wait on teardown.

### [instance.h](instance.h)
- **Role**: RAII wrapper for `VkInstance`. Performs one-time Volk dynamic loader initialization, optionally enables the Vulkan validation layer, and manages instance extensions.
- **Key Classes / Structs**: `Instance`.
- **Details**: Triggers `volkInitialize()` upon creation, queries required GLFW window extensions, and enables portability enumeration when the loader offers it (MoltenVK). With `enable_validation` (default `true`) it enables `VK_LAYER_KHRONOS_validation` plus a `DebugMessenger`; if the layer is not installed it warns and continues without it.

### [surface.h](surface.h)
- **Role**: RAII wrapper around `VkSurfaceKHR`. Integrates GLFW window handles with Vulkan and provides surface capability/format support query helpers (`query_support()`).
- **Key Classes / Structs**: `Surface`, `SwapchainSupportDetails`.
- **Details**: Uses `glfwCreateWindowSurface` to bind Vulkan to the platform window and queries surface capabilities, supported surface formats, and presentation modes.

### [swapchain.h](swapchain.h)
- **Role**: Swapchain management wrapper (`VkSwapchainKHR`). Handles image extent resolution, image format selection, present mode selection, swapchain image views, and dynamic recreation (`recreate()`) on window resize.
- **Key Classes / Structs**: `Swapchain`.
- **Details**: Chooses optimal surface format (`VK_FORMAT_B8G8R8A8_SRGB`), present mode (FIFO when `vsync` is true, the default; otherwise `VK_PRESENT_MODE_MAILBOX_KHR` with a FIFO fallback), extent resolution, builds swapchain image views, and handles full recreation (`recreate()`) on window resize events.

---

## Usage Example

```cpp
#include <gfxcoopa/core/instance.h>
#include <gfxcoopa/core/surface.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/presentation/window.h>

coopa::gfx::presentation::Window window("My Vulkan App", 1280, 720);
coopa::gfx::core::Instance       instance("app_name");
coopa::gfx::core::Surface        surface(instance, window.handle());
coopa::gfx::core::Device         device(instance, surface);
coopa::gfx::core::Swapchain      swapchain(device, surface, 1280, 720);

// Handle window resizing
if (window.was_resized()) {
    auto [width, height] = window.framebuffer_size();
    swapchain.recreate(width, height);
}
```
