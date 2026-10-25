# editor/schema

**This is where you add a component to the inspector.** Schemas describe each YAML block's keys
— label, widget kind, range, default — so the editor can draw friendly widgets instead of raw
values.

| File | Purpose |
|---|---|
| [`component_schema.h`](component_schema.h) | `FieldKind` / `FieldDesc` / `ComponentSchema`, the schema table, and `register_component_schema()`. One entry per component type. |
| [`inspector.h`](inspector.h) | Draws editable rows for a YAML block from a schema, with uicoopa's immediate-mode widgets. `EditResult` reports changed / still-dragging / finished so the caller can merge a drag into one undo step. `InspectorEnv` is what the inspector needs from the editor around it (asset lists, object names, the per-field hook the Components tab uses to mark prefab overrides). |
| [`settings_schema.h`](settings_schema.h) | `config.yaml`'s sections, grouped for the **Render Settings** and **Project Settings** tabs. |
| [`ui_schema.h`](ui_schema.h) | Schemas for uicoopa's UI components and composites — what the UI designer's Element and Components tabs show. |
| [`ui_theme_schema.h`](ui_theme_schema.h) | The fields of a game UI theme (`assets/ui/themes/*.yaml`), for the Theme editor. |

## The contract: schemas only DESCRIBE

There is no reflection in the engine — every component parser reads its keys by hand (gfxcoopa's
`components/register.h`, [`toyengine/scene/register.cpp`](../../toyengine/scene/register.cpp),
physxcoopa's `physx_yaml.h`). A schema *mirrors* those keys; it does not drive them. The
consequences are worth knowing before you add one:

- The inspector edits the component's **YAML node**, not a C++ object.
- A key the schema doesn't list is still shown and edited generically from its value.
- A component type with no schema at all still works — it just gets generic widgets.
- **Everything is saved verbatim**, schema or not. A schema can never cause data loss.
- A schema that disagrees with its parser produces a widget that writes a key the engine
  ignores. Nothing catches that but reading the parser.

## Adding a component to the inspector

1. The parser must exist first (`toyengine/scene/register.cpp` or the area's own `*_yaml.cpp`).
2. Add a `ComponentSchema` here, one `FieldDesc` per key. Set `in_default` on the keys a freshly
   added component should be written with.
3. For a project-local component, call `register_component_schema()` instead of editing the table.
4. Add the key table entry to the `create-scene` skill's `reference/components.md`.

Render-settings fields carry three extra flags that have no equivalent for components:
`startup_only` (changing it rebuilds the renderer), `tier_driven` (a `*_quality` preset sets it
when `config.yaml` doesn't) and `project_only` (baked in at engine start-up, so no scene
override). `tests/editor/unit/render_settings_schema_test.cpp` asserts every render field's
schema default equals the engine's own `ToyRenderConfig` default — **keep them in lockstep or
that test fails.**
