# physxcoopa/vehicle

A raycast-wheel vehicle model: suspension, tyres, a drivetrain, and six tunings that use them.

| File | Purpose |
|---|---|
| [`wheel.h`](wheel.h) | `WheelConfig` (anchor, radius, suspension rate and travel, which wheels steer / drive / brake) and `WheelState` (what the last substep computed: contact, compression, slip, forces, spin) |
| [`tire.h`](tire.h) | `TireConfig`, the slip curve and the friction circle. Pure functions over floats |
| [`drivetrain.h`](drivetrain.h) | `EngineConfig` torque curve, `GearboxConfig`, `DifferentialType`, `SteerMode`, and the automatic's shift decision |
| [`vehicle_sim.h`](vehicle_sim.h) | `VehicleConfig` / `VehicleControls` / `VehicleState` and `simulate()` -- one substep |
| [`vehicle_presets.h`](vehicle_presets.h) | `sports_car`, `pickup_4x4`, `hauler_6x6`, `buggy`, `dozer`, `trailer` |

The chassis is an ordinary dynamic body with an ordinary collider. The wheels are not bodies:
each is a ray from its chassis-local anchor, and its hit is the contact patch. Nothing here
touches a scene — `components/vehicle.h` and `system/vehicle_system.h` are the scene-facing half,
in the same relationship `character/character_motor.h` has to toyengine's `CharacterController`.

## One substep

`simulate()` runs from `PhysicsWorld::on_substep`, which fires after force integration and the
broadphase refit but **before the solve**. Two things follow from that, and both are load-bearing:

- Everything is applied as an **impulse** (force × h). A force added from a substep slot is not
  integrated until the next one, so it would arrive a substep late.
- The wheel raycasts see poses that are current and a broadphase that has just been refit, with
  only the velocities pre-solve. That is exactly the state a suspension cast wants.

Per wheel, in order: **steer** (rate-limited, speed-sensitive, Ackermann-split) → **cast** →
**suspend** (spring + damper + anti-roll along the contact normal, never pulling) → **grip**
(slip ratio and slip angle through the tyre curve and friction circle, scaled by the ground
material's friction) → **spin** (drive torque − brakes − the tyre force's reaction − rolling
resistance). Then once per vehicle: drag, downforce, and an optional righting torque while
airborne.

The continuous impulses deliberately do **not** wake a sleeping chassis, so a parked vehicle stays
parked; `simulate()` wakes the body itself as soon as a control input is non-zero.

## Recovery

`VehicleControls::recover` rights a vehicle that has ended up on its roof. It is not a teleport:
for `recover_time` the chassis floats `recover_lift` clear of whatever it is lying on and turns
back upright **about the heading it already had**, then its suspension catches it. Both the
rotation and the lift are exponential approaches (`velocity = error / tau`, applied as an impulse
that sets the velocity outright), so it moves fastest while furthest from upright and eases as it
arrives — and it is a real rigid body throughout, colliding on the way. Every wheel reports
airborne for the duration and no drive, brake or tyre force is applied.

`auto_recover_time` does the same thing unasked after that many seconds upside down and nearly
stationary; 0, the default, never does — recovery is the driver's to ask for.

## Which dial to reach for

