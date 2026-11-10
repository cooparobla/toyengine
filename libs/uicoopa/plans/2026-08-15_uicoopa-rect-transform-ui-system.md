# uicoopa — Unity-style RectTransform UI System

## Context

`/home/coopa/git/uicoopa` is an empty git repo. The goal is a retained-mode UI library with Unity's
RectTransform model — `anchor_min`/`anchor_max`, `pivot`, `anchored_position`, `size_delta`,
`offset_min`/`offset_max` — built on the two existing sibling libraries:

- **`libcoopa`** (`/home/coopa/git/libcoopa`) supplies the hierarchy: `coopa::scene::Component`,
  `SceneObject`, `Scene`, with DFS `start()` / `update(dt)`, `add_component<T>()` /
  `get_component<T>()`, plus glm 0.9.9.3 and fkYAML.
- **`gfxcoopa`** (`/home/coopa/git/gfxcoopa`) supplies Vulkan: `Pipeline` + `PipelineConfig`,
  `Buffer`, `Image`, `DescriptorSet`, `CommandBuffer`, `Renderer`, `Window`.

Exploration established three hard facts that shape the whole plan:

1. **There is no 2D/sprite/text/font/atlas code anywhere in either library.** `gfxcoopa/engine/data/`
   has only a 3D `Vertex` (`vec3 pos, vec3 normal, vec2 uv, vec4 tangent`). UI geometry, batching,
   text, and texture loading are all net-new.
2. **There is no mouse input at all.** `coopa::gfx::presentation::Window`
   (`gfxcoopa/gfxcoopa/presentation/window.h`) exposes only `is_key_pressed(int)` (level-triggered
   polling) and `handle()`. No cursor position, buttons, scroll, char input, or key events.
3. **`SceneObject` is append-only** — no `remove_component`, no `remove_child`, no reparenting — and
   `SceneLoader::parse_component_` is a hard-coded `if/else` chain over YAML tags with no
   registration hook. Both are blockers for a UI system that builds and tears down widgets at runtime.

So this plan includes small, surgical upstream edits to both libraries (Phase 1), then builds
`uicoopa` as a header-only C++20 library under `namespace coopa::ui`, matching the house style:
sibling-repo `include_directories`, `#include <uicoopa/...>` absolute-from-root includes, Doxygen
comments for coopadocs, and a single `test.cpp` with the `RUN_TEST` macro runner.

**Outcome:** a `Canvas` + `RectTransform` tree that resolves anchored layout, batches into a single
alpha-blended draw appended to blendy's existing swapchain render pass, renders sprites and TrueType
text, and routes mouse events to `Button` and `ScrollRect`.

---

## Key design decisions

**Rect solving is a pure function, isolated from Vulkan and from `SceneObject`.**
`coopa/scene/scene_object.h` transitively includes `mesh_renderer.h` → `gfxcoopa/engine/data/mesh.h`
→ Vulkan and `caml/caml.h`, so anything touching `SceneObject` cannot be unit-tested headlessly. The
core solver therefore lives in `uicoopa/layout/rect.h` as free functions over plain structs:

```cpp
namespace coopa::ui {

/// Axis-aligned rectangle in canvas pixel space. Origin is bottom-left, +Y up (Unity convention).
struct Rect { glm::vec2 min{0.0f}; glm::vec2 max{0.0f};
              glm::vec2 size()   const { return max - min; }
              glm::vec2 center() const { return (min + max) * 0.5f; } };

/// The Unity RectTransform parameter set, independent of any component or scene type.
struct RectParams {
    glm::vec2 anchor_min{0.5f};        ///< Fraction of parent rect; (0,0)=bottom-left, (1,1)=top-right.
    glm::vec2 anchor_max{0.5f};
    glm::vec2 pivot{0.5f};             ///< Fraction of own rect that anchored_position addresses.
    glm::vec2 anchored_position{0.0f}; ///< Pivot offset from the anchor reference point.
    glm::vec2 size_delta{100.0f};      ///< Size relative to the anchor rect (== absolute size when anchors coincide).
};

/// Resolves a child rect against its parent. Pure, allocation-free, headless-testable.
Rect resolve_rect(const Rect& parent, const RectParams& p);

/// Unity's stretch accessors, derived from the same params.
glm::vec2 offset_min(const Rect& parent, const RectParams& p);
glm::vec2 offset_max(const Rect& parent, const RectParams& p);
void set_offsets(const Rect& parent, RectParams& p, glm::vec2 off_min, glm::vec2 off_max);

}  // namespace coopa::ui
```

