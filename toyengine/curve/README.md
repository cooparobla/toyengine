# toyengine/curve

An authored Bezier path attached to an object.

| File | Purpose |
|---|---|
| [`curve_data.h`](curve_data.h) | `CurveData` — control points with in/out handles, a `closed` flag and a per-segment `resolution`, plus every function that reads one: `evaluate_segment`, `tangent_at`, `polyline`, `length`, `points_along` (even spacing **by arc length**), `frame_from_tangent`, and the authoring edits (`append_point`, `insert_point`, `remove_point`, `move_point`, `refresh_handles`). No scene, no device, no asset. |
| [`curve.h`](curve.h) | The `Curve` component: a `CurveData` in the owner's LOCAL space, plus `always_show`. There is no CurveSystem — nothing runs per frame. |

## Why evaluation lives here and nowhere else

Three things read a curve: the viewport draws it, the Pen tool hit-tests it, and a
`ProceduralGraph` instances along it. If each sampled the curve its own way they would disagree
by a fraction, and a post would sit visibly off the line it was drawn on. `polyline()` is what
the viewport draws AND what `points_along()` measures, so the agreement is structural rather than
two implementations happening to match.

## Handle modes

`Free` and `Aligned` **store** their handles; `Vector` and `Auto` **derive** them from the
neighbouring points. That difference is the thing to remember: moving a point translates a stored
handle and recomputes a derived one, and `refresh_handles()` must run after any edit that moves a
point, because an `Auto` point's shape depends on where its neighbours now are.

Dragging a derived handle by hand turns the point `Free` — it cannot be both computed and
hand-placed.

`Aligned` mirrors the opposite handle's **direction** but keeps its own **length**. Mirroring the
length too would make a deliberately short handle snap long the moment the other was touched.

## Reading one from a graph

A `ProceduralGraph` binds `{ curve: PathObject }`, which snapshots the component in **world**
space on the main thread (the bake runs on a worker and must never read a live component). The
**Curve to Points** node then spreads `count` points evenly along it, each rotated to the
tangent, which is what a railing, a fence or a row of lamps follows.

## Editing one

Pen Mode in the viewport (`editor/app/ui/curve_pen.h`). Every decision it makes —
what is under the cursor, where a click lands in depth, what a dragged handle does to its partner
— is in [`editor/viewport/curve_edit.h`](../../editor/viewport/curve_edit.h) and unit-tested
without a window.
