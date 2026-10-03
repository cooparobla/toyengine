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
| Viewport | `viewport/editor_camera.h`, `viewport/gizmo.h` | Orbit/pan/dolly/fly camera; translate/rotate/scale gizmo and picking, drawn and hit-tested at full window resolution. |
| Packaging | `build/packager.h` | Copies `assets/` with every YAML encoded to `.caml`. |

## Notes

- Saved files use a canonical key order and lose comments (fkYAML drops them on parse).
  load -> save -> load -> save is byte-stable.
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
