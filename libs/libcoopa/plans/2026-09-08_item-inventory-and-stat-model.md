# Item/Inventory Model (`coopa::item`) and Stat Model (`coopa::stat`)

## Context

uicoopa already had a **slot** widget (`InventoryGrid`/`InventorySlot`: drag-drop, stacking,
gamepad pick-place) but no **item** concept worth the name — `InventoryItem` was a POD living
inside the widget itself, so the grid *was* the data, with no model behind it and no way to
author items from YAML. Meanwhile libcoopa had zero gameplay-domain content at all. This work
adds that model to libcoopa so uicoopa's inventory UI (and a new gameplay HUD demo) can become
a *visualization* of it instead of owning storage itself, and adds a matching `coopa::stat`
model for health/stamina-style resources so HUD bars have something real to bind to.

Two new libcoopa modules, both header-only, both requiring **no CMake change** (the library is
an INTERFACE target — a module is just a directory of headers plus a `README.md`).

## `coopa::item` (`coopa/item/`)

| File | Contents |
|---|---|
| `item_id.h` | `ItemId` — copies `coopa::asset::AssetId`'s shape: a normalized string (lowercase + trim, not path-normalized) plus a precomputed FNV-1a 64-bit hash, `std::hash<>` specialization included. |
| `item_def.h` | `ItemCategory`, `ItemRarity`, `ItemDef` (name/description/icon/max_stack/category/rarity/`tint` with `alpha < 0` = unset), `k_default_max_stack = 99`. |
| `item_stack.h` | `ItemStack { ItemId item; int count; }` — the only per-slot instance data; everything else lives on the shared `ItemDef`. |
| `item_database.h` | `ItemDatabase` — an `ItemId -> ItemDef` table. No `Signal` member (copy-disabled), so it can be handed around as a `shared_ptr` by `AssetManager`. |
| `item_database_loader.h` | `ItemDatabaseLoader : TypedAssetLoader<ItemDatabase>`, mirroring `coopa::anim::AnimationClipLoader`'s pure-CPU shape. Public `parse_item_def()`/`parse_item_database()` free functions so tests parse fixtures directly; `parse_item_database()` **merges** into an existing database so C++- and YAML-defined items coexist. |
| `inventory.h` | `Inventory` — fixed-capacity `ItemStack` storage: `add`/`remove` (spanning slots), the authoritative `move_or_merge()` three-way rule (move/merge/swap, capped by the `ItemDatabase`'s `max_stack` — the one deliberate divergence from the UI widget's own rule, which reads a mirrored copy instead), `swap`, `split`, `count_of`. Publishes `on_slot_changed`/`on_slots_swapped`. Move-only. |
| `hotbar.h` | `Hotbar` — a selectable window over a contiguous range of an `Inventory`'s slots; wrapping `next()`/`prev()`, `on_selection_changed` fires only on an actual change. `selected_stack()` always reads through to the backing `Inventory`. |

## `coopa::stat` (`coopa/stat/`)

| File | Contents |
|---|---|
| `resource.h` | `Resource` — current/max plus optional `regen_per_second`/`regen_delay` (seconds after the last `damage()` before `tick()` resumes regenerating). `on_changed(current, max)` fires only past an epsilon; `on_depleted()` is edge-triggered (once per crossing to zero, not every frame at zero). Move-only. |
| `stat_block.h` | `StatBlock` — a named registry of `Resource`s, backed by `std::unordered_map` specifically because it's node-based: a bound UI widget can hold a `Resource*` across further insertions without it dangling. |

Both modules ship a `README.md` in the existing per-module style (ASCII diagram, file table,
usage example); the top-level `libcoopa/README.md` gained sections 10 and 11.

## Testing

123 assertions total added to `test.cpp`, in new `namespace item_test` / `namespace stat_test`
blocks after the existing `input_test` block — covering id normalization/hashing, database
define/find/clear, the YAML loader (including unknown category/rarity fallback and merge
semantics), every `Inventory` operation (including the `ItemDef` vs. mirrored-`max_stack`
divergence explicitly), hotbar wrap/selection-emits-once-on-change, and `Resource`'s clamping/
regen-delay/edge-triggered-signal behavior plus `StatBlock` pointer stability across growth.
All pass (`./build/libcoopa`).

## Consumers

uicoopa's `InventoryBinding` (`uicoopa/widgets/inventory_binding.h`) and the gameplay HUD demo
(`test_hud_builder.cpp`) are the first and, at the time of writing, only consumers — see
uicoopa's own `plans/2026-09-08_gameplay-hud-and-model-binding.md` for that side of the work.
