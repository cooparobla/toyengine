---
name: editor-user-docs
description: Generate or refresh the toyengine editor's user manual in docs/editor/ -- linked Markdown pages with embedded screenshots (getting started, interface, scenes, viewport, modelling, sculpt/paint, materials, animation, UI designer, settings, shortcuts) written from the editor's source for people using the editor, not developers. Use when asked to write, update, sync or audit the editor user docs / user guide / manual / screenshots, or after editor features, menus or shortcuts change.
---

# Editor user manual

Writes the manual for **using** `toyengine_editor` into `docs/editor/`, with screenshots in
`docs/images/editor/`. The reader is a game maker working in the editor's window: they want to
know what they are looking at, how to get a task done, and which key does what. They do not
read C++ and do not care how the editor is built.

**Scope: the editor only.** Document what a person sees and does in the editor window. Leave
out game code (C++ calls, bindings stubs beyond the editor button that copies them), engine
internals, class or file names from the source, YAML keys and file formats, build
instructions, and anything about the engine that isn't reached through the editor's UI. The
developer tour in `editor/README.md` covers internals; don't duplicate it. A file path appears
only when the user meets it in the editor (a file dialog, the asset list, a saved image).

The source code is the only source of truth for behaviour. Never document a feature, menu item
or shortcut you have not found in the code during this run; never keep a statement from the
existing pages that the code no longer supports.

## Page map

All pages live in `docs/editor/`. Keep these file names stable; add a page only when a new
editor area doesn't fit any of these, and add it to the index and this table.

| Page | Covers | Primary sources | Screenshots |
|---|---|---|---|
| `README.md` | What the editor is, opening it and projects, the page list, a first-steps path | `editor/editor.cpp`, `editor/app/project.h` | `overview` |
| `interface.md` | Window layout, every menu, Asset panel, Hierarchy, Properties tabs, Console, status bar, unsaved-changes prompt, editor themes | `app/ui/topbar.inl`, `layout.inl`, `assets.inl`, `browser.inl`, `outliner.inl`, `statusbar.inl`, `theme_editor.inl`, `app/editor_theme.h` | `overview` (with a numbered list of the areas), `menu_file`, `asset_panel_meshes`, `unsaved_prompt`, `theme_light` |
| `scenes-and-objects.md` | Scenes, adding / selecting / parenting / renaming objects, components and Add Component, object assets, Play / Pause / Step, undo | `app/ui/properties.inl`, `outliner.inl`, `schema/component_schema.h`, `schema/inspector.h`, `core/undo.h`, `app/editor_app.h` | `hierarchy_inspector`, `add_component`, `object_asset` |
| `viewport.md` | Navigating, views, shading, X-Ray, grid, selecting, moving / rotating / scaling, snapping, hiding, Render Image | `viewport/*.h`, `app/ui/viewport_chrome.inl`, `app/trackpad.h`, `app/editor_app.h` | `viewport_solid`, `gizmo_move`, `xray_edit` |
| `modeling.md` | Meshes, Edit Mode tools, Adjust Last Operation, mirror, UVs, material slots, saving and undo | `mesh/*.h`, `app/ui/mesh_tools.inl`, `app/editor_app.h` | `edit_mode`, `adjust_last_operation` |
| `sculpt-and-paint.md` | Sculpt Mode, Vertex Paint, Weight Paint, vertex groups | `mesh/sculpt.h`, `mesh/paint.h`, `app/ui/sculpt.inl`, `app/ui/paint.inl` | `sculpt`, `vertex_paint`, `weight_paint` |
| `materials-and-textures.md` | Material editor, the preview scene, assigning materials, textures | `app/asset_documents.h`, `app/ui/properties.inl`, `mesh/shader_ball.h` | `material_editor` |
| `animation.md` | Rigs, clips, Record and auto-key, keys on the dope sheet, playback | `anim/*.h`, `app/ui/timeline.inl` | `timeline`, `record_autokey` |
| `ui-designer.md` | UI assets and templates, preview sizes, selecting / moving / anchoring elements, widgets, Element / Canvas / Bindings tabs, UI themes, Interact mode | `app/ui/ui_canvas.inl`, `ui/*.h`, `viewport/rect_gizmo.h`, `schema/ui_schema.h`, `schema/ui_theme_schema.h` | `ui_designer`, `ui_new_menu`, `ui_bindings`, `ui_interact` |
| `project-settings.md` | Project Settings modal, scene overrides (Render / World / Scene tabs), Rebuild Renderer, Package Project | `schema/settings_schema.h`, `app/ui/properties.inl`, `app/ui/project_settings.inl`, `build/packager.h` | `render_settings`, `world_settings`, `package_dialog` |
| `shortcuts.md` | Every shortcut, grouped by where you are | the Controls modal in `app/editor_app.h` (`begin_modal("Controls"`), menu shortcut strings, every key handler | `controls_modal` |

Primary source paths are relative to `editor/` unless they start with `editor/`.

## Screenshots

Shots come from the `docs` group of the editor test suite (`editor/editor_test.cpp`, tests
named `docs_<shot>`): each test copies `assets/` to a scratch project, stages one editor state
through real input, and saves the window. Nothing opens on screen and the repo's assets are
never written. Capture with:

