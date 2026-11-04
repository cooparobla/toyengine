---
name: create-animation
description: Create toyengine animations. Two systems -- property animation (coopa::anim, the Animator component, clips in assets/animations/<name>/<clip>.yaml addressing objects by path) for rigid/non-skeletal motion like robot_arm or a bouncing ball; and skeletal animation (coopa::rig, a Rig + SkeletalAnimator addressing bones by NAME in a rig asset at assets/rigs/<name>.yaml, with BoneAttachment carrying rigid parts or a skinned mesh) for any character or creature with a skeleton. Covers clip events, root motion, and bone-space IK (TwoBoneIK / ChainIK / LookAtIK / FootIK). Use when asked to animate something, make a clip / walk / idle / wave / bounce, set up a rig, Animator or SkeletalAnimator, make a skinned (bending) mesh move, add footstep/animation events, root motion, or IK (reach, look-at, foot placement).
---

# Create an animation

**Two separate systems, picked per object:**

| | Property animation (`coopa::anim`) | Skeletal animation (`coopa::rig`) |
|---|---|---|
| Animates | any registered component property (Transform position/rotation/scale, a few UI/graphic properties) | bones -- indices into a rig asset, not SceneObjects |
| Addressed by | **path** from the Animator's object | **name** within the rig |
| Component | `Animator` | `Rig` + `SkeletalAnimator` |
| Clip lives in | `assets/animations/<name>/<clip>.yaml` | same, but `clip: bones:` not `clip: tracks:` |
| Use for | a rigid mechanism (`robot_arm`), a non-skeletal prop (`bouncing_ball`), a UI/graphic property | a character or creature with a skeleton: `mannequin`, `horse`, `tentacle`, `ik_watcher`, `ik_reacher` |

If the thing has rigid parts or a skin that should move with a skeleton, it's a rig. Otherwise
put a plain `Animator` on the object itself -- this section covers that case first, then the rig
case. Format reference: `libs/libcoopa/coopa/animation/README.md` (property) and
`libs/libcoopa/coopa/rig/README.md` (skeletal). Generator with helpers to copy:
`tools/gen_mannequin.py` (rig + skeletal clips), `tools/gen_animation_demo_scene.py`
(`quat_axis_angle`, `rot_keys` with hemisphere fix, both `clip(...)` for property clips and
`skeletal_clip(...)`/`write_rig(...)` for rig ones). Examples:
`assets/objects/animation/robot_arm.yaml` + `assets/animations/robot_arm/*.yaml`, `bouncing_ball`
(property); `assets/objects/characters/mannequin.yaml` + `rigs/mannequin.yaml`, `tentacle`,
`ik_watcher`, `ik_reacher` (skeletal). Scene: `./build/toyengine animation_demo`.

## Property animation (non-skeletal objects)

### Clip file (one clip per file)

```yaml
clip:
  name: wave
  wrap: loop                 # loop | once | pingpong (default loop)
  length: 2.0                # seconds; optional for keyframed clips (= last key), required if all tracks are procedural
  tracks:
    - object: base/shoulder  # path from the Animator's object; "" = the Animator's own object
      component: Transform   # default Transform
      property: rotation_quat   # position | scale | rotation (Euler deg) | rotation_quat [x,y,z,w]
      keys:
        - { time: 0.0, value: [0.0, 0.0, 0.0, 1.0], easing: ease_in_out }
        - { time: 1.0, value: [0.0, 0.0, -0.4226, 0.9063] }
    - object: base
      property: position.z   # channel suffixes allowed: .x .y .z .xy ...
      keys:
        - { time: 0.0, value: 0.0 }
        - { time: 1.0, value: 0.3, easing: step }
    - object: base/spinner
      property: rotation
      procedural: { type: spin, ... }   # orbit | sine | spin | constant -- see the loader for params
```

- Times are seconds (no fps; the editor timeline snaps to 30 fps).
- `easing` (linear default, `step`, `ease_in`, `ease_out`, `ease_in_out`) shapes the segment
  LEAVING that key.
- Prefer `rotation_quat`: `rotation` (Euler) interpolates per angle, not shortest path.
  Quaternions are nlerped per channel, so **keep consecutive keys in the same hemisphere**
  (dot >= 0; negate the next key if not) or the joint spins the long way. `rot_keys()` in the
  generator does this.
- Values: scalar, `[a, b, c, d]` or `{ x, y, z, w }`.

### Animator (on the object being animated)

