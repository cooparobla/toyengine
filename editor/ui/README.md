# editor/ui

Small editor-chrome pieces: things the editor draws that are not a panel and not a viewport tool.

| File | Purpose |
|---|---|
| [`ui_palette.h`](ui_palette.h) | The UI designer's widget palette — every entry is a ready-made object (or component) the designer can place. |
| [`ui_canvas_math.h`](ui_canvas_math.h) | The UI designer's geometry: mapping between the editor's frame and the game canvas. No GPU, no UI — unit-tested. |
| [`log_view.h`](log_view.h) | A scrolling view of streamed output lines (build logs, `tools/toyhub` runs) that keeps the tail in view. Shared with the hub. |
| [`logo.h`](logo.h) | The toyengine logo ([`toyengine/core/branding.h`](../../toyengine/core/branding.h)) drawn as immediate-mode triangles. Shared with the hub. |

For editor *colours and fonts* see [`../app/editor_theme.h`](../app/editor_theme.h) and
`editor/themes/`; for the game UI theme being edited, `../schema/ui_theme_schema.h`.
