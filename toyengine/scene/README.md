# toyengine/scene

toyengine-specific scene components, registered as `coopa::scene::SceneLoader` parsers
alongside gfxcoopa's (`register_render_components()`).

| File | Purpose |
|---|---|
| [`camera_controller.h`](camera_controller.h) | `CameraController` — Orbit (spherical yaw/pitch/distance around a fixed `target` or a named `tracker` object followed with `follow_smoothing`), Fly (WASD/E/Q along the world matrix's basis vectors, arrow keys to look) or FirstPerson (at the tracked object's `eye_height`; mouse yaw also turns the object — `CharacterController::set_facing_yaw()` when it has one). A *tracking* Orbit camera collides (`collide`, default on): a `collision_radius` sphere cast from the target toward the camera, ignoring the tracked object's subtree, pulls it in (`collision_in_speed`) and it eases back out (`collision_out_speed`). |
| [`character_controller.h`](character_controller.h) | `CharacterController` — a walking capsule character over physxcoopa's `character::move()` motor (depenetrate, collide-and-slide with skin, step up to `step_height`, `slope_limit`, ground snap). Owns velocity/acceleration (`move_speed`, `sprint_multiplier`, `acceleration`, `air_control`), gravity (`gravity_scale` × the world's), jumps (`jump_height` apex, `coyote_time`, `jump_buffer`), facing (`face_movement`, `turn_speed`), pushing dynamic bodies (`push_strength` = pushing mass in kg) and riding whatever it stands on (by the ground object's exact Transform delta this frame). Input is pushed: `move_input` relative to `move_basis_yaw_deg` (the engine writes the main camera's yaw), one-shot `jump`, held `sprint`. Signals `on_landed(impact_speed)`, `on_jumped`, `on_collision(CharacterHit)`. `use_root_motion` makes it move by an Animator's root motion (it is an `IRootMotionReceiver`: an Animator with `apply_root_motion` on the same object hands it each frame's clip travel, which still collides; `move_input` then only steers). Adds its own kinematic Rigidbody + CapsuleCollider (centre at `height/2`; the origin is the feet) when missing. Put it on a root object. It is an `ISaveable` (`toyengine/save/`): with a `SaveId` on its object, saves keep its position and facing. |
| [`character_anim_driver.h`](character_anim_driver.h) | `CharacterAnimDriver` — demo gameplay glue: crossfades the Animator (on the object or under it) between `idle_state` / `walk_state` / `run_state` / `jump_state` by the controller's speed (`walk_speed`, `run_speed`) and air time -- with `use_root_motion`, by the requested speed (input × `move_speed`). The "hardcode it per game" alternative to state machines. |
| [`scene_link.h`](scene_link.h) | `SceneLink` — a level-change trigger: when a `CharacterController` enters the box of `size` around the object (a physics overlap each frame; kinematic bodies form no trigger pairs with static colliders), it posts a `SceneLinkRequest` the Engine turns into `load_scene_async(target_scene)` with the `transition` (`fade` / `loading_screen` / `none`), `fade_time`, `color`, `loading_screen`, `min_display_time` and `spawn_point`. Fires on entry only (a player arriving inside a link must leave it first), once per scene run; `trigger()` fires it from code. See `toyengine/core/README.md`. |
| [`foot_ik.h`](foot_ik.h) | `FootIK` — a `coopa::anim::IkDriver` (solved by `IkSystem` at order 320, after the Animator): rays below each foot (`ray_up` above / `ray_down` below the base plane, ignoring the object's own Rigidbody) find the ground; the pelvis drops by the lower foot's offset (`max_pelvis_drop`), two internal `TwoBoneIK` solves (knees toward the facing) plant the feet with the clip's lift kept, and `align_feet` tilts them to the ground normal (`max_foot_angle`). Fades out (`blend_speed`) while a `CharacterController` on the object is airborne. Bone paths default to the mannequin's. On a rig whose legs are already IK-driven (`TwoBoneIK` components ending at its feet -- the fully IK `mannequin_ik`) `mode: auto` switches to **targets**: it probes under each leg's target and overrides that target's position instead of solving the legs itself (`mode: solve` / `targets` force one). |
| [`ragdoll.h`](ragdoll.h) | `Ragdoll` + `install_ragdoll_system()` — jointed physics bones on a rig root. `bones:` lists each bone's path, shape (`capsule` / `box` / `sphere` with `radius`, `height`, `direction`, `size`, `center`), `mass` and its joint to the nearest ancestor bone (`joint: cone_twist / hinge / ball`, `anchor`, `axis`, `swing`, `twist_min` / `twist_max`, `limit_min` / `limit_max`, degrees); `auto_generate: true` fits capsules between the rig's empties instead. start() adds a Rigidbody + Collider per bone; the joints are built from the BIND pose once the bodies exist. Modes: **Animated** (bones kinematic, following the Animator -- hits register), **Ragdoll** (`activate(impulse, point)`: bones dynamic, the Animator cleared, IK under the rig suspended, a `CharacterController` on the root suspended with its capsule off) and **Blending** (`deactivate(blend_time, state)`: the root stands up under the pelvis facing the way the body lies, the Animator plays `state`, and every bone's world pose blends from the fallen pose into the animation). Jointed pairs and bones within two links never collide (`collide_connected` re-enables the jointed ones). A limp body that stays calm for `rest_time` is put to sleep (`rest_speed` / `rest_spin`). `input_toggle` reacts to R (`Engine::drive_ragdolls_()`), with `toggle_impulse`; `mode: ragdoll` + `start_impulse` starts limp. Stages: order 90 (mode changes, before Physics), 290 (bind pose under the Animator during a blend), 330 (the blend, after Animation and IK). |
| [`kinematic_mover.h`](kinematic_mover.h) | `KinematicMover` — scripted motion for a kinematic Rigidbody: `pingpong`, `orbit` or `spin`. Only ever writes the owner's Transform; physxcoopa derives the body's velocity from the frame-to-frame delta. |
| [`free_mover.h`](free_mover.h) | `FreeMover` — input-driven movement along all three **world** axes (not the owner's basis), for an object that is not a physics body. The three-axis, physics-free sibling of `KinematicController`: it does its work in the ordinary `update()`, since nothing downstream needs the pose early. On an object with no renderer it is an invisible, steerable point — which is what a camera's orbit `tracker` follows and what the lens focuses on via `focus_object` (see `assets/scenes/terrain/terrain_demo`). |
| [`kinematic_controller.h`](kinematic_controller.h) | `KinematicController` — the input-driven counterpart to `KinematicMover`: moves the owner across the world XY plane from a pushed-in `move_input`, holding its authored height. Pairs with `Rigidbody { is_kinematic: true, use_gravity: false }`. Velocity is smoothed exponentially (`1 - exp(-k*dt)`), not because of feel but because PhysicsSystem derives the body's velocity from the Transform delta — an instant start/stop jolts everything resting on or pinned to it. |
| [`cloth_renderer.h`](cloth_renderer.h) | `ClothRenderer` — draws a sibling `Cloth` (physxcoopa) as a shaded, shadow-casting mesh, rewriting a dynamic GPU vertex buffer from the solver's particles each frame. One vertex per particle; normals are area-weighted, tangents follow the grid's +U. Needs a sibling `MeshRenderer` with **no `mesh_path`** and ideally `cull_backfaces: false`. |
| [`health_driver.h`](health_driver.h) | `HealthDriver` — demo component owning a `coopa::stat::Resource` and binding it to a `coopa::ui::ProgressBar` in its own subtree. |
| [`kinematic_control_system.h`](kinematic_control_system.h) | `KinematicControlSystem` + `install_kinematic_control_system()` — runs `KinematicController`, `KinematicMover` and then `CharacterController` (so platforms have already moved) at order **50**, ahead of `UpdatePhase::Physics` (100). Not stylistic: a component's `update()` runs at `Behaviour` (200), *after* `PhysicsSystem` has already read its Transform, so a kinematic body driven there is always one frame stale to the physics that consumes it. Harmless for rigid contacts, but it makes a driven body clip through cloth (the ball in `cloth_demo`). |
| [`register.h`](register.h) | `register_scene_components()` — registers every parser that needs no dependencies; the `(Device&, Allocator&, AssetManager&, frames_in_flight)` overload is a strict superset that additionally registers `ClothRenderer`. Call one of them once at startup. |

## Conventions

Three rules every component here follows, all worth knowing before adding another:

1. **Per-frame input is PUSHED in, never pulled.** `CameraController` and `KinematicController` both
   expose plain public fields (`mouse_delta`, `move_input`, …) that `Engine::tick()` writes before
   `Scene::update()` consumes them. A component that never touches `coopa::input::Input` stays
   testable with no live GLFW window, and `NO_INPUT=1` can zero the whole thing for a reproducible
   headless capture. `CharacterController` gets `move`/`jump`/`sprint` (WASD, Space, LeftShift)
   from `Engine::drive_character_controllers_()`, made camera-relative there. Note the consequence for tests: the Engine overwrites those fields at the top
   of every tick, so poking a value in from outside a running Engine cannot survive to `update()`.
2. **A component that drives a kinematic body's Transform writes it in `advance()`, not
   `update()`**, and is driven by `KinematicControlSystem` ahead of the physics phase — see that
   file's doc for the frame-order trace. `update()` on those components is deliberately empty, so a
   scene that forgets to install the system fails loudly (nothing moves) rather than subtly.
3. **Child/descendant lookup happens in `start()`, never in a YAML parser** — the parser runs before
   children exist (see `HealthDriver::find_bar_()`). `ClothRenderer` goes one step further and
   defers to its first `upload()`, because the thing it depends on (the simulated cloth) is not
   created until `PhysicsSystem`'s first reconcile pass, which is itself after `Scene::start()`.

## Ragdolls

`assets/objects/characters/mannequin_ragdoll.yaml` (generated by `tools/gen_mannequin.py`, so it
tracks the plain `mannequin`) adds a 14-bone `Ragdoll` to the mannequin: capsule pelvis / spine /
chest / limbs, a sphere head, box feet; cone-twist spine, neck, shoulders and hips, hinge elbows
(0..140), knees (-140..0) and ankles. `assets/scenes/gameplay/character_demo` (generated by
`tools/gen_character_demo_scene.py`) drops three of them down a flight of stairs, sweeps a
kinematic ram across the landing, and its player goes limp / recovers on R. The physics side --
hierarchical bodies, the joint types -- is described in `libs/physxcoopa/README.md`.

Limits worth knowing: the recovery stands the character up wherever the pelvis lies but blends
straight into the recovery state (no get-up animation); bones not in the list ride their parent
bone; the Animator must be on the Ragdoll's object.

## Character + camera rig

`assets/scenes/gameplay/character_demo` (generated by `tools/gen_character_demo_scene.py`)
is the bench: the `mannequin` prefab (`tools/gen_mannequin.py`: an object-hierarchy humanoid,
joints `pelvis`, `spine`, `chest`, `neck`, `head`, `upper_arm_l/r`, `lower_arm_l/r`, `hand_l/r`,
`thigh_l/r`, `shin_l/r`, `foot_l/r`, clips idle/walk/run/jump) with a `CharacterController` and
`CharacterAnimDriver`, a colliding third-person camera, and stations for stairs, a too-tall step,
walkable and steep ramps, a moving deck, pushable crates, a low ceiling and mesh terrain.
The player moves by root motion (the walk / run clips declare `root_motion: {object: pelvis,
translation: xy}`; the instance overrides its Animator with `apply_root_motion: true` and sets the
controller's `use_root_motion`), plants its feet with `FootIK`, and the walk / run clips fire
`footstep` events (string `left` / `right`; scene EventBus signal `anim_event`). A second, idle
mannequin stands with one foot on a 0.2 m block to show `FootIK` on its own. Root motion arrives one
frame late at the controller: it advances at order 50, the Animator at 300. It moves the character
only while grounded: in the air (a jump, a fall) the controller keeps the velocity it left the
ground with, steered by input (`air_control`) and slid by steep slopes -- the airborne clips carry
no travel, and following them once stranded a player that landed against a steep slope's foot,
ungrounded and unable to move (`root_motion_character_slides_off_a_steep_slopes_foot`).

`assets/scenes/gameplay/ik_character_demo` is the same bench with the FULLY IK mannequin
(`objects/characters/mannequin_ik`) as the player and the foot-IK idler: its clips
(`animations/mannequin_ik/`) key only target empties, and a `ChainIK` spine, `TwoBoneIK` legs and
arms and a `LookAtIK` head pose every bone. The clips are baked from the keyframed ones, so the two
benches move alike (`tests/engine/unit/ik_rig_test.cpp` holds them to within a millimetre on every
key).

Limits worth knowing: the controller queries the physics world as of the previous step (it runs
before physics), so a platform moving vertically leaves up to one frame of travel between feet
and deck; the capsule is Z-up only; the camera collision ignores triggers and the tracked
object's whole subtree.
