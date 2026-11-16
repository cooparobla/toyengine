# editor/ui

Small editor-chrome pieces: things the editor draws that are not a panel and not a viewport
tool. The editor's actual widget vocabulary -- buttons, headers, the reorder grip, drag &
drop -- lives one level down, in `libs/uicoopa/uicoopa/immediate/imm.h`; this directory holds
the handful of widgets specific to the editor itself.

| File | Purpose |
|---|---|
| [`log_view.h`](log_view.h) | A scrolling view of streamed output lines (build logs, `tools/toyhub` runs) that keeps the tail in view. Shared with the hub. |
| [`logo.h`](logo.h) | The toyengine logo ([`toyengine/core/branding.h`](../../toyengine/core/branding.h)) drawn as immediate-mode triangles. Shared with the hub. |
| [`curve_widget.h`](curve_widget.h) / [`curve_math.h`](curve_math.h) | The curve and gradient editors used by the inspector's `Curve` and `Gradient` fields: the drag/pick/insert/remove maths (`curve_math.h`, no GPU, no UI, unit-tested on its own) and the box you drag points or ramp stops in (`curve_widget.h`). |
| [`image_downsample.h`](image_downsample.h) | Shrinks an RGBA8 image on the CPU, for the asset browser's thumbnails. |

For editor *colours and fonts* see [`../app/editor_theme.h`](../app/editor_theme.h) and
`editor/themes/`; for the game UI theme being edited, `../schema/ui_theme_schema.h`. Editor
*chrome sizes* (area heights, gaps, toolbar button sizes) are `../app/ui/metrics.h`.
