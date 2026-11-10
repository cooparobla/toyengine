# Animation Module (`coopa::anim`)

The `animation` module provides a Unity-style scene animator: named clip
states, crossfade blending, keyframed and procedural tracks, and a
string-keyed property registry that lets an `Animator` drive fields on
component types libcoopa never names (uicoopa's `RectTransform`,
`Graphic::color`, a future gfxcoopa material property, ...) without a
virtual hook on `coopa::scene::Component`.

This is **property animation**: a clip's tracks address a scene object's path
and a named property on one of its components (a `Transform`, or anything
else `AnimatedPropertyRegistry` knows). It covers a camera move, a light
flicker, a UI panel slide, procedural sway — and still covers a rig built the
older way, as an object hierarchy posed by its own Transforms (`robot_arm`,
`bouncing_ball`). A Blender-style skeleton — named bones with a rest pose,
posed in a pose buffer and skinned to a mesh — is a separate module,
`coopa/rig/` (see its own README); its `SkeletalAnimator` reuses this
module's curve/keyframe/event/root-motion machinery directly, but blends by
slerp into bone-space pose deltas rather than lerping a Transform's fields.

It is a sibling of `coopa::scene`, not part of it — `coopa/scene/README.md`
keeps that module free of any dependency beyond vendored fkYAML/glm and
libcoopa's own `coopa::event`/`coopa::job`, and this module needs
`coopa::asset::AssetHandle` (an `AnimationClip` is a pure-CPU asset) and
`coopa::job::JobEngine` (evaluation can be batched across a scene). Nothing
here includes gfxcoopa, uicoopa, or any other sibling repo — every
component-specific animatable field is registered from outside libcoopa via
`AnimatedPropertyRegistry`, the same extension pattern
`coopa::scene::SceneLoader::register_component_parser()` and
`coopa::asset::AssetManager::register_loader<T>()` already established.

---

## Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                            ANIMATION MODULE CLASS OVERVIEW                                │
└──────────────────────────────────────────────────────────────────────────────────────────┘

                    ┌────────────────────────┐
                    │  AnimatedPropertyRegistry │  (function-local static, per component_type.name)
                    ├────────────────────────┤
                    │ cast / get / set        │  <-- registered from OUTSIDE libcoopa
                    └────────────────────────┘        (uicoopa::register_ui_animated_properties())

                    ┌────────────────────────┐        ┌─────────────────────────┐
                    │   ProceduralTrackRegistry│      │      AnimationClip       │ (immutable, shared,
                    ├────────────────────────┤        ├─────────────────────────┤  AssetHandle<AnimationClip>)
                    │ orbit / sine / spin /...│◄───────│ tracks: AnimationTrack[] │
                    └────────────────────────┘        │   - keyframed: curve     │
                                                       │   - procedural: params   │
                                                       └────────────┬────────────┘
                                                                    │ resolved once (bind time)
                                                                    ▼
┌───────────────────────────────────────────────────────────────────────────────────────────┐
│  Animator : coopa::scene::Component                                                       │
│    states_        : AnimatorState[]     (name -> clip, inline or asset-loaded)             │
│    bindings_       : Binding[]          (Component*, AnimatedProperty*) deduped            │
│    current_/previous_ Layer             (time, crossfade weight)                           │
│    advance_() / evaluate_() / apply_()  <-- private; only AnimationSystem may call them     │
└───────────────────────────────────────────────────────────────────────────────────────────┘
                                                                    ▲ drives
                                                                    │
                    ┌───────────────────────────────────────────────────────────┐
                    │  AnimationSystem : coopa::scene::ISceneSystem              │
                    │  registered at UpdatePhase::Animation (300)                │
                    │  advance_ (serial) -> evaluate_ (parallel via JobEngine    │
                    │  once worth it) -> apply_ (serial)                         │
                    └───────────────────────────────────────────────────────────┘
