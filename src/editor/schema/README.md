# editor/schema

**This is where you add a component to the inspector.** Schemas describe each YAML block's keys
— label, widget kind, range, default — so the editor can draw friendly widgets instead of raw
values.

| File | Purpose |
|---|---|
| [`component_schema.h`](component_schema.h) | `FieldKind` / `FieldDesc` / `ComponentSchema`, the schema table, and `register_component_schema()`. One entry per component type. Also `ShowWhen` / `InspectorSection` / `SectionBuilder`: a component's inspector LAYOUT. |
| [`component_sections.cpp`](component_sections.cpp) | That layout, one `layout("Type", {...})` per component -- which keys sit in which collapsible section, under what heading, and when. Kept apart from the key tables so each reads on its own. |
| [`inspector_sections.h`](inspector_sections.h) | The registry of hand-written drawers, for a component (or one of its sections) that rows of fields cannot express. Drawers live in [`sections/`](sections/). |
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
3. Over about ten keys, give it a layout in `component_sections.cpp`: group the keys by what the
   user is trying to do, write one sentence per section, and put mode-only keys behind a
   `ShowWhen`. A component with no layout is drawn as one flat list, which is fine for a small
   one. Keys no section lists still appear, in a trailing "More" block.
4. For a project-local component, call `register_component_schema()` instead of editing the table.
5. Add the key table entry to the `create-scene` skill's `reference/components.md`.

## Sections

A section is pure presentation data -- title, icon, blurb, the keys it draws, an optional
header checkbox, and a condition:

```cpp
sec("Flow", "ArrowRight").when("mode", {"flowing"})
    .says("Which way the water runs, and how fast.")
    .keys({"flow_speed", "flow_min_speed", "flow_slope_gain"}).done()
```

Conditions compare a key as the inspector spells it (`true`/`false` for a bool, the digits for
an int, the text for an enum), and a key the block does not have reads as its schema default --
so a row means the same thing before and after it is first written. `hidden_when()` is the
negation, for "every mode but this one".

Nothing here can lose data: a wrong condition hides a row, and the value is still saved
verbatim, as everything in this directory is.

## Custom sections

When rows of fields will not do -- a live read-out, a curve, a preview -- register a drawer:

```cpp
register_inspector_section("Spinner.preview", [](imm::Context& ctx, SectionContext& sc) {
    EditResult r;
    // ... draw anything; sc.comp is the component's node, sc.env reaches the editor.
    return r;
});
```

Registered under a bare type name it takes that component's whole inspector over (this is how
`ParticleSystem` and `TileTerrain` are drawn); under `"<Type>.<id>"` it fills one section that
names it in `custom`. Built-in drawers live in [`sections/`](sections/); a game project
registers its own beside its `register_component_schema()` call, under `#if TOY_EDITOR`.

Render-settings fields carry three extra flags that have no equivalent for components:
`startup_only` (changing it rebuilds the renderer), `tier_driven` (a `*_quality` preset sets it
when `config.yaml` doesn't) and `project_only` (baked in at engine start-up, so no scene
override). `tests/editor/unit/render_settings_schema_test.cpp` asserts every render field's
schema default equals the engine's own `ToyRenderConfig` default — **keep them in lockstep or
that test fails.**
