# Rig Module (`coopa::rig`)

The `rig` module is Blender-style skeletal animation: a named skeleton (a
`RigAsset`), a live pose on it (a `Rig` component), clips that key bones
instead of objects (`SkeletalAnimator`), bone-space IK (`TwoBoneIK`,
`ChainIK`, `LookAtIK`), and a way for an ordinary object — a rigid mesh
part, a physics body, a socket — to ride a bone (`BoneAttachment`).

It exists because a bone is not an object. `coopa::anim` (`coopa/animation/`,
see its own README) poses a scene object's own Transform; that is the right
model for a camera move or a UI slide, but wrong for a skeleton — a bone has
no mesh, no light, nothing a scene-graph node is for, and giving it one
means every mesh-less marker near an `Animator` (an IK target, an attach
socket) risks being mistaken for a bone too. Here a bone is an index into a
`Rig`'s own pose buffer: it cannot be clicked, parented to, or confused with
anything else in the scene graph.

Like `coopa::anim`, this is a sibling of `coopa::scene`, not part of it, and
reuses `coopa::anim`'s curve/keyframe/easing/event/root-motion types and
`coopa::anim::ik`'s pure solve math directly rather than duplicating either.

---

## The pieces

| File | What it is |
|---|---|
| `rig_asset.h` | `Bone` (name, parent index, `head`/`tail`/`roll`) and `RigAsset`: a flat, parent-first bone list. `bone_rest_matrix()` derives each bone's rest basis — local **+Y** is the bone axis (`normalize(tail - head)`), X/Z orthonormal and rolled by `roll`, Z always `X × Y` so the basis is right-handed by construction. `compute_rest()` bakes `connected` bones' heads onto their parent's tail, then `rest_model()` (bone space → rig space) and `rest_local()` (bone space → parent's bone space). |
| `rig_loader.h` | Parses a `rig:` YAML file (`assets/rigs/<name>.yaml`) into a `RigAsset` and registers it as a `coopa::asset::TypedAssetLoader`. |
| `rig_component.h` | `Rig : coopa::scene::Component` — an `AssetHandle<RigAsset>` plus the live pose. See **The pose buffer** below. |
| `rig_system.h` | `RigPoseSystem`, `UpdatePhase` 340 — a deterministic end-of-phase `resolve_()` pass; harmless, since `resolve_()` is idempotent, but guarantees a `Rig` nothing else touches this frame is still resolved for whatever runs after it. |
| `rig_yaml.h` | `register_rig_components()` — the `"Rig"` / `"SkeletalAnimator"` / `"BoneAttachment"` scene component parsers, called once at startup next to `coopa::anim::register_animation_components()`. |
| `skeletal_clip.h` | `SkeletalClip` / `SkeletalTrack` — immutable clip data, one `location`/`rotation`/`scale` `coopa::anim::AnimationCurve` per bone **name**. `sample_rotation()` is true slerp with hemisphere correction applied at sample time, not relied on at authoring time (contrast `coopa::anim`'s `rotation_quat` property, which lerps components and normalizes — correct only if the keys were authored same-hemisphere). |
| `skeletal_clip_loader.h` | Parses a `clip:` YAML file into a `SkeletalClip` (`bones:` sequence, not `tracks:`), registered as a `TypedAssetLoader`. |
| `skeletal_animator.h` | `SkeletalAnimator : coopa::scene::Component` — states, crossfade, events, root motion, all mirroring `coopa::anim::Animator`'s contract; writes straight into a `Rig`'s pose buffer rather than object Transforms. |
| `skeletal_animation_system.h` | `install_skeletal_animation_system()`, `UpdatePhase` 300 (same phase as `coopa::anim`'s `AnimationSystem` — they drive disjoint components, so order between them doesn't matter). |
| `ik_components.h` | `IkComponent` base plus `TwoBoneIK`, `ChainIK`, `LookAtIK` — identical contracts to their `coopa::anim` namesakes, bone-space: bones are addressed by **name within the rig**, targets/poles stay ordinary scene-object paths (`coopa::anim::resolve_ik_path`, reused directly). |
| `ik_system.h` | `install_ik_system()`, `UpdatePhase` 320 — after `SkeletalAnimator` (300), so IK overwrites whatever pose a clip wrote to the bones it targets. |
| `rig_pose_math.h` | Free-function versions of `IkComponent`'s bone world-space ⟷ pose-delta math (`bone_world_position`, `apply_world_rotation_delta`, `apply_world_translation_delta`, ...), for a driver that needs physics/raycasts and so cannot itself inherit `IkComponent` (e.g. a ground-adapting foot driver). |
| `bone_attachment.h` / `bone_attachment_system.h` | `BoneAttachment` — an object's own Transform becomes an offset from a bone's head or tail; `UpdatePhase` 345, after `Rig`'s own resolve (340) and before `coopa::scene`'s `TransformSystem` (350). |

## The pose buffer

A `Rig::PoseChannel` is a delta from rest — **identity means exactly the rest
pose** — expressed in the bone's own rest-local frame (Blender's pose-channel
semantics, not a full local transform): `pose_local[i] = rest_local[i] *
compose_trs(pose.loc, pose.rot, pose.scale)`, `model[i] = model[parent] *
pose_local[i]`. This is what makes `clear_pose()` trivial and a clip
retargetable onto any rig sharing its bone names, and why the inverse bind a
skinning palette needs is just `inverse(rest_model[i])` — derived from the
asset, never snapshotted from a scene graph.

