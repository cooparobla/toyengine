# Presentation Submodule (`coopa::gfx::presentation`)

The `coopa::gfx::presentation` submodule provides GLFW windowing wrappers and multi-buffered frame orchestration. `Renderer::draw_frame()` runs the whole acquire -> record -> submit -> present cycle in one call; `begin_frame()` is a deprecated alias for it.

---

## Presentation Frame Loop Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                           FRAME ORCHESTRATION ARCHITECTURE                               │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                                ┌───────────────────────┐
                                │        Window         │  (GLFW Handles & Input)
                                └───────────┬───────────┘
                                            │
                                            ▼
                                ┌───────────────────────┐
                                │       Renderer        │
                                ├───────────────────────┤
                                │ - Framebuffers        │
                                │ - Fences & Semaphores │
                                │ - Command Buffers     │
                                └───────────┬───────────┘
                                            │
       ┌────────────────────────────────────┼────────────────────────────────────┐
       ▼                                    ▼                                    ▼
┌───────────────┐                  ┌─────────────────┐                  ┌────────────────┐
│ Acquire Image │                  │ Record Commands │                  │ Submit/Present │
└───────────────┘                  └─────────────────┘                  └────────────────┘
```

---

## File Breakdown

### [renderer.h](renderer.h)
- **Role**: High-level double/triple-buffered frame orchestration (`acquire` → `record` → `submit` → `present`).
- **Key Classes / Structs**: `Renderer`.
- **Details**: Keeps `MAX_FRAMES_IN_FLIGHT` (2) fences, image-available semaphores and command buffers, plus one framebuffer and render-finished semaphore per swapchain image. `draw_frame(record, clear, on_resize, pre_pass, post_pass)` records `record` inside the swapchain render pass; `pre_pass`/`post_pass` record into the same command buffer before and after it (e.g. an offscreen pass sampled later), so the frame is a single submit. On an out-of-date or suboptimal swapchain it calls the handler from `set_resize_handler()` if one is set (`app::Context` installs one), otherwise rebuilds framebuffers and calls `on_resize`, leaving `Swapchain::recreate()` to the caller; it returns false for a skipped frame.

### [window.h](window.h)
- **Role**: GLFW window creation, event polling, and input handling wrapper.
- **Key Classes / Structs**: `Window`, `Window::IconImage`.
- **Details**: Encapsulates the GLFW window lifecycle (`resizable` and `visible` constructor flags; a hidden window still presents, for automated runs), event polling (`poll_events()`, `wait_events()`, `should_close()`), resize tracking (`was_resized()`/`reset_resized()`, `framebuffer_size()`, `content_scale()`) and `set_icon()`. Keyboard, mouse, scroll, text and cursor events feed a libcoopa `coopa::input::Input` exposed through `input()`; call `new_frame(dt)` before `poll_events()` each frame.

---

## Usage Example

```cpp
#include <gfxcoopa/presentation/window.h>
#include <gfxcoopa/presentation/renderer.h>

coopa::gfx::presentation::Window window("Application", 1280, 720, /*resizable=*/true);
coopa::gfx::presentation::Renderer renderer(device, swapchain, render_pass, cmd_pool);

while (!window.should_close()) {
    window.poll_events();

    renderer.draw_frame([&](coopa::gfx::command::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline);
        cmd.draw(3);
    });
}
device.wait_idle();
```