```sh
.claude/skills/editor-user-docs/scripts/capture_screenshots.sh            # every shot
.claude/skills/editor-user-docs/scripts/capture_screenshots.sh docs_sculpt # just some
```

It builds the tests, runs each shot on its own with retries (the headless UI tests have a
known flaky MoltenVK segfault), and writes `docs/images/editor/<shot>.jpg`.

- **Look at every image** (Read tool) before using it. Reject a shot whose menu isn't open,
  that is mid-transition, shows scratch / temp paths in the Console, or doesn't show what the
  page says. Fix its test and recapture.
- **New shot needed** (a new feature, a page without one): add a `docs_<shot>` test modelled
  on its neighbours, add it to the Screenshots column above, capture it.
- Never show the pixel-art post effects (outline / palette / dither); the engine is presented
  at full resolution.
- Never open a visible window, and never attach a debugger.

## Procedure

1. **Inventory.** Run `python3 .claude/skills/editor-user-docs/scripts/check_docs.py`: it
   lists pages whose sources changed since the page did. Check `git log --oneline -- editor/`
   and `git status editor/` too. Rewrite stale pages; leave current ones unless the user asked
   for a full regeneration.

2. **Capture screenshots** for the pages being written (see Screenshots), and look at them.

3. **Research from source, per page.** Read the primary sources. Harvest exact UI strings:
   `menu_item("...", "<shortcut>")`, `begin_menu("...")`, tab names, button labels, tooltips,
   key handlers (`ctx.shortcut(Key::...)`, `key_pressed`) and their modifiers. The existing
   pages and `editor/README.md` are maps of features but may lag the code -- confirm every
   claim. For a full regeneration, fan out one subagent per 2-3 pages in a single message,
   give each this whole file, and have them write the pages directly; then do steps 5-7
   yourself.

4. **Write the page** (Writing rules below), embedding its screenshots.

5. **Cross-link.** Every page is reachable from `README.md`. The first mention per section of
   something explained on another page links to that section (`modeling.md#unwrap-uvs`).
   `shortcuts.md` links each group to the section that explains it.

6. **Wire in.** Keep `editor/README.md`'s pointer line to `../docs/editor/README.md` (add it if
   missing). Don't touch other documents.

7. **Verify.** `check_docs.py` must report 0 errors (links, anchors, images, orphans, Sources
   lines). Spot-check 5 shortcut or menu claims against the code with grep. Read each page
   once top to bottom as a new user: anything that needs source knowledge to follow gets
   rewritten.

8. **Report** the pages and shots written, and anything in the code that looked unfinished
   (TODOs, keys shown but unbound) that the pages mark "(not yet available)".

## Writing rules

- **Lead with the picture.** After the one- or two-sentence intro, show the page's main
  screenshot, then explain what's in it. Each task section that has a shot shows it before the
  steps.
- **Tasks as numbered steps.** Section headings are things the user wants to do ("Add an
  object", "Undo a mistake"). Steps start with a verb, one action each:
  1. Click **Add** in the viewport header.
  2. Choose **Mesh > Cube**.
  Follow with the result ("The cube appears at the 3D cursor."), and a tip only if it saves
  real effort.
- **Plain words.** Second person, present tense, short sentences. Define an editor term the
  first time it's used on a page ("the **3D cursor**, the small red-and-white ring that marks
  where new objects appear"). No source jargon: say "the panel on the right", not "the
  inspector widget"; "saved", not "serialized".
- **Tables for reference** (option lists, shortcut groups), prose for tasks. Keep a table to
  what a user would look up; don't list every field the code has when the UI labels speak
  for themselves.
- **UI labels in bold**, spelled exactly as the editor shows them: **File > Save Scene As...**.
  Keys in bold: **Ctrl+S**, **Shift+A**, **Numpad 7**, **Middle mouse drag**. Only the
  window-wide commands (Save, Undo, Redo, New / Open Scene, Quit) accept **Cmd** on macOS --
  say so once in `shortcuts.md` and on `README.md`, not on every line.
- **Callouts** for the few things that surprise people, using GitHub alert syntax:
  `> [!NOTE]`, `> [!TIP]`, `> [!WARNING]` (e.g. editing a scene object's mesh changes every
  object that uses that mesh). At most two or three per page.
- Mark anything unfinished as "(not yet available)" rather than describing it as working.
- No emoji. Wrap lines near 100 columns.

## Images and linking

- Embed with a relative path and real alt text describing what the shot shows:
  `![The editor in Edit Mode with two faces extruded](../images/editor/edit_mode.jpg)`, then
  an italic caption line under it when the alt text alone doesn't say what to notice.
- Relative links only (`viewport.md#snapping`); anchors are GitHub-style heading slugs.
- First line of every page except `README.md`: `[Editor manual](README.md) > <Page title>`.
- Footer on every page: `---`, then `Sources: ` with the code paths the page was written from
  (repo-relative, in backticks, code only -- never Markdown files, since edits to READMEs
  would mark pages stale), then `Previous: [..](..) | Next: [..](..)` in page-map order.
