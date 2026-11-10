# uicoopa reference

The detailed, per-header guide to uicoopa. Start with the [README](../README.md) for an
overview, build steps and a minimal example. Everything here is also documented in the
headers themselves.

- [Layout](#layout-uicoopalayout) · [Rendering](#rendering-uicooparender) ·
  [Text](#text-uicoopatext) · [Widgets](#widgets-uicoopawidgets) ·
  [Groups](#groups-uicoopagroups) · [Input](#input-uicoopainput)
- [Builder](#builder-uicoopabuilder) · [Reactors](#reactors-uicoopareactors) ·
  [Audio](#audio-uicoopaaudio) · [YAML loading](#yaml-loading-uicoopaui_yamlh) ·
  [Events](#events-coopaeventsignal)
- [Gamepad navigation](#gamepad-navigation-uicoopainputnav_h-navigationh) ·
  [Immediate mode](#immediate-mode-layer-uicoopaimmediate)
- [Demos](#the-demos) · [Building](#building)

Everything under `uicoopa/` is header-only (`namespace coopa::ui`). The one compiled file,
`src/ui_impl.cpp`, holds stb_truetype's implementation and builds as `uicoopa_impl`.
Composition follows a `coopa::scene::SceneObject` tree: every UI node carries a
`RectTransform` plus zero or more of the components below.

## Layout (`uicoopa/layout/`)
- **`rect.h`** — the pure math core: `Rect`, `RectParams`, and `resolve_rect()`. Anchor-based
  resolution modeled directly on Unity's `RectTransform` (`anchor_min`/`anchor_max`, `pivot`,
  `anchored_position`, `size_delta`). Independent of Vulkan and `coopa::scene`, so it's
  exercised directly by headless tests.
- **`rect_transform.h`** — the `RectTransform` component every UI object has exactly one of:
  named anchor presets (`TopLeft`, `StretchAll`, `MiddleCenter`, ...), optional local
  rotation/scale about the pivot, `z_order` (elevates a subtree above later siblings and
  lets it escape an ancestor `Mask` — a popup or a modal has to draw over whatever clipped
  scrim/frame for why that matters), and the resolved `Rect` other components read.
- **`canvas.h`** — `CanvasComponent`, the root driver. Runs an explicit three-pass rebuild
  each frame: bottom-up **measure**, top-down **arrange** (`RectTransform::resolve()` +
  `on_rect_changed()`), then **emit** (draw list population). This explicit pipeline — not
  `SceneObject`'s plain pre-order `update()` walk — is what lets layout groups compute an
  aggregate child size before their own rect is resolved. `rebuild_layout()`/`rebuild_emit()`
  are public and independently callable — safe to use headlessly with no display, which is
  how the headless test suites (`tests/`) exercise resolved layout without a window.
- **`canvas_scaler.h`** — `CanvasScaler`, screen-size-independent scaling (Unity's
  `CanvasScaler` equivalent): a reference resolution plus a width/height blend factor.
  **Screen-space only** — a world canvas ignores it entirely (see below).
- **`layout_element.h`** — `LayoutElement`, explicit min/preferred/flexible size overrides
  for a child inside a layout group, and an `ignore_layout` escape hatch.

### World-space canvases

`CanvasComponent::render_mode` is uicoopa's equivalent of Unity's `Canvas.renderMode`:

```cpp
enum class CanvasRenderMode { ScreenSpaceOverlay, WorldSpace };
enum class CanvasBillboard  { CameraFacing, Transform };
```

A `WorldSpace` canvas hosts the **entire** widget library unchanged — `Image`, `Text`,
every immediate widget, the draw list, text and themes — all of it.
That is not a coincidence to be preserved carefully; it is structural. Every widget already
emits nothing but canvas-space rects into a `DrawList`, and knows nothing about screens,
projections or Vulkan. World space changes exactly two things around them: where the root rect
comes from, and one matrix at the pass level.

```yaml
- type: Canvas
  render_mode: WorldSpace
  billboard: CameraFacing     # or Transform
  world_size: { x: 150.0, y: 38.0 }   # CANVAS PIXELS, not world units
  pixels_per_unit: 110.0
  text_supersample: 2.0               # glyph atlas density; see below
  occlude: false
```

- **`world_size`** is the authored design size every child anchors against — the exact role
  the framebuffer size plays in screen space. **`pixels_per_unit`** alone converts it to world
  units (`world_size / pixels_per_unit`), so it is the single size knob: change it and the
  canvas grows or shrinks with no re-layout. `CanvasScaler` is **ignored**, because its whole
  job is relating canvas pixels to *screen* pixels and a world canvas has no fixed
  relationship to the screen — its on-screen size is whatever perspective makes it.
- **`CameraFacing`** builds the canvas basis from the view matrix's own right/up axes, so the
  quad sits at a constant view-space depth. That has a payoff beyond looks: it projects to a
  screen-**axis-aligned** rectangle, which is what lets `UiWorldPass` turn a `Mask`'s clip rect
  into an exact `vkCmdSetScissor`. **`Transform`** instead honours the owner `SceneObject`'s own
  3D rotation and scale — true Unity WorldSpace behaviour, visible edge-on — via
  `local_right_axis`/`local_up_axis`, which default to a **Z-up** host (canvas right = local
  +X, canvas up = local +Z, so an unrotated canvas faces world −Y). A Y-up host sets
  `local_up_axis = {0,1,0}` and needs no code change. For a rotated `Transform` canvas the
  projected clip rect is no longer axis-aligned, so scissors fall back to the corners' screen
  AABB — a mask clips a little loosely, never incorrectly.
- **`text_supersample`** bakes this canvas's glyph atlases that many times denser than the
  authored `font_size`. A glyph atlas is a bitmap, so it has no resolution to spare: baked at
  `font_size` and then magnified on screen it can only go soft, and solid quads hide this
  because they sample a 1x1 white texel where magnifying a constant is exact. Set it to roughly
  the magnification the canvas is usually seen at,
  `(render_height / (2 * distance * tan(fov/2))) / pixels_per_unit`, and round **down** — mild
  magnification is a soft edge, while minification through a sampler with no mipmaps aliases.
  Layout is unaffected at any value: `Text` divides everything the atlas reports back down by
  the same factor. **Screen-space canvases need nothing here** — their
  magnification is exactly `scale_factor()`, which is folded in automatically; the default
  `1.0` adds no supersampling.
- **`occlude`** hides fragments behind opaque geometry. Off by default: the floating-nameplate
  look, always legible. It is a fragment-shader depth compare and `discard`, **not** a hardware
  depth test, because a world canvas composites into whatever colour target the host already
  has open and that target generally carries no *scene* depth. The host supplies the real scene
  depth as a sampled texture at set 1 — see `ExtraSets` on `UiWorldPass`'s constructor and
  `assets/shaders/ui_world_occlude.glsl`.

Two things the host must do, neither of which uicoopa can do for itself:

```cpp
// Between Scene::update() and Scene::late_update(): world matrices are current only after the
// transform resolve inside update(), and EventSystem::process() runs inside late_update().
canvas->set_default_texture(world_ui_pass.white_view());
canvas->update_world_transform(view);
auto hit = canvas->ray_to_canvas(ray_origin, ray_dir);      // mouse ray -> canvas pixels
// A miss must be parked far outside the canvas -- (0,0) is a real point INSIDE it.
canvas->set_world_input(input, hit ? *hit : glm::vec2(-1.0e6f));
```

`ray_to_canvas()` is what makes a world canvas genuinely interactive: give it a world-space
pointer ray and `Raycaster`/`EventSystem` — and therefore `Button`, `Slider`, `ScrollRect`,
drag-and-drop — work completely unmodified. One ray/plane path serves both billboard modes.

`toyengine`'s `assets/scenes/world_canvas_test/` is the worked example: a cube with a health
bar floating above it, plus a second `Transform`-mode canvas, driven by
`toyengine/render/passes` wiring and `toyengine/scene/health_driver.h`.

## Rendering (`uicoopa/render/`)
- **`ui_vertex.h`** — the single 2D vertex format (`UiVertex`) every draw call uses, plus
  `pack_color()` for the packed RGBA8 vertex color.
- **`draw_list.h`** — `DrawList`, accumulated per frame and batched by `(VkImageView, clip
  Rect)` into indexed draw calls. Carries raw `VkImageView`s rather than resolved descriptor
  sets, so it has no notion of Vulkan descriptors at all.
- **`ui_world_pass.h`** — `UiWorldPass`, the world-space sibling of `UiPass`. Same
  `TexturedQuad2DPass` backbone; differs in exactly three places: a full
  `proj * view * CanvasComponent::model()` push constant instead of a 2D scale/offset (hence
  its own `ui_world.vert`), clip rects that become scissors by **projecting** the rect's
  corners rather than scaling them, and the optional scene-depth occlusion compare. Note
  `begin_frame()` — unlike `UiPass`, it must be called once per frame before the draws,
  because a scene can hold many world canvases and they all append into one streaming buffer
  pair per frame-in-flight.
- **`ui_pass.h`** — `UiPass`, the actual Vulkan pipeline: owns the per-frame streaming
  vertex/index buffers and an image-view-to-descriptor-set cache, and **composites into the
  same render pass an existing 3D pass already opened** rather than clearing and owning its
  own — the render pass it's built against always clears on load, so a second pass would
  erase whatever was drawn before it.

  **Destroying a texture requires telling it so.** Both of `UiPass`'s caches — the
  descriptor-set cache in the underlying `TexturedQuad2DPass` and its own text-atlas set —
  are keyed by `TextureView`, which is the raw `VkImageView` handle value, and a driver
  hands a freed handle straight back to the next view created. Call
  `unregister_texture(view)` before destroying a `Texture`, while no in-flight frame still
  references it, or the *next* texture to land on that handle will silently be drawn
  through the dead one's descriptor — showing the old image, or being sampled as R8
  coverage if the handle used to belong to a glyph atlas.
- **`texture_factory.h`** — builds a device-local Vulkan image from host pixel data
  (`Texture`/`TextureView` themselves come from gfxcoopa).
- **`sprite.h`** — `Sprite`, a named sub-region of a texture, with optional 9-slice border
  insets.
- **`sprite_sheet.h`** / **`sprite_sheet_loader.h`** — `SpriteSheet`, a named multi-sprite
  atlas parsed from a YAML descriptor, with a `TypedAssetLoader` integration and hot-reload.
- **`icon_library.h`** — `IconLibrary`, a process-wide singleton (`IconLibrary::instance()`)
  resolving icon names to sprites from one or more loaded sheets; every builder factory that
  can draw a real icon degrades gracefully to a plain shape when
  nothing is published.

## Text (`uicoopa/text/`)
- **`font_atlas.h`** — `FontAtlas`, an R8 coverage atlas baked from a TrueType font at one
  fixed pixel size via stb_truetype.
- **`font.h`** — `Font`, owning one `FontAtlas` per size actually requested, plus the
  word-wrap layout logic (`layout()`) shared between `Text::emit()`'s per-glyph placement and
  `ContentSizeFitter`'s aggregate-size-only queries.
- **`font_defaults.h`** — `FontDefaults`, the process-wide default-font holder every widget
  factory falls back to when a `UITheme` doesn't specify its own `Font*`.

## The widget layer is gone

`widgets/`, `groups/`, `builder/`, `reactors/`, `binding/`, the gamepad navigation set and
`ui_yaml.h` were removed when game UI moved onto the immediate layer. There is one UI model
now: a screen declares itself once per frame (see **Immediate-mode layer** below, and
`toyengine/ui/README.md` for the engine-side components). What survives here is the substrate
that layer draws through -- the canvas, the draw list and text -- plus the immediate API itself.

## Events: `coopa::event::Signal`
Anything in the engine that publishes a transition — `AssetManager::on_reloaded`,
`UserSettings::on_changed` — is built on [libcoopa](../../libcoopa)'s
`coopa::event::Signal<Args...>` (`libcoopa/coopa/event/signal.h`) — a generic, non-UI-specific
multicast signal/slot type: `connect()` returns a `Connection` token that stays safe to
query/disconnect even after the `Signal` is destroyed, `ScopedConnection` wraps that in
move-only RAII, and `emit()` is safe to re-enter (a slot may connect, disconnect itself, or
even destroy its own `Signal` mid-emit). `uicoopa` doesn't define its own event primitive —
it's a consumer of libcoopa's rather than defining its own.

## Base types
- **`ui_component.h`** — `UIComponent`, the base every widget above derives from. Extends
  `coopa::scene::Component` with the three hooks `CanvasComponent` drives explicitly:
  `measure()`, `on_rect_changed()`, and `emit()`/`on_children_emitted()`.

## Immediate-mode layer (`uicoopa/immediate/`)

For tool UIs whose structure changes with their data (editors, debug panels), `imm.h`
provides an immediate-mode API: menus, popups, modals, trees, property rows, text fields, a
colour picker, splitters, and drag and drop. It draws through the same `DrawList`, `Font`
and `UiPass` as the retained widgets. The UI is re-declared every frame from the model, so
nothing has to be rebuilt or re-wired when the data changes.

```cpp
ctx.begin_frame(draw_list, input, canvas_size);          // imm::Context ctx; input is an imm::FrameInput
if (ctx.begin_menubar({0, 0, w, 24})) {
    if (ctx.begin_menu("File")) {
        if (ctx.menu_item("Save", "Ctrl+S")) save();
        ctx.end_menu();
    }
    ctx.end_menubar();
}
ctx.begin_panel("inspector", rect);
ctx.drag_float("Roughness", &material.roughness, 0.01f, 0.0f, 1.0f);
ctx.end_panel();
ctx.end_frame();
```

Coordinates are canvas pixels with a top-left origin and +Y down. Popups, menus, modals and
tooltips are `DrawList` z-order layers. Hover and clicks resolve against the previous
frame's layer rects, so a widget under an open popup does not react.

- **`imm_canvas.h`** — `ImmediateCanvas`, a `UIComponent` whose `emit()` runs one frame
  through its `on_draw` callback. `build_immediate_canvas(scene)` creates a screen-space
  canvas holding one. Restructure the scene after `late_update()`, not inside the callback.
- **`imm_icons.h`** — a vector icon set drawn from lines, triangles and circles, so it needs
  no image assets and stays sharp at any scale.
- **`imm_file_dialog.h`** — `imm::FileDialog`, a Finder-style Open / Save / Choose Folder
  modal. Keep one, call `draw(ctx)` every frame after the rest of the UI, and `open()` it with
  a mode, title, start folder, extension filter and callback. It has back / forward / up, a
  path bar with live search, a Favorites / Recent / Locations sidebar, a sortable list, a
  Format menu, and full keyboard navigation; Shift+Cmd+N (or New Folder in the list's
  right-click menu) creates a folder and renames it in place. Apps name their own document
  kinds with `kind_name` / `format_group` and add Favorites with `favorites` / `launch_dir`.
- **`imm_theme.h`** — themes, below.

`combo()` / `combo_box()` popups with more than 15 items get a search field that has the
keyboard as soon as the popup opens: typing filters by substring (any case), Up / Down move a
highlight, Enter picks it. An optional `int* hovered` out-parameter reports the item under
the pointer (or keyboard highlight) for live previews. `keyboard_consumed()` tells code that
reads keys directly whether a widget already used the keyboard this frame.

The toyengine editor is built entirely on this layer.

### Themes (`imm_theme.h`)

`imm::Style` (every metric and colour the widgets use) is the look; a theme file sets it.
`imm::load_theme(path)` reads YAML (or its `.caml` twin) into an `imm::Theme`: `name`,
optional `inherits:` (another theme in the same folder to start from), `font:`, `metrics:`,
`colors:` (`"#rrggbb[aa]"`, `[r, g, b, a]` or `{r, g, b, a}`), plus any number of
application sections, such as an editor's `viewport:` colours, read with
`theme.color(section, role, fallback)`. Missing keys keep the inherited value, so a theme
can be a few overrides. `visit_style()` is the single list of themable fields that both
the loader and `theme_to_yaml()` walk; `list_themes(dir)` finds the themes in a folder.
Apply one with `ctx.style = theme.style`. Style's compiled-in values are only a fallback.

Two metrics shape property rows (`property_row()`, which every labelled inspector widget goes
through): `label_ratio` is the fraction of the row given to the label column, and
`label_align` places the label within it, from `0` (flush left, as in a form) to `1` (the
default: right-aligned against the widget, as Blender does).

## Building

### As a CMake subdirectory: `coopa::ui`

```cmake
add_subdirectory(path/to/uicoopa ${CMAKE_CURRENT_BINARY_DIR}/uicoopa-build)
target_link_libraries(myapp PRIVATE coopa::gfx coopa::ui)   # coopa::ui LAST -- see below
add_dependencies(myapp uicoopa_shaders)                      # compiles ui*.vert/frag to .spv
```

`coopa::ui` is an INTERFACE target. It carries the include paths and links `coopa::gfx`
(plus `coopa::sfx` and `UICOOPA_HAS_AUDIO` when audio is on), and a small `uicoopa_impl`
STATIC library that compiles stb_truetype's implementation (`src/ui_impl.cpp`). The demos,
`ctest` registration, the generated `root_directory.h` and the leak gate are behind a
`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` guard, so a consumer gets none of them.

- Link `coopa::ui` **after** `coopa::gfx`. This repo's `includes/` also carries
  `stb_image.h`/`stb_image_write.h`, whose implementations live in `gfxcoopa_impl`.
- `uicoopa_shaders` is not behind the standalone guard, because `UiPass`/`UiWorldPass` take
  `.spv` paths a consumer needs at runtime. The `.spv` files are written next to their
  sources in `assets/shaders/`.
- If `coopa::lib` / `coopa::gfx` / `coopa::sfx` targets already exist, uicoopa reuses them
  instead of adding the sibling directories again.

### Audio option

`UICOOPA_WITH_AUDIO` (ON by default) links `sfxcoopa` and defines `UICOOPA_HAS_AUDIO` when
the sibling `sfxcoopa` checkout exists. If it is missing, configure prints a warning and
builds without audio. Every audio call site is guarded by `#ifdef UICOOPA_HAS_AUDIO`, so both
configurations build. Pass `-DUICOOPA_WITH_AUDIO=OFF` to opt out.
