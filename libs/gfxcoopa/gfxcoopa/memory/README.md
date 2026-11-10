# Memory Submodule (`coopa::gfx::memory`)

The `coopa::gfx::memory` submodule provides Vulkan Memory Allocator (VMA) RAII integration, GPU buffer management, and image allocation/transitions.

---

## Memory Subsystem Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                            MEMORY SYSTEM MANAGEMENT GRAPH                                │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                                ┌───────────────────────┐
                                │       Allocator       │  (VmaAllocator)
                                └───────────┬───────────┘
                                            │
                    ┌───────────────────────┴───────────────────────┐
                    ▼                                               ▼
        ┌───────────────────────┐                       ┌───────────────────────┐
        │        Buffer         │                       │         Image         │
        ├───────────────────────┤                       ├───────────────────────┤
        │ - VkBuffer            │                       │ - VkImage             │
        │ - VmaAllocation       │                       │ - VkImageView         │
        │ - upload()/download() │                       │ - current_usage()     │
        └───────────────────────┘                       └───────────────────────┘
```

---

## File Breakdown

### [allocator.h](allocator.h)
- **Role**: RAII wrapper around Vulkan Memory Allocator (`VmaAllocator`).
- **Key Classes / Structs**: `Allocator`.
- **Details**: Initializes VMA using device and instance handles, providing low-overhead sub-allocated memory management for GPU buffers and textures.

### [buffer.h](buffer.h)
- **Role**: RAII abstraction for GPU memory buffers (`VkBuffer` + `VmaAllocation`).
- **Key Classes / Structs**: `Buffer`.
- **Details**: Named factories for host-visible, persistently mapped buffers (`vertex()`, `index()`, `uniform()`, `staging()`, `storage()`), plus constructors taking either the sealed `BufferUsage`/`MemoryResidency` vocabulary or raw Vk/VMA flags. `upload()`/`download()` copy through the persistent mapping, or map and unmap for an unmapped buffer.

### [image.h](image.h)
- **Role**: RAII abstraction for GPU images (`VkImage`, `VkImageView`, `VmaAllocation`).
- **Key Classes / Structs**: `Image`.
- **Details**: Owns a 2D `VkImage` + `VmaAllocation` + `VkImageView` and tracks the `TextureUsage` it was last transitioned to (`current_usage()`), which is what lets `command::CommandBuffer::transition()` take only a destination. Constructors accept either the sealed `Format`/`ImageUsage`/`MemoryResidency`/`SampleCount` vocabulary or raw Vk/VMA types.

### [image_upload.h](image_upload.h)
- **Role**: Staged pixel upload into a new 2D image.
- **Key Classes / Structs**: `upload_image_2d()`.
- **Details**: Copies tightly packed pixels into a staging buffer, then uses `CommandPool::submit_once()` to transition the image to `TransferDst`, copy, and transition to `ShaderRead`. Blocks until done, so the returned image is ready to sample.

---

## Usage Example

```cpp
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/image_upload.h>

using namespace coopa::gfx;

// Create VMA Allocator
memory::Allocator allocator(instance, device);

// Host-visible, persistently mapped vertex buffer
VkDeviceSize bytes = sizeof(Vertex) * vertices.size();
memory::Buffer vertex_buffer = memory::Buffer::vertex(device, allocator, bytes);
vertex_buffer.upload(vertices.data(), bytes);

// A custom combination through the sealed vocabulary
memory::Buffer ssbo(device, allocator, bytes,
                    BufferUsage::Storage | BufferUsage::TransferDst, MemoryResidency::GpuOnly);

// Upload RGBA8 pixels into a sampled image (blocks until ready)
std::unique_ptr<memory::Image> tex = memory::upload_image_2d(
    device, allocator, cmd_pool, pixels, width, height, Format::RGBA8_Unorm, 4);
```
