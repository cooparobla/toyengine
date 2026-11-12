# hub/ -- toyengine_hub

A Unity-Hub-style launcher for toyengine projects, drawn with the editor's immediate-mode UI,
themes and file dialog so it looks like the editor.

| File | Purpose |
|---|---|
| [`hub.cpp`](hub.cpp) | `main()`. Creates the Engine, runs `HubApp`, honours the headless env vars. |
| [`hub_core.h`](hub_core.h) | The model, with no window or device: the project list, the hub's settings, and background shell tasks. |
| [`hub_app.h`](hub_app.h) | The UI: pages, dialogs, the log drawer. |

## The division of labour that matters

**The hub is a front end over `tools/toyhub`.** Creating, adding, building, re-pinning and
opening a project all run that script as a `Task` ([`editor/core/process.h`](../editor/core/process.h)),
with its output streamed into the log drawer — so the CLI and the GUI can never disagree about
what a project is. Only the cheap synchronous parts are native: reading the project list and
settings, and forgetting a project.

**The hub does not build.** Building is the editor's job (Build > Refresh). The hub builds a
project only the first time it is opened — since the editor it opens is part of that build — or
on "Rebuild (clean)" when that build is broken.

**The hub loads no scene.** It embeds a `toy::core::Engine` purely for its window, device and UI
rendering; the UI is an overlay scene, as in the editor.

## State on disk

Everything lives in `~/.toyengine/`, shared with `tools/toyhub`:

| File | Holds |
|---|---|
| `projects.yaml` | `projects:` — the absolute path of every project the hub lists. Only `new` and `add` put a project here; nothing is auto-detected. |
| `settings.yaml` | The hub's own settings (`theme`; default `blender_dark`). |

A project's name is always its folder's name. **The engine checkout itself is never a project.**

## Pages

`Projects` (open in the editor, reveal, clean rebuild, switch which of the three ways the project
gets its engine, remove; New / Add with the same three options) · `Engine` (this checkout, CLI
install, macOS app) · `Settings` (the hub's theme).

## The three engine modes

A project row's **...** menu and the New / Add dialog offer the same three choices, each one a
`tools/toyhub` command:

| Choice | Command | What the project builds |
|---|---|---|
| Share a checkout | `toyhub link <dir> [<engine>]` | `.libs/toyengine` symlinks a checkout shared with every other linked project |
| Its own worktree | `toyhub worktree <dir> [--branch B]` | `<project>/engine` is a git worktree of that checkout, on a branch named after the project |
| Its own clone | `toyhub own <dir> [--ref R]` | `<project>/engine` is the project's own clone, independent of every checkout on this machine |

`HubProject::mode()` is what the badge, its tooltip and the menu read; the script is the only
thing that moves an engine.

## Running it

```bash
cmake --build build --target toyengine_hub -j4
./build/toyengine_hub
# headless smoke run (no visible window):
HEADLESS=1 MAX_FRAMES=30 HUB_SCREENSHOT=out.png HUB_PAGE=projects ./build/toyengine_hub
#   HUB_PAGE = projects | engine | settings | new | add
```

On macOS, `tools/toyhub app` writes `~/Applications/toyengine Hub.app` as a launcher for this
binary.
