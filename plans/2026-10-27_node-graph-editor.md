# A node graph editor for toyengine's editor

> **Difficulty: high — but it is mostly wiring, not invention.** The data model, the file format,
> the node registry and the evaluator all exist and are tested. What is missing is a canvas to draw
> them on, and ~50 small registration sites that each fail *silently* if skipped.
>
> On approval, copy to `plans/2026-10-27_node-graph-editor.md`.

## Context

`coopa/nodegraph/` (the generic evaluator) and `toyengine/procgraph/` (the `ProceduralGraph`
component that bakes a graph into a scene subtree) **landed earlier today** — 94 tests green, and
`assets/graphs/{parametric_stair,spline_railing,marker_lights}.yaml` render in `procgraph_demo`.

But those three graphs were hand-written in a text editor, and that is the whole gap. A node graph
is a *visual* medium: the reason Blender's geometry nodes are usable is that you can see the wires.
Authoring one as YAML means counting link tuples by hand — I did exactly that while building the
demo, and got the wiring wrong four times in ways that produced a silently empty bake.

**The intended outcome:** a node graph workspace in the editor — the viewport switches to a canvas
when a graph asset is open — that is **generic over any `coopa::nodegraph::Graph`** and happens to
be set up for procgraph's node and socket types. A second graph kind later (a material graph, a
VFX graph) needs no editor code at all: node types, categories, socket types and conversions all
come from the registries the library already exposes.

### What is already there, and must be reused

| Need | Already exists |
|---|---|
| Nodes, links, multi-input order, `ui_position` | `coopa/nodegraph/graph.h` — `Node` carries `glm::vec2 ui_position`, *"Authoring only. The evaluator never reads it."* |
| The file format, with `pos:` per node | `graph_yaml.h`, byte-identical round trip, unknown `props` keys preserved |
| The palette's backing store | `NodeTypeRegistry::categories()` / `by_category()` / `all()` — already sorted; `NodeType` carries `category`, `label`, `tooltip` |
| Per-instance socket lists | `NodeType::declaration_for(props)` → `SocketDecl{identifier, label, type, default_value, flags}` |
| "May this wire connect?" | `TypeRegistry::can_convert(from, to)` — the *same* predicate `validate()` uses, so highlighting and validation cannot disagree |
| Per-node error markers | `Diagnostic{severity, node, node_label, message}` — `node` is the authored `NodeId`. `graph.validate(diag)` is cheap enough per edit; `compile()` on demand also resolves groups |
| Which nodes actually contribute | `CompiledGraph::steps()[i].source_id` — the set of these is exactly the live nodes, so dead branches can be greyed |
| Live re-bake | `find_procgraph(scene)->rebake(component)`, `last_result(component)` |
| Thick lines, rounded rects, clipping, popups, z-order | `imm::Context` (see the gap below) |
| Pan/zoom gestures, rubber band, snapped drag → one undo step | `editor/app/ui/timeline.cpp` and `editor/app/editor_app.cpp:2423-2454` |

## The three decisions

**1. Generic canvas, procgraph binding.** `editor/node/` holds pure graph math; the panel draws any
registered graph. procgraph contributes nothing but a socket-colour table.

**2. Full editor in the first landing** — navigate, edit topology, edit values, save, and live
feedback. Staged below so each stage is shippable, but all four land.

**3. Polyline and bezier go into `uicoopa`'s `imm`.** `imm` has `line()` (already a thick oriented
quad) but no path API; wires need one, and the curve widget would benefit too.

> ⚠️ **Coordination risk.** `libs/uicoopa/uicoopa/immediate/imm.h` was last edited at 11:17 today by
> the `finish-ui-consolidation` session, which is still active. Keep the addition **strictly
> additive** — new methods only, no change to any existing signature or behaviour — and check the
> file's mtime immediately before editing. If that session is mid-flight, the fallback is to
> tessellate in `editor/node/` and move it into `imm` later; nothing else in the plan depends on
> where the tessellator lives.

## Architecture

### 1. `libs/uicoopa/uicoopa/immediate/imm.h` — a path API (additive only)

