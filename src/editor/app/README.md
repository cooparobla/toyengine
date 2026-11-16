# editor/app

`EditorApp` — the editor application: menus, tabs, panels, viewport tools, everything above the
embedded `Engine`.

| File | Purpose |
|---|---|
| [`editor_app.h`](editor_app.h) / `.cpp` | The app: document lifecycle, open / save / package, play / stop, undo / redo, picking, the frame hooks. |
| [`project.h`](project.h) | A project on disk: its `.toy`, asset roots, `config.yaml`, and creating a new one. |
| [`scene_sync.h`](scene_sync.h) | Keeps the live engine scene in step with the edited document. |
| [`asset_documents.h`](asset_documents.h) | The open-asset set (one at a time per type) and their dirty state. |
| [`editor_theme.h`](editor_theme.h) | **Editor chrome colours and metrics** — where to change how the editor looks. |
| [`sculpt_preview.h`](sculpt_preview.h), [`paint_preview.h`](paint_preview.h) | Live previews for the mesh modes. |
| [`trackpad.h`](trackpad.h) | Trackpad gesture interpretation. |
| [`thumbnail_cache.h`](thumbnail_cache.h) | Small GPU copies of image assets, so the UI can show a texture instead of its file name. Read its file doc before touching eviction. |
| [`ui/`](ui/) | One file per panel. |

**A component's inspector is not a panel.** Which rows a component shows, in which collapsible
section and under what conditions, is data in
[`editor/schema/component_sections.cpp`](../schema/component_sections.cpp); a component that
needs more than rows registers a drawer with `register_inspector_section()` and puts it in
[`editor/schema/sections/`](../schema/sections/), where it is unit-testable with `ImmHarness`.
`ParticlePanel` and `TileTerrainPanel` still live here because they reach into the running
editor, but they are reached through that registry, not by name from `properties.cpp`.

## Frame flow

`EditorApp` runs inside `Engine::tick()`'s hooks:

| Hook | What happens |
|---|---|
| `pre_scene_update` | — (viewport camera/gizmo input is read inside the UI callback instead) |
| `late_update` | the overlay scene's `ImmediateCanvas` runs `draw_()`: **all** UI and document edits. Anything that restructures the engine's scenes is queued in `deferred_`. |
| `post_late_update` | `deferred_` runs: scene rebuilds, play/stop, tab scene switches |
| `pre_render` | editor camera made main, display region set, grid lines pushed |

Restructuring the scene from inside the UI callback is what `deferred_` exists to prevent —
the canvas is mid-traversal.

## Panels

Each panel is a `.cpp` in [`ui/`](ui/).

### The shape every panel follows

One class per panel, owning its own state, reaching the editor only through
[`EditorContext`](editor_context.h). `EditorApp` holds it behind a *forward declaration*, so
the panel's header never reaches `editor_app.h`:

```cpp
// ui/console_panel.h -- forward-declares EditorContext, includes no editor header
class EditorContext;
class ConsolePanel {
public:
    explicit ConsolePanel(EditorContext& ed);
    void draw(imm::Context&, const imm::Box& hb, const imm::Box& body);
private:
    EditorContext& ed_;
    bool show_[3] = {true, true, true};   // panel-private state lives HERE
};
```

`editor_app.h` then carries only `class ConsolePanel;` and
`std::unique_ptr<ConsolePanel> console_;`.

**Why it matters, measured on this tree:** touching `ui/console_panel.h` rebuilds **3**
translation units. Touching `ui/properties.h` rebuilds **27**.

[`ui/console_panel.h`](ui/console_panel.h) is the reference implementation, and all 27 panels
follow it.

The directory's one `.inl`, [`ui/layout.inl`](ui/layout.inl), is **not** a panel — it is
`EditorApp`'s own `draw_()` dispatch plus the chrome helpers (`area_`, `area_header_`,
`icon_for_component_`, `object_icon_`) that many panels call, so it belongs to `EditorApp`.