`resolve_rect` is exactly Unity's rule and is the single source of truth for the whole system:

```
anchor_rect.min = parent.min + parent.size() * anchor_min
anchor_rect.max = parent.min + parent.size() * anchor_max
size            = anchor_rect.size() + size_delta          // stretch when anchor_min != anchor_max
pivot_point     = anchor_rect.min + anchor_rect.size() * pivot + anchored_position
rect.min        = pivot_point - size * pivot
rect.max        = rect.min + size
```

**Layout is driven by `Canvas`, not by `Component::update` ordering.** `SceneObject::update` is
pre-order DFS over components in insertion order, which *happens* to visit parents first — but layout
groups need a bottom-up measure pass before the top-down arrange pass, which that ordering cannot
express. `CanvasComponent::update(dt)` therefore owns an explicit three-phase rebuild over its own
subtree (recursing through the public `SceneObject::children()`):

1. **Measure** (post-order): each `LayoutElement` / layout group reports preferred + min size.
2. **Arrange** (pre-order): `resolve_rect` each node against its parent's resolved rect; layout
   groups overwrite their children's `RectParams` first.
3. **Emit** (pre-order): graphics components append vertices to the `DrawList`.

**`RectTransform` does not extend `coopa::util::Transform`.** That class is TRS-only
(`vec3` position / Euler degrees / scale) and cannot express anchor-driven sizing. `RectTransform`
owns its `RectParams`, its resolved `Rect`, and an optional local rotation/scale about the pivot,
composing its own `glm::mat3` world matrix. `TransformComponent` is left untouched for 3D objects.

**Rendering hooks into the existing swapchain pass — no new render pass.**
`pipeline::RenderPass` hardcodes `loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR`
(`gfxcoopa/gfxcoopa/pipeline/render_pass.h`), so a second pass over the swapchain image would erase
the 3D scene. Instead `UiPass::draw()` is called inside the lambda blendy already passes to
`Renderer::begin_frame` (`blendy/src/blendy/render/pbr_render_pipeline.h:421-426`), right after
`present_pass_->draw(cmd, sw, sh)`. Same render pass, `depth_test=false`, `depth_write=false`,
`cull_mode=NONE`, `blending=true`. Viewport and scissor are already dynamic state on every
gfxcoopa pipeline, so per-batch clip rects cost nothing.

---

## Phase 1 — Upstream enablement edits

Small and surgical; each is independently useful to the existing libraries.

### 1a. `libcoopa` — dynamic hierarchy
**`/home/coopa/git/libcoopa/coopa/scene/scene_object.h`**

- `bool remove_component(Component* comp)` — erase from `components_`, clear `owner`.
- `template<typename T> bool remove_component()` — first match by `dynamic_cast`.
- `std::unique_ptr<SceneObject> detach_child(SceneObject* child)` — release ownership, null the
  child's `parent_`, return it.
- `void set_parent(SceneObject* new_parent)` — detach from the current owner and re-add, keeping the
  `unique_ptr` alive across the move.
- `template<typename T> std::vector<T*> get_components_in_children() const`.

**`/home/coopa/git/libcoopa/coopa/util/transform.h`** — add the missing `#include <algorithm>`
(`remove_child` uses `std::remove` and currently compiles only by accident).

### 1b. `libcoopa` — component parser registry
**`/home/coopa/git/libcoopa/coopa/scene/scene_loader.h`**

Replace the tail of `parse_component_`'s `if/else` chain (the current silent-ignore branch) with a
lookup into a static registry, so `uicoopa` can register its own tags without libcoopa depending on
it:

