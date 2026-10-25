# toyengine/ui

Game-side helpers for UI assets (`assets/ui/*.yaml`). The UI *system* is uicoopa's; this is the
thin layer that opens one as a scene object and binds game logic to it by widget name.

| File | Purpose |
|---|---|
| [`ui_assets.h`](ui_assets.h) | `open()` / `close()` / `find()` — a UI asset is an object asset whose root carries a `Canvas`, so opening one is spawning a prefab and wrapping it in a `coopa::ui::UiHandle`. `UiController` is the component base for binding: override `bind(UiHandle&)`, reach widgets by their authored name, and `keep()` each connection so it dies with the component. |

## The two ways a UI reaches the screen

```yaml
# 1. authored into the scene -- present from the start
- prefab: ui/hud
```

```cpp
// 2. opened on demand
coopa::ui::UiHandle pause = toy::ui::open(engine.scene(), "ui/pause_menu");
keep(pause.on_click("Resume", [&] { toy::ui::close(engine.scene(), pause); }));
```

## Conventions

- **Names are the contract.** Game code addresses widgets by the name given in the editor's UI
  designer, never by index or path. The designer's **Bindings** tab lists every name a
  `UiHandle` can reach and flags duplicates — that panel is the authoritative view of this
  module's API surface for a given asset.
- **`close()` goes through the scene's command buffer**, so destroying a UI from inside one of
  its own callbacks is safe: the object outlives the frame that asked for it to go.
- **Authoring lives in the editor, not here.** To build or restyle a UI see the `create-ui` and
  `create-ui-theme` skills and [`../../editor/README.md`](../../editor/README.md)'s UI designer
  section.