### Supporting pieces

| File | Role |
|---|---|
| [`editor_panel.h`](editor_panel.h) | `EditorPanel` — the base: holds `EditorApp& app_`, nothing else. |
| [`editor_types.h`](editor_types.h) | The enums and aliases a panel's *declarations* need (`AssetType`, `PropTab`, `ObjectId`, `Node`, `EditResult`, and the structs more than one panel shares). Keeping these out of `editor_app.h` is what lets a panel header avoid it. |
| [`editor_context.h`](editor_context.h) | The narrow accessor surface (`engine()`, `log()`); `ConsolePanel` uses it. Prefer it where it already covers the need. |

Each panel class is a `friend` of `EditorApp` and vice versa, so panel-private state lives in
the panel without widening the editor's public API — nothing outside the editor can reach it.
`EditorApp` keeps thin out-of-line forwarders for the API its callers and tests use
(`build_refresh()`, `set_preference()`, `open_world_generator()`, ...).

### What the arrangement buys, and what it leaves

**The build coupling is narrow.** A panel header recompiles only its own dependents: 8 for
`properties.h`, 15 for `assets.h` (many panels call into the Asset panel), 3 for a panel
nothing else calls. `editor_app.h` and `editor_types.h` recompile all 27 — they are genuine
hubs, and a panel's own change does not touch either.

**`EditorApp` is still a large class.** Most *shared* state lives on it: ~660 declarations and
~290 data members, the forwarders included. Moving a panel's state the rest of the way in is
per-panel work — when a member turns out to be used by one panel only, move it into that panel
and drop the `app_.` qualification.

**Keep `EditorContext` small.** Every accessor on it is paid for by every panel — which is the
cost the split exists to remove. Add one only when more than one panel needs it.

See `plans/2026-10-24_structural-cleanup.md`, Stage 7.

## Where to change what

| Change | Go to |
|---|---|
| A panel's contents | `ui/<panel>.cpp` (+ its `.h` for new members) |
| Editor colours / metrics | `editor_theme.h` |
| A component's inspector rows | [`../schema/`](../schema/README.md) — **not here** |
| A viewport manipulator | [`../viewport/`](../viewport/README.md) |
| A modelling operation | [`../mesh/`](../mesh/README.md) |
| Build / packaging | [`../build/`](../build/README.md) |

## Widget conventions: add, remove, reorder, empty state

A list of items in a panel -- Material Slots, Vertex Groups, UI designer rows, a weather
condition, a component -- follows one of these, so the next list does not invent a sixth:

- **Add.** One full-width `ctx.button("Add " + noun, -1, under_cap, Icon::Plus)` at the
  *bottom* of the list. The sole exception is a list drawn inside an area *header*, which has
  no room for a row: a right-pinned `ctx.icon_button(id, Icon::Plus, "Add " + noun)` there
  instead. Where a *type* must be chosen first, the Add button opens a popup of types -- one
  affordance, then a choice -- rather than the choice standing in for the button. **Not this:**
  a combo whose first item is a verb ("Add Object Asset..."), several differently-typed Add
  buttons side by side, or an Add reachable only from a context menu.
- **Remove.** The per-item header's built-in X (`collapsing_header`'s `remove_clicked`) for a
  list of collapsible items; a right-edge *unframed* `icon_button(Trash)` for a single-line
  item (a node-graph socket). A context-menu Remove may exist *in addition*, never instead.
  **Not this:** a full-width `Trash` button, which reads as "delete the whole list"; a text
  `"Remove"` button.
- **Reorder.** `ctx.reorder_grip(list_id, grip_box, index, count, pitch)` at the row's left
  edge, `style.row_height` wide, and nothing else -- see `imm.h` for the primitive and
  `schema/inspector.cpp`'s `FieldKind::ItemList` case for the canonical caller. A list whose
  index *is* the meaning (a splat channel, a face slot, a value sorted by time) declares
  `fixed_order()` in its schema and shows the item's ordinal instead of a grip. **Not this:**
  text `Move Up` / `Move Down` buttons; `Icon::ArrowRight`/`ArrowDown` or `Icon::Undo`/`Redo`
  standing in for up/down (`Icon::ArrowUp`/`ArrowLeft` exist now -- use them); a reorder
  reachable only from a context menu.