```cpp
using ComponentParser = std::function<void(const fkyaml::node&, SceneObject&)>;
static void register_component_parser(const std::string& tag, ComponentParser fn);
static std::unordered_map<std::string, ComponentParser>& parsers_();  // function-local static
```

Unknown tags still fall through silently, preserving forward compatibility. The existing built-in
branches are untouched.

### 1c. `gfxcoopa` — input on `Window`
**`/home/coopa/git/gfxcoopa/gfxcoopa/presentation/window.h`**

`Window` already owns `glfwSetWindowUserPointer` (window.h:68), so the callbacks must be installed
*inside* `Window` rather than by `uicoopa` — otherwise the back-pointer used by
`framebuffer_resize_callback` gets clobbered. Register `glfwSetCursorPosCallback`,
`glfwSetMouseButtonCallback`, `glfwSetScrollCallback`, `glfwSetKeyCallback`, `glfwSetCharCallback`
alongside the existing resize callback, and add:

```cpp
glm::dvec2 cursor_position() const;          ///< In window coordinates, +Y down (GLFW convention).
bool  is_mouse_button_pressed(int button) const;
glm::dvec2 scroll_delta() const;             ///< Accumulated since the last new_frame().
const std::vector<unsigned int>& char_input() const;   ///< UTF-32 codepoints this frame.
const std::vector<KeyEvent>&     key_events() const;   ///< {key, scancode, action, mods}
void  new_frame();                           ///< Clears per-frame accumulators. Call before poll_events().
void  set_cursor(CursorShape shape);         ///< Arrow / IBeam / Hand, for hover feedback.
```

Keep `is_key_pressed` as-is. Guard the glm include so `Window` stays usable without it, or use a
plain `std::pair<double,double>` to avoid adding a glm dependency to gfxcoopa's presentation layer.

---

## Phase 2 — Repo scaffolding and the layout core

`/home/coopa/git/uicoopa/`
```
CMakeLists.txt              # test executable only, mirroring libcoopa/gfxcoopa
test.cpp                    # RUN_TEST macro runner
README.md
.coopadocs                  # include: [uicoopa]
includes/stb/stb_truetype.h # vendored, Phase 4
assets/shaders/             # ui.vert/.frag + committed .spv
uicoopa/
  layout/   rect.h  rect_transform.h  canvas.h  canvas_scaler.h  layout_element.h
  render/   ui_vertex.h  draw_list.h  ui_pass.h  texture.h  sprite.h
  text/     font.h  font_atlas.h
  input/    ui_input.h  event_system.h  raycaster.h
  widgets/  graphic.h  image.h  text.h  button.h  mask.h
  groups/   layout_group.h  grid_layout_group.h  content_size_fitter.h  scroll_rect.h
  ui_component.h  ui_yaml.h
```

`CMakeLists.txt` follows `blendy/CMakeLists.txt` exactly — `CMAKE_CXX_STANDARD 20`, sibling paths via
`${ROOT_DIR_PARENT}/{libcoopa,gfxcoopa,caml}`, `include_directories` for both the repo root and
`includes/`, and `-DVOLK_IMPLEMENTATION -DVMA_IMPLEMENTATION -DGLM_FORCE_DEPTH_ZERO_TO_ONE`,
linking `Vulkan::Vulkan glfw dl`.

**Files:**
- **`layout/rect.h`** — `Rect`, `RectParams`, `resolve_rect`, offset accessors, `contains(Rect, vec2)`,
  `intersect(Rect, Rect)`. Zero dependencies beyond glm. This is the headless-testable core.
- **`ui_component.h`** — `class UIComponent : public coopa::scene::Component`, adding the hooks
  `Component` lacks: `virtual void on_rect_changed(const Rect&)`, `virtual void emit(DrawList&)`,
  `virtual bool wants_raycast() const`.
- **`layout/rect_transform.h`** — `RectTransform : UIComponent`. Holds `RectParams params;`, the
  resolved `Rect rect_`, `float local_rotation_` and `glm::vec2 local_scale_` about the pivot, and a
  cached `glm::mat3 world_matrix_` with a dirty flag mirroring `coopa::util::Transform`'s push-down
  invalidation. Accessors named for Unity parity: `set_anchor_min/max`, `set_pivot`,
  `set_anchored_position`, `set_size_delta`, `set_offset_min/max`, `rect()`, `world_corners()`.
  Convenience presets: `anchor_preset(AnchorPreset)` for TopLeft/Center/StretchAll/StretchBottom/etc.
