# toyengine/debug

The on-screen stats HUD, and the game-facing API for publishing your own lines into it.

| File | Purpose |
|---|---|
| [`debug_overlay.h`](debug_overlay.h) | `DebugOverlay` — frame times always; in full mode the renderer, scene, physics, navigation and GPU-memory numbers too. `FrameTimeRing` holds the last 240 frames and derives average / min / max and the 1% low for the sparkline. `GameLines` is the game's way in: `toy::debug::watch("name", value)` publishes a labelled value that is replaced each frame, `toy::debug::text(line)` appends a one-frame line. `format_watch()` overloads turn bools, strings and numbers into display text. |

## Three modes

Cycled by **F3** (the `debug_overlay` input action), chosen at startup by `config.yaml`'s
`debug.overlay: off | fps | full`:

| Mode | Shows |
|---|---|
| `off` | Nothing. No overlay scene is ticked or drawn, no profiler runs, and the game API returns immediately — **the frame is byte-identical to one without the overlay**, which is what makes it safe to leave compiled into a render test. |
| `fps` | FPS, frame ms (avg / min / max over the last 240 frames), the 1% low, and a sparkline of the same ring. |
| `full` | Adds CPU phase and GPU pass timings (a live `render::FrameProfile`), draw stats, scene counts, physics, navigation and VMA heap budgets. |

## Conventions

- **Off must cost nothing.** The `off` path is the reason `GameLines::accepting()` exists: a
  `watch()` call from game code returns before formatting its value, so instrumentation can be
  left in shipping code. `k_overlay_compiled` removes it outright in a `TOY_SHIPPING` build
  unless that build sets `TOY_DEBUG_OVERLAY` (`cmake -DTOY_SHIPPING_DEBUG_OVERLAY=ON`).
- **GPU numbers land two frames late.** Timestamps are read back after the frame that wrote them
  retires (see [`../render/gpu_profiler.h`](../render/gpu_profiler.h)); the overlay shows the most
  recent *complete* set, not this frame's.
- **The overlay is an overlay layer, not a pass.** While visible, its scene is one of the
  Engine's `overlay_layers_`, drawn by the normal UI path — so it is subject to the same display
  rect and scaling as any other screen UI.
