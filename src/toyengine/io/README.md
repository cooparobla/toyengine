# `toyengine/io/` — where the game's files live, and how they are read

Four leaf utilities every other layer may use, and which use nothing of the engine themselves.
That is the whole criterion for living here: no header in this directory includes a
`toyengine/` header from outside it, so anything may depend on `io/` without acquiring a
dependency on a feature. (`user_settings.h` includes `runtime_paths.h`; that is the only edge.)

| File | What it is |
|---|---|
| [`runtime_paths.h`](runtime_paths.h) | `RuntimeLayout` — where the executable is, where assets, shaders, saves, logs and the user's data directory are, on each platform and in each build. `k_shipping` and `debug_env()` are here too, because "is this a player's build" is the question that decides which of those paths exist. |
| [`caml_codec.h`](caml_codec.h) | The `.caml` container: a YAML document compressed with zstd and encrypted with AES-256-GCM (`libs/caml`). `install_caml_codec()` registers it with libcoopa's YAML loader, after which a `.caml` asset loads anywhere a `.yaml` one does. |
| [`user_settings.h`](user_settings.h) | `UserSettings` — the process-wide `settings.yaml` under the user's data directory. A player's choices, which must persist outside the game's own (possibly read-only) install. |
| [`yaml_read.h`](yaml_read.h) | `read()` and its siblings: present-if-set reads from a YAML mapping. Every component parser in the engine is hand-written (there is no reflection), and they all want the same thing — overwrite this field only if the key is present. |

## Why these are not in `core/`

`toyengine/core/` is the composition root: `Engine` owns every system, so `core/` depends on
nearly every other directory. A utility that `save/`, `render/`, `world/`, `particles/` and
`scene/` all need cannot live there without those directories depending *back* on the
composition root — a cycle, and one that makes `core/`'s real position in the layering
unreadable.

So the split is by role rather than by subject: `core/` is the top of the graph and `io/` is
the bottom. Adding a file here means asserting it has no engine dependencies at all, and the
assertion is checkable — `grep '#include <toyengine' io/*.h` returns nothing.
