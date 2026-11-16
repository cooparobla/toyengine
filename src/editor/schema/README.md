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

## Item lists (`FieldKind::ItemList`)

`f_items(key, item_fields, ...)` declares a list of mappings -- Vehicle `wheels`, a particle
system's `bursts`, a heightmap terrain's `layers` -- drawn by `schema/inspector.cpp`'s
`FieldKind::ItemList` case as a column of collapsible, grip-reorderable items, each with its
own `item_fields` as rows. The naming and defaulting rules are pure functions in
`schema/item_list.h`, unit-tested in `tests/editor/unit/item_list_test.cpp` -- read them
there before changing this prose.

**An item's header title**, in the order checked:
1. `item_title_key`'s value, if the decorator is set and the key is present and non-empty;
2. else the first of `label`, `name`, `title`, `id` the item carries, present and non-empty;
3. else the first of the item's own `item_fields` that holds text (`String`, `AssetRef`,
   `ChildRef`, `Enum`), non-empty -- this is how Vehicle `wheels` gets a sensible title
   (`object`, e.g. `wheel_fl`) with no schema change at all;
4. else `item_noun() + " " + (1-based index)` ("Wheel 3").

A list declared `fixed_order()` prefixes the title with the item's index, because there the
position *is* the meaning rather than an accident of ordering.

**Decorators**, beyond the kind itself: `with_item_noun()` (the singular, when dropping a
trailing "s" from the display name won't do), `with_item_title()` (pin the title to one key),
`fixed_order()` (no reorder grip -- see `app/README.md`'s widget-conventions table for which
lists need this and why), `collapsed_items()` (items start closed; for an item of many keys,
like Vehicle's 17-key wheels), `capped(n)` (Add disables past `n` items).

**`ShowWhen` works inside an item**, exactly as it does on a top-level field: an `item_fields`
entry's `show` is checked against that item's own data, so one item schema can carry several
mutually-exclusive key families (a TileTerrain LOD's `field` vs. `ratio` vs. `tile_set` keys,
gated on a `mode` enum) instead of showing all of them at once with a tooltip explaining which
are inert.

**There is no per-element override marking.** `env.after_field` fires once per sub-row, same
as any other field, but it is called with the *list's* key, not a per-element one
(`wheels[2].radius` is not a thing this model understands). This is not a shortcut: a prefab
override stores the whole sequence as one value, so there is no finer override to point at --
"the list is marked overridden when any part of it is" is the honest answer, not an
approximation of one.
