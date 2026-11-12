# Input Module (`coopa::input`)

The `input` module owns the complete keyboard/mouse vocabulary and state
model — key/button enums, discrete events, level state, edges, deltas, held
time, modifiers, and cursor/clipboard control — so that every consumer of
this workspace (gfxcoopa, uicoopa, pixengine, toyengine, blendy) talks to
**one** input API and never names a windowing library's own input calls.

The module lives in libcoopa rather than in a graphics package because none of
it depends on Vulkan, GLFW, or any windowing library: naming `Key::Space` must
not require pulling in a full graphics stack. Input *state* — level, edges,
held time, accumulators — lives here too, in `coopa::input::Input`, rather than
inside whichever class happens to own the OS window, so consumers query one
complete API instead of reimplementing edge detection or cursor-delta tracking
for themselves. A windowing backend only feeds it via `push_*()`.

---

## Architecture

```text
┌──────────────────────────────────────────────────────────────────────────┐
│                         INPUT MODULE CLASS OVERVIEW                      │
└──────────────────────────────────────────────────────────────────────────┘

  keys.h                         input_backend.h
  Key / MouseButton / KeyAction  IInputBackend
  Mods / KeyEvent / CursorMode     set_cursor_shape/mode/position
  CursorShape / key_name()         set/get_clipboard_text
         │                                 ▲
         │ vocabulary                      │ commands, forwarded
         ▼                                 │ if a backend is attached
┌────────────────────────────────────────────────────────────┐
│  Input                                                      │
│    push_key/push_char/push_mouse_button/push_cursor_position│  <-- called
│    push_scroll/push_focus/push_cursor_enter/release_all     │      by the
│    begin_frame(dt)                                          │      backend
│    key_down/key_pressed/key_released/key_held_time           │
│    button_down/button_pressed/button_released                │
│    cursor_position/cursor_delta/scroll_delta                 │  <-- queried
│    chars/key_events/button_events/mods/focused                │      by
│    set_cursor_shape/mode/position, set/get_clipboard_text     │      everyone
└────────────────────────────────────────────────────────────┘
         ▲
         │ reads
  InputMap
    bind(action, Key|MouseButton[, Mods])   is_down/is_pressed/is_released
    bind_axis(name, +/-)  ->  axis(name, input)
    bind_vector(name, +x/-x/+y/-y) -> vector(name, input)
```

`Input` is a **sink and a cache**, not a poller: it has no idea what GLFW,
SDL, or any other backend is. Something else (`gfxcoopa::presentation::Window`
today) owns the actual OS event loop and calls `push_*()` as events arrive,
and implements `IInputBackend` so `Input`'s `set_cursor_mode()` etc. have
somewhere to go. Nothing here includes a windowing library, which is what
lets `coopa::input` live in libcoopa (no Vulkan/GLFW dependency anywhere)
rather than gfxcoopa.

---

## Frame contract

```cpp
input.begin_frame(dt);   // clears this frame's edges/events/deltas
window.poll_events();    // OS calls back into input.push_key()/push_cursor_position()/...
// ... game code reads input.key_down()/key_pressed()/cursor_delta()/... ...
```

`begin_frame()` and the `push_*()` calls are meant to be made by the backend
only (gfxcoopa's `Context::poll()` does this on every consumer's behalf); a
headless test instead calls them directly with no window at all. "Down"
state persists frame to frame; `pressed`/`released` edges and every
accumulator (`chars()`, `key_events()`, `cursor_delta()`, `scroll_delta()`,
...) are scoped to exactly one frame.

Two behaviors worth knowing about:

- **First-frame cursor delta suppression.** The very first `push_cursor_position()`
  ever, and the first one after `set_cursor_mode()` changes mode, reports a
  zero delta rather than jumping. A backend's cursor position is liable to be
  undefined right when `CursorMode::Disabled` is first applied (GLFW's
  virtual-cursor mode does this) — without suppression, a mouse-look camera
  would visibly snap on the very frame capture begins.
- **Focus-loss releases everything.** `push_focus(false)` calls
  `release_all()`. The OS is not guaranteed to deliver a key's release event
  once the window loses focus (e.g. alt-tabbing away while holding W) — without
  this, that key would read as held down indefinitely.

---

## Usage

```cpp
#include <coopa/input/input_map.h>

coopa::input::InputMap input_map;
input_map.bind("quit", coopa::input::Key::Escape);
input_map.bind("save", coopa::input::Key::S, coopa::input::Mods::Control);
input_map.bind_vector("move", Key::D, Key::A, Key::W, Key::S);

// Per frame, given a coopa::input::Input& (gfxcoopa: ctx.input() / window.input()):
if (input_map.is_down("quit", input)) window.set_should_close(true);
glm::vec2 move = input_map.vector("move", input);

// Anything not worth naming as an action can be read directly:
if (input.key_pressed(Key::F1)) toggle_debug_overlay();
glm::vec2 look = input.cursor_delta();
```

A headless test constructs an `Input` with no backend at all:

```cpp
coopa::input::Input input;
input.begin_frame(0.016f);
input.push_key(Key::Space, 0, KeyAction::Press, Mods::None);
ASSERT_TRUE(input.key_pressed(Key::Space));
```

---

## Testing

Unit tests live in `libcoopa`'s top-level `test.cpp`, under `input_test`:
`InputMap` binding/unbind/axis behavior, plus `Input` coverage for
press/release edges clearing on `begin_frame()`, held-time accumulation, a
press+release inside one frame setting both edges, cursor delta including
first-frame suppression and re-arming after `set_cursor_mode()`,
`release_all()` on focus loss, modifier chords, and axis/vector reads.
