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
  1.  aerodynamics    per-triangle drag + lift -> per-particle acceleration
  2.  integrate       v += a*hs; v *= 1/(1+damping*hs); prev = p; p += v*hs
  3.  anchors         pinned particles written from their body's pose (lerped across substeps)
  3b. skin pins       skin-driven particles written from their skinned targets (skin cloth only)
  4.  constraints     stretch, shear, bend, anim-drive, batch by batch, cloth_iterations (1) times
  5.  tethers         one-sided clamp on distance to the nearest anchor
  5b. skin limits     max-distance clamp + backstop push around the skinned targets (skin cloth only)
  6.  self-collision  optional spatial-hash particle separation
  7.  collisions      project out of every candidate rigid shape (incl. its swept volume),
                      re-swept per particle until it is clear of all of them (4 passes max)
  7b. surface         the same projection for points BETWEEN particles (see below)
  7c. collisions      stage 7 again, so a PARTICLE has the last word (see below)
  8.  finalize        v = (p - prev)/hs, then Coulomb friction at recorded contacts
```

**Stage 7b is why a collider does not show through the sheet.** Collision projects *particles*,
which are points, so a sheet whose every particle sits exactly at the standoff still draws
triangles that chord across a convex collider and dip below it — a 25×25 sheet over a 1 m ball
sags ~8 mm mid-chord, most of a 30 mm `thickness`, and all of it once the sheet stretches.
`collide_surface` (on by default) adds a `ClothSurfaceSample` at every unique edge midpoint and
every triangle centroid; each is projected like a particle and its correction distributed back
to the three real ones by mass-weighted barycentric weight. This is NvCloth's "virtual
particles", and it costs ~3× the collision stage against the alternative of subdividing.

The `3b`/`5b` stages and the anim-drive pass exist only for **skin cloth** — a cloth whose
`Cloth::skin` carries painted per-particle channels (`max_distance`, `backstop_distance`/`_radius`,
`anim_drive`) and whose `Cloth::skin_targets` are written each frame by the caller from a skinned
mesh's current pose. A grid cloth leaves `skin` empty and takes exactly the original path. See
`ClothSkinChannels`' doc for channel semantics (they track Unreal's cloth paint maps).

Rigid collision runs **last of the position stages**. Constraints, tethers and self-collision can
all push a particle back into a body, so whichever runs last is the one that holds; visible
interpenetration costs far more than a fraction of a millimetre of constraint violation. Self-
collision is the sharp case -- it can displace a particle by up to half of `self_distance`, which
would otherwise wipe out the entire `thickness` standoff with nothing left to re-project it.

Stage 7b is subject to that same rule, which is why stage **7c** repeats stage 7 after it: 7b
guarantees its sample POINTS are outside the collider, not the particles it moved to put them
there, and a shared correction can carry a corner into a neighbouring collider. A particle inside
a collider is a mesh *vertex* inside it — the most visible failure a garment has — while repeating
stage 7 gives back only a fraction of a triangle-interior standoff.

**One pass over the collider list is not enough when colliders overlap**, and a body is nothing
but overlapping colliders: a garment meets a skeleton of capsules that overlap at every joint, so
pushing a particle out of the thigh regularly puts it inside the shin, already visited and never
revisited. Substepping cannot save it — every substep repeats the same mistake. Stage 7 therefore
re-sweeps each particle against the whole set until that particle is clear, bounded by
`k_collider_passes` (4). The cost falls only on particles actually in contact, since a pass that
corrects nothing breaks the loop, and a particle touching nothing is decided by the AABB rejects
alone. Measured on the character garment demo over a 17-capsule skeleton: the single pass left up
to **4.2 cm** inside a shin across six different bones; the re-sweep leaves 0 over 150 frames
(`sheet_clears_every_one_of_a_chain_of_overlapping_colliders`, and toyengine's
`demo_jacket_never_enters_the_body`).

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

For the same reason `step_cloths_()` does not sweep colliders (inflate a moving collider into the
volume it would be drawn sweeping before the next solve): there is no un-solved drawn frame to
cover. `ClothCollider::sweep` is for callers whose colliders move between solves for a different
reason — notably toyengine's `ClothMeshSystem`, whose bone colliders are re-posed by a rig every
frame and can travel several centimetres in one, so a garment needs the volume a limb crossed and
not just the two poses' endpoints.

Velocities are re-derived from positions at stage 8 rather than tracked through the projections.
That is the defining property of PBD — every correction automatically becomes a velocity change,
with no per-stage impulse bookkeeping that could disagree with the positions.

## Why XPBD and not the impulse solver

1. **Stiffness.** Cloth wants to be near-inextensible. A velocity-level spring at that stiffness is
   the stiff-ODE case semi-implicit Euler handles worst. XPBD is unconditionally stable at any
   stiffness, including infinite (`stretch_compliance = 0`, the default).
2. **Resolution independence.** Compliance is a material parameter rather than a spring constant,
   so a 15×15 and a 45×45 sheet authored identically hang identically — but only because every
   compliance here is **per unit mass**: `project_distance()` multiplies it by the constraint's own
   inverse mass, so the fraction of a violation left standing after one iteration is
   `α / (1 + α)` with `α = compliance / h²`, whatever the particles weigh. Without that division a
   compliance in m/N is measured against `1/m`, and a cloth particle is 1.6 g — every value small
   enough to read as "stiff" came out rigid to four decimal places, which is why a 4 m sheet used to
   settle over the ball demo as a taut cone with 0.6 m of air under it.
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

**Those figures are the solver alone and predate `collide_surface`.** What a scene actually pays is
higher, because both optional stages are on in `cloth_demo`. Measured 2026-11-18 as
`cpu.scene.frame_profile`'s `cpu.scene.physics` over that scene, 400 headless frames at
`FIXED_DT=1/60` (4 substeps), changing one key at a time:

| Config | `cpu.scene.physics` | Attributable to |
|---|---|---|
| as shipped | **0.959 ms** | — |
| `collide_surface: false` | 0.739 ms | surface samples: **0.22 ms (23%)** |
| `self_collision: false` | 0.410 ms | self-collision: **0.55 ms (57%)** |
| both off | 0.193 ms | the solver core: 20% |

**Self-collision, not the surface samples, is the bill** — which is the opposite of what the sample
counts suggest (2928 surface samples against 625 particles). The samples are cheap because the
per-collider AABB reject decides most (sample, collider) pairs in six compares, while stage 6 is a
serial 27-cell hash scan over every particle every substep with no early-out at all.

The cost is also flat for the whole run on that scene: `wind_turbulence: 0.7` keeps peak particle
speed above `sleep_threshold` forever, so the sleep gate never engages and the sheet never drops to
~0. That is authored deliberately there, but it means the demo pays full price always.

Mesh colliders are the expensive shape: a 25×25 sheet resting on a 2048-triangle mesh floor costs
0.52 ms/step against 0.12 ms on a sphere, dominated by the per-particle BVH proximity query. The
swept crossing test is a small part of that — forcing it to run every substep instead of honouring
its length gate costs only a further 0.07 ms — so the gate is worth keeping but the query is what
sets the price. A resting sheet also sleeps (`ClothParams::sleep_time`), dropping to ~0.

## Collider support

| Collider | How a particle is resolved | Interior case | `ClothCollider::sweep` (direct callers only) |
|---|---|---|---|
| `SphereCollider` | analytic centre distance | yes | **exact** — a swept sphere is a capsule |
| `BoxCollider` | `geometry::closest_point_on_obb` | yes, exits along the minimum-penetration axis | start pose then **end pose** |
| `CapsuleCollider` | `geometry::closest_point_on_segment` | yes | start pose then **end pose** |
| `MeshCollider` | `MeshBVH` + `collision::closest_point_on_triangle` | recovery only (see below) | no |
| `ConvexCollider` | `collision::hull_vs_sphere` with the particle as a radius-`thickness` probe | per that function's single-most-violated-face approximation | no |

A box or capsule with a `sweep` is projected out of the start pose **and then** the end pose, in
that order — not out of whichever of the two is deeper. Only the end pose is drawn, so only its
surface is the one the result has to be outside of; stopping at a deeper start-pose correction
leaves the particle clear of where the shape *was* and inside where it now is. On a fast rotating
collider (a shin through a walk cycle) that shows up as cloth sinking in on the handful of frames
at peak speed and nowhere else. Chaining keeps the sweep's push — the start pose still moves the
particle ahead of the shape — while making the drawn pose authoritative
(`swept_capsule_leaves_the_particle_outside_the_end_pose`).

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
sheet is supported) and triggers (skipped by design). Anchored particles skip collision entirely, so
a pin authored inside a collider stays there.

## Files

| File | Purpose |
|---|---|
| [`cloth.h`](cloth.h) | `ClothId`, `ClothParticle`, `ClothConstraint` (lambda accumulator on the struct, as `HingeJoint` does), `ClothTether`, `ClothAnchor`, `ClothSkinChannels` + `ClothSkinTargets` (skin cloth), `ClothParams` (Unity-shaped tunables), `ConstraintBatch`, `Cloth`; `compute_bounds()`, `max_particle_speed()`. |
| [`cloth_builder.h`](cloth_builder.h) | `GridClothDesc` + `make_grid_cloth()` (particles, structural/shear/bend constraints, disjoint batches, triangle list); `pin_to_body()`, `pin_static()`, `build_tethers()` / `build_tethers_from_pinned()` (Dijkstra over the stretch graph — *geodesic* distance to the nearest seed, not Euclidean). |
| [`cloth_mesh_builder.h`](cloth_mesh_builder.h) | `MeshClothDesc` + `make_mesh_cloth()`: cloth from an arbitrary welded triangle mesh — edge/apex constraints, deterministic greedy-colouring batches, area-weighted masses, painted-channel pinning. The character-clothing path. |
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
  bend_compliance: 0.005
  thickness: 0.03
  friction: 0.5
  self_collision: true
  anchors:
    - object: ball
      point: { x: 0.0, y: 0.0, z: 1.0 }   # the target's LOCAL frame
      radius: 0.4
```