`Rig` resolves **lazily**, mirroring `coopa::util::Transform`'s own
dirty-flag pattern exactly: writing a pose channel (`pose()`/`clear_pose()`)
marks it dirty; the next read of `model_matrix()`/`world_matrix()` resolves
first. A single eager per-frame pass could not serve both halves of one
frame — IK (320) needs to read bone world positions reflecting the
`SkeletalAnimator`'s pose from the SAME frame (300), then write new deltas
of its own that a later reader (skinning, at upload time) must see already
resolved. `RigPoseSystem` (340) is kept anyway as a safety net, not the
mechanism: `resolve_()` is a pure function of the current pose, so forcing
it again there is always safe.

## IK, in bone space

`coopa::anim::ik`'s pure solve math (`solve_two_bone`, `solve_chain`
FABRIK, `look_at_delta`) is reused completely unchanged — it already works
on bare world positions and has no opinion on what it is posing.
`apply_world_rotation_delta()` is the piece specific to a pose buffer: it
converts a WORLD rotation delta into the bone's own pose-local delta via
`to_local_delta(parent_world_rot, rest_local_rot * current_pose_rot,
blended_delta)`, then strips `rest_local_rot` back out so what gets stored
is a pure delta, not the bone's full local rotation.

There is **no `IkPoseGuard`** here, unlike `coopa::anim::ik_components.h`.
That guard exists only because a SceneObject's Transform is simultaneously
an `Animator`'s output and an IK solver's input/output, with nothing but a
bit-exact float comparison to tell "did IK write this last frame" from "did
the clip." A `Rig`'s pose channel has exactly one writer per frame per bone
(whichever of `SkeletalAnimator` / IK / a physics driver runs last for that
bone, by `UpdatePhase` order) and nothing it needs to distinguish from
itself — a bone nothing touches this frame simply sits at `clear_pose()`'s
identity, with no feedback loop to guard against.

## Converting an existing object-hierarchy rig

A bone's local **+Y** is its axis; an old object-hierarchy rig has no such
convention (its "bones" are just translated Transforms with identity rest
rotation), so a mechanical conversion needs:

- **Rotation keys**: a bone's rest basis `R` (its `rest_model()` rotation)
  conjugates every old key: `pose.rotation(t) = Rᵗ · key_old(t) · R`. This
  holds for every bone, root or not, including the root's own key (if any).
- **Position keys** (normally only a root bone like a `pelvis` or a `body`):
  `pose.location(t) = Rᵗ · (old_absolute_position(t) − head_rest)`.
- **A rigid part riding a bone via `BoneAttachment`** needs the same fixed
  correction applied once to its own Transform: `new_position = Rᵗ ·
  old_position`, `new_rotation = Rᵗ · old_rotation` (its scale is untouched
  — a pure rotation prepended on the left never touches it).
- **Root motion** (`coopa::anim::RootMotionTranslation::XY`) extracts bits
  0/1 of `pose.location` verbatim — X and Y in the bone's OWN axes, not the
  world's. A root bone meant to carry root motion must have its own **+Y**
  axis chosen along the character's forward direction (not, say, "up the
  spine"), so that "horizontal" lands on bits 0/1 and "vertical" lands on
  bit 2 (excluded) — exactly the old convention's Z-up split, just rotated
  into the bone's own frame.

See `tools/gen_mannequin.py` / `tools/gen_horse.py` for worked examples (they
emit this format; the generator is the source of truth, not the committed
YAML).

---

## YAML

```yaml
# assets/rigs/<name>.yaml
rig:
  name: mannequin
  bones:
    - name: pelvis
      head: {x: 0, y: 0, z: 0.95}
      tail: {x: 0, y: 0.1, z: 0.95}
    - name: spine
      parent: pelvis
      head: {x: 0, y: 0, z: 1.05}
      tail: {x: 0, y: 0, z: 1.25}
      # roll: 0            -- degrees about the head->tail axis
      # connected: true    -- locks this bone's head to its parent's tail
```

```yaml
# a scene object
- type: Rig
  rig: rigs/mannequin.yaml
- type: SkeletalAnimator
  auto_play: idle
  default_crossfade: 0.2
  states:
    - {name: idle, clip: animations/mannequin/idle.yaml}
    - {name: walk, clip: animations/mannequin/walk.yaml}
- type: TwoBoneIK
  upper: thigh_l
  lower: shin_l
  end: foot_l
  target: foot_target_l    # an ordinary scene-object path
  pole: knee_pole_l
```

```yaml
# a skeletal clip: bone names, not object paths/properties
clip:
  name: walk
  wrap: loop
  length: 0.9
  root_motion: {bone: pelvis, translation: xy, rotation: none}
  events:
    - {time: 0.2333, name: footstep, string: left}
  bones:
    - bone: pelvis
      location: [{time: 0, value: [0, 0, 0.03]}, ...]
    - bone: spine
      rotation: [{time: 0, value: [0, 0, 0, 1]}, ...]
```

```yaml
# a rigid part riding a bone
- type: Transform
  position: {x: 0, y: 0.215, z: 0}   # offset from the bone's head, in the BONE's own axes
- type: BoneAttachment
  bone: thigh_l
  # rig: <path>   -- optional explicit Rig; empty finds the nearest ancestor carrying one
  # at: tail      -- head (default) or tail
```

---

## Testing

Engine-side unit tests (libcoopa has no ctest entry of its own — see
`coopa::anim`'s README): `tests/engine/unit/rig_test.cpp` (rest-basis
derivation, roll, `compute_rest()`, pose-delta composition, lazy
resolution, YAML parsing), `skeletal_animator_test.cpp` (bone-by-name
tracks, slerp crossfade, events, root-motion hold), `rig_ik_test.cpp`
(two-bone reach, chain FABRIK, a spine solving before the arms hanging off
it, `match_target_rotation`, `BoneAttachment` riding a posed bone). GPU:
`tests/engine/gpu/rig_render_test.cpp` (a `SkinnedMeshRenderer` following an
animated bone end to end).