```cpp
/** @brief A connected run of segments, drawn as oriented quads with round joins. */
void polyline(const glm::vec2* pts, size_t n, const glm::vec4& c, float t = 1.0f, bool closed = false);
void polyline(const std::vector<glm::vec2>& pts, const glm::vec4& c, float t = 1.0f, bool closed = false);

/** @brief A cubic bezier, flattened to `segments` (0 auto-picks from its screen length). */
void bezier(glm::vec2 p0, glm::vec2 c0, glm::vec2 c1, glm::vec2 p1,
            const glm::vec4& c, float t = 1.0f, int segments = 0);
```

`line()` is `DrawList::add_line`, an oriented quad with **no caps and no joins**, so a chain of
segments above ~2 px shows a wedge gap on the outside of every bend. `polyline()` closes that by
dropping a `circle()` at each interior joint (what `ring()` already does implicitly), which is why
it belongs in the library rather than being open-coded per caller. `bezier()` is flattening plus
`polyline()`.

Also worth taking while here: `fill_rounded`/`outline_rounded` are hand-built and **aliased**,
while `DrawList::add_rounded_rect`/`add_rounded_border` are feathered and AA'd but never called by
imm. Node bodies look markedly better through the latter. Reaching them needs the private
`to_canvas_` flip done by hand (`Rect{{b.x, H - b.bottom()}, {b.right(), H - b.y}}`), so a small
`fill_rounded_aa()` / `outline_rounded_aa()` pair is the honest fix — but it is **optional** and
should be dropped if the imm file is contended.

### 2. `coopa/nodegraph/graph.h` — four missing operations

An editor needs these and the library has none of them. All are small, all belong on `Graph`
(the alternative is every consumer re-scanning `links()` by hand):

```cpp
/// @brief Every link leaving `from`. The mirror of links_into(), which an editor needs to
///        redraw and re-route when a node moves or an output is disconnected.
std::vector<const Link*> links_from(const SocketRef& from) const;
/// @brief Every link touching `node`, either end. For deletion preview and node dragging.
std::vector<const Link*> links_of(NodeId node) const;
/// @brief Removes exactly one link. remove_links_into() removes ALL of them, which is wrong
///        for a multi-input where the user clicked one wire.
bool remove_link(const Link& link);
/// @brief Copies `ids` with fresh ids, remapping links whose BOTH ends are in the set.
std::vector<NodeId> duplicate_nodes(const std::vector<NodeId>& ids, glm::vec2 offset);
```

Plus one correctness fix the loader exposes: `graph_from_node` pushes links straight onto `links()`,
bypassing `add_link`, so a hand-written file may contain **several links into a non-multi input**
and `validate()` does not flag it. Add that check to `validate()` — it is the editor's invariant too.

### 3. `editor/node/` — the pure math, unit-tested (new module)

Follows `editor/ui/curve_math.h`'s rule verbatim: *"every decision about where a point goes lives
in [the math], which is unit-tested on its own."* Note `editor/ui/ui_canvas_math.h` and
`editor/viewport/rect_gizmo.h` — the obvious templates — were **deleted** in the recent UI refactor
(`ff112e4`); recover their shape with `git show ff112e4^:editor/ui/ui_canvas_math.h` if useful, but
`editor/viewport/gizmo.h` is the surviving convention and already provides
`point_segment_distance(p, a, b)` for wire picking.

| File | Owns |
|---|---|
| `node_view.h/.cpp` | `NodeView{pan, zoom}` + `to_screen`/`to_graph`/`delta_to_graph`/`box_of`, and `zoom_at(mouse, factor)` (cursor-anchored, lifted from `ui_canvas.cpp:444-452`). The transform imm does not have. |
| `node_layout.h/.cpp` | A node's screen box from its declaration: header height, per-socket rows, width from the widest label. One function, so drawing and hit-testing cannot disagree about where a socket is. |
| `node_hit.h/.cpp` | `hit_node`, `hit_socket`, `hit_link` (sampled polyline + `point_segment_distance`), `nodes_in_box`. |
| `node_wire.h/.cpp` | `wire_points(a, b, segments)` — horizontal-tangent cubic, `d = clamp(abs(dx) * 0.5, 30, 200)` in graph units. Segment count from screen length, not fixed. |
| `node_edit.h/.cpp` | Graph mutations as pure functions over `Graph&`: `connect` (consults the target `SocketDecl` for multi-input and `can_convert` for validity), `disconnect`, `add_node_at`, `delete_selection`, `duplicate_selection`. Returns a `NodeGraphEdit{changed, active, finished, std::string key}` mirroring `editor/schema/inspector.h`'s `EditResult`. |

