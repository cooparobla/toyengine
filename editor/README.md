# editor/ -- toyengine_editor

toyengine's asset editor (Blender look and keymaps, Unity's component model). It builds a
project's assets -- every one a file the game loads directly: `scenes/*/scene.yaml`,
`objects/*.yaml` (object assets, instanced with `prefab:`), `meshes/*.yaml` (with material
slots), `materials/*.yaml`, `textures/`, `config.yaml` (or `.caml`, via Build > Package).

**Layout.** Left: the Asset panel -- a tab per asset type, listing the project's assets.
Clicking one opens it (one asset at a time; unsaved changes prompt first). Middle: the viewer
(Console under it). Right: the Hierarchy (scenes / object assets; it mirrors the file) above
Properties (tabs depend on the asset type).

| Open asset | Viewer | Properties |
|---|---|---|
| Scene | the live scene; play / pause / step (F5). While playing, click the viewer to give the game the mouse and keys; Esc hands them back without stopping | Tool, Render, Output, Scene, World + the selected object's tabs |
| Object | the object alone, studio-lit | the object's tabs; instances of other object assets can be placed |
| Mesh | mesh viewer: Object / Edit / Sculpt / Vertex Paint / Weight Paint modes | Tool, Mesh (stats, material slots: name, assign faces, preview materials) |
| Material | lookdev scene (a shader ball by default -- curves, a cutaway with flat hard-edged walls, a square chamfered plinth -- or sphere / rounded cube / plane / cylinder; ground, key / fill / rim + environment, turntable) | the material editor |
| Texture | the image on a plane (a texture editor is a TODO) | info |

Edit / Sculpt Mode on a scene object edits the **mesh asset itself**: its MeshRenderer's mesh,
or, for a water object, its WaterBody's.
- **Who follows the edit:** every object using that mesh, live, including outlines and water.
- **Saving:** the mesh is saved automatically when you leave the mode. Ctrl+S also saves. A mesh
  opened as an asset tab saves on Ctrl+S only.
- **Refresh on save:** every asset built from the file reloads at once (render meshes,
  MeshColliders, water sources).

Undo:
- **In Edit / Sculpt Mode,** Ctrl+Z steps through that mesh's own history.
- **In Object Mode,** Ctrl+Z is one timeline across the scene and every mesh edited from it,
  newest first. A mesh step undone there is applied live and saved.
- **Each mesh keeps its history** when you switch to another one: the 8 most recent mesh
  histories are kept and resumed if the file is unchanged.
- **Depth:** a history holds up to 200 steps, trimmed to ~384 MB of snapshots for dense meshes,
  never below 16.

## How it is put together