```

---

## Why an `Animator` has no `update()`

Every other libcoopa component drives itself from `Component::update()`. An
`Animator` deliberately does not — it has no `update()`/`late_update()`
override at all. It is advanced only by a registered `AnimationSystem`, via a
`friend`-only `advance_()`/`evaluate_()`/`apply_()` split:

1. `advance_()` — serial, main thread: clocks, crossfade weights,
   `on_state_finished`, lazy rebinding, and snapshotting any procedural
   track's live `target_object` position (see below).
2. `evaluate_()` — samples every active track into a caller-owned scratch
   buffer. **Worker-safe by construction**: it touches no `Component`, no
   `SceneObject`, no `Scene` — only const clip data and the snapshot
   `advance_()` already wrote. `AnimationSystem` runs this inline, or as jobs
   across a `coopa::job::JobEngine` once a scene has enough active tracks to
   make that worthwhile (`AnimationSystem::set_parallel_threshold()`).
3. `apply_()` — serial, main thread: writes the accumulated result back onto
   every binding's target component.

This is what lets `AnimationSystem` batch evaluation across every `Animator`
in a scene into one job dispatch, instead of each `Animator` fighting for its
own place in the `Component::update()` order.

**Add `coopa::anim::install_animation_system(scene)` to any scene that uses
an `Animator`** — without it, `Animator::start()` still binds tracks and
`play()` still updates playback state, but nothing ever calls `advance_()`,
so nothing visibly animates.

---

## The `Scene` phase pipeline

`AnimationSystem` registers at `coopa::scene::UpdatePhase::Animation` (300),
which sits between the built-in `Behaviour` (200, the `Component::update()`
walk) and `LateBehaviour` (400, the `Component::late_update()` walk) — see
`coopa/scene/README.md`'s "Update Phases" section. Practically:

- A `Button::update()` calling `animator->crossfade(...)` takes effect the
  **same frame** (Behaviour runs before Animation).
- A UI `CanvasComponent::late_update()` (measure → arrange → emit) always
  sees the **final** animated pose (Animation runs before LateBehaviour).
- Two writers to the same property resolve deterministically by phase rather
  than by component-list order: a `Button`'s own color chase (phase 200) and an
  `Animator` track (phase 300) both targeting `Graphic::color` always resolve
  with the `Animator`'s write winning.

---

## Property binding: how an `Animator` touches `RectTransform`

`AnimatedPropertyRegistry` maps `(Component::type_name(), property name)` to
a typed `get`/`set`/`cast` triple. A downcast is resolved **once**, at an
`Animator`'s bind time, and cached — there is no per-frame RTTI.

Built in for `Transform`: `position`, `scale`, `rotation` (Euler degrees,
per-channel and deliberately not shortest-path, so 0 -> 720 spins twice) and
`rotation_quat` (x, y, z, w). A `rotation_quat` track interpolates channel by
channel and the setter normalises — nlerp, the short way round as long as
consecutive keys share a hemisphere (dot >= 0; the toyeditor's Timeline flips a
key's sign when recording to keep it so). Rigs key rotations this way: no
gimbal lock, no long way round.

```cpp
// uicoopa/ui_yaml.h — registered from OUTSIDE libcoopa
reg.register_vec<RectTransform, glm::vec2>("RectTransform", "anchored_position",
    &RectTransform::anchored_position, &RectTransform::set_anchored_position);