- **`layout/canvas_scaler.h`** — `ScaleMode::{ConstantPixelSize, ScaleWithScreenSize, ConstantPhysicalSize}`,
  `reference_resolution` (default 1920×1080), `match_width_or_height` in [0,1]; returns the scale
  factor via Unity's log-space blend.
- **`layout/canvas.h`** — `CanvasComponent : UIComponent`. Owns the root `Rect` (screen size ÷ scale
  factor), the `CanvasScaler`, sort order, and the rebuild driver:
  `void rebuild(uint32_t screen_w, uint32_t screen_h)` running measure → arrange → emit, plus
  `DrawList& draw_list()`. `update(dt)` calls `rebuild` only when the screen size or a dirty flag
  changed.
- **`layout/layout_element.h`** — `min_size`, `preferred_size`, `flexible_size`, `ignore_layout`.

**Tests (`test.cpp`, headless — no Vulkan):** `resolve_rect` for the nine anchor presets; stretch
anchors with non-zero `size_delta`; nested three-level rects; pivot-relative positioning;
`set_offsets` round-tripping through `offset_min`/`offset_max`; `CanvasScaler` at 1080p/4K/ultrawide
against hand-computed Unity values.

---

## Phase 3 — Rendering

- **`render/ui_vertex.h`**
  ```cpp
  struct UiVertex {
      glm::vec2 position;  ///< Canvas pixels, origin bottom-left.
      glm::vec2 uv;
      uint32_t  color;     ///< Packed RGBA8, VK_FORMAT_R8G8B8A8_UNORM.
  };
  static VkVertexInputBindingDescription binding_description();
  static std::vector<VkVertexInputAttributeDescription> attribute_descriptions();  // R32G32_SFLOAT, R32G32_SFLOAT, R8G8B8A8_UNORM
  ```
- **`render/draw_list.h`** — `DrawList` accumulates `std::vector<UiVertex>` + `std::vector<uint32_t>`
  and a `std::vector<DrawBatch>{ first_index, index_count, VkDescriptorSet texture, VkRect2D clip }`.
  Helpers `add_quad(Rect, Rect uv, uint32_t color)`, `add_nine_slice(Rect, Sprite, uint32_t color)`,
  `push_clip(Rect)` / `pop_clip()`. Batches break on texture or clip-rect change; consecutive quads
  sharing both merge into one `vkCmdDrawIndexed`.
- **`render/texture.h`** — `Texture` wrapping `memory::Image` + a staging upload. Follow the proven
  recipe in `gfxcoopa/gfxcoopa/engine/util/smaa_textures.h:61-148`: staging `Buffer` →
  `transition_layout(UNDEFINED → TRANSFER_DST)` → `vkCmdCopyBufferToImage` →
  `transition_layout(TRANSFER_DST → SHADER_READ_ONLY_OPTIMAL)` → submit via
  `CommandPool::begin_single_use` / `end_single_use`. Vendor `stb_image.h` alongside stb_truetype for
  PNG decoding. A 1×1 white texture is created as the default so untextured quads share the sprite
  pipeline.