```yaml
- type: Animator
  auto_play: idle            # state played on start
  speed: 1.0
  default_crossfade: 0.15
  apply_root_motion: false   # true: clips' root_motion moves this object (see Root motion)
  states:
    - { name: idle, clip: animations/<rig>/idle.yaml }
    - { name: wave, clip: animations/<rig>/wave.yaml, wrap: once, speed: 0.75 }
```

Clip paths resolve against the scene/prefab folder first, then `assets/`. The animation system
is installed by the engine automatically.

### Events

```yaml
clip:
  events:                    # markers on the clip's timeline, sorted by time at load
    - { time: 0.225, name: footstep, string: left }    # optional `string:` / `float:` payload
    - { time: 0.675, name: footstep, string: right, float: 0.8 }
```

When the playhead crosses one, the Animator emits it on `Animator::on_event` and on the scene
EventBus as `(<owner name>, "anim_event")` with args `name`, `string`, `float`, `state`, `time`.
A loop fires each event once per cycle (an event at `0` too); during a crossfade only the
incoming state fires. In the editor, events live on the timeline's Events lane. Skeletal clips
support the exact same `events:` block and firing rules, under `SkeletalAnimator::on_event` --
see `assets/animations/mannequin/walk.yaml` for a real skeletal example.

### Root motion

```yaml
clip:
  root_motion: { object: pelvis, translation: xy, rotation: none }   # translation xy | xyz | none; rotation yaw | none
```

The named object's keyframed `position` (and with `rotation: yaw`, its `rotation_quat` yaw)
travel is extracted each frame and the object is held at its clip-start value, so author the walk
moving forward (+Y) and the mesh stays over its owner. Nothing moves the object unless the
Animator has `apply_root_motion: true`: then a CharacterController with `use_root_motion: true`
on the same object moves by it (colliding); otherwise the delta goes straight onto the owner's
Transform. Only keyframed root tracks count (not procedural ones). A skeletal clip's
`root_motion: { bone: pelvis, ... }` works identically through `SkeletalAnimator::apply_root_motion`
-- same fields, `bone:` instead of `object:` since it names a bone, not a scene path.

## Skeletal animation (rigs)

Use this whole section for anything with a skeleton -- a character, a creature, a tentacle.
Bones are **not SceneObjects**: they are a flat, parent-first list of indices in a rig asset,
addressed by name in clips, IK and attachments. A `Rig` with no animator at all is still usable
(posed by hand, by IK, or by a ragdoll), which is why `Rig` and `SkeletalAnimator` are separate
components.

### Rig asset -- `assets/rigs/<name>.yaml`

```yaml
rig:
  name: mannequin
  bones:
    - name: pelvis
      head: {x: 0, y: 0, z: 0.95}     # rig space (the Rig component's own object)
      tail: {x: 0, y: 0.1, z: 0.95}   # local +Y axis = head -> tail
      roll: 0                        # degrees about that axis; orients X/Z (optional, default 0)
    - name: spine
      parent: pelvis                 # index of an earlier bone; omit for a root bone
      head: {x: 0, y: 0, z: 1.05}
      tail: {x: 0, y: 0, z: 1.25}
      connected: true                # optional: head snaps to the parent's tail at load (Blender)
```

Bones must be listed parent-first (every `parent` index less than its own). Rest basis: Y is
`normalize(tail - head)`; X/Z come from a stable seed axis, then `roll` turns them about Y.
`rig_asset.h`'s `bone_rest_matrix()` is the exact algorithm. Referenced from an object with:

```yaml
- type: Rig
  rig: rigs/mannequin.yaml
```

### Skeletal clip file (one clip per file, `bones:` not `tracks:`)

```yaml
clip:
  name: walk
  wrap: loop
  length: 0.9
  root_motion: {bone: pelvis, translation: xy, rotation: yaw}
  events: [{time: 0.2333, name: footstep, string: left}]
  bones:
    - bone: pelvis                 # bone NAME in the rig -- not a path
      location:                    # pose-delta location, in the bone's own rest-local frame
        - {time: 0, value: [0, 0, 0.006]}
        - {time: 0.3, value: [0, 0, 0.001757]}
      rotation:                    # [x, y, z, w]; always slerped (never nlerped), so no
        - {time: 0, value: [0, 0, 0, 1], easing: ease_in_out}   # hemisphere bookkeeping needed
        - {time: 0.3, value: [0, 0, 0.009256, 0.999957], easing: ease_in_out}
      # scale: same shape as location, default [1,1,1]
```

