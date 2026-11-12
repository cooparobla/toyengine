# uicoopa

**A C++20 immediate-mode UI library: tool panels and game interfaces, rendered through Vulkan.**

uicoopa is header-only. A UI is declared every frame from your own data -- `if (ctx.button("Save"))
save();` -- rather than built as a tree of widget objects that has to be kept in step with the
model. The same layer draws the [toyengine](https://github.com/cooparobla/toyengine) editor,
its hub, and the games built on it.

It composites into a render pass the host has already opened, so it layers over an existing 3D
frame. A canvas can be screen-space or a quad placed in the 3D world.

## Features

### The immediate layer (`immediate/`)
- **`imm::Context`.** Panels, menus, popups, modals, trees, property rows, drag fields, text
  input, a colour picker, splitters, tab bars, drag and drop -- re-declared every frame.
- **Game composites.** `stat_bar` with a damage trail, `slot_grid`, `hotbar`, `cooldown`,
  `message_log`, `prompt_bar`, `panel_frame`, `menu_list`, `drag_handle`, `floating_text`,
  `nameplate`.
- **Anchored placement.** `anchor()` / `stretch()` pin a panel to a corner or an edge, so an
  interface keeps its layout on any resolution.
- **Themes.** `imm::Theme` -- YAML with `inherits:`, hex or float colours, a per-theme font, and
  any number of application colour sections. One field list (`visit_style()`) is walked by the
  loader, the serializer and any editor, so they cannot drift.
- **Built-in vector icons**, a Finder-style `imm::FileDialog`, and a UI sound hook fired from
  the one state machine every widget passes through.

### The canvas host (`layout/`, `render/`, `text/`)
- **`CanvasComponent`.** Screen-space, or world-space with `CameraFacing` / `Transform`
  billboarding, depth occlusion and supersampled glyphs. `ray_to_canvas()` turns a pointer ray
  into a point in canvas pixels, so a world panel is clickable.
- **`DrawList` / `UiPass` / `UiWorldPass`.** One batched vertex stream, broken only when the
  texture, clip rect or z-order changes.
- **Fonts.** stb_truetype atlases, cached per (face, size), with a registry so a host can mark
  glyph atlases for the text pipeline variant.

## Dependencies

uicoopa expects its sibling libraries to be checked out next to it, in the same parent
folder. `CMakeLists.txt` pulls them in as follows:

| Library | How | Used for |
|---|---|---|
| [gfxcoopa](../gfxcoopa) | required, `add_subdirectory` | Vulkan rendering: `UiPass`/`UiWorldPass` build on its `TexturedQuad2DPass`, and textures and font atlases are its images. Direct includes are limited to `uicoopa/render/` and `uicoopa/text/`, but widgets reach it through `DrawList`. The demos also use its window and app context. |
| [libcoopa](../libcoopa) | required, brought in by gfxcoopa | The scene graph every widget lives in, signals and event bus, input, YAML, assets, and the `item`/`stat` models the HUD widgets bind to. |
| [sfxcoopa](../sfxcoopa) | optional, `UICOOPA_WITH_AUDIO` (ON) | UI sound in `uicoopa/audio/` only. If the folder is missing, uicoopa builds without audio and prints a warning. |
| [caml](../caml) | include path only | `CMakeLists.txt` adds its `includes/` (fkYAML) to the include path. No uicoopa header includes a caml header directly. |

You also need the Vulkan SDK (with `glslc`), GLFW, CMake 3.20+ (gfxcoopa's shader rules need
it) and a C++20 compiler. stb_truetype is vendored in `includes/`.

## Getting started

### 1. Clone

Clone uicoopa and its siblings into one folder. gfxcoopa has a nested submodule, so clone it
recursively:

```bash
mkdir coopa && cd coopa
git clone git@github.com:cooparobla/uicoopa.git
git clone --recurse-submodules git@github.com:cooparobla/gfxcoopa.git
git clone git@github.com:cooparobla/libcoopa.git
git clone git@github.com:cooparobla/sfxcoopa.git   # optional, for UI sound
git clone git@github.com:cooparobla/caml.git
```

### 2. Build

```bash
cd uicoopa
cmake -B build && cmake --build build -j
```

### 3. Run the demos

```bash
ctest --test-dir build -j8          # every headless suite
./build/uicoopa --list              # every test, as <suite> <name>
```

Esc quits (F10 in the gamepad demo). Each demo writes a screenshot of its last frame to
`output/<name>.png`. `MAX_FRAMES=<n>`, `ONESHOT=1`, `THEME=light` and `UI_AUDIO=0` help with
scripted runs; the [reference](docs/reference.md#running-a-demo) lists every variable.

### 4. Draw a UI

```cpp
#include <uicoopa/immediate/imm_canvas.h>

auto* ui = coopa::ui::build_immediate_canvas(scene, "UI");
ui->context().text.set_font(&font);
ui->on_draw = [&](coopa::ui::imm::Context& ctx) {
    const auto b = ctx.anchor(coopa::ui::imm::Anchor::TopLeft, {20, 20}, {260, 28});
    ctx.stat_bar("hp", b, "Health", player.health, player.max_health);
    if (ctx.button("Heal")) player.health = player.max_health;
};
```

The canvas emits during `Scene::late_update()`; the host hands its `DrawList` to `UiPass`
inside its own frame loop. For a plate in the 3D world use
`build_world_immediate_canvas(scene, parent, ...)` instead.


## Use it in your project

```cmake
add_subdirectory(path/to/uicoopa ${CMAKE_CURRENT_BINARY_DIR}/uicoopa-build)
target_link_libraries(myapp PRIVATE coopa::gfx coopa::ui)   # coopa::ui after coopa::gfx
add_dependencies(myapp uicoopa_shaders)                      # compiles the UI shaders
```

`coopa::ui` carries the include paths, links gfxcoopa (and sfxcoopa when audio is on), and
adds a small static library for stb_truetype. The demos, tests and leak check are only
built when uicoopa is the top-level project.

## Testing

```bash
ctest --test-dir build -j8               # every suite in parallel, plus the no-raw-Vulkan check
ctest --test-dir build -R uicoopa_modal  # one suite
./build/uicoopa --list                   # every test, as <suite> <name>
./build/uicoopa --suite slider           # one suite directly; -v prints every check
```

`tests/` holds one `<suite>_test.cpp` per system (layout math, text wrapping, raycasting,
YAML parsing, each widget, the builder, dialogs and the modal stack, gamepad navigation, the
HUD bindings, ...) on the shared coopa test framework (`coopa/testing/test.h` in libcoopa),
with shared fixtures in `tests/support/`. All of it builds into the one `uicoopa` executable;
ctest runs each suite as its own `uicoopa_<suite>` entry. A new `*_test.cpp` is picked up on the
next configure. The suites need no window or GPU, and write only to per-test scratch
directories under the system temp dir. `GFX_LEAK_CHECK` (ON by default) also fails the build
if library code names a raw Vulkan or GLFW symbol.

The windowed programs in `demos/` are demos, not tests: they open a visible window and are
never registered with ctest.

## Project layout

```
uicoopa/
├── immediate/   imm::Context, the game composites, icons, themes, FileDialog, ImmediateCanvas
├── layout/      CanvasComponent, CanvasScaler, Rect/RectParams, RectTransform
├── render/      DrawList, UiPass, UiWorldPass, sprites, sprite sheets, the icon library
├── text/        Font, FontAtlas (stb_truetype), the text-atlas registry
├── input/       UiInput: window pixels -> canvas pixels, for screen and world canvases
├── audio/       optional UI sound (sfxcoopa)
└── ui_resources.h  the process-wide font cache and text-atlas marking
assets/          shaders, fonts, icon sheets, sounds
src/             ui_impl.cpp: the one compiled file (stb_truetype)
tests/           headless suites, one <suite>_test.cpp per system
```

## Notes

- **Shaders build in place.** `uicoopa_shaders` writes `.spv` and `.spv.d` files next to the
  sources in `assets/shaders/`, and the `.spv` files are committed. A build can therefore
  touch tracked depfiles; `git checkout -- '*.spv.d'` restores them.
- **Destroying a texture.** Call `UiPass::unregister_texture(view)` before destroying a
  texture the UI has drawn. Its caches are keyed by image-view handle, and drivers reuse
  freed handles.
- **`Scene::start()` timing.** Dialogs and tab views apply their initial visibility in
  `start()`. Build the whole UI first, then start the scene once. UI built later needs its
  own `start()` call.
- **Windows.** The demos open a visible window. gfxcoopa's `ContextConfig::visible = false`
  gives a never-mapped window for automated captures.

## Documentation

- [docs/reference.md](docs/reference.md): the per-header guide. It covers world-space
  canvases, every widget, the builder (splits, dialogs, tabs, menus, file dialogs, HUD),
  themes and the cursor, reactors, audio, YAML and animatable properties, gamepad
  navigation, the immediate-mode layer, every demo and environment variable, and build
  details.
- The headers are documented in Doxygen style. With `coopadocs` installed, `coopadocs build`
  generates the HTML API reference.
