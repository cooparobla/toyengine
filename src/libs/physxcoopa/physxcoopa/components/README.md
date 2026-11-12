# physxcoopa/components (`coopa::physx::components`)

Unity-equivalent `coopa::scene::Component`s. All passive descriptor data — none of them creates
or destroys a physics body itself; `system::PhysicsSystem`'s reconcile pass is the only thing that
does, diffing the scene's current `Collider` set against last frame's rather than rebuilding on
every change (a collider owns a live broadphase proxy, warm-start impulses and a sleep timer that
must survive an unrelated collider's parameter change elsewhere in the scene).

| File | Purpose |
|---|---|
| [`collider.h`](collider.h) | `Collider` (base) — `is_trigger`/`layer`/`center`/`material`, the six `on_collision_*`/`on_trigger_*` signals, a `revision_` counter bumped by every setter (what `PhysicsSystem` diffs against to know a shape needs rebuilding), and the pure-virtual `make_shape(world_scale)`. |
| [`box_collider.h`](box_collider.h) | `BoxCollider` — `size` (full extents, Unity semantics; half-extents internally). |
| [`sphere_collider.h`](sphere_collider.h) | `SphereCollider` — `radius`. |
| [`capsule_collider.h`](capsule_collider.h) | `CapsuleCollider` — `radius`/`height`/`direction` (default `2`/Z, not Unity's Y, given this world is Z-up). |
| [`mesh_collider.h`](mesh_collider.h) | `MeshCollider` — `mesh` (an `AssetHandle<TriangleMesh>`) + `convex` (stored but not implemented: non-convex mesh colliders are static or kinematic only, matching Unity, and binding one to a dynamic `Rigidbody` throws). A dynamic body needs a hull: use `ConvexCollider` instead, a deliberately separate component since the two invert their dynamic-body legality. |
| [`convex_collider.h`](convex_collider.h) | `ConvexCollider` — legal on a dynamic `Rigidbody`, unlike `MeshCollider`. Either `set_hull_mesh()` (an `AssetHandle<TriangleMesh>`; the hull is built from its vertices lazily, once, cached against the loaded mesh's identity) or `set_hull(shared_ptr<const ConvexHull>)` for a runtime-built hull (e.g. a slice/fracture piece in its own principal frame). SAT-only downstream (`collision/sat_hull.h`) — no GJK/EPA. |
| [`rigidbody.h`](rigidbody.h) | `RigidbodyComponent` — mass/drag/`is_kinematic`/`interpolation`/`constraints`, plus the gameplay-facing `add_force()`/`add_torque()`/`add_impulse_at_position()`/`velocity()`/`velocity_at_point()`/`move_position()`/`move_rotation()` API, and `local_center_of_mass()`/`world_center_of_mass()`/`rotation()` -- the live body pose a substep callback needs to place points authored in the owner's frame (e.g. buoyancy pontoons). Only takes effect alongside a `Collider` on the same object or a descendant (descendants' colliders form one compound body) — PhysicsSystem's reconcile is keyed on `Collider`, so a bare `Rigidbody` with no `Collider` is a known limitation (reachable via `PhysicsWorld::add_body()` directly, just not from scene YAML). Its fields are re-read every frame, except the creation-time-only `initial_velocity`, `initial_angular_velocity` and `center_of_mass_override`. |
| [`hinge_joint.h`](hinge_joint.h) | `HingeJointComponent` — `connected_object`, one `anchor`/`axis` in this object's local frame (the connected side is derived at bind time, like Unity's auto-configured connected anchor), optional `limits` in degrees. Resolved by `PhysicsSystem::regather_joints_()`; creation-time only. |
| [`joint_component.h`](joint_component.h) | `JointComponent` -- the base every joint component shares: `connected_object`, `anchor`, `enable_collision`, the bound `joint_id()`. |
| [`ball_joint.h`](ball_joint.h) | `BallJointComponent` -- ball-and-socket at `anchor`. |
| [`cone_twist_joint.h`](cone_twist_joint.h) | `ConeTwistJointComponent` -- ball joint plus `swing_limit_deg` cone about `axis` and a `twist_min_deg`/`twist_max_deg` range. Like the hinge, the connected side is derived at bind time and the bind pose is the rest pose. |
| [`collision.h`](collision.h) | `Collision` — the payload of `on_collision_*`: the other `collider`/`object`, `normal`, `relative_velocity`, `impulse`, and up to 4 `contacts`. |
| [`damage_dealer.h`](damage_dealer.h) | `DamageDealer` -- put on a Collider (a weapon head, a bullet, a falling weight) to say what kind of blow it deals, so an axe reads as `damage::DamageType::Slash` and not the generic Impact a bare contact would otherwise read as. Consumed by `toyengine::scene::Damageable`, one layer up; see [`../damage/README.md`](../damage/README.md). |
| [`cloth.h`](cloth.h) | `ClothComponent` — a rectangular XPBD sheet built in the owner's local XY plane at bind time; every `cloth::ClothParams` field is authorable flat, plus `ClothAnchorSpec` pinned regions naming a sibling object. Resolved by `PhysicsSystem::regather_cloths_()`, never by the YAML parser (the named object may not exist yet while parsing). Binding is creation-time only; the runtime tunables (wind, friction, damping) are live through `cloth_mut()`. |
| [`fixed_update_behaviour.h`](fixed_update_behaviour.h) | `FixedUpdateBehaviour` — subclass and override `fixed_update(world, h)` for a per-substep callback, without adding a `Component::fixed_update()` hook to libcoopa itself; connects to `PhysicsWorld::on_substep` lazily on first `update()`. |
| [`vehicle.h`](vehicle.h) | `VehicleComponent` (`Vehicle`) -- a driveable vehicle on this object's Rigidbody chassis. `preset:` picks a tuning from [`vehicle/vehicle_presets.h`](../vehicle/README.md) and every other key overrides one of its values; `wheels:` names the child object drawn at each wheel. Controls are pushed in (`throttle`, `brake`, `steer`, `handbrake`), live state is read back (`speed_kmh()`, `gear()`, `engine_rpm()`, `is_slipping()`), and `on_gear_change` / `on_landed` / `on_wheel_slip` are the hooks for audio and VFX. Inert until `install_vehicle_system()`. |

## Usage Example

```cpp
auto* col = object.add_component<coopa::physx::components::BoxCollider>();
col->set_size(glm::vec3(1.0f));
col->set_is_trigger(true);
col->on_trigger_enter.connect([](auto& self, auto& other) {
    // ...
});

auto* rb = object.add_component<coopa::physx::components::RigidbodyComponent>();
rb->mass = 2.0f;
```
