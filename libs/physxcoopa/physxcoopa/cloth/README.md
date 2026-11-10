# physxcoopa/cloth (`coopa::physx::cloth`)

Realtime cloth: small-substep XPBD sheets that drape over rigid colliders and follow rigid bodies
they are pinned to. One-way coupled — a cloth is pushed by the rigid world, never the reverse
(the same choice Unity's `Cloth` makes, and what keeps a 1 g sheet from destabilising a body it
hangs on).

## Where it runs

```text
PhysicsWorld::step(dt)
  0..max_substeps x step_fixed(h)   -- rigid bodies only
  step_cloths_(dt)                  <-- HERE, once per frame, against the poses the substeps produced
```

Last, so every cloth collides against the frame's *final* rigid poses rather than stale ones —
which is what stops a sheet visibly lagging a body it is draped over. Nothing downstream reads the
cloth result, because coupling is one-way. `step_fixed()` on its own never advances cloth.

## One cloth substep

```text
for s in 0..cloth_substeps (4):          hs = h / cloth_substeps
  1. aerodynamics    per-triangle drag + lift -> per-particle acceleration
  2. integrate       v += a*hs; v *= 1/(1+damping*hs); prev = p; p += v*hs
  3. anchors         pinned particles written from their body's pose (lerped across substeps)
  4. constraints     stretch then bend, batch by batch, cloth_iterations (1) times
  5. tethers         one-sided clamp on distance to the nearest anchor
  6. self-collision  optional spatial-hash particle separation
  7. collisions      project out of every candidate rigid shape (incl. its swept volume)
  8. finalize        v = (p - prev)/hs, then Coulomb friction at recorded contacts
```

Rigid collision runs **last of the position stages**. Constraints, tethers and self-collision can
all push a particle back into a body, so whichever runs last is the one that holds; visible
interpenetration costs far more than a fraction of a millimetre of constraint violation. Self-
collision is the sharp case -- it can displace a particle by up to half of `self_distance`, which
would otherwise wipe out the entire `thickness` standoff with nothing left to re-project it.

## One clock

Cloth is advanced **once per frame, by that frame's `dt`** -- from `PhysicsWorld::step()`, after the
rigid substep loop, never from inside `step_fixed()`. One-way coupling is what makes that legal: no
rigid body ever reads a cloth, so nothing in the substep loop needs the result.

It is also what keeps a sheet from jittering. A cloth is drawn straight from its particle
positions, with none of the render interpolation a dynamic rigid body gets, while the kinematic
bodies sheets are actually pinned to are drawn wherever the wall clock put them this frame. Solving
on the 1/60 s grid would put those two on different clocks: every time the accumulator crossed a
substep boundary a frame would run zero substeps -- sheet frozen in world space while the body
travelled a full frame -- and the next would run two and catch up in a jump. On the frame clock the
sheet is always solved against the poses that frame draws, at any refresh rate.

The substep *count* per frame is sized to hold the internal substep length at `fixed_dt /
cloth_substeps` (1/240 s), so cloth cost per second and cloth feel are both frame-rate independent:
4 substeps at 60 Hz, 2 at 144 Hz, 8 at 30 Hz.

For the same reason the engine does not sweep colliders (inflate a moving collider into the volume
it would be drawn sweeping before the next solve): there is no un-solved drawn frame to cover.
`ClothCollider::sweep` and its exact swept-sphere projection are available for a direct caller of
`project_particle()`, but the engine leaves them at zero.

Velocities are re-derived from positions at stage 8 rather than tracked through the projections.
That is the defining property of PBD — every correction automatically becomes a velocity change,
with no per-stage impulse bookkeeping that could disagree with the positions.

## Why XPBD and not the impulse solver

1. **Stiffness.** Cloth wants to be near-inextensible. A velocity-level spring at that stiffness is
   the stiff-ODE case semi-implicit Euler handles worst. XPBD is unconditionally stable at any
   stiffness, including infinite (`stretch_compliance = 0`, the default).
2. **Resolution independence.** Compliance is a material parameter (m/N), so a 15×15 and a 45×45
   sheet authored identically hang identically. A raw spring constant does not survive a
   resolution change.
3. Unity's `Cloth` is NvCloth, which is PBD — so this is also the "mimic Unity" answer.

## Parallelism and determinism

`make_grid_cloth()` emits constraints in **`ConstraintBatch`es whose particle sets are pairwise
disjoint** (structural by index parity, shear by column parity, bend modulo 4). A batch can
therefore be solved in any order, on any thread, with a bit-identical result — the disjointness,
not just thread safety, is the requirement, because `PhysicsWorld::world_state_hash()` must stay
reproducible. `solve_cloth()` is templated on its work dispatcher so a `JobEngine`-backed one can
be dropped in.

It runs serially (`SerialDispatch`), because at realistic sheet sizes that measures
faster: a 25×25 sheet costs ~0.12 ms per 1/60 s of simulation serially, against which the parallel path
would pay job-dispatch overhead 30+ times per substep. Measured, single-threaded, `-O2`:

| Sheet | Particles | per 1/60 s step | with self-collision |
|---|---|---|---|
| 15×15 | 225 | 0.04 ms | — |
| 25×25 | 625 | 0.12 ms | 0.52 ms |
| 41×41 | 1681 | 0.35 ms | 1.77 ms |