## What makes a MOVING collider work

A cloth is solved once per frame, in substeps, against colliders the caller snapshots. Two things
about that snapshot are load-bearing, and both were wrong until a driven ball was actually
measured rather than looked at:

**The collider has to arrive, not teleport.** `ClothCollider` spans the step -- `position` at the
top of it, `position + sweep` at the pose the frame draws -- and stage 7 advances it one substep
at a time. Holding it at the end pose for all of them means a body doing 4 m/s is 6.7 cm inside
the sheet on substep one, and since PBD reads velocity back out of position (`(p - prev) / hs`),
removing that intrusion inside a 1/240 s substep reports **16 m/s of cloth**. Measured on the ball
demo: 13.7 m/s on the first frame the ball moved, and the sheet gone from the ball within two
seconds. `cloth_solver_test`'s `a_moving_collider_arrives_instead_of_teleporting` pins the
difference at 1.8 m/s swept against 6.8 m/s teleported.

**Friction opposes RELATIVE sliding.** A sheet lying on a ball driven at 4 m/s has 4 m/s of world
velocity and none relative to the ball. Measuring the contact in the world frame makes friction
brake it toward world-zero -- a 2.4 m/s-per-substep tangential force scrubbing the fabric off the
back of the ball, which took the ball demo's upper-cap coverage from 91% to 10%. `ClothCollider`
therefore carries the body's linear and angular velocity, and stage 8 resolves both the inelastic
normal clamp and Coulomb friction against the surface's own motion.