### 4. `editor/app/ui/node_graph.{h,cpp}` — the panel

Shape per `editor/app/editor_panel.h` (and unlike `UiCanvasPanel`, actually inherit `EditorPanel`).
Header must not include `editor_app.h`.

```cpp
class NodeGraphPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;
    bool open_graph(const std::filesystem::path& p);   // also sets app_.active_path_
    bool new_graph(const std::string& name);
    void save();
    bool dirty() const;
    void draw_graph_view_(imm::Context&, const imm::Box& area);      // the canvas
    void draw_graph_palette_(imm::Context&, const imm::Box& area);   // left: node list + search
    void draw_graph_props_(imm::Context&);                           // PropTab::Graph: name, interface
    void draw_node_props_(imm::Context&);                            // PropTab::Node: props + socket defaults
    void frame_selection_();
};
```

Four traps the exploration surfaced, all of which produce a broken canvas rather than a compile
error:

- **Do not host it in `begin_region(..., scroll = true)`.** The region consumes the scroll wheel and
  `wheel_consumed_` is private, so zoom silently stops working. Use a plain `Box` + `push_clip`, as
  both existing canvases do.
- **Do not use `invisible_button` for node drags.** Once it claims `active_id_`, `is_hovered()`
  returns false for *everything else* — so a drop-target socket cannot highlight while a wire is
  being dragged. Use raw `ctx.is_hovered(box) && in.pressed[0]` plus an owned
  `{bool active; glm::vec2 start;}`, the timeline's pattern.
- **Thread a `claimed` flag through the draw loop** in priority order socket → node → wire →
  background. imm has no hit-test tree; the timeline threads exactly such a flag (`on_key`).
- **Key widget ids by `NodeId`, never by node pointer.** `get_id(const void*)` still hashes the
  address, and `Graph` stores nodes in a vector that reallocates. `ctx.push_id(int64_t(n.id))`,
  and sockets by their `identifier` string (which the library guarantees is never renamed).

Z-order 1..999 is free (`kPopupZ = 1000`): wires 10, bodies 20, dragged node 30, rubber band 40 —
and **restore `set_z_order(0)` before returning** or every later panel inherits it.

An LOD below ~0.4 zoom (header-only boxes, no sockets or labels) keeps a large graph legible and
cheap; socket dots as `fill_rounded` pills rather than `circle()+ring()` (~36 triangles each).

### 5. The document: reuse `MaterialDocument`, do not copy `UiCanvasPanel`

```cpp
using NodeGraphDocument = MaterialDocument;   // alias precedent: ThemeDocument, asset_documents.h:100
```
held as `NodeGraphDocument graph_;` on `EditorApp` beside `material_` and `game_theme_`.

This matters more than it looks. `UiCanvasPanel` hand-rolls `std::vector<Node> undo_, redo_;` and
`bool dirty_`, and the consequence is live and verifiable: **`ui_canvas_panel_->save()`, `::undo()`,
`::redo()` and `::dirty()` have zero callers anywhere in `editor/`.** Ctrl+S, Ctrl+Z, the
unsaved-changes guard and "Discard changes" all silently ignore the UI workspace today. Copying
that model reproduces the bug. `MaterialDocument` gives `load/save/commit/do_undo/do_redo` and the
uniform `dirty() == undo.revision() != saved_revision`, and `UndoStack`'s global `g_undo_sequence`
is what lets Ctrl+Z share one timeline.

### 6. Theme

Add a `struct NodeGraph { … } nodegraph;` to `editor/app/editor_theme.h`, one
`v("nodegraph", "<role>", …)` line per colour in `visit_editor_theme`, and the same block in the
three `editor/themes/*.yaml`. Roles: grid, grid_major, node_bg, node_header (per category),
node_outline, node_selected, node_error, socket (per socket *type*), wire, wire_dragging,
wire_invalid, band_fill, band_outline.

**Socket colour lives in the editor, not the library.** `TypeInfo` carries no colour and should not
— the library is deliberately domain-free. A `TypeId → vec4` table built once from
`TypeRegistry::find("Float")`, `find("Geometry")`, … keeps colours in the theme where every other
editor colour lives, and an unregistered type falls back to grey, which is correct.

## The wiring — ~50 sites, and three that bite

