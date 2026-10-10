---
name: create-physics-material
description: Create a toyengine physics material -- friction and bounciness for colliders in assets/physics_materials/<name>.yaml -- and assign it to BoxCollider/SphereCollider/CapsuleCollider/MeshCollider components, or set up a physics object (Rigidbody + collider). Use when asked for a bouncy, slippery, grippy or heavy surface, ice/rubber/metal physics, friction or restitution, or to make an object physical.
---

# Create a physics material

A physics material is friction + restitution for colliders. Loader:
`libs/physxcoopa/physxcoopa/loaders/physics_material_loader.h`; struct and combine rules:
`libs/physxcoopa/physxcoopa/dynamics/physics_material.h`. Shipped:
`assets/physics_materials/` -- clay, concrete, ice, metal, rubber, superball, wood. Names match
the render materials where both exist, so a renderer and its collider pair up by name.

## File

```yaml
# <name> -- <what surface it models>.
dynamic_friction: 0.6        # sliding friction (default 0.6)
static_friction: 0.6         # friction at rest (default 0.6; usually >= dynamic)
restitution: 0.0             # bounciness 0..1 (default 0)
friction_combine: Average    # Average | Minimum | Maximum | Multiply
restitution_combine: Average
```

Combine modes are case-sensitive; an unknown value falls back to `Average`. When two colliders
disagree, the higher-priority mode wins: Multiply > Maximum > Minimum > Average. Ice uses
`Minimum` friction so it stays slippery against anything; a superball uses `Maximum`
restitution so it bounces off anything.

## Assigning it

The collider's `material:` is a **bare name** (the folder and `.yaml` are added for you) -- not
the `materials/<name>` path a MeshRenderer uses:

```yaml
- type: Rigidbody            # dynamic object: Rigidbody + collider on the same object
  mass: 2.0
- type: SphereCollider
  radius: 0.5
  material: rubber           # -> physics_materials/rubber.yaml
# or inline:
  material: { dynamic_friction: 0.2, static_friction: 0.3, restitution: 0.8, restitution_combine: Maximum }
```

No `material:` = the default (0.6 / 0.6 / 0). Collider and Rigidbody keys:
[../create-scene/reference/components.md](../create-scene/reference/components.md) (Physics).
Remember BoxCollider `size` is the FULL size, and MeshCollider needs its own
`<mesh>_collider.yaml` (static unless `convex: true`).

## Procedure

1. Reuse a shipped material if one fits; otherwise write `assets/physics_materials/<name>.yaml`
   with a header comment and only the keys that matter.
2. If it pairs with a render material, give both the same name.
3. Mention it in `assets/README.md` (the Materials section notes the shared physics names).

## Verify

Drop a dynamic object onto a surface using it in a scene and check behaviour over time.
`scenes/physics/physics_demo` already compares materials side by side (`bounce_*` drops,
`lane_*`/`slider_*` friction lanes) -- borrow that layout:

```sh
FRAMES=60  .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/phys_a.png"
FRAMES=180 .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/phys_b.png" debug_view=lines
./build/toyengine_tests caml_roundtrips_every_asset
```

`debug_view=lines` overlays collider wireframes and contacts. Physics groups in the test suite:
`./build/toyengine_tests --list | grep -i phys`.