Self-collision is ~4× the cost of everything else, which is why it is opt-in.

Mesh colliders are the expensive shape: a 25×25 sheet resting on a 2048-triangle mesh floor costs
0.52 ms/step against 0.12 ms on a sphere, dominated by the per-particle BVH proximity query. The
swept crossing test is a small part of that — forcing it to run every substep instead of honouring
its length gate costs only a further 0.07 ms — so the gate is worth keeping but the query is what
sets the price. A resting sheet also sleeps (`ClothParams::sleep_time`), dropping to ~0.

## Collider support

| Collider | How a particle is resolved | Interior case | `ClothCollider::sweep` (direct callers only) |
|---|---|---|---|
| `SphereCollider` | analytic centre distance | yes | **exact** — a swept sphere is a capsule |
| `BoxCollider` | `geometry::closest_point_on_obb` | yes, exits along the minimum-penetration axis | start/end pose probe |
| `CapsuleCollider` | `geometry::closest_point_on_segment` | yes | start/end pose probe |
| `MeshCollider` | `MeshBVH` + `collision::closest_point_on_triangle` | recovery only (see below) | no |

Static, Kinematic and Dynamic bodies all collide with cloth (both broadphase trees are queried), and
compound colliders work for free since the gather is per *shape slot*. A shape must be `enabled`,
must not be a trigger, and must pass `layer_matrix_.should_collide(cloth.params.layer, shape.layer)`
— so a scene's `ignore_layer_collisions` governs cloth too, via the `Cloth` component's `layer:`.

**Mesh colliders** are the one shape with no cheap "inside" to test against, so they get two extra
mechanisms instead:

- A **swept crossing test**: the particle's motion over the substep (`prev_position` → `position`,
  which covers integration, constraints, tethers *and* self-collision) is raycast against the BVH,
  and a crossing puts the particle back on the side it came from. Which side that is comes from the
  motion, not from a normal, so it carries none of the internal-edge ambiguity `mesh_contact.h`
  needs `TriangleAdjacency` to resolve for rigid contacts. Gated on the particle having moved
  further than `thickness` — it cannot have crossed a surface it is held that far clear of
  otherwise.
- **Penetration recovery**: a particle behind *every* triangle it is in contact range of is pushed
  out through the nearest one's front face. Requiring *every* one is what stops a particle sitting
  in a concave pocket — in front of one face, behind its neighbour — from being flipped across the
  mesh.

Still not handled for meshes: a particle that *starts* inside with no motion (an authoring error),
and a kinematic mesh collider that moves onto a sheet (the crossing test only follows the
particle's own motion).

**Not supported at all**: cloth-vs-cloth (cloths have no broadphase proxy; self-collision *within* a
sheet is supported), convex hulls (no such shape type exists in the engine), and triggers (skipped by
design). Anchored particles skip collision entirely, so a pin authored inside a collider stays there.

## Files

| File | Purpose |
|---|---|
| [`cloth.h`](cloth.h) | `ClothId`, `ClothParticle`, `ClothConstraint` (lambda accumulator on the struct, as `HingeJoint` does), `ClothTether`, `ClothAnchor`, `ClothParams` (Unity-shaped tunables), `ConstraintBatch`, `Cloth`; `compute_bounds()`, `max_particle_speed()`. |
| [`cloth_builder.h`](cloth_builder.h) | `GridClothDesc` + `make_grid_cloth()` (particles, structural/shear/bend constraints, disjoint batches, triangle list); `pin_to_body()`, `pin_static()`, `build_tethers()` (Dijkstra over the stretch graph — *geodesic* distance to the nearest anchor, not Euclidean). |
| [`cloth_collision.h`](cloth_collision.h) | `ClothCollider` pose snapshot + `project_particle()` — see the collider table above. Deliberately *not* through `narrowphase.h`: `ContactManifold` caps at 4 points, and a sheet on a sphere makes hundreds. |
| [`cloth_solver.h`](cloth_solver.h) | `ClothSolverScratch` (allocate-once workspace), `SerialDispatch`, `solve_cloth()`. Allocates nothing per step. |

## Authoring

`components/cloth.h` (`ClothComponent`) + the `"Cloth"` parser in `physx_yaml.h`; anchors name a
sibling object and are resolved by `PhysicsSystem::regather_cloths_()`, never by the parser (the
named object may not exist yet while parsing). Global substep counts come from
`PhysicsConfig::cloth_substeps` / `cloth_iterations`, settable from a scene's `physics.cloth:` block.

```yaml
- type: Cloth
  resolution: { x: 25, y: 25 }
  size: { x: 4.0, y: 4.0 }
  mass: 1.0
  bend_compliance: 0.00005
  thickness: 0.03
  friction: 0.5
  self_collision: true
  anchors:
    - object: ball
      point: { x: 0.0, y: 0.0, z: 1.0 }   # the target's LOCAL frame
      radius: 0.4
```

## Deferred

Two-way coupling (Unity's cloth is one-way by design), cloth-vs-cloth collision, tearing, a
`DebugDrawFlags::Cloth` bit, and importing an arbitrary (non-grid) mesh as a cloth. The solver
itself is topology-agnostic — only `make_grid_cloth()` and the self-collision 1-ring test assume a
grid — so an imported-mesh path is mostly a builder and a batch colouring pass.