Values, easing and times all follow the same rules as a property clip's tracks (see above) --
only the grouping (per bone, not a flat track list) and the addressing (name, not path) differ.
`rotation` values are a pose DELTA from rest (identity = exactly the rest pose), not a world or
local-to-parent rotation -- see `rig_component.h`'s `Rig::PoseChannel` doc. A bone with no
`location`/`rotation`/`scale` key for a channel keeps whatever that channel already holds (rest,
or another layer's contribution during a crossfade). Real examples:
`assets/animations/mannequin/*.yaml`, `assets/animations/tentacle/sway.yaml`.

### SkeletalAnimator (on the same object as the Rig)

```yaml
- type: Rig
  rig: rigs/mannequin.yaml
- type: SkeletalAnimator
  auto_play: idle
  default_crossfade: 0.2
  apply_root_motion: false    # identical semantics to Animator's own flag
  states:
    - {name: idle, clip: animations/mannequin/idle.yaml}
    - {name: walk, clip: animations/mannequin/walk.yaml}
    - {name: jump, clip: animations/mannequin/jump.yaml, wrap: once}
```

Crossfade blends with true slerp in the pose buffer (not a componentwise nlerp), so authoring
never needs quaternion sign-flip bookkeeping the way a property `rotation_quat` track does. At
most one `SkeletalAnimator` is expected to drive a given `Rig`.

### BoneAttachment -- rigid parts and a skinned mesh riding the bones

A rigid part (capsule, saddle, visor) is a child object with its own `Transform` (the local
offset from the bone, as if it were parented to it) plus:

```yaml
- name: head_shape
  components:
    - type: Transform
      position: {x: 0, y: 0.11, z: 0}
      rotation: {x: -90, y: 90, z: 0}
    - type: BoneAttachment
      bone: head               # bone name in the rig
      at: head                 # head | tail (default head)
      rig: ""                  # optional explicit rig path; default: nearest ancestor with a Rig
    - type: MeshRenderer
      mesh_path: mannequin_head
      material: {albedo: {r: 0.62, g: 0.66, b: 0.72}}
```

A **skinned (bending) mesh** instead carries `SkinnedMeshRenderer { mesh_path: <mesh> }` (no
`BoneAttachment` -- it follows every bone weight, not one). The mesh's `weights:` vertex groups
are named after bones in the nearest ancestor `Rig` (create-mesh); `preserve_volume: true` blends
bones as dual quaternions so a twisted joint keeps its cross section instead of pinching shut
(Blender's option of the same name). The weights need not be authored by hand: in the editor,
`Object > Parent > Bind to Rig with Automatic Weights` generates them from the rig's bones (bone
heat) in one step; headless, the same generator is `toy::scene::compute_auto_weights()`
(`toyengine/scene/auto_skin_weights.h`). Real example: `assets/objects/animation/tentacle.yaml`.

### IK (bone-space; after the SkeletalAnimator, overriding the rig's pose)

The IK components live on the SAME object as the `Rig` (not inside it). Bone fields (`upper`,
`lower`, `end`, `root`, `tip`, `bone`) are NAMES within that rig; `target`/`pole` remain ordinary
scene-object paths, resolved from the IK component's own object (so `foot_target_l` or
`pelvis_socket/hand_target_l`, not a bone path).

