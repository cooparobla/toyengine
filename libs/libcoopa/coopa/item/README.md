# Item Module (`coopa::item`)

The `item` module is the authoritative, engine-agnostic model behind any slot-based
inventory UI: item identity and definitions, a definition database (C++- and
YAML-authored), fixed-capacity inventory storage with add/remove/move/merge/swap/split,
and a selectable hotbar window over an inventory. A UI layer (e.g. uicoopa's
`InventoryGrid` via `InventoryBinding`) *visualizes* this model rather than owning it —
every mutation is expected to route back through `Inventory`'s methods, so the model
stays the single source of truth regardless of which input device (mouse drag, gamepad
pick-place, a console `give` command) triggered the change.

---

## Item Module Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────────┐
│                              ITEM MODULE CLASS HIERARCHY                          │
└──────────────────────────────────────────────────────────────────────────────────┘

        ┌───────────────┐        ┌──────────────────────┐
        │    ItemId      │◄───────│       ItemDef         │  (immutable, shared per kind)
        │ name + FNV hash│        │ name/icon/max_stack/  │
        └───────┬───────┘        │ category/rarity/tint  │
                │ keys            └──────────┬───────────┘
                ▼                            │ owned by
        ┌───────────────┐                    ▼
        │   ItemStack    │        ┌──────────────────────┐
        │  item + count  │        │     ItemDatabase      │
        └───────┬───────┘        │  ItemId -> ItemDef     │
                │ stored in       └──────────────────────┘
                ▼                            ▲ resolves max_stack for
        ┌───────────────┐                    │
        │   Inventory    │────────────────────┘
        │ slots_: vector │
        │ on_slot_changed│
        │ on_slots_swapped│
        └───────┬───────┘
                │ windowed by
                ▼
        ┌───────────────┐
        │    Hotbar      │
        │ selected index │
        │ on_selection_  │
        │   changed      │
        └───────────────┘
```

---

## File Breakdown

### [`item_id.h`](item_id.h)

`ItemId::from_name()` normalizes an author-chosen short name (lowercase, trimmed) and
hashes it (FNV-1a 64-bit), mirroring `coopa::asset::AssetId`'s shape. Two ids differing
only by case or incidental whitespace resolve to the same identity, and `ItemId` is
directly usable as an `unordered_map`/`unordered_set` key.

### [`item_def.h`](item_def.h)

`ItemDef` — the immutable, shared description of one kind of item: `name`,
`description`, `icon` (an `IconLibrary` name, resolved by the UI layer), `max_stack`,
`category` (`ItemCategory`), `rarity` (`ItemRarity`), and an optional `tint` override
(`alpha < 0` means "unset", matching uicoopa's `InventoryItem::color` convention).
`parse_item_category()`/`parse_item_rarity()` degrade unrecognized strings to
`Misc`/`Common` rather than throwing.

### [`item_stack.h`](item_stack.h)

`ItemStack` — the *instance* data for one inventory slot: which `ItemId`, and how many.
Deliberately minimal; everything else about an item lives on its shared `ItemDef`.

### [`item_database.h`](item_database.h)

`ItemDatabase` — an `ItemId -> ItemDef` lookup table. Plain value type (copyable,
movable, no `coopa::event::Signal` member) so it can be handed around as a
`std::shared_ptr<ItemDatabase>` by `AssetManager`/`ItemDatabaseLoader`. C++-defined
(`define()`) and YAML-defined (`item_database_loader.h`'s `parse_item_database()`,
which merges into an existing instance) entries coexist freely.

### [`item_database_loader.h`](item_database_loader.h)

`ItemDatabaseLoader` — a `coopa::asset::TypedAssetLoader<ItemDatabase>` that parses a
YAML `items:` list, mirroring `coopa::anim::AnimationClipLoader`'s pure-CPU decode/
pass-through-finalize shape. `parse_item_def()`/`parse_item_database()` are public free
functions so tests (and any tool) can parse a fixture directly without going through
`AssetManager`.

### [`inventory.h`](inventory.h)

`Inventory` — fixed-capacity `ItemStack` storage, the authoritative three-way transfer
rule (`move_or_merge()`: move into an empty slot, merge same-item stacks up to the
`ItemDatabase`'s `max_stack`, or swap two different items), plus `add()`/`remove()`
(spanning multiple slots), `swap()`, `split()`, and `count_of()`. Publishes
`on_slot_changed(int, const ItemStack&)` and `on_slots_swapped(int, int)` — the seam a
UI binding subscribes to instead of polling. Move-only (holds `Signal` members).

### [`hotbar.h`](hotbar.h)

`Hotbar` — tracks which of a contiguous window of an `Inventory`'s slots is selected
(wrapping `next()`/`prev()`, direct `select()`), publishing `on_selection_changed` only
when the selection actually changes. `selected_stack()` always reads through to the
backing `Inventory` rather than caching a copy.

---

## Usage Example

```cpp
#include <coopa/item/item_database.h>
#include <coopa/item/inventory.h>
#include <coopa/item/hotbar.h>

coopa::item::ItemDatabase db;
db.define(coopa::item::ItemDef{
    coopa::item::ItemId::from_name("potion_health"), "Health Potion",
    "Restores 25 health.", "potion", /*max_stack=*/16,
});

coopa::item::Inventory backpack(27, &db);
coopa::item::Hotbar    hotbar(&backpack, /*first_slot=*/0, /*count=*/9);

backpack.on_slot_changed.connect([](int index, const coopa::item::ItemStack& stack) {
    // Push `stack` into a UI slot, e.g. via uicoopa's InventoryBinding.
});

backpack.add(coopa::item::ItemId::from_name("potion_health"), 5);
hotbar.select(0);
```