// Graphic::color is a member of the ABSTRACT base Graphic; register it once
// per concrete subclass key (Image, Text, ...) — the cast dynamic_casts to
// Graphic*, which succeeds for any of them.
reg.register_vec_member_as<Graphic, glm::vec4>("Image", "color", &Graphic::color);
```

A channel suffix (`.x`/`.y`/`.z`/`.w`, `.r`/`.g`/`.b`/`.a`, or a combination
like `.xy`) narrows a track to a subset of a property's channels —
`split_channel_suffix()` decodes it into a bitmask stored on the binding, not
the property. Two tracks resolving to the *same* `(Component*,
AnimatedProperty*)` — e.g. one driving `position.x` and another
`position.z`, or a crossfading previous/current pair on the same property —
are deduplicated into **one** shared accumulator (4 weighted-value slots + 4
weight slots). `apply_()` renormalizes by accumulated weight rather than each
track independently calling `set()`, which is what makes crossfade blending
and multi-track channel masking share one code path with no clobbering.

**This dedup is per-`Animator`, not scene-wide** — two *different* `Animator`s
targeting the same property still clobber each other by execution order.
`AnimationSystem::find_conflicts()` is a debug aid that lists every
`(object, component_type, property)` written by more than one `Animator`.

---

## Keyframed vs. procedural tracks

A track is either:

- **Keyframed** (`AnimationCurve`): a sorted list of `Keyframe { time, value[4], easing }`.
  `easing` (`Linear`, `Step`, `EaseIn`, `EaseOut`, `EaseInOut`) applies to the
  segment *leaving* that key. Sampling is stateless (`std::upper_bound`, no
  mutable cursor) because one clip's curve is sampled by many `Animator`s at
  once, potentially from worker threads.
- **Procedural** (`ProceduralTrack`): a string `type` (`orbit`, `sine`,
  `spin`, `constant`, or a consumer-registered type) plus a
  `ProceduralParams` bag, evaluated each frame by a pure function registered
  in `ProceduralTrackRegistry`. An evaluator **must be stateless and
  reentrant** — the same constraint as curve sampling, since evaluators also
  run inside `evaluate_()`.

Both kinds feed the *same* binding/blend path (see above), so a procedural
track and a keyframed track can crossfade against each other with zero
special-casing.

`orbit`'s `target_object` (an optional live-position input, replacing the
`center` parameter) is the one place a track needs *live* scene data. That
read happens in `advance_()`, on the main thread, and is snapshotted into a
per-track `ProceduralInputs` the worker-safe `evaluate_()` later reads —
never resolved from inside an evaluator itself.

A procedural track has no intrinsic length; it contributes nothing to
`AnimationClip::effective_length()`. A clip made up entirely of procedural
tracks needs an explicit `length:` (see the YAML schema below) — without one,
`effective_length()` is 0, which `Animator` treats as **infinite**: time is
never wrapped and `wrap:` is ignored, since there's nothing to wrap against.

---

## Events

A clip may carry `events:` -- named markers on its timeline (`AnimationEvent { time, name,
string_value, float_value }`, sorted by time at parse). `Animator::advance_layer_()` collects
every event the playhead crossed this frame, run by run (`for_each_sample_run_()` splits raw time
into the monotonic stretches of sample time it covers), and they are emitted at the end of
`advance_()`:

- through `Animator::on_event` (`Signal<const AnimationEvent&>`), and
- on the scene `EventBus` as `(owner name, "anim_event")`, args `name`, `string`, `float`,
  `state`, `time` -- so a reactor or any component can listen without a pointer to the Animator.

Firing rules (exact, tested): a forward run fires `[from, to)`, a backward run (`speed < 0`, or a
ping-pong's return leg) `(to, from]`. So a looping clip fires an event at `0` once per cycle; a
ping-pong fires each endpoint once per visit and interior events both ways; an event exactly at
the end of a loop / once clip fires when the playhead reaches it; a once clip fires nothing after
it has clamped. A big step that crosses several cycles fires each crossed event once per cycle,
in order. **During a crossfade only the target (current) state fires**, so a clip crossfading out
never double-fires with the one coming in. Events are dispatched after the layer state is
settled, so a handler may `play()` / `crossfade()` the same Animator.

## Root motion

```yaml
clip:
  root_motion: { object: pelvis, translation: xy, rotation: none }   # translation xy|xyz|none, rotation yaw|none
```

The root bone's keyframed `position` / `position.<channels>` tracks (and, with `rotation: yaw`, its
`rotation_quat` track's yaw about +Z) are sampled at the start and end of every run of sample time
the frame covered: a loop wrap contributes `(end - prev) + (new - start)`, each further cycle a
whole cycle's travel. Each layer's delta is blended by crossfade weight, so fading from a walk to
an idle ramps the travel down. The result is `Animator::root_motion_delta()` (`RootMotionDelta {
translation, yaw_deg }`, in the owner's local frame), and the extracted channels are held at their
clip-start value on the bone (`hold_root_()` in `evaluate_()`), so the mesh stays over its owner.
Extraction always happens when a clip declares `root_motion`; what consumes it is up to
`Animator::apply_root_motion`:

- off (default): nothing moves -- gameplay may read `root_motion_delta()`;
- on: the delta goes to the first component on the owner implementing `IRootMotionReceiver`
  (`root_motion.h`) -- e.g. toyengine's `CharacterController`, which accepts it while
  `use_root_motion` is on and moves by it through its collide-and-slide motor -- or, when there is
  none or it declines, straight onto the owner's Transform.

Only keyframed root tracks are extracted (procedural ones are ignored).

## IK

`ik.h` is the math, scene-free: `solve_two_bone()` (law of cosines; the middle joint goes in the
plane of root, target and pole, on the pole's side; `soft_reach()` eases the last `softness` of the
chain's length so a knee never pops straight), `solve_chain()` (FABRIK over any number of joints:
the root stays, every segment keeps its length, and from the input pose each joint moves the least
it can), `look_at_delta()` (clamped to `max_angle`, keeping the bone's up axis from rolling) and the
world/local rotation helpers.

`ik_components.h` wraps it, addressing bones as paths through the scene-object hierarchy -- for an
embedder with no skeleton component of its own. **toyengine itself does not use this path**:
`Engine::init()` installs `coopa::rig`'s own `TwoBoneIK`/`ChainIK`/`LookAtIK` (same class names,
bone-space, `coopa/rig/ik_components.h`) via `register_component_parser()` AFTER this module's,
and that call is last-registration-wins, so in every toyengine asset these YAML keys always mean
the rig-space form -- see `coopa/rig/README.md` and the `create-animation` skill.

```yaml
- type: TwoBoneIK
  upper: shoulder                      # bone paths, relative to this object (like a track's object)
  lower: shoulder/elbow
  end: shoulder/elbow/hand
  target: reach_target                 # or set_target_position() from code
  pole: elbow_pole                     # optional
  weight: 1.0
  soft_limit: 0.02
  match_target_rotation: false         # true: the end bone also takes the target's rotation
