# physxcoopa/damage (`coopa::physx::damage`)

What a blow IS, and what it does to a material -- pure, scene-free, header-only, in the
library's own algorithm / component / system convention (see `vehicle/README.md` for the same
shape applied to a different subsystem). The whole reason this is typed rather than one scalar
"damage" number: an axe cleaves what a bullet punches through, and the same impulse means
neither. The response that actually CARVES a mesh lives one layer up, in
`toyengine/destruct/damage_field.h` and `toyengine::scene::Damageable` -- this directory only
ever decides what SHOULD happen, never touches geometry.

| File | Purpose |
|---|---|
| [`damage_types.h`](damage_types.h) | `DamageType` (Impact/Slash/Pierce/Explosive/Fire) and `DamageEvent` (type, amount, point, direction, edge, radius) -- the vocabulary every other file here is written against. No scene types: a projectile, a swung weapon, a contact or a script all produce the same struct. |
| [`damage_material.h`](damage_material.h) | `DamageMaterial` -- per-DamageType `resistance` (divides incoming amount), plus `hardness`/`toughness`/`fibrous`/`hit_points`, the three dials `resolve_damage()` reads to pick a response. Loaded from `damage_materials/<name>.yaml` (`loaders/damage_material_loader.h`), same convention as `dynamics::PhysicsMaterial`. |
| [`damage_resolve.h`](damage_resolve.h) | `resolve_damage()` -- the one function that encodes "an axe cleaves what a bullet punches through": a `DamageEvent` + a `DamageMaterial` in, a `ResolvedDamage` (HP cost, footprint, and a `DamageResponse` -- None/Dent/Chip/Hole/Cleave/Fracture) out. The policy table is in this file's own doc. Below a fixed floor (`detail::k_min_effective`), ANY type resolves to `DamageResponse::None` -- what keeps a resting crate's steady settling-contact impulse from slowly sanding it down over a thousand frames. |
| [`impact.h`](impact.h) | `impact_blow()` -- the one place this library turns physics into damage: reads a solved contact's accumulated NORMAL IMPULSE (not a speed -- by the time a contact is reported the solver has already stopped the thing that hit, so its relative velocity is only the leftover) into an Impact `DamageEvent`. |

## Where the rest of the picture lives

- `components/damage_dealer.h` -- `DamageDealer`, put on a weapon head or a bullet's Collider to
  say what kind of blow it deals (so an axe reads as Slash, not the generic Impact
  `impact_blow()` would otherwise assume). Parsed in `physx_yaml.h`.
- `loaders/damage_material_loader.h` -- the `damage_materials/<name>.yaml` asset loader. Ship a
  set matching `physics_materials/`'s own names: `wood`, `concrete`, `metal`, `glass`, `flesh`,
  `clay`.
- `toyengine/destruct/damage_field.h` -- `DamageField`, the localized accumulation (a grid of
  0..1 saturation cells) that turns repeated `ResolvedDamage` deposits into a region, and fits a
  cleave plane through it once that region reads flat enough.
- `toyengine::scene::Damageable` / `DestructionSystem` -- the scene-facing component and the
  system that drains its blow queue, deposits into the field, and carries out Fracture, Cleave,
  and Dent/Chip (pushing vertices inward on a per-object mesh clone,
  `toyengine/destruct/dent.h`). Hole is not implemented -- punching through is a topology change
  a dynamic Mesh's fixed index buffer cannot express; see `Damageable`'s own file doc.

## Usage example

```cpp
#include <physxcoopa/damage/damage_resolve.h>
#include <physxcoopa/damage/impact.h>

using namespace coopa::physx::damage;

DamageMaterial wood;
wood.toughness = 0.55f;
wood.fibrous = 0.8f;
wood.hit_points = 120.0f;

DamageEvent blow = impact_blow(contact);      // from a Collider::on_collision_enter payload
blow.type = DamageType::Slash;                // a DamageDealer on the other collider said so
blow.amount = 150.0f;

ResolvedDamage r = resolve_damage(blow, wood);
// r.response: Cleave once a slash on fibrous wood clears the material's own cut cost.
```