```yaml
- type: TwoBoneIK            # an arm or leg reaching for a target
  upper: thigh_l              # required: upper / lower / end bone NAMES in the rig
  lower: shin_l
  end: foot_l                 # its head is put on the target
  target: foot_target_l       # object path from this component's own object
  pole: knee_pole_l           # optional: the elbow/knee bends toward it (empty keeps the animated bend)
  weight: 1.0                 # 0..1
  soft_limit: 0.02            # fraction of reach eased near full extension
  match_target_rotation: false   # true: the end bone (hand/foot) also takes the target's rotation
  priority: 0                 # any solver: lower solves first
- type: ChainIK              # a chain of any length (spine, neck, tail) reaching for a target (FABRIK)
  root: spine                 # bone names: the chain is root and its RIG-HIERARCHY descendants down to tip
  tip: chest                  # its head is put on the target
  target: chest_target
  iterations: 10
  tolerance: 0.001
  match_target_rotation: true
- type: LookAtIK             # a bone turning to face a target
  bone: head                  # bone name; empty = the rig's root bone (index 0)
  target: look_target
  forward_axis: { x: 0, y: 1, z: 0 }   # the bone's local looking axis (default +Y)
  up_axis: { x: 0, y: 0, z: 1 }        # kept from rolling (default +Z)
  max_angle: 70              # degrees from the animated facing
  weight: 1.0
  smoothing: 0.0             # seconds; 0 snaps
- type: FootIK               # plants legs on uneven ground (raycasts; toyengine only, not libcoopa)
  pelvis: pelvis               # bone name
  left: {thigh: thigh_l, shin: shin_l, foot: foot_l}    # bone names (default already thigh_l/shin_l/foot_l)
  right: {thigh: thigh_r, shin: shin_r, foot: foot_r}
  legs: []                     # non-empty: a list of {thigh, shin, foot, pole, front} replaces left/right -- any number of legs (a quadruped's four)
  body_pitch: false            # quadruped: raise/drop and pitch the pelvis bone by the legs' ground offsets
  max_body_pitch: 20           # degrees
  ray_up: 0.5                  # highest step a foot rises onto
  ray_down: 0.5                # deepest dip a foot follows
  max_pelvis_drop: 0.35
  align_feet: true             # tilt feet to the ground normal, up to max_foot_angle (30)
  blend_speed: 12              # 1/s; 0 snaps
  weight: 1.0
  layer_mask: -1               # physics layers the rays hit (default all)
  mode: auto                   # auto: targets if a leg bone is already TwoBoneIK-driven, else solve
```

Order each frame: `SkeletalAnimator` (300), then FootIK's leg probes/solves and every other IK
solver by `priority` (320) -- at equal priority every `ChainIK`, every `TwoBoneIK`, every
`LookAtIK` (spine before arms before head), then `BoneAttachment` (345) reads the final pose. A
target is usually an empty object a clip or gameplay moves. FootIK fades out while a sibling
CharacterController is airborne. Examples: `assets/objects/animation/ik_reacher.yaml`,
`ik_watcher.yaml` (in `animation_demo`), and the mannequin in `character_demo`.

**Fully IK rigs** (every bone solved, clips key only targets): copy
`assets/objects/characters/mannequin_ik.yaml` (in `ik_character_demo`). A `SkeletalAnimator` on
the same object still plays the ordinary clips, but only for bones no IK solver targets (the
pelvis's height bob, root motion) -- any bone an IK component poses overwrites the animator's
contribution, since IK runs later in the same frame. Foot targets and knee poles hang off the rig
root; hand, elbow, chest and look targets hang off a `pelvis_socket` child; limbs and the chest
use `match_target_rotation`; `FootIK` on such a rig switches to `targets` mode by itself.
`tools/gen_mannequin.py` shows how to bake target clips from keyframed ones (forward kinematics).

## Procedure

1. Decide property vs. skeletal (see the table at the top). For a rig: sketch the bone list
   (name, parent, head/tail/roll) and write/extend the rig asset.
2. Write clips by hand for a few keys, or extend/copy `tools/gen_mannequin.py` (skeletal) /
   `gen_animation_demo_scene.py` (either) for anything with many rotation keys. Store clips in
   `assets/animations/<name>/` (scene-only: `assets/scenes/<scene>/animations/<name>/`).
3. Add the `Animator` (property) or `Rig` + `SkeletalAnimator` (skeletal) with states; set
   `auto_play` so a scene shows it without code. For a rig, attach rigid parts/the skin with
   `BoneAttachment`/`SkinnedMeshRenderer`, then add any IK on the same object.
4. Place the object in a scene.

## Verify

Animation needs time: render at several frame counts and compare.

```sh
FRAMES=30  .claude/skills/create-scene/scripts/render_scene.sh <scene> "${TMPDIR:-/tmp}/anim_a.png"
FRAMES=75  .claude/skills/create-scene/scripts/render_scene.sh <scene> "${TMPDIR:-/tmp}/anim_b.png"
./build/toyengine_tests caml_roundtrips_every_asset
```

The poses must differ (and match intent). A property clip that never moves usually means a track
path doesn't match the object names; a skeletal clip, that a `bone:` name doesn't match the rig's
bones (check `[SkeletalAnimator]`'s stderr warning), or that `auto_play` names no state. A joint
that whips around the long way in a PROPERTY clip means a quaternion hemisphere flip (skeletal
clips always slerp, so this cannot happen there). Run the `rig`/`animation` tests if you changed
a generator's outputs (`./build/toyengine_tests --list | grep -iE 'rig|anim'`).
