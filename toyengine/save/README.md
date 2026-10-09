# toyengine/save

Save games. The engine decides where saves are stored and how they are written and read; the
game decides what goes in them. Nothing is saved unless the game (or a component) asks for it.

Demo: `./build/toyengine character_test`
([assets/scenes/tests/gameplay/character_test](../../assets/scenes/tests/gameplay/character_test/scene.yaml)).
Pick up a coin or two, press **F5**, walk somewhere else, press **F9**: the player, the coins and
the clock come back.

| File | Purpose |
|---|---|
| [`save_system.h`](save_system.h) | `SaveSystem` (`engine.saves()`): slots on disk, `save` / `load` / `list_slots` / `delete_slot` / `has_slot`, the `on_save` / `on_load` / `on_loaded` signals, migrations, and the scene-crossing load. |
| [`save_game.h`](save_game.h) | `SaveGame`, the document, and `SaveNode`, a typed handle onto one section of it (`get` / `set` / `section`). |
| [`saveable.h`](saveable.h) | The `ISaveable` interface, the `SaveId` component and `find_by_save_id()`. |
| [`save_demo.h`](save_demo.h) | `SaveDemo`, the character_test demo component (coins and clock through the global hooks). |
| [`register.h`](register.h) | The `SaveId` and `SaveDemo` scene parsers. |

Outside this directory:
- `CharacterController` ([`scene/character_controller.h`](../scene/character_controller.h)) is
  an `ISaveable`: with a `SaveId` on its object, its position and facing are saved.
- The editor gives a new `SaveId` a fresh unique id, and a duplicated object new ones
  (`SceneDocument::add_component()` / `duplicate_objects()`).

## Where saves go

```
<user data dir>/saves/            ~/Library/Application Support/<app> on macOS,
  slot_1.save                     $XDG_DATA_HOME/<app> on Linux (runtime_paths.h)
  slot_1.meta
```

- **`<slot>.save`** is the whole document. **`<slot>.meta`** is a small sidecar a slot menu
  reads without opening the save: `version`, `scene`, `saved_at`, `timestamp`, `play_time`
  and the game's `summary`.
- Both are written **atomically** (a temp file, then a rename). A crash during a save leaves the
  previous save as it was.
- **Encoding** (`config.yaml`): `save.encode: auto` writes plain YAML in development and caml
  (compressed and encrypted, see [`core/caml_codec.h`](../core/caml_codec.h)) in a
  `TOY_SHIPPING` build. `yaml` and `caml` force one. Reading accepts either.
- **Quick slot**: `save.quick_slot: quicksave` is what F5 saves to and F9 loads (the
  `quick_save` / `quick_load` input actions). `""` turns the keys off. They are off in the
  editor.
- Slot names are letters, digits, `_`, `-` and `.`; nothing that could leave the directory.
- `saves.set_root(dir)` moves the directory (tests, or a game with its own layout).

## What goes in a save

```yaml
version: 3                 # SaveSystem::set_version(): the game's save format
meta:                      # written by the engine
  slot: slot_1
  scene: assets/scenes/level_2/scene.yaml
  saved_at: 2026-10-08T14:03:12Z
  timestamp: 1791468192
  play_time: 4120.5
  summary: { level: Docks, coins: 12 }      # the game's, for slot menus
global:                    # the game's (on_save / on_load)
  gold: 120
  quests: { done: [intro, bridge] }
objects:                   # one section per SaveId, one sub-section per ISaveable
  player: { character: { position: { x: 4, y: -2.5, z: 1.25 }, yaw: 135 } }
  chest_3f9a1c: { chest: { open: true } }
```

The game fills it in three ways. Use whichever fits.

### 1. Global state: `on_save` / `on_load`

```cpp
TOY_MODULE(my_game) {
    auto& saves = engine.saves();
    saves.on_save.connect([](toy::save::SaveGame& g) {
        g.global().set("gold", Game::gold);
        g.global().section("quests").set("done", Game::finished_quests);   // std::vector<std::string>
        g.summary().set("level", Game::level_name);                        // shown by slot menus
    });
    saves.on_load.connect([](const toy::save::SaveGame& g) {
        Game::gold = g.global().get("gold", 0);                            // fallback if missing
        Game::finished_quests = g.global().section("quests").get("done", std::vector<std::string>{});
    });
}
```

