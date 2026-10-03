# editor/ -- toyengine_editor

A Unity/Blender-style editor for toyengine projects. Everything it saves is a file the game
already loads: `scenes/*/scene.yaml`, `meshes/*.yaml`, `materials/*.yaml`, `config.yaml`
(or their `.caml` encodings, via Build > Package).

## How it is put together

| Piece | File | Role |
|---|---|---|
| Engine embedding | `toyengine/core/engine.h` (`EngineOptions`, `FrameHooks`, `push_scene`, `set_display_region`, `set_overlay_scene`) | The editor owns a normal `Engine` in edit mode: scenes don't simulate (only `runs_in_edit_mode()` systems run), the scene renders into the viewport panel's rect, and the editor UI is a second "overlay" scene drawn on top. |
| UI | `libs/uicoopa/uicoopa/immediate/` | uicoopa's immediate-mode layer: menus, popups, modals, trees, property rows, colour picker, drag & drop, text fields -- drawn through uicoopa's own `DrawList`/`Font`/`UiPass`. |
| Documents | `core/scene_document.h`, `app/asset_documents.h` | The YAML document is the source of truth. Edits go through `edit()` (whole-document snapshot undo). Object nodes carry a private `__eid`, stripped on save. |
| Live scene | `app/scene_sync.h` | Rebuilds as little as an edit needs: transform patch, single-object rebuild, or a full `SceneLoader::load_from_node()`. |
| Schemas | `schema/component_schema.h`, `schema/settings_schema.h`, `schema/inspector.h` | Describe component / config keys for friendly widgets and "Add Component" defaults. Unknown keys and component types are still shown generically and always saved verbatim. |
| Modelling | `mesh/edit_mesh.h`, `mesh/mesh_ops.h`, `mesh/primitives.h` | Welded-vertex face-list mesh, round-tripping the engine's per-corner mesh YAML; extrude, inset, bevel, delete, merge, flip, smooth/flat, box/planar UVs. |
| Quad tools | `mesh/mesh_topology.h`, `mesh/mesh_loops.h`, `mesh/mesh_subdivide.h`, `mesh/mesh_bvh.h`, `app/ui/mesh_tools.inl` | Blender's quad workflow: edge loops / rings (Alt / Ctrl+Alt click), Loop Cut and Slide (Ctrl+R; wheel = cuts), Edge Slide (G G), Subdivide, Subdivide Smooth (Catmull-Clark), Triangulate (Ctrl+T), Tris to Quads (Alt+J), Dissolve, Bridge Edge Loops (Ctrl+E), Shift+A in Edit Mode, and the Adjust Last Operation panel. Picking and the edit overlay only see visible elements unless X-ray is on. |
| Axis locking | `viewport/modal_transform.h`, `viewport/gizmo.h` | X / Y / Z (again: Local or Normal orientation), Shift+axis planes, MMB auto-constraint, Tab between typed components, gizmo plane handles, header orientation Global / Local / Normal. |
| Sculpt Mode | `mesh/sculpt.h`, `app/sculpt_preview.h`, `app/ui/sculpt.inl` | Draw / Smooth / Inflate / Grab / Flatten brushes, F / Shift+F radius and strength, X/Y/Z symmetry; the Sculpting workspace. Strokes are one undo step and update a dynamic GPU mesh instead of re-exporting the YAML each frame. |
| Isolation | `app/editor_app.h` (`enter_mesh_mode_`, `isolate_`) | Edit and Sculpt Mode hide the other renderable objects (lights stay) and frame the mesh, Local-View style; the header toggle turns it off (saved in prefs). |
| Viewport | `viewport/editor_camera.h`, `viewport/gizmo.h` | Orbit/pan/dolly/fly camera; translate/rotate/scale gizmo and picking, drawn and hit-tested at full window resolution. |
| Themes | `themes/*.yaml`, `app/editor_theme.h` | The look, loaded at startup (`blender_dark` by default; Edit > Theme switches, the choice is saved in `~/.toyengine` prefs). Widget colours/metrics come from uicoopa's `imm_theme.h`; the editor's own roles live in the `chrome:`, `viewport:` and `outliner:` sections. Theme files hot-reload; Edit > Theme > Export Full Theme writes every role out as a starting point. |
| Packaging | `build/packager.h` | Copies `assets/` with every YAML encoded to `.caml`. |

## Notes

- Saved files use a canonical key order and lose comments (fkYAML drops them on parse).
  load -> save -> load -> save is byte-stable.
- The viewport renders with `resolution_mode: fill` (the project's vertical resolution, width
  following the panel), so it fills its area; the pipeline is rebuilt a few frames after a
  resize settles. Solid / Material Preview draw BLEND materials through their own pass and can
  darken with SSAO (Viewport Shading popover).
- Startup-only render settings (marked `*`) apply via Build > Restart Renderer, which
  rebuilds the Engine and carries open documents across.
- Config edits only write keys you touch: "x" on a row removes the key so its quality
  preset applies again.
- An object that uses `inherit_from` is edited as an override and rebuilt with the whole
  scene; writing minimal delta overrides is future work.

## Tests

`toyengine_editor_tests` -- groups `writer`, `document`, `mesh`, `imm`, `viewport`,
`config` (no GPU) and `editor_shell`, `package` (headless Engine). Set
`EDITOR_DUMP_DIR=<dir>` to save screenshots of every tab during `editor_shell`.