- type: ChainIK                        # a chain of any length: a spine, a neck, a tail
  root: spine                          # the chain is root and its descendants down to tip
  tip: spine/chest/neck                # its origin reaches the target
  target: chest_target
  iterations: 10                       # FABRIK passes at most
  tolerance: 0.001
  match_target_rotation: true          # the tip also takes the target's rotation
- type: LookAtIK
  bone: neck/head                      # empty = this object
  target: look_target
  forward_axis: {x: 0, y: 1, z: 0}
  up_axis: {x: 0, y: 0, z: 1}
  max_angle: 70                        # degrees from the animated facing
  weight: 1.0
  smoothing: 0.1                       # seconds; 0 snaps
```

`IkSystem` (`ik_system.h`, `install_ik_system()`, order **320** -- between Animation 300 and
TransformResolve 350) gathers them with one scene walk per frame (spawned rigs need no refresh)
and runs `IkDriver::ik_pre_solve()` (drivers such as toyengine's `FootIK` set targets there), every
solver, then `IkDriver::ik_post_solve()`. Solvers run by `priority` (any solver; lower first,
default 0); equal priorities run every `ChainIK`, then every `TwoBoneIK`, then every `LookAtIK`, each
in scene pre-order -- a spine settles before the arms hanging from it, and a head turn rides on
both (`IkSystem::solve_order()` lists the frame's order). Solvers read the live pose through
`get_world_matrix()`, which re-walks any parent chain dirtied by the Animator or an earlier solve.

**A fully IK rig** is the same components at weight 1 with every limb and the spine driven: the
Animator keys only target empties, and IK poses every bone from them. Hang the targets the way the
bones they stand for hang -- foot targets and knee poles from the rig's root (a planted foot stays
put), hand / chest / look targets from the pelvis (a lowered pelvis carries them) -- and set
`match_target_rotation` where a bone's own orientation matters (hands, feet, the chest). toyengine's
`objects/characters/mannequin_ik` is the worked example (`tools/gen_mannequin.py` bakes its target
clips from the keyframed mannequin's, so the two move alike).

IK overrides the animated pose. A bone no clip animates would feed last frame's IK output back in
as this frame's input, so each solved bone remembers what it wrote (`IkPoseGuard`) and restores the
pre-IK rotation when it finds that value untouched -- weight 0 always returns exactly the unsolved
pose, and clamps (look-at `max_angle`) are always relative to the animated pose. A driver that reads
the animated pose before choosing targets calls `TwoBoneIK::restore_input()` first.

`register_animation_components()` also registers the `TwoBoneIK` / `ChainIK` / `LookAtIK` parsers.

---

## YAML schema

A clip is a standalone file (`type:`/flat-mapping form, never `!Tag` block
style — see `coopa/scene/scene_loader.h`'s documented fkYAML limitation):

```yaml
# assets/animations/banner_idle.yaml
clip:
  name: banner_idle
  wrap: loop            # loop | once | pingpong
  length: 2.4           # optional for keyframed clips; REQUIRED if every track is procedural
  events:               # optional -- see "Events"
    - { time: 0.6, name: footstep, string: left, float: 1.0 }
  root_motion: { object: pelvis, translation: xy, rotation: none }   # optional -- see "Root motion"
  tracks:
    - object: ""                     # "" or omitted = the Animator's own owner;
                                      # "A/B/C" walks descendants segment-by-segment;
                                      # any other name resolves via find_descendant(), then scene-wide.
      component: RectTransform       # optional; defaults to "Transform"
      component_index: 0             # optional; disambiguates two same-typed components on one object
      property: anchored_position.y  # AnimatedPropertyRegistry key, optional channel suffix
      keys:
        - { time: 0.0, value: -20.0, easing: ease_in_out }
        - { time: 1.2, value: -34.0, easing: ease_in_out }
        - { time: 2.4, value: -20.0 }

    - object: sphere.000              # procedural track -- no `keys:`
      property: position
      procedural:
        type: orbit
        target_object: cube.000       # live center, re-snapshotted every frame
        center: { x: -1.5, y: 0.0, z: 0.0 }   # fallback if target_object doesn't resolve
        radius: 2.7
        speed: 1.0
        height: 1.0
        initial_angle: 0.0