- **`render/sprite.h`** — `Texture*`, UV `Rect`, `pixels_per_unit`, and a `border` vec4 for 9-slice.
- **`render/ui_pass.h`** — `UiPass`, modeled directly on
  `gfxcoopa/gfxcoopa/engine/passes/present_pass.h`. Constructor takes
  `(Device&, Allocator&, pipeline::RenderPass& swapchain_pass, vert_spv, frag_spv)`. Builds the
  blended pipeline (`cull_mode=NONE, depth_test=false, depth_write=false, blending=true`), a
  `VK_SHADER_STAGE_VERTEX_BIT` push constant `{ vec2 inv_screen; }`, and **two** vertex/index buffer
  pairs indexed by `renderer.current_frame()` — `MAX_FRAMES_IN_FLIGHT == 2`, so single-buffering
  would corrupt geometry the GPU is still reading. Buffers come from `Buffer::vertex/index`, which
  are host-visible and persistently mapped, so a per-frame `upload()` is the right streaming path.
  `draw(cmd, frame_index, w, h, const DrawList&)` uploads, binds, then loops batches issuing
  `cmd.set_scissor(...)` + `cmd.draw_indexed(index_count, first_index, 0, 1)`
  (note gfxcoopa's unusual argument order — instance count is *last*).
- **Descriptor sets per texture.** `DescriptorSet::bind_image` calls `vkUpdateDescriptorSets`
  immediately, so sets must not be rebound mid-frame. `UiPass` keeps a `unordered_map<VkImageView,
  DescriptorSet>` cache populated at texture-registration time, never during recording.
- **`assets/shaders/ui.vert` / `ui.frag`** — vert converts canvas pixels to NDC via the push constant
  and flips Y (canvas is +Y up, Vulkan NDC is +Y down); frag samples the atlas and multiplies by the
  vertex color. `ui.frag` branches on a `is_text` flag packed into the vertex color's unused bits or a
  second push constant, sampling `.r` as coverage for R8 glyph atlases. Compile with
  `glslc assets/shaders/ui.vert -o assets/shaders/ui.vert.spv` and **commit the `.spv`** — that is the
  convention in both sibling repos, and there is no CMake shader step.
- **`widgets/graphic.h` / `image.h`** — `Graphic : UIComponent` with `color` and `raycast_target`;
  `Image : Graphic` with `Sprite* sprite` and `ImageType::{Simple, Sliced, Filled}`, emitting through
  `DrawList`.

**Validation:** a `uicoopa` demo target rendering a full-screen background plus four corner-anchored
100×100 quads; resize the window and confirm the corners track and the center panel stretches.

---

## Phase 4 — Text

- **`includes/stb/stb_truetype.h`** vendored (single header, public domain, no build changes).
- **`text/font_atlas.h`** — bakes a codepoint range into an `R8_UNORM` atlas using
  `stbtt_PackBegin` / `stbtt_PackFontRange`, uploads via `render/texture.h`, and exposes
  `const GlyphInfo* glyph(uint32_t codepoint)` (`uv_rect`, `bearing`, `advance`, `size`) plus
  `ascent`, `descent`, `line_gap`. Sampler: `Sampler::linear(device)` — `VK_FILTER_LINEAR` with
  `CLAMP_TO_EDGE`, which `engine/util/sampler.h` already provides.
- **`text/font.h`** — loads a `.ttf`, owns per-size atlases, `measure(text, size, wrap_width)`.
- **`widgets/text.h`** — `Text : Graphic` with `font`, `font_size`, `HorizontalAlign`,
  `VerticalAlign`, `line_spacing`, `TextOverflow::{Overflow, Wrap, Truncate}`. Emits one quad per
  glyph into the atlas batch, laying out lines within the resolved `Rect`.

Note gfxcoopa's blend state is hardcoded straight (non-premultiplied) alpha
(`pipeline.h:279-289`). That is correct for the single-layer UI here; if text AA ever needs
premultiplied compositing, that requires adding a blend-mode field to `PipelineConfig`.

**Validation:** render a paragraph at several sizes with each alignment, wrapping inside a stretched
rect; verify no glyph clipping and correct baseline spacing.

---

## Phase 5 — Interaction

- **`input/ui_input.h`** — `UiInput` reads the Phase 1c `Window` API each frame and converts window
  coordinates (+Y down) to canvas space (+Y up, divided by the scale factor). Adds edge detection —
  `pressed_this_frame`, `released_this_frame`, `delta` — since GLFW polling is level-triggered.
- **`input/raycaster.h`** — walks the canvas tree back-to-front, testing the cursor against each
  `Graphic` with `raycast_target == true` using the inverse of its `glm::mat3` world matrix, honoring
  active `Mask` clip rects. Returns the topmost hit.
- **`input/event_system.h`** — `EventSystem` owns hover/press/focus state and dispatches
  `on_pointer_enter/exit/down/up/click/drag/scroll` to an `IPointerHandler` interface implemented by
  widgets. Tracks the press target so a click only fires when press and release hit the same element.
- **`widgets/button.h`** — `Button : UIComponent, IPointerHandler` with `interactable`, a
  `ColorTransition { normal, highlighted, pressed, disabled, fade_duration }` tinting its target
  `Graphic`, and `std::function<void()> on_click`.

**Validation:** three overlapping buttons — confirm only the topmost receives the click, hover tint
transitions, click fires on press+release over the same button and not on press-then-drag-away, and
`interactable = false` suppresses everything.

---

## Phase 6 — Layout groups, masking, scrolling

- **`groups/layout_group.h`** — `HorizontalLayoutGroup` / `VerticalLayoutGroup` sharing a base with
  `padding`, `spacing`, `child_alignment`, `child_force_expand_{width,height}`,
  `child_control_{width,height}`. Distributes along the axis using each child's `LayoutElement`
  min/preferred/flexible sizes, in the measure→arrange order Phase 2's `Canvas::rebuild` provides.
- **`groups/grid_layout_group.h`** — `cell_size`, `spacing`, `start_corner`, `start_axis`,
  `constraint::{Flexible, FixedColumnCount, FixedRowCount}`.
- **`groups/content_size_fitter.h`** — drives `size_delta` from measured content along either axis.
- **`widgets/mask.h`** — pushes the owner's resolved `Rect` onto the `DrawList` clip stack; realized
  as a `vkCmdSetScissor` batch break, which is free given the always-dynamic scissor state.
- **`groups/scroll_rect.h`** — viewport + content rects, `horizontal`/`vertical` toggles,
  `MovementType::{Unrestricted, Elastic, Clamped}`, inertia with deceleration, mouse-wheel and drag
  scrolling via `IPointerHandler`, clamping content within the viewport.

**Validation:** a vertical layout group of 50 items inside a scroll rect — verify clipping at the
viewport edge, wheel and drag scrolling, elastic overscroll snap-back, and correct behavior after a
window resize.

---

## Phase 7 — Serialization and blendy integration

- **`ui_yaml.h`** — registers `!Canvas`, `!RectTransform`, `!Image`, `!Text`, `!Button`,
  `!HorizontalLayoutGroup`, `!VerticalLayoutGroup`, `!ScrollRect` with the Phase 1b
  `SceneLoader::register_component_parser` registry, via a single
  `coopa::ui::register_ui_components()` call. Schema mirrors the existing component style:
  ```yaml
  - !RectTransform
    anchor_min: {x: 0.0, y: 1.0}
    anchor_max: {x: 0.0, y: 1.0}
    pivot:      {x: 0.0, y: 1.0}
    anchored_position: {x: 20.0, y: -20.0}
    size_delta:        {x: 200.0, y: 60.0}
  - !Image
    sprite: ui/panel.png
    color:  {r: 1.0, g: 1.0, b: 1.0, a: 0.85}
    type:   Sliced
    border: {l: 8, r: 8, t: 8, b: 8}
  ```
  There is no writer anywhere in libcoopa, so this is load-only, consistent with the rest of the
  scene system.
- **blendy integration** (`/home/coopa/git/blendy/`):
  - `CMakeLists.txt` — add `include_directories(${ROOT_DIR_PARENT}/uicoopa)` and
    `${ROOT_DIR_PARENT}/uicoopa/includes/`.
  - `src/blendy/render/pbr_render_pipeline.h` — construct `UiPass` alongside `present_pass_`, and add
    one line inside the existing `begin_frame` lambda at **line 421-426**:
    ```cpp
    return renderer.begin_frame(
        [&](coopa::gfx::command::CommandBuffer& cmd) {
            present_pass_->draw(cmd, sw, sh);
            if (ui_pass_) ui_pass_->draw(cmd, renderer.current_frame(), sw, sh, ui_draw_list_);
        }
    );
    ```
  - `test.cpp` — after `scene_mgr.update(dt)`, call `window.new_frame()`, `ui_input.update(window)`,
    `event_system.process(ui_input, canvas)`, and `canvas.rebuild(sw, sh)`.
- **Demo scene** — a `uicoopa` HUD overlay on blendy's existing cube scene: corner-anchored FPS text,
  a stretched bottom bar, and a settings panel with buttons and a scrolling list.

---

## Testing and validation

### Automated (`/home/coopa/git/uicoopa/test.cpp`)

Follow the house pattern from `/home/coopa/git/libcoopa/test.cpp` — the `RUN_TEST(fn)` macro with
`<cassert>`, ANSI-colored output, and `g_tests_run` / failure counters. Register in `CMakeLists.txt`
with `enable_testing()` + `add_test(NAME uicoopa_tests COMMAND uicoopa)`.

The Phase 2 layout core is pure glm math with no Vulkan and no `SceneObject`, so the bulk of the
suite runs headless in CI:

| Test | Covers |
|---|---|
| `test_resolve_rect_anchor_presets` | All nine corner/edge/center presets against hand-computed values |
| `test_resolve_rect_stretch` | `anchor_min != anchor_max` with non-zero `size_delta`, all four edges |
| `test_resolve_rect_nested` | Three-level nesting; child rect correct in canvas space |
| `test_rect_offsets_roundtrip` | `set_offsets` → `offset_min`/`offset_max` is identity |
| `test_pivot_positioning` | Pivot at (0,0), (0.5,0.5), (1,1) with identical `anchored_position` |
| `test_canvas_scaler` | Each `ScaleMode` at 1080p / 4K / 2560×1080, `match` at 0, 0.5, 1 |
| `test_layout_group_distribution` | Flexible/preferred/min size distribution, padding, spacing |
| `test_grid_layout_constraints` | Fixed column and fixed row counts, all start corners |
| `test_draw_list_batching` | Same texture+clip merges; texture or clip change breaks the batch |
| `test_nine_slice_geometry` | 9 quads, 16 verts, borders preserved under non-uniform scale |
| `test_raycast_z_order` | Overlapping graphics → topmost hit; `raycast_target=false` skipped |
| `test_raycast_masked` | Hits outside an active `Mask` rect return no target |
| `test_text_measure_wrap` | Line breaking at `wrap_width`, advance accumulation, trailing spaces |

Run with:
```bash
cd /home/coopa/git/uicoopa && cbuild && ./build/uicoopa
# fallback: cmake -B build && cmake --build build && ./build/uicoopa
```

Also re-run the existing suites after the Phase 1 edits, since both were modified:
```bash
cd /home/coopa/git/libcoopa && cbuild && ./build/libcoopa
cd /home/coopa/git/gfxcoopa && cbuild && ./build/gfxcoopa
```

### Manual / visual

Per the recorded workflow for this project, build with `cbuild --vulkan` and run with a bounded frame
count so the app exits cleanly and writes its output image — never kill the process, since it saves
on exit:

```bash
cd /home/coopa/git/blendy && cbuild --vulkan && MAX_FRAMES=120 cplay
```

Then inspect `output/output-runtime.png`. Checklist:

1. **Anchoring** — resize the window; corner elements stay pinned to their corners, stretched
   elements grow with the viewport, and the center panel stays centered.
2. **Compositing** — UI draws on top of the 3D scene with correct alpha; the 3D scene is *not*
   cleared (this is the specific failure mode if the UI ever ends up in its own render pass).
3. **DPI/scaling** — `ScaleWithScreenSize` against a 1920×1080 reference produces proportionally
   identical layout at 1280×720 and 3840×2160.
4. **Text** — glyphs crisp at multiple sizes, no atlas bleeding at quad edges, correct baselines.
5. **Interaction** — hover tint, click fires on press+release over the same widget, drag-away
   cancels, and clicks do not leak to widgets behind a panel.
6. **Clipping** — scroll rect contents clip exactly at the viewport edge with no overdraw.
7. **Validation layers** — run with `VK_LAYER_KHRONOS_validation` enabled and confirm zero errors,
   particularly around descriptor set updates (they must never occur mid-frame) and buffer
   write-after-read hazards on the double-buffered UI geometry.
8. **Stability** — 500+ frames with no growth in `DrawList` capacity or descriptor allocations.
