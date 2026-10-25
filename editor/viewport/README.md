# editor/viewport

**This is where viewport tools live** — the camera, the picking math, and the interactive
manipulators. All of it is window-pixel space: the user aims in the window, not in the scene
image (which renders at its own internal resolution).

| File | Purpose |
|---|---|
| [`editor_camera.h`](editor_camera.h) | Blender-style orbit / pan / dolly / fly, and the framing operations (`frame_selected`, `frame_all`). |
| [`gizmo.h`](gizmo.h) | Viewport math — projection, rays, picking — and the translate / rotate / scale gizmo. Reports **cumulative** deltas since the drag began, so the caller applies them to the drag-start values and never accumulates rounding. |
| [`modal_transform.h`](modal_transform.h) | Blender's modal operators: G / R / S with axis constraints and numeric entry, plus interactive inset and bevel. |
| [`rect_gizmo.h`](rect_gizmo.h) | The UI designer's 2D counterpart: move, resize, anchor, pivot and rotate one UI element, in canvas space. |

## Conventions

- **Cumulative, not incremental.** Every manipulator reports the total delta since the gesture
  started. Applying per-frame deltas would accumulate float error over a long drag and make
  undo imprecise.
- **Pure where it can be.** `rect_gizmo` and the picking math are tested without a device
  (`tests/editor/unit/rect_gizmo_test.cpp`, `ui_canvas_math_test.cpp`); keep new geometry in
  functions that take values and return values.
- **One gesture, one undo step.** A drag is merged by the `EditResult::active` / `finished`
  protocol from [`../schema/inspector.h`](../schema/inspector.h).
