# physxcoopa/system (`coopa::physx::system`)

The `coopa::scene` adapter. Together with `components/` and `physx_yaml.h`, it is the only part of
physxcoopa that depends on `coopa::scene`. `PhysicsWorld` itself (`world.h`) never sees a `Scene`
reference.

```text
PhysicsSystem::execute(scene, ctx)
  1. regather_() + regather_joints_() + regather_cloths_()
                                -- only after install or refresh(): diff the scene's Collider set
                                   against last frame's (create/destroy bodies), then bind joints
                                   and cloths
  2. prime_transforms_()        -- serial pass so the parallel transform reads below are race-free
  3. update_changed_shapes_()   -- every bound collider's revision() checked for in-place param changes
  4. update_changed_rigidbodies_() -- live RigidbodyComponent fields (mass, drag, is_kinematic, ...)
  5. sync_transforms_in_()      -- kinematic velocity derivation, dynamic-body teleport detection
  6. world_.step(ctx.delta_time)
  7. write_transforms_back_()   -- interpolated (or raw) transform write, teleport-detection bookkeeping
  8. dispatch_events_()         -- fires each of this frame's ContactEvents on both Colliders involved
```

| File | Purpose |
|---|---|
| [`physics_system.h`](physics_system.h) | `PhysicsSystem : ISceneSystem` (registered at `UpdatePhase::Physics = 100` by default) and `install_physics_system(scene[, settings][, order])`. Also exposes scene-level `raycast()` / `raycast_all()` whose hits resolve to the `Collider`, `SceneObject` and `RigidbodyComponent`. Owns the `PhysicsWorld`; every method above is a private step of `execute()`. The reconcile pass runs `regather_()` → `regather_joints_()` → `regather_cloths_()` in that order: a joint needs both connected objects already bound to a body, and a `ClothComponent`'s anchors name objects whose `Rigidbody` must likewise already be bound. |
| [`vehicle_system.h`](vehicle_system.h) | `VehicleSystem` + `install_vehicle_system()` -- two stages. Order **96** (before Physics at 100) binds each `Vehicle` to its chassis body, moves its wheel anchors into the body's centre-of-mass frame, and holds one connection to `PhysicsWorld::on_substep` for the whole scene. Order **150** (after Physics) places the wheel visuals from the step just run and emits the frame's signals. |

**PhysicsSystem's transform reads/writes always use `Transform::get_world_matrix()`** (the
lazy-recomputing accessor), never the non-recomputing `world_matrix()` — physics runs
before `Animation`/`TransformResolve`/`LateBehaviour` all run this frame, so it cannot assume any
resolve pass has completed yet for objects it wasn't itself responsible for dirtying.

## Usage Example

```cpp
#include <physxcoopa/physx_yaml.h>

coopa::physx::register_physics_components(assets);      // once, alongside other component registrations
coopa::physx::system::install_physics_system(scene);     // once, after the scene's first load

// Per frame: scene.update(dt) already drives every installed ISceneSystem, physics included.
```
