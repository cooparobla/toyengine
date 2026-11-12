# editor/instance

The instance painter's tools, as **pure functions** over an `InstanceSetData` — no scene, no GPU,
so they are unit-tested directly. The same arrangement `editor/terrain/` uses, and for the same
reason.

| File | Purpose |
|---|---|
| [`instance_brush.h`](instance_brush.h) | `InstanceTool` (Paint / Erase / Place / Cluster / Select), `InstanceBrushSettings`, the candidate generators (`brush_candidates`, `cluster_candidates`, `snap_to_grid`), `make_record()`, and the undo patch a stroke records. |
| [`instance_select.h`](instance_select.h) | `InstanceSelection` — which instances are picked, and the edits a group operation is built from (`turn_instance`, `orbit_instance`, `scale_instance`, `offset_instance`). |

The surface a candidate lands on is **handed in**, never raycast here: that is
[`editor/viewport/surface_raycast.h`](../viewport/surface_raycast.h), which offers the same
nearest-wins search over terrain (either mode) and mesh objects. Keeping the two apart is what
lets a test paint onto a plane it made up.

The mode is [`../app/ui/instance_paint.h`](../app/ui/instance_paint.h); the engine side is
[`toyengine/instance/`](../../toyengine/instance/README.md).

**A selection is a list of indices**, so it survives edits that rewrite records in place and goes
stale the moment a chunk's records are added to or removed from — erasing compacts the vector and
every later index shifts down. The panel clears the selection whenever the set changes under it,
and `validate()` drops whatever is left addressing nothing. Acting on a stale ref would silently
edit the wrong instance, so neither guard is optional.

Tests: `tests/editor/unit/instance_brush_test.cpp`, `instance_select_test.cpp`,
`surface_raycast_test.cpp`, `tests/editor/gpu/instance_paint_test.cpp`.