| Piece | File | Role |
|---|---|---|
| Engine embedding | `toyengine/core/engine.h` (`EngineOptions`, `FrameHooks`, `push_scene`, `set_display_region`, `set_overlay_scene`) | The editor owns a normal `Engine` in edit mode: scenes don't simulate (only `runs_in_edit_mode()` systems run), the scene renders into the viewport panel's rect, and the editor UI is a second "overlay" scene drawn on top. |
| UI | `libs/uicoopa/uicoopa/immediate/` | uicoopa's immediate-mode layer: menus, popups, modals, trees, property rows, colour picker, drag & drop, text fields -- drawn through uicoopa's own `DrawList`/`Font`/`UiPass`. |
| Documents | `core/scene_document.h`, `app/asset_documents.h` | The YAML document is the source of truth. Edits go through `edit()` (whole-document snapshot undo). Object nodes carry a private `__eid`, stripped on save. |
| Live scene | `app/scene_sync.h` | Rebuilds as little as an edit needs: transform patch, single-object rebuild, or a full `SceneLoader::load_from_node()`. |
| Schemas | `schema/component_schema.h`, `schema/settings_schema.h`, `schema/inspector.h` | Describe component / config keys for friendly widgets and "Add Component" defaults. Unknown keys and component types are still shown generically and always saved verbatim. |
| Modelling | `mesh/edit_mesh.h`, `mesh/mesh_ops.h`, `mesh/primitives.h` | Welded-vertex face-list mesh, round-tripping the engine's per-corner mesh YAML; extrude, inset, bevel, delete, merge, flip, smooth/flat, box/planar UVs. The round trip keeps everything Blender's export writes: per-corner `colors`, vertex groups (`weights`), `joints` / `joint_weights`, authored `tangents` (re-exported unchanged while a corner's normal is unchanged, computed from the UVs otherwise) and any key the editor does not know. Every operation that creates vertices or corners interpolates their colours and weights. |
| Quad tools | `mesh/mesh_topology.h`, `mesh/mesh_loops.h`, `mesh/mesh_subdivide.h`, `mesh/mesh_bvh.h`, `app/ui/mesh_tools.inl` | Blender's quad workflow: edge loops / rings (Alt / Ctrl+Alt click), Loop Cut and Slide (Ctrl+R; wheel = cuts), Edge Slide (G G), Subdivide, Subdivide Smooth (Catmull-Clark), Triangulate (Ctrl+T), Tris to Quads (Alt+J), Dissolve, Bridge Edge Loops (Ctrl+E), Shift+A in Edit Mode, and the Adjust Last Operation panel. Picking and the edit overlay only see visible elements unless X-ray is on. |
| Axis locking | `viewport/modal_transform.h`, `viewport/gizmo.h` | X / Y / Z (again: Local or Normal orientation), Shift+axis planes, MMB auto-constraint, Tab between typed components, gizmo plane handles, header orientation Global / Local / Normal. |
| Sculpt Mode | `mesh/sculpt.h`, `app/sculpt_preview.h`, `app/ui/sculpt.inl` | Draw / Smooth / Inflate / Grab / Flatten brushes, F / Shift+F radius and strength, X/Y/Z symmetry in local or global axes (header Mirror button). Strokes are one undo step and update a dynamic GPU mesh instead of re-exporting the YAML each frame. |
| Vertex / Weight Paint | `mesh/paint.h`, `app/paint_preview.h`, `app/ui/paint.inl`, `assets/shaders/editor_paint.*` | Blender's paint modes (mode menu). Draw (Mix / Add / Subtract / Multiply / Lighten / Darken; Mix / Add / Subtract for weight), Blur, Average; Ctrl paints the secondary colour / subtracts, Shift blurs, X swaps colours, S samples, Shift+K fills, F / Shift+F radius and strength, symmetry, front faces only, Auto Normalize. Vertex groups (Properties > Tool) add / remove / rename, and in Edit Mode assign / remove / select the selection. Painting draws through an editor-only preview mesh and `editor_paint` shader (weights as Blender's heatmap); colours and weights are data for the game, saved in the mesh file, never rendered in-game. |
| Animation | `anim/clip_model.h`, `anim/clip_pose.h`, `app/ui/timeline.inl` | A rig is an object hierarchy whose root has an Animator (coopa::anim); the Timeline tab (bottom area) follows the selection's rig -- try the test rigs in the **Objects** tab (`objects/robot_arm`, `objects/tentacle`, `objects/bouncing_ball`: open one to animate it, drag one into a scene to place it) or `scenes/animation_test`, which has all three inline (all generated by `tools/gen_animation_test_scene.py`; their clips and meshes are shared in `assets/animations/` and `assets/meshes/`). A placed copy plays its asset's clips; its Timeline offers to open the asset, where they are edited. **Clips** belong to their object: each is an Animator state backed by a file under `<scene>/animations/<rig>/`, created / renamed / deleted only from the Timeline's clip menu, never the asset browser. **Animating:** turn on Record (red dot; the viewport gets a red frame) and move, rotate or scale rig objects -- the changed channels are AUTO-KEYED at the playhead (a gizmo drag is one undo step), and the rest pose in the scene is never touched. I keys position, rotation (quaternion) and scale of the selection; the inspector's Transform diamonds key one channel (filled = keyed on this frame). **Dope sheet:** Summary, the animated and selected objects (the list button: all of them), each expandable into channel rows; click / Shift-click / drag keys (frame-snapped), X / Delete, right-click for interpolation (Linear, Constant, Ease In / Out / In-Out). Space plays, Left / Right step a frame, Up / Down jump between keys, wheel zooms, Shift-wheel / middle-drag pans, the wheel over the names scrolls. Clip edits have their own undo and are written once a frame; Play reloads changed clips. |
| Branding | `toyengine/core/branding.h`, `app/ui/topbar.inl`, `tools/icon/export_icon.cpp` | The toyengine logo (a toy block) and version, defined once. The top-left logo opens the app menu (About, Controls, Quit); About lists version, build, compiler, platform, GPU and project, with Copy Info. Engine sets the logo as the window / taskbar icon, or the Dock icon on macOS. `./build/toyengine_icon` exports PNGs (16-1024) and `toyengine.icns` to `editor/branding/` (kept out of `assets/`, which is game content only). |
| Mirror | `mesh/mesh_mirror.h`, `app/ui/mesh_tools.inl` | Edit Mode symmetry: the header Mirror button (or Properties > Tool > Symmetry) repeats G / R / S and gizmo drags across X / Y / Z, the mesh's local axes or the world's; vertices on the mirror plane stay on it. Mesh > Mirror (Ctrl+M) flips the selection along a local or global axis through its centre, keeping faces outward. |
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