## What makes a sheet drape

A flat sheet cannot be laid on a sphere isometrically (Gauss), so conforming to one means losing
circumference -- 36% of it at the equator -- and **where that length is lost decides what the
result looks like**. Three parameters, measured on the ball demo as the fraction of the ball's
upper cap the sheet ends up lying against, and the mean strain left in its threads:

| in-plane model | `bend_compliance` | lies on the ball | thread strain |
|---|---|---|---|
| rigid threads, rigid shear | 5e-5 | 24% | 0% |
| rigid threads, rigid shear | 5e-3 | 36% | 0% |
| threads free to SHORTEN | 5e-3 | 86% | **-12%** |
| rigid threads, soft SHEAR | 5e-3 | **56%** | **0%** |

The third row is the trap, and it is why `compression_compliance` defaults to 0. Letting the
threads shorten "so the fabric can buckle" buys the best-looking contact number in the table and
is completely wrong: a distance constraint that shortens does not buckle, it SHRINKS. The sheet
conforms by wrapping itself tighter and tighter onto the ball, its hem creeping up above the
equator until the ball is bare underneath. It is a creep, invisible in one frame and obvious after
ten seconds.

The fourth row is the real mechanism. A weave scissors -- warp and weft rotate toward each other
while every thread keeps its length -- and that is where the circumference goes, along with the
folds soft bending allows. 56% is not worse than 86%; it is the geometric optimum for this scene,
and `sheet_drapes_onto_the_ball` derives the bound rather than asserting a number: the sheet is
pinned over a flat disc 5 cm above the crown, so from that rim it must run TANGENT to the ball
before it can touch it, and roughly the upper half of the cap band is that tangent run.
`the_sheet_folds_rather_than_shrinking` holds the other end, on the threads.

**Bending** still matters as much as it did: the excess has to go into folds, and a sheet that
cannot crease cannot fold. The useful band is narrow -- by 5e-3 there is essentially no bending
resistance left to remove.

## Deferred

Two-way coupling (Unity's cloth is one-way by design), cloth-vs-cloth collision, tearing, and a
`DebugDrawFlags::Cloth` bit. Arbitrary-mesh cloth is `cloth_mesh_builder.h`; one grid-ism remains
there: the self-collision 1-ring exemption is an index test that only applies to grids, so a mesh
cloth relies on `self_distance` staying below its shortest edge (the builder's default) to avoid
fighting its structural constraints.