There is **no `-Werror`/`-Wswitch`**, so every switch over `AssetType` that misses the new case
compiles clean and silently does nothing. The exhaustive list is long but mechanical; these three
are the ones that are not merely inert:

1. **`editor/app/editor_app.h:1300` — `Shading shading_by_type_[9]` must become `[10]`.** It is
   indexed by `static_cast<int>(active_type_)` in `set_view_()`. An out-of-bounds **write**, with
   no warning, the first time the workspace opens.
2. **`editor/app/ui/properties.cpp:53-59` — the tab-strip branch.** Miss it and `tabs` is empty,
   and `properties.cpp:112` does `tabs[0]`. This one crashes.
3. **`editor/app/editor_app.cpp:1518` — `update_scene_ui_placement_()`.** `NodeGraph` is not
   `asset_view_()`, so without a guard the open scene's HUD is placed over `display_rect()` and
   paints on top of the graph canvas. (Same function I fixed for the UI preview leak this session.)

The rest, grouped — each is one line:

- **Types:** `AssetType::NodeGraph` (append, never insert), `PropTab::Graph` + `PropTab::Node`.
- **Asset browser** (`editor/app/ui/assets.cpp`): the type-table row `{…, "graphs"}` at `:29`
  (absent → `asset_type_info_()` silently falls back to *Scenes*); `asset_type_of_()` `:163` (absent
  → every drop and open-by-path sees `None`); `open_asset_now_()` `:454` — copy the UI case's shape
  verbatim, `if (open_graph(abs)) app_.set_view_(AssetType::NodeGraph);`; `list_assets_`,
  `list_engine_assets_`, `open_asset_dirty_`, `new_asset_`, the `"graph"` string-kind router, the
  rename-reopen guard `:1207`.
- **App:** forward declaration, `friend class NodeGraphPanel;` (the one compile-time safety net),
  the `unique_ptr` member, construction in the ctor, a `graph_workspace()` accessor so it is
  testable at all, `set_view_()`'s `prop_tab_` ternary, `draw_viewport_()`'s early return (which
  must set `app_.viewport_box_` itself), `frame_selected()` / `frame_all()`, the viewport drop case.
- **Save and undo — seven sites, all currently missing for UI:** `UndoTarget::NodeGraph`,
  `undo_target_()` (without it **Ctrl+Z undoes the open scene**), `undo()`/`redo()`,
  `current_undo_label_()`, `has_unsaved()`, `save_all_()` + an `EditorApp::save_graph()`,
  `discard_all_changes_()`.
- **Layout** (`editor/app/ui/layout.cpp`): the left-column split for the palette at `:101`, and the
  `hierarchy` decision at `:72` (a node list belongs in the left column, not the outliner).
- **Properties:** the body-dispatch branch `:130` and `active_asset_label_()` `:164`.
- **Shortcuts:** `editor_app.cpp:4071,4080` gate on `!asset_view_()`, which is *true* for a graph —
  so X / Delete / F2 would act on the scene document while a graph is open.

## Staging

| Stage | Scope | What it buys |
|---|---|---|
| **1** | `editor/node/` math + its unit tests, and the four `Graph` additions. No UI. | Every geometric and topological decision is pinned by a test that runs in milliseconds without a device. |
| **2** | `imm::polyline` / `imm::bezier`. | Wires that do not show wedge gaps at their bends. Independently useful; land it alone so a conflict with the UI-consolidation session is cheap to resolve. |
| **3** | The workspace: `AssetType`, the panel, the document, the ~50 wiring sites, the theme section. **View and navigate only** — open a graph, see it laid out, pan/zoom/frame/select. | The whole rendering and hit-testing layer, provable on the three shipped graphs. |
| **4** | Edit topology: search-to-add palette, delete, duplicate, drag-to-connect with `can_convert` highlighting, rubber band. Undo through `UndoStack`. | It becomes an editor. |
| **5** | Edit values + live feedback: socket defaults and `props` in the Properties panel, save through the YAML round trip, per-node error badges from `validate()`, and `rebake()` on save so the scene updates while authoring. | Closes the loop: change a node, see the stair change. |

## Verification

**Unit — `tests/editor/unit/` (no device, milliseconds).** Model on `curve_math_test.cpp`.
- `node_view_test.cpp` — `to_screen`/`to_graph` round-trip; `zoom_at` keeps the point under the
  cursor fixed (the property that makes zooming feel right, and the easiest to get subtly wrong);
  zoom clamps; an empty graph has a sane frame-all.
