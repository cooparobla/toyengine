#!/usr/bin/env python3
"""Writes the animation test rigs: object assets, their clips and meshes, and a scene using them.

Outputs (all under assets/):
  * objects/robot_arm.yaml, objects/tentacle.yaml, objects/bouncing_ball.yaml -- the rigs as
    OBJECT ASSETS (the editor's Objects tab): open one to animate it, or place it in any scene.
  * animations/<rig>/*.yaml -- their clips; meshes/tentacle.yaml, meshes/ball.yaml.
  * scenes/animation_test/scene.yaml -- the three rigs inline (editable in the scene itself),
    sharing those clips and meshes (paths resolve next to the scene, then from assets/).


  * robot_arm: a rig of plain objects (base > shoulder > upper_arm, elbow > forearm, wrist > hand,
    fingers). Two clips: `wave` (auto-played) and `idle`. Rotations are `rotation_quat` keys with
    eased interpolation -- exactly what the toyeditor's Timeline records.
  * tentacle: a SKINNED rig. A tube mesh whose vertex groups (seg_0..seg_3, as Weight Paint writes
    them) blend smoothly between four chained bones; no `bones:` list and no inverse bind
    matrices -- the SkinnedMeshRenderer binds the groups to the rig's objects by name against the
    rest pose. It also carries a vertex-colour gradient (data, as Vertex Paint writes it). Clip
    `sway`.
  * ball: an object animating ITSELF (track object ""): a bounce with squash and stretch, using
    step and ease keys. Clip `bounce`.

Run:  python3 tools/gen_animation_test_scene.py   then   ./build/toyengine animation_test
Open it in the toyeditor and select a rig object: the Timeline (bottom panel) shows its clips.
"""

import math
import os

ASSETS = os.path.join(os.path.dirname(__file__), "..", "assets")
ROOT = os.path.join(ASSETS, "scenes", "animation_test")


def fmt(v):
    return "[" + ", ".join(f"{x:.6g}" for x in v) + "]"


def quat_axis_angle(axis, deg):
    a = math.radians(deg) * 0.5
    s = math.sin(a)
    n = math.sqrt(sum(c * c for c in axis))
    return [axis[0] / n * s, axis[1] / n * s, axis[2] / n * s, math.cos(a)]


# ---------------------------------------------------------------------------------------------
# Meshes
# ---------------------------------------------------------------------------------------------

