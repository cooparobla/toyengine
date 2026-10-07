---
name: create-animation
description: Create toyengine animations -- keyframed or procedural clips in assets/animations/<rig>/<clip>.yaml, the Animator component and its states on a rig object, and skinned-mesh rigs (bones as child objects, vertex-group weights). Use when asked to animate something, make a clip / walk / idle / wave / bounce, set up a rig or Animator, or make a skinned (bending) mesh move.
---

# Create an animation

Clips animate properties (Transform position/rotation/scale, and a few UI/graphic properties) of
objects addressed **by path from the Animator's object**. Format reference:
`libs/libcoopa/coopa/animation/README.md` ("YAML schema"); loader
`animation_clip_loader.h`. Generator with helpers to copy: `tools/gen_animation_test_scene.py`
(`quat_axis_angle`, `rot_keys` with hemisphere fix, `clip(...)`, skinned tube mesh).
Examples: `assets/objects/animation/robot_arm.yaml` + `assets/animations/robot_arm/*.yaml`,
`bouncing_ball`, `tentacle` (skinned). Scene: `./build/toyengine animation_test`.

## Clip file (one clip per file)

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

## Animator (on the rig's root object)

```yaml
- type: Animator
  auto_play: idle            # state played on start
  speed: 1.0
  default_crossfade: 0.15
  states:
    - { name: idle, clip: animations/<rig>/idle.yaml }
    - { name: wave, clip: animations/<rig>/wave.yaml, wrap: once, speed: 0.75 }
```

Clip paths resolve against the scene/prefab folder first, then `assets/`. The animation system
is installed by the engine automatically.

## Rigs

- Build the rig as an object asset (create-object): **joints are empty objects** at their pivot,
  each visible part a child of its joint (so scaling a shape never scales a joint). Name every
  joint: tracks address them by path, so renaming or reparenting a joint breaks its tracks.
- Animate relative to the rig root (e.g. a bounce animates a child, not the root), so an
  instance placed anywhere animates in place.
- **Skinned mesh**: bones are child objects; the mesh's `weights:` vertex groups are named after
  them (create-mesh). The skin object gets a MeshRenderer (material only, no `mesh_path`) and
  `SkinnedMeshRenderer { mesh_path: <mesh> }` (optional `bones:` list, `rig:`). Bind pose = the
  rest pose at start.

## Procedure

1. Sketch the rig hierarchy and pivots; build/extend the object asset.
2. Write clips by hand for a few keys, or extend/copy `gen_animation_test_scene.py`'s helpers
   for anything with many rotation keys (it handles quaternion sign flips). Store clips in
   `assets/animations/<rig>/` (scene-only: `assets/scenes/<scene>/animations/<rig>/`).
3. Add the Animator with states; set `auto_play` so a scene shows it without code.
4. Place the rig in a scene.

## Verify

Animation needs time: render at several frame counts and compare.

```sh
FRAMES=30  .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/anim_a.png"
FRAMES=75  .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/anim_b.png"
./build/toyengine_tests caml_roundtrips_every_asset
```

The poses must differ (and match intent). A rig that never moves usually means a track path
doesn't match the object names, or `auto_play` names no state. A joint that whips around the
long way means a quaternion hemisphere flip. Run the `animation` tests if you changed
`tools/gen_animation_test_scene.py` outputs (`./build/toyengine_tests --list | grep -i anim`).
