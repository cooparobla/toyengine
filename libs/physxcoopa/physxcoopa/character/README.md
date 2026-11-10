# physxcoopa/character (`coopa::physx::character`)

A kinematic capsule character motor: pure algorithm over `PhysicsWorld`'s queries
(`shape_cast`, `compute_penetration`, `raycast`), with no scene types and no state between calls.
The caller keeps velocity, grounded state and timers; toyengine's `CharacterController` is one
such caller.

| File | Purpose |
|---|---|
| [`character_motor.h`](character_motor.h) | `move(world, centre, displacement, filter, MotorSettings, MoveOptions) -> MoveResult`. Depenetrates (deepest first), collides and slides the horizontal part (walkable slopes followed, steeper ones flattened into walls), retries a blocked move from `step_height` up (taken only onto a ledge no taller than that), slides the vertical part (steep slopes redirect it), then probes the ground and snaps down by `snap_distance` while grounded. `MoveResult` has the final centre, `grounded`, `ground_normal/point/body`, `ground_velocity` (a kinematic ground's velocity at the contact), `hit_ceiling`, `stepped`, `snapped`, `depenetration` and every `MotorHit` (point, normal, direction, body, `walkable`). |

Conventions: Z-up, capsule axis Z, the position is the capsule's centre, triggers never block,
and the filter should `ignore` the character's own body. A round-bottom contact on a ledge edge
counts as ground when a short raycast just past it finds walkable ground; the horizontal slide
does not use that rule, so the round bottom never rides up a riser taller than `step_height`.

Like every query, only valid between physics steps.