- **Empty state.** `ctx.label_dim(plural + " -- none yet.")` (see
  `item_list_empty_text()`), drawn once, where the items would be, above the Add button. A
  *generative* empty state (Weather's "Using the engine's stock conditions" + a button that
  creates them) is allowed only where there is a real default to create, and is then an
  enabled `primary_button`, not a sentence.
- **Per-item header**, for a list of more-than-one-row items:
  `ctx.collapsing_header_id(ctx.get_id("item"), title, open, &remove, Icon::None, nullptr,
  grip_w)` -- keyed by *position* (`collapsing_header_id`), not by the title text, because the
  title is often one of the item's own editable fields and keying the fold on it would fold
  the item shut on every keystroke while it's being renamed. Title by `item_title()`'s rule in
  [`../schema/README.md`](../schema/README.md#item-lists-fieldkinditemlist).

**Deliberately exempt**, each for a stated reason -- do not "fix" these to match the above:

| List | Why it is different |
|---|---|
| The animation / rig-pose dope sheets (`timeline.cpp`) | A keyframe canvas, not a list: its order is time, not rank. Gets theme roles (`EditorTheme::Timeline`) only. |
| The node-graph canvas (nodes themselves) | Position is the data; there is no row order to have. |
| Material slots (`properties.cpp`) | The index is the mesh's face-slot attribute. `fixed_order`. |
| Heightmap terrain splat layers (`terrain.cpp`'s hand-written panel) | The index is the splat channel the brush paints; reordering would repaint the terrain. `fixed_order`, capped at `toy::world::k_heightmap_layer_count`. The schema's own `layers` `ItemList` (reached from the Components tab) is declared the same way, so both read identically even though the panel keeps its own richer editor (a "paint this layer" button, a live brush binding) -- collapsing the two onto one widget is out of scope for a standardization pass, not an oversight. |
| ParticleSystem `color_over_life` / `size_over_life` / `alpha_over_life` | Sorted by time; a grip there would silently un-sort a gradient. `fixed_order`. These are also the textbook case for the inspector's existing `Curve`/`Gradient` field kinds (`gradient_edit()` in `editor/ui/curve_widget.h`), which have no users today -- a real fix, left as a follow-up rather than folded into this pass. |
| StringList fields (comma-separated text), including order-significant bone chains like `FootIK.left`/`right` | Turning a text field into a reorderable list is a new editor, not a standardized one. Follow-up. |
| The Components tab's component list | `move_component()`'s reorder stays in the right-click menu (now with `Icon::ArrowUp`, not `ArrowRight`). A component row is a whole inspector -- hundreds of pixels, collapsible, no uniform pitch -- so `reorder_grip`'s stepping-by-pitch drag has nothing to measure against. A direct-manipulation reorder here needs a different primitive, one that compares against the neighbouring headers' actual drawn rects, not a follow-up grip. |
| Outliner sibling order | Drag-and-drop already reparents (`outliner.cpp`); `SceneDocument::reparent()` already accepts an insertion index, but the drop site never passes one, so there is no drop-line indicator and no sibling reorder. Deferred with the tree-drop-between work below. |

**Follow-ups this pass found but did not do:**
- A drop-line indicator for the Outliner, so a drag can reorder siblings (not just reparent):
  `TreeNodeResult::drop_edge { None, Above, Into, Below }` from where in the row the drop
  lands, feeding `reparent()`'s already-accepted index.
- Wiring the inspector's `Curve`/`Gradient` field kinds onto the three ParticleSystem ramps
  above, instead of their `fixed_order()` `ItemList`s.
- A direct-manipulation reorder for the component list (see the table).
