# physxcoopa/hair (`coopa::physx::hair`)

Realtime hair: small-substep XPBD strands pinned at the root, which swing from the head they grow
on and rest on the shoulders below it. One-way coupled — a strand is pushed by the rigid world and
never pushes back, exactly as cloth is, and for a stronger version of the same reason: a 0.1 g hair
destabilising a 50 kg body would be absurd.

Only **guide** strands are simulated. A groom's thousands of render strands follow them, so the
cost here scales with the guide count (dozens to a few hundred), not with how much hair is drawn.
The engine side of that lives in [`toyengine/hair/`](../../../../toyengine/hair/README.md).

## Where it runs

```text
PhysicsWorld::step(dt)
  0..max_substeps x step_fixed(h)   -- rigid bodies only
  step_cloths_(dt)                  -- once per frame, against the poses the substeps produced
  step_strands_(dt)                 <-- HERE, last
```

Last, so a groom pinned to something a cloth is draped over still sees the frame's **final** rigid
poses. Nothing downstream reads the result, because the coupling is one-way.

**One clock.** Hair is advanced once per frame by that frame's `dt`, never from inside
`step_fixed()` — the same decision cloth makes and for the same three reasons, set out in
[`../cloth/README.md`](../cloth/README.md): it sees final poses, nothing reads it, and it is drawn
straight from its particles with no render interpolation, so solving it on the 1/60 s grid would
make it stutter against the kinematic head it is pinned to (a frame with zero substeps would freeze
the hair in world space while the head travelled; the next would run two and snap).

## One substep

```text
for s in 0..substeps:                     hs = h / substeps
  1. aerodynamics     wind drag -> per-particle acceleration
  2. integrate        v += a*hs; v *= 1/(1+damping*hs); prev = p; p += v*hs
  3. roots            pinned particles written from the strand's root, lerped across substeps
  4. stretch          XPBD distance constraints, batch by batch, `iterations` times
  5. global shape     toward the combed pose, carried by the live root frame
  6. local shape      restores each segment's rest angle to the one before it
  7. collisions       project out of every candidate rigid shape
  8. finalize         v = (p - prev)/hs, then Coulomb friction at recorded contacts
```

**Collisions run last of the position stages.** Stretch and both shape constraints can each push a
particle back into a body, so whichever runs last is the one that holds — and a strand visibly sunk
into a shoulder costs far more than a fraction of a millimetre of constraint violation. Same rule
cloth follows.

**The shape constraints run after stretch**, not before: they are soft blends toward a target and a
stiff distance projection afterwards would simply undo them.

Velocities are re-derived from positions at stage 8 rather than tracked through the projections.
That is the defining property of PBD — every correction automatically becomes a velocity change,
with no per-stage bookkeeping that could disagree with where the particles ended up.

### Collision candidates: broadphase, or an explicit allowlist

`StrandSet::collide_bodies` is empty by default, and `PhysicsWorld::step_strands_()` then queries
both broadphase trees for stage 7's candidates — the same query cloth uses. When the engine
(`toyengine/hair`'s `collide_with` key) fills it in instead, the broadphase is skipped entirely:
candidates come straight from `shapes_of()` on each named `BodyId`. Both paths apply the same four
guards (`shape.enabled && !is_trigger`, the layer matrix, an AABB overlap reject, and the
collider-moving wake test) — the wake test matters even on the explicit path, since a kinematic
body that moves without it would not wake a settled groom.

Stage 7 records the **deepest** contact's normal and depth for stage 8's friction, not the last
one in the candidate list — with two bodies touching one particle (a head resting against a neck,
say) last-wins order is arbitrary, while the deepest contact is the one actually holding the
particle in place. Every contact's position correction still applies, in order; only which normal
friction uses changes.

## What a strand needs that a sheet does not

This module reuses `cloth::ClothParticle`, `cloth::ClothConstraint`, `cloth::ConstraintBatch`,
`cloth::ClothCollider` and `cloth::project_particle()` **verbatim** — a chain pinned at one end is
most of what cloth already does, and the sphere/box/capsule/mesh projection is exactly what hair
wants. Two constraints are genuinely new, and they are why a strand is not a one-wide sheet:

### The global shape constraint

Pulls each particle toward its rest position carried by the strand's **current** root frame. It is
what makes a groom return to the shape it was combed into instead of hanging limp the first time
gravity touches it, and it doubles as the cheap stiffness knob.

`global_shape_falloff` fades it from root to tip, so hair swings from its ends. Without the falloff
a stiff groom moves as one rigid board.

### The local shape constraint

Restores the angle each segment makes with the segment before it. **This is what holds a curl, and
cloth's bend constraint cannot do it**: cloth bends by constraining the distance between particles
*i* and *i+2*, which preserves *straightness* — and a helix has the same such distance at every
angle, so a curl would unwind freely under it. Here the rest offset is carried by however much the
previous segment has actually turned (`rotation_between`), which commits to no frame convention and
so introduces no roll ambiguity.

The correction is split between the two particles by inverse mass, so it adds no net momentum; a
pinned root takes none of it.

## Roots are driven from outside

The one structural difference from a cloth anchor: a groom follows a **mesh**, not a rigidbody. So
`hair::set_strand_roots()` is called once per frame by the engine from the scalp's transform, and
`prev_root_*` keeps the previous target for the solver to lerp from across its substeps — without
which a head that turns quickly would yank every root in one whole-frame jump at the top of the
first substep and then hold still, which the distance constraints turn into a visible whip-crack.

## Parallelism and determinism

`make_strands()` emits stretch constraints in **two** `ConstraintBatch`es whose particle sets are
pairwise disjoint: a chain splits by index parity, and separate strands never share a particle at
all. A batch can therefore be solved in any order with a bit-identical result — the disjointness,
not merely thread safety, is the requirement, because `PhysicsWorld::world_state_hash()` must stay
reproducible.

The solve runs serially today. Guide counts are small by construction (that is the whole point of
simulating guides rather than render strands), so job-dispatch overhead would dominate at the sizes
this actually runs at — the same measurement that keeps cloth on `SerialDispatch`.

## Files

| File | Purpose |
|---|---|
| [`strand.h`](strand.h) | `StrandSetId`, `Strand`, `StrandParams`, `StrandSet`; `compute_bounds()`, `max_particle_speed()`. |
| [`strand_builder.h`](strand_builder.h) | `StrandDesc` + `make_strands()` (particles, constraints, the rest pose both shape constraints read, disjoint batches); `set_strand_roots()`. |
| [`strand_solver.h`](strand_solver.h) | `StrandSolverScratch` (allocate-once) + `solve_strands()`. Allocates nothing per step. |

## Deferred

Strand-vs-strand collision (grooms have no broadphase proxy and no self-collision), two-way
coupling, per-strand parameter variation, and cutting or growing a strand at runtime.
