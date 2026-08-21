# toyengine/util

| File | Purpose |
|---|---|
| [`screenshot.h`](screenshot.h) | `save_image_png()` — copies a Vulkan color image to a PNG via a staging buffer, extracted from blendy's ~90-line inline exit-time screenshot block in `test.cpp` into a reusable function. Blocking (`vkQueueWaitIdle`); intended for exit-time or `ONESHOT` capture, never a per-frame call. |