`SaveNode` values: `bool`, integers, `float` / `double`, `std::string`, `glm::vec2/3/4`,
`glm::quat` and `std::vector`s of them. `get_node` / `set_node` take any YAML. A missing or
mistyped value reads as the fallback. A missing section reads as empty. Sections handed to
`on_load` are read-only.

A component can connect too (through `toy::save::SaveSystem::active()`), with
`connect_scoped` so it disconnects when the scene goes away. `SaveDemo` does this.

### 2. Per-object state: `ISaveable` + `SaveId`

Put a `SaveId` on the object, and implement `ISaveable` on any of its components:

```yaml
- type: SaveId
  id: chest_3f9a1c       # unique per scene; the editor fills one in. Empty = the name path.
```

```cpp
class Chest : public coopa::scene::Component, public toy::save::ISaveable {
public:
    bool open = false;
    std::string save_key() const override { return "chest"; }
    void save(toy::save::SaveNode& out) override { out.set("open", open); }
    void load(const toy::save::SaveNode& in) override { open = in.get("open", open); }
    std::string type_name() const override { return "Chest"; }
};
```

Objects with a `SaveId` are visited whether they are active or not. A component with no
section in the save keeps what the scene file gave it. Two objects with the same id are a mistake:
only the first is saved, with a warning.

`SaveId` belongs on objects in a scene, not inside an object asset (prefab): every instance
would share the id. Put it on the instance.

### 3. Old saves: migrations

```cpp
saves.set_version(3);
saves.register_migration(1, [](toy::save::SaveGame& g) {          // 1 -> 2: coins became gold
    g.global().set("gold", g.global().get("coins", 0));
    g.global().erase("coins");
});
saves.register_migration(2, [](toy::save::SaveGame& g) { ... });   // 2 -> 3
```

Loading a version-1 save runs 1 -> 2 then 2 -> 3, in order, before anything reads it. A step
with no migration just bumps the version. A save newer than the build is loaded as it is, with a
warning.

## Loading

`saves.load("slot_1")`:
1. Reads and decodes the slot, then runs the migrations.
2. If the save's scene is not the current one, loads it with `Engine::load_scene_async()` (a
   fade) and continues once it is live. `saves.loading()` is true meanwhile. Otherwise it
   continues at once.
3. Before the scene's first update: `on_load`, then every `ISaveable` on a `SaveId` object,
   then `on_loaded(slot)`.

`on_load` runs before the per-object pass on purpose, so the game can recreate objects first.

### Spawned and destroyed objects

The engine does not track objects created or destroyed at runtime. A scene reload brings back
exactly what the scene file holds. The game records what changed in `global` and redoes it in
`on_load`. For example, dropped loot spawned from an object asset, and enemies killed for good:

```cpp
saves.on_save.connect([&](toy::save::SaveGame& g) {
    toy::save::SaveNode drops = g.global().section("drops");
    for (const Drop& d : world.drops) {                  // what the game spawned, keyed by SaveId
        toy::save::SaveNode e = drops.section(d.save_id);
        e.set("prefab", d.prefab);                       // e.g. "objects/loot_bag"
        e.set("position", d.position);
    }
    g.global().set("killed", world.killed_ids);          // SaveIds of scene enemies gone for good
});
saves.on_load.connect([&](const toy::save::SaveGame& g) {
    const toy::save::SaveNode drops = g.global().section("drops");
    for (const std::string& id : drops.keys()) {
        const toy::save::SaveNode e = drops.section(id);
        auto* obj = engine.spawn(e.get("prefab", ""), e.get("position", glm::vec3(0.0f)));
        if (obj) obj->add_component<toy::save::SaveId>()->id = id;
    }   // the ISaveable pass that follows restores each drop's own state by its SaveId
    for (const std::string& id : g.global().get("killed", std::vector<std::string>{}))
        if (auto* o = toy::save::find_by_save_id(engine.scene(), id)) o->set_active(false);
});
```

## Slot menus

```cpp
for (const toy::save::SaveSlotInfo& s : engine.saves().list_slots()) {   // newest first
    show(s.slot, s.saved_at, s.play_time, s.summary_node().get("level", "?"));
}
```

`list_slots()` reads only the sidecars. A slot whose sidecar is missing is described from its
`.save`.

## Limitations

- One save per call, on the main thread. A save is small YAML and is written synchronously.
- Only the active scene is saved. Additive scenes are not.
- In the editor's play mode, a save's scene is empty (play runs a copy of the edited scene), so
  loading applies to the running scene in place and never switches scenes.