```

Key/parameter values accept a bare scalar, `{x,y,z,w}`/`{r,g,b,a}`, or a
`[a,b,c,d]` sequence — parsed into 4 floats pre-initialized to `{0,0,0,1}`
(the glm xyzw/rgba identity), so a color key omitting `a` gets `1.0` and a
vec2 key gets `0` in `z`. One clip per file — `AssetManager` enforces one C++
type per resolved path, so a multi-clip container would need a second asset
type.

An `Animator` component references one or more clips by name:

```yaml
- type: Animator
  auto_play: idle
  speed: 1.0
  default_crossfade: 0.15   # informational; crossfade() always takes an explicit duration
  apply_root_motion: false  # move the owner by the clips' root motion (see "Root motion")
  states:
    - name: intro
      clip: ../animations/banner_intro.yaml
      wrap: once            # overrides the clip's own wrap for this state
    - name: idle
      clip: ../animations/banner_idle.yaml
      speed: 0.75           # overrides playback rate for this state
```

`"Animator"` is **not** built into `SceneLoader` — it is registered via
`register_animation_components(AssetManager&)`, alongside the
`AnimationClipLoader`. Trajectories are data, loaded through the asset system,
rather than anything hardcoded into the scene layer.

---

## Usage

```cpp
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/animation_system.h>

// Once, at startup:
coopa::anim::register_animation_components(assets); // registers AnimationClipLoader + "Animator" parser

coopa::scene::Scene scene = coopa::scene::SceneLoader::load("scene.yaml");
coopa::anim::install_animation_system(scene); // registers AnimationSystem at UpdatePhase::Animation

// Per frame:
scene.update(dt);       // Behaviour(200) -> Animation(300)
scene.late_update(dt);  // LateBehaviour(400) -- e.g. uicoopa's Canvas layout, sees the final pose

// Imperative control from anywhere with the Animator*:
animator->crossfade("hover", 0.15f);
```

For scaling evaluation across many `Animator`s onto a `coopa::job::JobEngine`:

```cpp
coopa::job::JobEngine engine(std::thread::hardware_concurrency());
scene.set_job_engine(&engine); // AnimationSystem evaluates large batches on its workers
```

The same engine can also be handed to `coopa::asset::AssetManager`; job
handles are reclaimed individually, so sharing it needs no coordination.

---

## Testing

Unit tests live in `libcoopa`'s top-level `test.cpp`, under the
`animation_test` namespace (curve sampling, property registry, binding
dedup/no-clobber, wrap modes, crossfade blending, name/path resolution,
lazy-bind-when-initially-inactive, rotation non-wrapping, YAML loading
including a malformed-file failure path, procedural-vs-keyframed blending,
the orbit evaluator against its closed-form formula, and a
serial-vs-forced-parallel bit-identical check), plus events (exact fire counts across loop,
ping-pong, once, multi-cycle steps and crossfades; a handler crossfading mid-dispatch), root motion
(deltas over a loop boundary, a multi-cycle step, crossfade ramp-down, yaw extraction, the
`IRootMotionReceiver` hand-off and its fallback), two-bone IK (reach, pole plane and side, soft
limit, stability over frames, weight 0 restoring the pose) and look-at clamping / smoothing.
libcoopa has no ctest entry in the engine build: configure `libs/libcoopa` on its own and run its
`libcoopa` executable. The `coopa::scene` phase
pipeline itself is covered separately, under `scene_pipeline_test`.
