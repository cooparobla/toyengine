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
translation units. Touching `ui/properties.h` rebuilds **26**.

[`ui/console_panel.h`](ui/console_panel.h) is the reference implementation, and all 24 panels
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
nothing else calls. `editor_app.h` and `editor_types.h` recompile all 26 — they are genuine
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