| Want | Change |
|---|---|
| it rides too low / bottoms out | `natural_frequency` up (it derives the spring rate from the corner's real load) or `travel` up. A corner should settle near 40% of its travel |
| it pogoes after a kerb | `rebound_ratio` up, above `damping_ratio` |
| it rolls over in corners | `anti_roll` up — but keep it well under the wheel rate, and lower the chassis' centre of mass first |
| it won't turn in | `tire.peak_slip_angle` down, or `max_steer_deg` up |
| it snaps into oversteer | `tire.plateau` up (more grip left once sliding) and `peak_slip_ratio` up |
| it spins its wheels instead of moving | lower gearing (`final_drive` up), or a `Locked` differential |
| it has no top speed | `drag` up, or shorter gearing. The rev limiter also sets one: `engine_torque()` falls to nothing just past `max_rpm` |
| a jump lands on its roof | `air_righting` up |
| it tips over in ordinary corners | **the chassis' centre of mass is too high** — this is a `Rigidbody` property, not one of these. A vehicle rolls once lateral acceleration passes `half-track / centre-of-mass height`, so set `center_of_mass` well below the collider's centre and aim for a ratio of 1.1–1.4. Stiffening `anti_roll` only changes how much it *leans* on the way over |
| it ends up on its roof | `recover_time` / `recover_lift` tune the righting; `auto_recover_time` does it unasked |

## Limits worth knowing

- **A wheel is one ray, not a swept cylinder.** It can clip the lip of an obstacle narrower than
  its own width. `width` only widens the reported contact patch for debug drawing. A sphere-cast
  wheel mode would fix it (`PhysicsWorld::sphere_cast` already exists) and is not implemented.
- **No terrain deformation.** Mud is a friction and rolling-resistance surface
  (`PhysicsMaterial::rolling_resistance`), not a rut that remembers being driven through.
- **No joint motors in physxcoopa**, so a blade, bucket or crane arm is static geometry parented
  to the chassis, not something the driver can raise.
- **A trailer is a second vehicle** with no drive share and an empty gear list, joined by an
  ordinary `HingeJoint`. There is no tow-hitch type.
- **A parked vehicle sleeps, and a sleeping chassis is skipped entirely.** It has to be: the
  integrator ignores a sleeping body, so suspension impulses applied to one are never integrated
  or damped and simply pile up in its velocity field (a vehicle standing still for ever ends up
  reporting most of a metre per second of climb). Driver input wakes it. Anything else that moves
  it or removes the ground under it must wake it too — `RigidbodyComponent::wake()` — since the
  chassis collider is not touching the ground and so has no contacts to wake it through.
- Engine audio, tyre smoke and skid decals are not here; `VehicleState`'s transition fields
  (`gear_changed`, `landing_impact`, `slip_started_mask`) are what a host hangs them off.

## Traps this model walked into

Recorded because each one reads as "the tuning is wrong" rather than as the bug it is.

- **The damper's sign.** With `axis` pointing down the chassis, the suspension compresses when the
  hub moves *along* it. Negating that makes the damper pump energy in, and every vehicle launches
  itself off the ground on the first contact.
- **The impulse clamp has to include the wheel.** The bound on the longitudinal impulse is the one
  that nulls the slip, `|rim − v| / (1/m + r²/I)`. Leave out the `r²/I` term and a heavy wheel's
  reaction torque overshoots every substep: the spin flips sign at 60 Hz, the tyre force averages
  to nothing, and the vehicle sits at full throttle going nowhere.
- **An unbounded anti-roll bar diverges.** Transferring load from the compressed wheel to the
  extended one is right, but with no cap it drains the loaded side past zero, rolls the chassis
  further, and runs away. It also must not act when one of the pair is in the air.
- **Free-revving through a gear change makes an automatic run away up its gearbox.** The next
  shift decision reads those free revs, upshifts, and repeats. Keep the engine coupled to the
  wheels; a change cuts torque, it does not disconnect the engine.
- **Clamping the revs at the redline removes every gear's top speed.** The engine stays pinned at
  full torque and the vehicle accelerates past any gearing. Let the rpm exceed `max_rpm` and let
  the torque curve's roll-off be the limiter.
- **Engine braking must oppose the wheel, not the gear.** Folding drive and braking into one
  signed number is wrong wherever the wheel's rotation disagrees with the gear — most sharply
  under differential steering, where it drove a parked dozer's tracks backwards at idle.
- **An automatic shifting on wheel speed hunts itself to a standstill.** Engine speed is derived
  from the driven wheels, so a locked-differential truck spinning all six wheels off the line
  reports a redline it is nowhere near. It upshifts on that, the change cuts torque for
  `shift_time`, the wheels slow, the revs fall through the downshift threshold, it drops back
  down, and it repeats — spending nearly all of its time mid-change. The hauler took 10 s to
  reach 10 m/s like that, against 4 s once the decision read **road** speed (what a real
  transmission's output shaft measures) and gained a `shift_cooldown` dwell. It read exactly like
  an engine with no power, and towing a trailer made it look like a towing bug.
- **Mass at the collider's geometric centre tips every tall vehicle over.** The rollover threshold
  is `half-track / centre-of-mass height` in g, and a box whose mass sits at its middle lands
  around 0.6–0.75 — under what any tyre here delivers, so the pickup flipped on a flat skidpad at
  12 m/s and the hauler at any speed at all. Real vehicles carry their mass low. `Rigidbody`
  always had `center_of_mass_override`; nothing could reach it from YAML until now.
- **A tracked machine needs a hydrostatic drive and lateral grip below 1.** Geared to its tracks,
  a pivot turn reads as 13,000 rpm and the limiter kills all torque. And skid steering *is*
  dragging both tracks sideways: rollers fore and aft of the centre resist a yaw on a longer arm
  than the drive pushes on, so lateral grip above the longitudinal limit means it cannot turn at
  all, however much torque the two sides fight each other with.