- `node_layout_test.cpp` — a node's socket rows match its declaration's order and count; a node with
  a dynamic declaration (`Switch` with `data_type: Vector`) relayouts when props change; a hidden
  socket takes no row.
- `node_hit_test.cpp` — click on a node, on a socket, on a wire, and on empty space; a wire is
  pickable along its curve, not just near its endpoints; `nodes_in_box` is inclusive of partial
  overlap; every degenerate case (empty graph, zero-size box, click outside the viewport).
- `node_edit_test.cpp` — connecting a Float output to a Vector input succeeds *because*
  `can_convert` says so, and Vector→Float is refused; connecting to a non-multi input replaces,
  to a multi-input appends with an increasing `order`; delete removes the node *and* its links;
  duplicate remaps only links whose both ends were copied.
- `node_graph_document_test.cpp` — load → edit → save → load is byte-identical for the three
  shipped graphs (the library promises this; this proves the editor does not break it), and an
  unknown `props` key written by a newer build survives a round trip through the editor.

**Unit — `libs/libcoopa/tests/nodegraph_graph_test.cpp`** gains: `links_from`/`links_of` find both
directions; `remove_link` removes exactly one of several into a multi-input; `duplicate_nodes`
remaps internal links and leaves external ones alone; `validate()` now rejects two links into a
non-multi input.

**GPU — `tests/editor/gpu/node_graph_test.cpp`**, modelled on `ui_workspace_test.cpp` and driving
real clicks through `EditorSession`'s `InputDriver`:
- opening `graphs/parametric_stair.yaml` enters the workspace (`active_asset_type()`), and the
  canvas draws without the 3D viewport behind it;
- the scene's HUD does **not** paint over the canvas (the `update_scene_ui_placement_()` guard);
- drag a node → its `ui_position` moved, and the file round-trips with the new `pos:`;
- drag from an output to a compatible input → a link exists; drag to an incompatible one → none,
  and nothing crashed;
- delete a node → it and its links are gone, and **Ctrl+Z restores them** (the undo wiring, which
  is the thing most likely to be silently absent);
- Ctrl+S writes the file and clears `dirty()`; `has_unsaved()` sees the edit before that;
- a graph with a deliberate error shows a per-node badge on the right node id;
- a scene with a `ProceduralGraph` re-bakes after a save (`bake_count()` moved).

**Manual, once:** `EDITOR_SCREENSHOT=/tmp/g.png MAX_FRAMES=120 ./build/toyengine_editor` with a
graph open — a screenshot is the only thing that proves the wires read as wires.

**Build discipline.** `editor_app.h` is a 26-TU hub and `imm.h` is included by everything — batch
all header edits before the first build of a stage. `-j4` builds, `-j2` ctest, one heavy job at a
time, `run_in_background` rather than detaching. Rebuild `toyengine_editor` before any manual check.

**Gate:** `toyengine_editor_modelling` and `toyengine_env_render` currently fail from other
sessions' in-flight work (confirmed not mine — `modelling` fails identically with my changes
reverted). Re-check both before starting so a new failure is attributable.

## What this deliberately does NOT do

- **No node groups UI.** The library inlines `Group` nodes and the format supports them, but
  "enter a group", breadcrumbs and "make group from selection" are a second workspace mode. The
  canvas will *draw* a Group node as an ordinary node with the sub-graph's interface as its sockets.
- **No auto-layout.** Nodes sit where `pos:` says. A graph authored by hand with no `pos:` will pile
  at the origin; a one-shot "tidy" button is a follow-up.
- **No reroute nodes, frames, comments, or node mute/bypass.** None exist in the data model, and
  adding them means adding fields to `Node` that the evaluator must then ignore.
- **No editing the graph interface's min/max from the canvas.** `InterfaceSocket` carries them and
  the Properties panel will show them, but the generated-inspector work for
  `ProceduralGraph::inputs` is separate.
- **No fixing the UI workspace's missing save/undo wiring.** It is a real bug discovered while
  tracing this (Ctrl+S and Ctrl+Z silently ignore the UI designer) and it is adjacent — but it is
  not this task, and fixing it touches the same seven sites this plan already edits, so doing both
  at once would make a regression hard to attribute. Worth its own small follow-up.
