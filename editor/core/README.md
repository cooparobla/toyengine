# editor/core

The editor's data layer: documents, undo, YAML access, child processes and naming. No UI, no
viewport, no engine.

| File | Purpose |
|---|---|
| [`scene_document.h`](scene_document.h) | `SceneDocument` — the model of one scene file: its YAML document, stable object ids, selection and undo. The editor's central data structure. |
| [`undo.h`](undo.h) | Snapshot undo/redo for any copyable document value. |
| [`yaml_util.h`](yaml_util.h) | Typed reads with defaults, the engine's `{x,y,z}` / `{r,g,b}` spellings, `tidy()` (rounds away the 1e-7 drift a drag leaves, so saved files stay readable), and key removal. The editor's whole data-access layer. |
| [`process.h`](process.h) | `Task` — a cancellable child process whose output streams into a log view. Used for builds and every `tools/toyhub` invocation. |
| [`naming.h`](naming.h) | The snake_case names the editor gives what it creates, and uniquing. |

## Conventions

- **Every document is a plain `fkyaml::node`** — the same value the engine's loaders parse.
  There is no separate editor object model to keep in sync, and anything the editor does not
  understand is preserved verbatim on save.
- **Undo is snapshot-based**, not command-based: cheap to add an operation to (nothing to write
  an inverse for), at the cost of copying the document. Merge a continuous drag into one step
  via the inspector's `EditResult`.
- **Reads go through `yaml_util`**, not `node.at(...)` by hand — that is what keeps a missing
  key from throwing in the middle of a frame.
