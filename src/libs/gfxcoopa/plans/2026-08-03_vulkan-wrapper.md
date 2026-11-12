# gfxcoopa — Complete Vulkan Wrapper Library

A header-only C++20 Vulkan wrapper that provides an OpenGL-developer-friendly abstraction, following the structural and stylistic conventions of [libcoopa](file:///home/coopa/git/libcoopa).

---

## Introduction

Vulkan's raw C API is verbose and hostile to newcomers — especially developers with OpenGL backgrounds who expect single-call operations like `glCreateShader`, `glBindBuffer`, and `glDrawArrays`. **gfxcoopa** will wrap the entire Vulkan lifecycle into clean, composable C++ classes that:

1. **Read like OpenGL** — Methods named `create()`, `bind()`, `draw()`, `set_viewport()` instead of `vkCreateXxx` / `vkCmdBindXxx`.
2. **RAII everything** — Resources are acquired in constructors and released in destructors. No manual `vkDestroy*` calls.
3. **Sane defaults** — A triangle on screen in ~20 lines of `test.cpp`, not 800.
4. **Header-only** — Exactly like libcoopa. Every module is a `.h` file under `gfxcoopa/`.
5. **Coopadocs-compatible** — All public API uses Doxygen `@brief`, `@param`, `@return` tags parsed by [coopadocs](file:///home/coopa/git/coopadocs).

---

## Proposed Changes

### Repository scaffold (mirroring libcoopa)

```
gfxcoopa/
├── CMakeLists.txt                         # [NEW]
├── .coopadocs                             # [NEW]
├── .gitignore                             # [MODIFY] add build/ and .docs/
├── README.md                              # [NEW]
├── configuration/
│   └── root_directory.h.in                # [NEW] (same template as libcoopa)
├── test.cpp                               # [NEW] — full test suite
├── includes/                              # [NEW] — third-party vendored headers
│   ├── volk/                          # vendored — zero link-time Vulkan dependency
│   └── vma/                           # vendored — VulkanMemoryAllocator
└── gfxcoopa/                              # [NEW] — all library headers
    ├── core/
    │   ├── instance.h
    │   ├── device.h
    │   ├── surface.h
    │   └── swapchain.h
    ├── pipeline/
    │   ├── shader.h
    │   ├── pipeline.h
    │   ├── render_pass.h
    │   └── descriptor.h
    ├── memory/
    │   ├── buffer.h
    │   ├── image.h
    │   └── allocator.h
    ├── command/
    │   ├── command_pool.h
    │   ├── command_buffer.h
    │   └── sync.h
    ├── presentation/
    │   ├── window.h
    │   └── renderer.h
    └── util/
        ├── debug_messenger.h
        ├── error.h
        └── format.h
```

---

### Component 1: Build system & configuration

#### [NEW] [CMakeLists.txt](file:///home/coopa/git/gfxcoopa/CMakeLists.txt)

Mirrors libcoopa's CMake structure:

```cmake
cmake_minimum_required(VERSION 3.0)
cmake_policy(VERSION 3.0)

project(gfxcoopa)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

# Vulkan headers only (volk provides the loader at runtime)
find_package(Vulkan REQUIRED)

# Window system (GLFW)
find_package(glfw3 REQUIRED)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/includes/)   # volk/, vma/
include_directories(${CMAKE_SOURCE_DIR})

# Configure root directory header
configure_file(configuration/root_directory.h.in configuration/root_directory.h)
include_directories(${CMAKE_BINARY_DIR}/configuration)

# volk implementation (compiled once here, all other TUs use VOLK_H_ONLY)
add_definitions(-DVOLK_IMPLEMENTATION)

# Add executable
add_executable(gfxcoopa test.cpp)
target_link_libraries(gfxcoopa Vulkan::Vulkan glfw dl)
```

#### [NEW] [configuration/root_directory.h.in](file:///home/coopa/git/gfxcoopa/configuration/root_directory.h.in)

Identical template to libcoopa.

#### [NEW] [.coopadocs](file:///home/coopa/git/gfxcoopa/.coopadocs)

```yaml
include:
  - gfxcoopa
```

#### [MODIFY] [.gitignore](file:///home/coopa/git/gfxcoopa/.gitignore)

Add `build/` and `.docs/` entries.

---

### Component 2: `gfxcoopa/util/` — Error handling & helpers

#### [NEW] `gfxcoopa/util/error.h`

- `VkResultCheck(VkResult result, const std::string& context)` — throws `std::runtime_error` on failure with human-readable message.
- Maps every `VkResult` enum to a string via a constexpr lookup table.
- Macro `GFX_VK_CHECK(expr)` — wraps any `vkXxx` call and auto-checks.

#### [NEW] `gfxcoopa/util/debug_messenger.h`

- `class DebugMessenger` — RAII wrapper around `VkDebugUtilsMessengerEXT`.
- Installs a default callback that routes through `coopa::debug::Logger` (from libcoopa).
- Created/destroyed automatically by `Instance`.
- Uses volk's `vkCreateDebugUtilsMessengerEXT` loaded at runtime (no link-time requirement).

#### [NEW] `gfxcoopa/util/volk_init.h`

- Calls `volkInitialize()` once at startup (done inside `Instance` constructor).
- Guards against double-init with a `static bool` flag.
- All other headers include `<volk/volk.h>` instead of `<vulkan/vulkan.h>`.

#### [NEW] `gfxcoopa/util/format.h`

- OpenGL → Vulkan format mapping helpers.
- `VkFormat format_from_channels(int channels, bool srgb)` — Returns the right `VkFormat` for a given channel count (analogous to `GL_RGBA8` etc.).
- `uint32_t format_byte_size(VkFormat)` — returns bytes per pixel.

---

### Component 3: `gfxcoopa/core/` — Instance, Device, Surface, Swapchain

#### [NEW] `gfxcoopa/core/instance.h`

```cpp
namespace coopa {
namespace gfx {
namespace core {

class Instance {
public:
    /**
     * @brief Creates a Vulkan instance with validation layers enabled in debug builds.
     * @param app_name Application display name.
     * @param enable_validation Enable Vulkan validation layers.
     */
    Instance(const std::string& app_name, bool enable_validation = true);
    ~Instance();

    VkInstance handle() const;
};

} // namespace core
} // namespace gfx
} // namespace coopa
```

- RAII `VkInstance` lifecycle.
- Auto-enables `VK_LAYER_KHRONOS_validation` when `enable_validation` is true.
- Dynamically queries available extensions and layers.
- Internally creates a `DebugMessenger` when validation is on.

#### [NEW] `gfxcoopa/core/surface.h`

- `class Surface` — RAII wrapper around `VkSurfaceKHR`.
- Constructor takes `Instance&` + `GLFWwindow*`.
- Provides `get_capabilities()`, `get_formats()`, `get_present_modes()`.

#### [NEW] `gfxcoopa/core/device.h`

```cpp
class Device {
public:
    /**
     * @brief Selects a physical device and creates logical device + queues.
     * @param instance The Vulkan instance.
     * @param surface The window surface (used to pick a present-capable queue family).
     */
    Device(Instance& instance, Surface& surface);
    ~Device();

    VkDevice handle() const;
    VkPhysicalDevice physical() const;
    VkQueue graphics_queue() const;
    VkQueue present_queue() const;
    uint32_t graphics_family() const;
    uint32_t present_family() const;

    /**
     * @brief Waits for the device to finish all operations (analogous to glFinish).
     */
    void wait_idle();
};
```

- Auto-scores physical devices by type (discrete > integrated > other).
- Finds graphics + present queue families.
- Enables required device extensions (`VK_KHR_swapchain`).

#### [NEW] `gfxcoopa/core/swapchain.h`

```cpp
class Swapchain {
public:
    /**
     * @brief Creates a swapchain for the given surface and device.
     * @param device The logical device.
     * @param surface The window surface.
     * @param width Initial framebuffer width.
     * @param height Initial framebuffer height.
     * @param vsync Enable vertical sync (FIFO present mode).
     */
    Swapchain(Device& device, Surface& surface,
              uint32_t width, uint32_t height, bool vsync = true);
    ~Swapchain();

    void recreate(uint32_t width, uint32_t height);
    VkFormat image_format() const;
    VkExtent2D extent() const;
    uint32_t image_count() const;
    const std::vector<VkImageView>& image_views() const;
};
```

- FIFO (vsync) / MAILBOX (uncapped) present mode selection.
- Automatic image view creation.
- `recreate()` for window resize handling.

---

### Component 4: `gfxcoopa/pipeline/` — Shaders, RenderPass, Pipeline, Descriptors

#### [NEW] `gfxcoopa/pipeline/shader.h`

```cpp
class Shader {
public:
    /**
     * @brief Loads a SPIR-V shader module from a file path.
     * @param device The logical device.
     * @param filepath Path to the compiled .spv file.
     * @param stage Shader stage (vertex, fragment, compute, etc.).
     */
    Shader(Device& device, const std::string& filepath, VkShaderStageFlagBits stage);
    ~Shader();

    VkPipelineShaderStageCreateInfo stage_info() const;
};
```

- Reads SPIR-V binary from disk.
- RAII `VkShaderModule`.
- OpenGL-like naming: just point at a file and specify a stage.

#### [NEW] `gfxcoopa/pipeline/render_pass.h`

```cpp
class RenderPass {
public:
    /**
     * @brief Creates a render pass with a single color attachment.
     * @param device The logical device.
     * @param color_format The swapchain image format.
     * @param enable_depth Attach a depth/stencil buffer.
     */
    RenderPass(Device& device, VkFormat color_format, bool enable_depth = true);
    ~RenderPass();

    VkRenderPass handle() const;
};
```

- Sane defaults: single color attachment + optional depth.
- Builder-pattern overload for advanced multi-subpass configurations.

#### [NEW] `gfxcoopa/pipeline/pipeline.h`

```cpp
class Pipeline {
public:
    /**
     * @brief Creates a graphics pipeline with sensible OpenGL-like defaults.
     * @param device The logical device.
     * @param render_pass The render pass this pipeline is compatible with.
     * @param shaders Vertex + fragment shader stages.
     * @param vertex_bindings Vertex input binding descriptions.
     * @param vertex_attributes Vertex input attribute descriptions.
     */
    Pipeline(Device& device, RenderPass& render_pass,
             const std::vector<Shader*>& shaders,
             const std::vector<VkVertexInputBindingDescription>& vertex_bindings,
             const std::vector<VkVertexInputAttributeDescription>& vertex_attributes);
    ~Pipeline();

    VkPipeline handle() const;
    VkPipelineLayout layout() const;

    // OpenGL-like state setters (recorded into command buffers)
    static void set_viewport(VkCommandBuffer cmd, float x, float y, float w, float h);
    static void set_scissor(VkCommandBuffer cmd, int32_t x, int32_t y, uint32_t w, uint32_t h);
};
```

- Defaults: triangle list topology, fill polygon, no blending, back-face culling, CCW winding — mirrors OpenGL defaults.
- Dynamic viewport and scissor state.
- Helper struct `PipelineConfig` for full customization.

#### [NEW] `gfxcoopa/pipeline/descriptor.h`

```cpp
class DescriptorPool {
public:
    DescriptorPool(Device& device, uint32_t max_sets,
                   const std::vector<VkDescriptorPoolSize>& pool_sizes);
    ~DescriptorPool();
};

class DescriptorSetLayout {
public:
    DescriptorSetLayout(Device& device,
                        const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    ~DescriptorSetLayout();
    VkDescriptorSetLayout handle() const;
};

class DescriptorSet {
public:
    DescriptorSet(Device& device, DescriptorPool& pool,
                  DescriptorSetLayout& layout);

    /**
     * @brief Binds a uniform buffer to this descriptor set (like glUniformBuffer).
     * @param binding The binding point index.
     * @param buffer The uniform buffer to bind.
     */
    void bind_buffer(uint32_t binding, Buffer& buffer);

    /**
     * @brief Binds a texture/sampler to this descriptor set (like glBindTexture).
     * @param binding The binding point index.
     * @param image_view The image view.
     * @param sampler The sampler.
     */
    void bind_image(uint32_t binding, VkImageView image_view, VkSampler sampler);

    VkDescriptorSet handle() const;
};
```

---

### Component 5: `gfxcoopa/memory/` — Buffers, Images, Allocator

#### [NEW] `gfxcoopa/memory/buffer.h`

```cpp
class Buffer {
public:
    /**
     * @brief Creates a Vulkan buffer with device memory.
     * @param device The logical device.
     * @param size Buffer size in bytes.
     * @param usage Usage flags (vertex, index, uniform, storage, transfer).
     * @param memory_flags Memory property flags.
     */
    Buffer(Device& device, VkDeviceSize size,
           VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memory_flags);
    ~Buffer();

    /**
     * @brief Uploads data to the buffer (analogous to glBufferData / glBufferSubData).
     * @param data Pointer to source data.
     * @param size Number of bytes to copy.
     * @param offset Byte offset into the buffer.
     */
    void upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

    VkBuffer handle() const;
    VkDeviceSize size() const;
};
```

- Static factory methods: `Buffer::vertex(...)`, `Buffer::index(...)`, `Buffer::uniform(...)`.
- `upload()` maps/memcpy/unmaps for host-visible; staging + copy for device-local.

#### [NEW] `gfxcoopa/memory/image.h`

```cpp
class Image {
public:
    /**
     * @brief Creates a 2D image with memory (analogous to glTexImage2D).
     * @param device The logical device.
     * @param width Image width in pixels.
     * @param height Image height in pixels.
     * @param format Image format.
     * @param usage Usage flags.
     * @param memory_flags Memory property flags.
     */
    Image(Device& device, uint32_t width, uint32_t height,
          VkFormat format, VkImageUsageFlags usage,
          VkMemoryPropertyFlags memory_flags);
    ~Image();

    /**
     * @brief Uploads pixel data to the image (like glTexSubImage2D).
     */
    void upload(Device& device, CommandPool& pool, const void* pixels,
                uint32_t width, uint32_t height, uint32_t channels);

    VkImage handle() const;
    VkImageView view() const;
};
```

- Auto-creates `VkImageView`.
- Layout transition helpers (`transition_layout()`).

#### [NEW] `gfxcoopa/memory/allocator.h`

- `find_memory_type(VkPhysicalDevice, uint32_t type_filter, VkMemoryPropertyFlags)` — utility used internally by Buffer and Image.
- Could later be extended to wrap VMA (Vulkan Memory Allocator) if needed.

---

### Component 6: `gfxcoopa/command/` — Command pools, buffers, synchronization

#### [NEW] `gfxcoopa/command/command_pool.h`

```cpp
class CommandPool {
public:
    CommandPool(Device& device, uint32_t queue_family_index);
    ~CommandPool();

    /**
     * @brief Allocates a set of command buffers from this pool.
     * @param count Number of command buffers to allocate.
     * @return A vector of raw VkCommandBuffer handles.
     */
    std::vector<VkCommandBuffer> allocate(uint32_t count);

    /**
     * @brief Allocates and begins a single-use command buffer (like glBegin for one-shot ops).
     * @return A transient command buffer ready for recording.
     */
    VkCommandBuffer begin_single_use();

    /**
     * @brief Ends, submits, and waits for a single-use command buffer.
     * @param cmd The command buffer to submit.
     * @param queue The queue to submit to.
     */
    void end_single_use(VkCommandBuffer cmd, VkQueue queue);
};
```

#### [NEW] `gfxcoopa/command/command_buffer.h`

- Helper class wrapping command buffer recording:
  - `begin()`, `end()` — analogous to `glBegin/glEnd` for recording.
  - `begin_render_pass(...)`, `end_render_pass()`.
  - `bind_pipeline(Pipeline&)`.
  - `bind_vertex_buffer(Buffer&)`, `bind_index_buffer(Buffer&)`.
  - `bind_descriptor_set(DescriptorSet&)`.
  - `draw(uint32_t vertex_count)`, `draw_indexed(uint32_t index_count)`.
  - `set_viewport(...)`, `set_scissor(...)`.

#### [NEW] `gfxcoopa/command/sync.h`

```cpp
class Fence {
public:
    Fence(Device& device, bool signaled = true);
    ~Fence();

    void wait();
    void reset();
    VkFence handle() const;
};

class Semaphore {
public:
    Semaphore(Device& device);
    ~Semaphore();

    VkSemaphore handle() const;
};
```

---

### Component 7: `gfxcoopa/presentation/` — Window & Renderer

#### [NEW] `gfxcoopa/presentation/window.h`

```cpp
class Window {
public:
    /**
     * @brief Creates a GLFW window configured for Vulkan.
     * @param title Window title string.
     * @param width Window width in pixels.
     * @param height Window height in pixels.
     */
    Window(const std::string& title, uint32_t width, uint32_t height);
    ~Window();

    bool should_close() const;
    void poll_events();
    GLFWwindow* handle() const;
    std::pair<uint32_t, uint32_t> framebuffer_size() const;
};
```

#### [NEW] `gfxcoopa/presentation/renderer.h`

```cpp
class Renderer {
public:
    /**
     * @brief Initializes the full Vulkan rendering pipeline.
     * @param window The GLFW window to render into.
     * @param app_name Application name.
     * @param enable_validation Enable Vulkan validation layers.
     */
    Renderer(Window& window, const std::string& app_name,
             bool enable_validation = true);
    ~Renderer();

    /**
     * @brief Acquires the next swapchain image and begins a frame.
     * @return A command buffer ready for recording, or nullptr on resize.
     */
    VkCommandBuffer begin_frame();

    /**
     * @brief Ends recording, submits the command buffer, and presents.
     */
    void end_frame();

    Device& device();
    Swapchain& swapchain();
    RenderPass& render_pass();
};
```

- Orchestrates the full per-frame acquire → record → submit → present cycle.
- Handles swapchain recreation on `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR`.
- Manages frames-in-flight sync primitives (fences, semaphores).

---

### Component 8: `test.cpp` — Comprehensive test suite

Mirrors [libcoopa/test.cpp](file:///home/coopa/git/libcoopa/test.cpp) exactly — same ANSI color macros, `RUN_TEST`, `ASSERT_TRUE`, `ASSERT_EQ`, and `main()` summary.

#### Planned test functions:

| Test function | What it validates |
|---|---|
| `test_error_check` | `GFX_VK_CHECK` throws on bad result, passes on `VK_SUCCESS` |
| `test_format_helpers` | `format_from_channels()` and `format_byte_size()` round-trip |
| `test_window_creation` | Window opens, `framebuffer_size()` returns positive values |
| `test_instance_creation` | Instance creates with validation, `handle()` is non-null |
| `test_device_selection` | Device picks a GPU, queues are valid |
| `test_surface_creation` | Surface is created from window, capabilities non-empty |
| `test_swapchain_creation` | Swapchain creates, `image_count() > 0`, views valid |
| `test_buffer_vertex` | Create vertex buffer, upload float data, verify size |
| `test_buffer_uniform` | Create uniform buffer, upload mat4, verify size |
| `test_image_creation` | Create 2D image, verify view is non-null |
| `test_shader_loading` | Load SPIR-V shader, stage_info is valid |
| `test_render_pass` | RenderPass creates successfully |
| `test_pipeline_creation` | Full pipeline creates with shaders + vertex layout |
| `test_command_pool` | Allocate command buffers, count matches |
| `test_sync_primitives` | Fence + Semaphore create, fence wait/reset works |
| `test_descriptor_set` | Pool + Layout + Set create, bind_buffer succeeds |
| `test_renderer_frame` | Full begin_frame → record triangle → end_frame cycle |
| `test_swapchain_resize` | Swapchain recreates on simulated resize |

The test will also include two bundled SPIR-V test shaders (passthrough vertex + solid-color fragment) under `assets/shaders/`.

---

### Component 9: Documentation & README

#### [NEW] [README.md](file:///home/coopa/git/gfxcoopa/README.md)

Same structure as [libcoopa's README](file:///home/coopa/git/libcoopa/README.md): module overview with file links, build instructions using `cbuild`/`cplay`, and a quick-start code snippet.

---

## Style & Convention Summary

All headers will follow these patterns observed in libcoopa:

| Convention | Detail |
|---|---|
| **Namespace** | `coopa::gfx::core`, `coopa::gfx::pipeline`, `coopa::gfx::memory`, `coopa::gfx::command`, `coopa::gfx::presentation`, `coopa::gfx::util` |
| **Include guards** | `#ifndef COOPA_GFX_CORE_INSTANCE_H` / `#define` / `#endif` |
| **Vulkan header** | `#include <volk/volk.h>` — never `<vulkan/vulkan.h>` directly |
| **File header** | `/** @file instance.h @brief ... */` |
| **Class docs** | `/** @class Instance @brief ... */` |
| **Method docs** | `/** @brief ... @param ... @return ... */` |
| **Members** | Trailing `/**< description */` for inline member docs |
| **Section comments** | `// --- Section Name ---` |
| **Private suffix** | Underscore suffix: `device_`, `handle_`, `logger_` |
| **Deleted copy** | `ClassName(const ClassName&) = delete;` with `/// @brief Non-copyable.` |
| **Logger** | `coopa::debug::Logger` for internal diagnostics (from libcoopa) |
| **Memory** | VMA (`vma/vk_mem_alloc.h`) for all `VkBuffer`/`VkImage` allocations |
| **C++ standard** | C++20, header-only, no exceptions in hot paths |

---

## Resolved Decisions

| Decision | Resolution |
|---|---|
| **Namespace** | `coopa::gfx::` — aligns with the updated `coopa::` root namespace in libcoopa |
| **Window system** | **GLFW** |
| **Memory allocator** | **VMA** vendored under `includes/vma/vk_mem_alloc.h` |
| **Vulkan loader** | **volk** vendored under `includes/volk/` — zero link-time `libvulkan.so` dependency |

---

## Implementation Phases

### Phase 1 — Scaffold & utilities (files: 6)
1. Create `CMakeLists.txt`, `configuration/root_directory.h.in`, `.coopadocs`, update `.gitignore`.
2. Implement `gfxcoopa/util/error.h`, `gfxcoopa/util/format.h`, `gfxcoopa/util/debug_messenger.h`.
3. Write initial `test.cpp` with test harness macros + `test_error_check` + `test_format_helpers`.

### Phase 2 — Core objects (files: 4)
1. Implement `gfxcoopa/core/instance.h`.
2. Implement `gfxcoopa/presentation/window.h` (needed for surface/device tests).
3. Implement `gfxcoopa/core/surface.h`.
4. Implement `gfxcoopa/core/device.h`.
5. Add tests: `test_window_creation`, `test_instance_creation`, `test_surface_creation`, `test_device_selection`.

### Phase 3 — Swapchain & memory (files: 4)
1. Implement `gfxcoopa/core/swapchain.h`.
2. Implement `gfxcoopa/memory/allocator.h`.
3. Implement `gfxcoopa/memory/buffer.h`.
4. Implement `gfxcoopa/memory/image.h`.
5. Add tests: `test_swapchain_creation`, `test_buffer_vertex`, `test_buffer_uniform`, `test_image_creation`.

### Phase 4 — Pipeline & commands (files: 6)
1. Implement `gfxcoopa/pipeline/shader.h`.
2. Implement `gfxcoopa/pipeline/render_pass.h`.
3. Implement `gfxcoopa/pipeline/pipeline.h`.
4. Implement `gfxcoopa/pipeline/descriptor.h`.
5. Implement `gfxcoopa/command/command_pool.h`, `command_buffer.h`, `sync.h`.
6. Add tests: `test_shader_loading`, `test_render_pass`, `test_pipeline_creation`, `test_command_pool`, `test_sync_primitives`, `test_descriptor_set`.

### Phase 5 — Renderer & integration (files: 2)
1. Implement `gfxcoopa/presentation/renderer.h`.
2. Create `assets/shaders/test.vert.spv` and `assets/shaders/test.frag.spv`.
3. Add tests: `test_renderer_frame`, `test_swapchain_resize`.

### Phase 6 — Polish & documentation (files: 2)
1. Write `README.md`.
2. Run `coopadocs build` and verify all public symbols are documented.
3. Final compilation test with `cbuild` / `cplay`.

---

## Testing and Validation

### Automated Tests
```bash
# Build
cbuild

# Run the test suite
cplay
```

All 18 test functions must pass with green `[  OK  ]` output.

### Documentation Validation
```bash
cd /home/coopa/git/gfxcoopa
coopadocs build
coopadocs show
```

Verify all classes, methods, parameters, and member variables render correctly in the generated HTML.

### Manual Verification
- Confirm `test_renderer_frame` actually displays a triangle briefly (or at least completes a full acquire→submit→present cycle without validation errors).
- Verify swapchain resize test handles `VK_ERROR_OUT_OF_DATE_KHR` gracefully.
