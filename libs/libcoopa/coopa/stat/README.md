# Stat Module (`coopa::stat`)

The `stat` module models clamped, optionally-regenerating gameplay quantities —
health, stamina, mana, or anything else shaped like a current/max meter. It exists so
a HUD bar (e.g. uicoopa's `ProgressBar`) can bind to a real model and stay in sync via
a signal, the same "UI visualizes a libcoopa model" pattern `coopa::item` establishes
for inventories.

---

## File Breakdown

### [`resource.h`](resource.h)

`Resource` — `current`/`max` plus optional `regen_per_second` and `regen_delay` (how
long after the last `damage()` before `tick(dt)` resumes regenerating — the "stamina
waits a beat before climbing back" pattern). `damage()`/`heal()`/`drain()`/
`set_current()` all clamp to `[0, max]`; `set_max(new_max, keep_ratio)` can either
clamp `current` against the new ceiling or rescale it to preserve `normalized()`.

Publishes two signals a UI binds to instead of polling every frame:
- `on_changed(current, max)` — fires only when the value moves past a small epsilon,
  so a full, undamaged resource ticking every frame stays silent.
- `on_depleted()` — edge-triggered, fires once per crossing from `> 0` to `<= 0`, not
  on every frame the value stays at zero.

Move-only (holds `Signal` members).

### [`stat_block.h`](stat_block.h)

`StatBlock` — a named registry of `Resource`s (`resource("health")`,
`resource("stamina")`, creating on first access). Backed by
`std::unordered_map<std::string, Resource>` specifically because it is node-based:
inserting a new named resource never invalidates a `Resource*` obtained from an
earlier call, which matters because a bound UI widget holds that pointer for the
lifetime of its binding.

---

## Usage Example

```cpp
#include <coopa/stat/stat_block.h>

coopa::stat::StatBlock stats;
coopa::stat::Resource& health = stats.resource("health");
health.max = health.current = 100.0f;

coopa::stat::Resource& stamina = stats.resource("stamina");
stamina.max = stamina.current = 100.0f;
stamina.regen_per_second = 12.0f;
stamina.regen_delay = 1.2f;

health.on_changed.connect([](float current, float max) {
    // Update a bound HUD bar, e.g. via uicoopa's ProgressBar::bind().
});

health.damage(30.0f);

// Per frame:
stamina.tick(delta_time);
```