def tentacle_mesh(path, height=3.0, radius=0.22, sides=10, rings=24, bones=4):
    """An open tube along +Z, tapering, with smooth vertex-group weights over `bones` segments."""
    seg = height / bones
    verts, normals, uvs, colors, weights, faces = [], [], [], [], [], []
    for r in range(rings + 1):
        z = height * r / rings
        rad = radius * (1.0 - 0.65 * r / rings)
        # Weights: a tent function per bone centred on the bone's start, blended with its neighbours.
        w = []
        for b in range(bones):
            centre = (b + 0.5) * seg
            d = abs(z - centre) / seg
            w.append(max(0.0, 1.0 - d))
        if z < 0.5 * seg:
            w[0] = 1.0
        total = sum(w) or 1.0
        w = [x / total for x in w]
        t = r / rings
        col = [0.95 - 0.6 * t, 0.35 + 0.4 * t, 0.25 + 0.6 * t, 1.0]   # warm base -> cool tip
        for s in range(sides + 1):
            a = 2.0 * math.pi * s / sides
            verts.append([rad * math.cos(a), rad * math.sin(a), z])
            normals.append([math.cos(a), math.sin(a), 0.0])
            uvs.append([s / sides, t])
            colors.append(col)
            weights.append({f"seg_{b}": round(w[b], 4) for b in range(bones) if w[b] > 1e-4})
    row = sides + 1
    for r in range(rings):
        for s in range(sides):
            a = r * row + s
            faces.append([a, a + 1, a + row + 1, a + row])
    lines = ["vertices:"] + [f"  - {fmt(v)}" for v in verts]
    lines += ["normals:"] + [f"  - {fmt(v)}" for v in normals]
    lines += ["uvs:"] + [f"  - {fmt(v)}" for v in uvs]
    lines += ["faces:"] + [f"  - {fmt(f)}" for f in faces]
    lines += ["colors:"] + [f"  - {fmt(c)}" for c in colors]
    lines += ["weights:"]
    for wmap in weights:
        lines.append("  - {" + ", ".join(f"{k}: {v:.4g}" for k, v in wmap.items()) + "}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def sphere_mesh(path, radius=0.5, segs=24, rings=12):
    """A smooth UV sphere (per-ring rows; poles as triangles)."""
    verts, normals, uvs, faces = [], [], [], []
    for r in range(rings + 1):
        phi = math.pi * r / rings - math.pi / 2
        for s in range(segs + 1):
            th = 2 * math.pi * s / segs
            n = [math.cos(phi) * math.cos(th), math.cos(phi) * math.sin(th), math.sin(phi)]
            verts.append([radius * c for c in n])
            normals.append(n)
            uvs.append([s / segs, r / rings])
    row = segs + 1
    for r in range(rings):
        for s in range(segs):
            a = r * row + s
            if r == 0:
                faces.append([a, a + row + 1, a + row])
            elif r == rings - 1:
                faces.append([a, a + 1, a + row])
            else:
                faces.append([a, a + 1, a + row + 1, a + row])
    lines = ["vertices:"] + [f"  - {fmt(v)}" for v in verts]
    lines += ["normals:"] + [f"  - {fmt(v)}" for v in normals]
    lines += ["uvs:"] + [f"  - {fmt(v)}" for v in uvs]
    lines += ["faces:"] + [f"  - {fmt(f)}" for f in faces]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------------------------
# Clips
# ---------------------------------------------------------------------------------------------

def clip(path, name, wrap, length, tracks):
    lines = ["clip:", f"  name: {name}", f"  wrap: {wrap}", f"  length: {length}", "  tracks:"]
    for obj, prop, keys in tracks:
        lines += [f"    - object: {obj!r}" if obj == "" else f"    - object: {obj}", f"      property: {prop}", "      keys:"]
        for k in keys:
            t, v = k[0], k[1]
            ease = k[2] if len(k) > 2 else None
            entry = f"        - {{time: {t:.6g}, value: {fmt(v)}" + (f", easing: {ease}" if ease else "") + "}"
            lines.append(entry)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def rot_keys(axis, frames, ease="ease_in_out"):
    """[(time, degrees)] -> rotation_quat keys about `axis`, consecutive keys in one hemisphere."""
    out, prev = [], None
    for t, deg in frames:
        q = quat_axis_angle(axis, deg)
        if prev and sum(a * b for a, b in zip(prev, q)) < 0:
            q = [-c for c in q]
        out.append((t, q, ease))
        prev = q
    return out


def write_clips():
    arm = os.path.join(ASSETS, "animations", "robot_arm")
    clip(os.path.join(arm, "wave.yaml"), "wave", "loop", 2.0, [
        ("base/shoulder", "rotation_quat", rot_keys((0, 0, 1), [(0, -50), (1.0, 50), (2.0, -50)])),
        ("base/shoulder/upper_arm/elbow", "rotation_quat", rot_keys((1, 0, 0), [(0, 10), (0.5, 75), (1.0, 10), (1.5, 75), (2.0, 10)])),
        ("base/shoulder/upper_arm/elbow/forearm/wrist", "rotation_quat",
         rot_keys((0, 1, 0), [(0, 0), (0.25, 35), (0.5, -35), (0.75, 35), (1.0, 0), (2.0, 0)])),
        ("base/shoulder/upper_arm/elbow/forearm/wrist/hand/finger_l", "position",
         [(0, [-0.12, 0, 0.18], "ease_in_out"), (0.5, [-0.04, 0, 0.18], "ease_in_out"), (1.0, [-0.12, 0, 0.18]), (2.0, [-0.12, 0, 0.18])]),
        ("base/shoulder/upper_arm/elbow/forearm/wrist/hand/finger_r", "position",
         [(0, [0.12, 0, 0.18], "ease_in_out"), (0.5, [0.04, 0, 0.18], "ease_in_out"), (1.0, [0.12, 0, 0.18]), (2.0, [0.12, 0, 0.18])]),
    ])
    clip(os.path.join(arm, "idle.yaml"), "idle", "loop", 3.0, [
        ("base/shoulder", "rotation_quat", rot_keys((0, 0, 1), [(0, -8), (1.5, 8), (3.0, -8)])),
        ("base/shoulder/upper_arm/elbow", "rotation_quat", rot_keys((1, 0, 0), [(0, 20), (1.5, 28), (3.0, 20)])),
    ])
    tent = os.path.join(ASSETS, "animations", "tentacle")
    tracks = []
    path = ""
    for b in range(4):
        path = f"seg_{b}" if b == 0 else f"{path}/seg_{b}"
        amp = 14 + 6 * b
        frames = [(0.0, -amp), (1.0, amp), (2.0, -amp)]
        keys = rot_keys((0, 1, 0), frames)   # side to side, across the camera's view
        # Delay the deeper segments: shift their middle key so the wave travels up the chain.
        keys[1] = (1.0 + 0.15 * b, keys[1][1], "ease_in_out")
        tracks.append((path, "rotation_quat", keys))
    clip(os.path.join(tent, "sway.yaml"), "sway", "loop", 2.0, tracks)
    ball = os.path.join(ASSETS, "animations", "bouncing_ball")
    # The ball (a child) bounces relative to its rig, so a placed copy bounces where it stands.
    clip(os.path.join(ball, "bounce.yaml"), "bounce", "loop", 1.0, [
        ("ball", "position", [(0.0, [0, 0, 2.2], "ease_in"), (0.45, [0, 0, 0.3]), (0.55, [0, 0, 0.3], "ease_out"),
                              (1.0, [0, 0, 2.2])]),
        ("ball", "scale", [(0.0, [0.6, 0.6, 0.6], "step"), (0.4, [0.6, 0.6, 0.6], "ease_in"), (0.5, [0.8, 0.8, 0.36], "ease_out"),
                           (0.62, [0.6, 0.6, 0.6]), (1.0, [0.6, 0.6, 0.6])]),
    ])


# ---------------------------------------------------------------------------------------------
# Scene
# ---------------------------------------------------------------------------------------------

def cube(name, pos, scale, color, children="[]", extra=""):
    return f"""- name: {name}
  components:
    - type: Transform
      position: {{x: {pos[0]}, y: {pos[1]}, z: {pos[2]}}}
      scale: {{x: {scale[0]}, y: {scale[1]}, z: {scale[2]}}}
    - type: MeshRenderer
      mesh_path: cube
      material: {{albedo: {{r: {color[0]}, g: {color[1]}, b: {color[2]}}}, metallic: 0.2, roughness: 0.45}}{extra}
  children: {children}"""


def empty(name, pos, children):
    return f"""- name: {name}
  components:
    - type: Transform
      position: {{x: {pos[0]}, y: {pos[1]}, z: {pos[2]}}}
  children:
{children}"""


def indent(text, n):
    pad = " " * n
    return "\n".join(pad + line if line.strip() else line for line in text.split("\n"))


def rigs(at):
    """The three rigs as list items; `at` maps rig name -> root position."""
    steel, orange, dark = (0.55, 0.58, 0.62), (0.95, 0.5, 0.15), (0.2, 0.22, 0.25)
    # Shapes are child cubes, so scaling them never scales the joints below.
    fingers = indent(cube("finger_l", (-0.12, 0, 0.18), (0.06, 0.12, 0.28), dark), 4) + "\n" + \
              indent(cube("finger_r", (0.12, 0, 0.18), (0.06, 0.12, 0.28), dark), 4)
    hand = empty("hand", (0, 0, 0.1), indent(cube("palm", (0, 0, 0), (0.34, 0.16, 0.12), orange), 4) + "\n" + fingers)
    wrist = empty("wrist", (0, 0, 0.9), indent(hand, 4))
    forearm = empty("forearm", (0, 0, 0), indent(cube("forearm_shape", (0, 0, 0.45), (0.2, 0.2, 0.9), steel), 4) + "\n" + indent(wrist, 4))
    elbow = empty("elbow", (0, 0, 1.1), indent(cube("elbow_joint", (0, 0, 0), (0.3, 0.3, 0.3), orange), 4) + "\n" + indent(forearm, 4))
    upper = empty("upper_arm", (0, 0, 0), indent(cube("upper_arm_shape", (0, 0, 0.55), (0.26, 0.26, 1.1), steel), 4) + "\n" + indent(elbow, 4))
    shoulder = empty("shoulder", (0, 0, 0.35), indent(cube("shoulder_joint", (0, 0, 0), (0.36, 0.36, 0.36), orange), 4) + "\n" + indent(upper, 4))
    base = empty("base", (0, 0, 0), indent(cube("base_plate", (0, 0, 0.15), (0.9, 0.9, 0.3), dark), 4) + "\n" + indent(shoulder, 4))
    robot = f"""- name: robot_arm
  components:
    - type: Transform
      position: {{x: {at['robot_arm'][0]}, y: {at['robot_arm'][1]}, z: {at['robot_arm'][2]}}}
    - type: Animator
      auto_play: wave
      states:
        - {{name: wave, clip: animations/robot_arm/wave.yaml}}
        - {{name: idle, clip: animations/robot_arm/idle.yaml}}
  children:
{indent(base, 4)}"""

    segs = ""
    for b in reversed(range(4)):
        z = 0.0 if b == 0 else 0.75
        inner = indent(segs, 4) if segs else "    []"
        segs = f"""- name: seg_{b}
  components:
    - type: Transform
      position: {{x: 0, y: 0, z: {z}}}
  children:
{inner}"""
    tentacle = f"""- name: tentacle
  components:
    - type: Transform
      position: {{x: {at['tentacle'][0]}, y: {at['tentacle'][1]}, z: {at['tentacle'][2]}}}
    - type: Animator
      auto_play: sway
      states:
        - {{name: sway, clip: animations/tentacle/sway.yaml}}
  children:
{indent(segs, 4)}
    - name: tentacle_skin
      components:
        - type: Transform
        - type: MeshRenderer
          material: {{albedo: {{r: 0.45, g: 0.75, b: 0.6}}, metallic: 0.0, roughness: 0.5}}
        - type: SkinnedMeshRenderer
          mesh_path: tentacle
      children: []"""

    ball = f"""- name: bouncing_ball
  components:
    - type: Transform
      position: {{x: {at['bouncing_ball'][0]}, y: {at['bouncing_ball'][1]}, z: {at['bouncing_ball'][2]}}}
    - type: Animator
      auto_play: bounce
      states:
        - {{name: bounce, clip: animations/bouncing_ball/bounce.yaml}}
  children:
    - name: ball
      components:
        - type: Transform
          position: {{x: 0, y: 0, z: 2.2}}
          scale: {{x: 0.6, y: 0.6, z: 0.6}}
        - type: MeshRenderer
          mesh_path: ball
          material: {{albedo: {{r: 0.85, g: 0.15, b: 0.2}}, metallic: 0.0, roughness: 0.35}}
      children: []"""
    return {"robot_arm": robot, "tentacle": tentacle, "bouncing_ball": ball}


OBJECT_FILES = {"robot_arm": "robot_arm", "tentacle": "tentacle", "bouncing_ball": "bouncing_ball"}
OBJECT_NOTES = {
    "robot_arm": "an object-hierarchy rig (joints are empties, shapes are child cubes); clips wave + idle",
    "tentacle": "a skinned rig: vertex groups seg_0..seg_3 bind the tube to its chain of bones; clip sway",
    "bouncing_ball": "a squash-and-stretch bounce of the child ball, relative to where the rig is placed; clip bounce",
}


def write_objects():
    """Each rig as an object asset (assets/objects/), its root at the origin."""
    os.makedirs(os.path.join(ASSETS, "objects"), exist_ok=True)
    origin = {k: (0, 0, 0) for k in OBJECT_FILES}
    for name, text in rigs(origin).items():
        lines = text.split("\n")
        obj = [lines[0][2:]] + [ln[2:] if ln.startswith("  ") else ln for ln in lines[1:]]
        doc = (f"# {name}: {OBJECT_NOTES[name]}.\n"
               f"# GENERATED by tools/gen_animation_test_scene.py; edit that, not this file.\n"
               f"format: toyengine-object\nobject:\n" + indent("\n".join(obj), 2) + "\n")
        with open(os.path.join(ASSETS, "objects", OBJECT_FILES[name] + ".yaml"), "w") as f:
            f.write(doc)


def scene():
    r = rigs({"robot_arm": (-2.2, 0.0, 0.0), "tentacle": (0.8, 0.6, 0.0), "bouncing_ball": (3.2, -1.2, 0.0)})
    robot, tentacle, ball = r["robot_arm"], r["tentacle"], r["bouncing_ball"]


    text = f"""# animation_test -- rigs as object hierarchies, animated by clip files (toyengine + toyeditor).
# GENERATED by tools/gen_animation_test_scene.py; edit that, not this file.
#
#   * robot_arm: an object-hierarchy rig, clips `wave` (auto-played) and `idle`.
#   * tentacle: a skinned rig -- vertex groups bind the tube mesh to its chain of bones.
#   * bouncing_ball: a squash-and-stretch bounce.
# Clips and meshes are shared with the object assets (assets/objects/): assets/animations/,
# assets/meshes/ -- paths resolve next to this scene first, then from assets/.
#
# In the toyeditor, select any rig object and open the Timeline tab (bottom panel).
format: blender
scene:
  scene_name: animation_test
  root_objects:
    - name: ground
      components:
        - type: Transform
          position: {{x: 0, y: 0, z: -0.05}}
          scale: {{x: 12, y: 8, z: 0.1}}
        - type: MeshRenderer
          mesh_path: cube
          material: {{albedo: {{r: 0.42, g: 0.45, b: 0.48}}, metallic: 0.0, roughness: 0.9}}
        - type: BoxCollider
          size: {{x: 1.0, y: 1.0, z: 1.0}}
      children: []
{indent(robot, 4)}
{indent(tentacle, 4)}
{indent(ball, 4)}
    - name: camera
      components:
        - type: Transform
          position: {{x: 0.5, y: -9.0, z: 3.4}}
          rotation: {{x: 78.0, y: 0.0, z: 0.0}}
        - type: Camera
          main: true
          projection: Perspective
          fov: 50.0
          near_clip_plane: 0.1
          far_clip_plane: 200.0
        - type: CameraController
          mode: orbit
          target: {{x: 0.5, y: 0.0, z: 1.3}}
          distance: 9.0
          yaw_deg: 0.0
          pitch_deg: 16.0
          movement_smoothing: 0.25
          min_distance: 2.0
          max_distance: 40.0
          capture_cursor: true
      children: []
    - name: sun
      components:
        - type: Transform
          position: {{x: 0, y: 0, z: 10}}
        - type: DirectionalLight
          direction: {{x: -0.45, y: 0.4, z: -0.8}}
          color: {{r: 1.0, g: 0.96, b: 0.9}}
          intensity: 1.3
          cast_shadows: true
          shadow_intensity: 1.0
      children: []
    - name: sky
      components:
        - type: Transform
        - type: EnvironmentLight
          sky_color: {{r: 0.5, g: 0.62, b: 0.85}}
          ground_color: {{r: 0.22, g: 0.2, b: 0.18}}
          sky_intensity: 1.0
      children: []
"""
    with open(os.path.join(ROOT, "scene.yaml"), "w") as f:
        f.write(text)


def main():
    os.makedirs(ROOT, exist_ok=True)
    tentacle_mesh(os.path.join(ASSETS, "meshes", "tentacle.yaml"))
    sphere_mesh(os.path.join(ASSETS, "meshes", "ball.yaml"))
    write_objects()
    write_clips()
    scene()
    print("wrote", os.path.normpath(ROOT))


if __name__ == "__main__":
    main()
