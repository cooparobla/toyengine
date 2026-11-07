#!/usr/bin/env python3
"""Writes the `mannequin`: a humanoid Rig + SkeletalAnimator, its capsule part meshes (riding
the rig's bones via BoneAttachment) and its clips.

Outputs (all under assets/):
  * rigs/mannequin.yaml -- the skeleton as a RIG ASSET: a flat, parent-first bone list
    (head/tail/roll in rig space; see coopa/rig/rig_asset.h).
  * objects/characters/mannequin.yaml -- a Rig + SkeletalAnimator OBJECT ASSET (prefab), its
    capsule parts attached to bones via BoneAttachment. Place it with `prefab: objects/mannequin`;
    add a CharacterController (+ CharacterAnimDriver) to the instance to make it a player (see
    scenes/gameplay/character_demo).
  * objects/characters/mannequin_ragdoll.yaml -- the same rig plus a Ragdoll (capsule/box bones,
    cone-twist hips/shoulders/spine/neck, hinge knees/elbows/ankles, addressed by BONE NAME; see
    RAGDOLL below and toyengine/scene/ragdoll.h). A separate prefab, so plain mannequins carry no
    bone bodies.
  * objects/characters/mannequin_ik.yaml -- the same rig FULLY IK-driven: a SkeletalAnimator
    plays the ordinary clips (only the pelvis bob/root-motion bone-space survives past IK, since
    IK runs at a later update order and overwrites every bone it targets), and a plain Animator
    drives floating target/pole empties -- foot / hand targets, knee / elbow poles, a chest
    target, a look target -- which bone-space TwoBoneIK/ChainIK/LookAtIK pose every other bone
    from (match_target_rotation). The target clips are BAKED from the keyframed ones (forward
    kinematics over the OLD identity-rest joint tree below, unrelated to the Rig's own bone
    basis), so it moves like the plain mannequin; FootIK on it adjusts the foot targets (mode
    auto -> targets).
  * meshes/characters/mannequin_*.yaml -- one capsule mesh per body part (Z-up, centred).
  * animations/mannequin/{idle,walk,run,jump,ride}.yaml -- the SKELETAL clips the SkeletalAnimator
    plays (ride: seated in a saddle, for a Rider -- toyengine/scene/rider.h). walk and run carry
    ROOT MOTION (the pelvis's travel, `root_motion: {bone: pelvis, translation: xy}`) and
    `footstep` events (string left / right) as each foot plants. Each bone's `location`/
    `rotation` is a POSE DELTA from the Rig's rest pose (coopa/rig/rig_component.h's
    Rig::PoseChannel), not a plain local Transform value -- see CONJUGATION below.
  * animations/mannequin_ik/{idle,walk,run,jump,ride}.yaml -- the same motion as OLD-style
    property clips on the target/pole empties (object paths, not bone names) -- these drive
    plain SceneObjects, not the rig, so they keep the property-clip format untouched.

The rig, Z-up and facing local +Y (its right hand is +X, its left -X), origin at the feet,
about 1.8 m tall -- the default CharacterController capsule (radius 0.3, height 1.8):

  mannequin (Rig)                SkeletalAnimator (idle / walk / run / jump / ride)
    pelvis                       z 0.95
      spine                        abdomen
        chest                      torso
          neck
            head                   head + visor (the visor marks the front)
          upper_arm_l / _r         shoulders at x -/+0.21
            lower_arm_l / _r
              hand_l / _r
      thigh_l / _r                 hips at x -/+0.1
        shin_l / _r
          foot_l / _r

Bones are flat entries in rigs/mannequin.yaml (head/tail/roll in rig space; NOT SceneObjects),
addressed by name. Limbs hang along local -Z; limbs' REST bone axis (head->tail) determines
each bone's own basis (coopa/rig/rig_asset.h's bone_rest_matrix -- Y = head->tail, X/Z from a
stable seed). Each visible part is a `<bone>_shape` object carrying `BoneAttachment { bone:
<bone> }`, flat (not nested) under the rig's root object. Bone names are the rig's contract:
clips, IK, FootIK and ragdolls all address them by name -- renaming one here breaks every
clip/IK/ragdoll entry below.

CONJUGATION: this generator's clip math (`rot_track`/`gait`/`idle`/`jump`/`ride`) is still
written in terms of the OLD convention -- "each joint's local-to-parent rotation, assuming an
identity rest pose" -- because that convention is also what the mannequin_ik's forward-kinematics
baking (`fk_pose`, further below) needs, independent of the Rig system. `write_clip()` converts
that OLD per-joint local rotation into the NEW Rig pose-delta convention (a delta from rest, IN
THE BONE'S OWN REST-LOCAL FRAME) by conjugating it through the bone's own rest basis quaternion
(from `bone_rest_matrix`): if R_old is the old joint's local rotation and B is the bone's own
rig-space rest-basis quaternion, the stored pose-delta is `inverse(B) * R_old * B`. Derivation:
Rig::resolve_() composes `model_i = model_parent * rest_local_i * pose_i`, and by induction
`model_i == old_world_i * rest_model_i` (the new model matches the old world pose, composed with
the bone's own rest basis) exactly when `pose_i = rest_model_i^-1 * R_old_i * rest_model_i` --
verified numerically against every bone in the real walk/run clips (e.g. spine's constant lean
flips from the old local X axis to the new local Z axis, same magnitude, because spine's own
rest basis is a 120-degree axis permutation of rig space). The pelvis's LOCATION channel (the
only bone with one) undergoes the analogous translation-side conversion: the old absolute
position becomes `inverse(B_pelvis) * (old_pos - pelvis_head)`.

Run:  python3 tools/gen_mannequin.py
"""

import math
import os

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
MESH_DIR = os.path.join(ASSETS, "meshes", "characters")
OBJECT_PATH = os.path.join(ASSETS, "objects", "characters", "mannequin.yaml")
RAGDOLL_OBJECT_PATH = os.path.join(ASSETS, "objects", "characters", "mannequin_ragdoll.yaml")
IK_OBJECT_PATH = os.path.join(ASSETS, "objects", "characters", "mannequin_ik.yaml")
RIG_PATH = os.path.join(ASSETS, "rigs", "mannequin.yaml")
ANIM_DIR = os.path.join(ASSETS, "animations", "mannequin")
IK_ANIM_DIR = os.path.join(ASSETS, "animations", "mannequin_ik")

GENERATED = "GENERATED by tools/gen_mannequin.py; edit that, not this file."
IK_HEADER_NOTE = "Fully IK: its clips key only the *_target / *_pole empties; IK poses every bone (see the generator)."


def fmt(v):
    return "[" + ", ".join(f"{x:.6g}" for x in v) + "]"


def fmt6(v):
    """Like fmt(), but 6 DECIMAL PLACES (not 6 significant digits) -- the skeletal clip's
    `location` curve (small pose-delta offsets) uses this precision, not fmt()'s .6g."""
    def one(x):
        s = f"{x:.6f}".rstrip("0").rstrip(".")
        return s if s not in ("", "-") else "0"
    return "[" + ", ".join(one(c) for c in v) + "]"


def clean0(v):
    """Folds -0.0 to 0.0 componentwise -- used only where the skeletal clip's conjugated values
    need it (the OLD property-clip format keeps whatever sign the original math produced)."""
    return [c + 0.0 for c in v]


def num(x):
    return f"{x:.6g}"


# ---------------------------------------------------------------------------------------------
# Capsule meshes
# ---------------------------------------------------------------------------------------------

def capsule_mesh(path, radius, length, sides=16, cap_rings=6, label="mannequin part", generated=None):
    """A capsule along Z centred at the origin: `length` between the cap centres (0 = sphere).

    A lathe: rings from the bottom pole to the top pole, a duplicated u = 1 seam column, and
    triangles (not degenerate quads) at the poles.
    """
    half = length * 0.5
    rings = []  # (radius at ring, z, normal_r, normal_z, v)
    for i in range(cap_rings + 1):  # bottom cap, pole -> equator
        a = -math.pi / 2 + (math.pi / 2) * i / cap_rings
        rings.append((radius * math.cos(a), -half + radius * math.sin(a), math.cos(a), math.sin(a)))
    for i in range(cap_rings + 1):  # top cap, equator -> pole
        a = (math.pi / 2) * i / cap_rings
        rings.append((radius * math.cos(a), half + radius * math.sin(a), math.cos(a), math.sin(a)))
    total = (length + 2 * radius) or 1.0
    verts, normals, uvs, faces = [], [], [], []
    row = sides + 1
    for (rr, z, nr, nz) in rings:
        v = (z + half + radius) / total
        for s in range(sides + 1):
            a = 2 * math.pi * s / sides
            c, sn = math.cos(a), math.sin(a)
            verts.append([rr * c, rr * sn, z])
            n = [nr * c, nr * sn, nz]
            ln = math.sqrt(sum(x * x for x in n)) or 1.0
            normals.append([x / ln for x in n])
            uvs.append([s / sides, v])
    last = len(rings) - 1
    for r in range(last):
        for s in range(sides):
            a = r * row + s
            b, c, d = a + 1, a + row + 1, a + row
            if r == 0:
                faces.append([a, c, d])          # bottom pole: ring 0 is a point
            elif r == last - 1:
                faces.append([a, b, c])          # top pole: ring `last` is a point
            else:
                faces.append([a, b, c, d])
    lines = [f"# {label}: capsule r {num(radius)}, cap-centre distance {num(length)}, along Z, centred.",
             f"# {generated or GENERATED}",
             "vertices:"]
    lines += [f"  - {fmt([round(x, 5) for x in p])}" for p in verts]
    lines.append("normals:")
    lines += [f"  - {fmt([round(x, 5) for x in n])}" for n in normals]
    lines.append("uvs:")
    lines += [f"  - {fmt([round(x, 5) for x in uv])}" for uv in uvs]
    lines.append("faces:")
    lines += [f"  - {f}" for f in faces]
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


# name -> (radius, cap-centre length)
PART_MESHES = {
    "mannequin_pelvis": (0.13, 0.14),
    "mannequin_abdomen": (0.12, 0.08),
    "mannequin_torso": (0.16, 0.14),
    "mannequin_head": (0.11, 0.05),
    "mannequin_neck": (0.05, 0.04),
    "mannequin_upper_arm": (0.055, 0.17),
    "mannequin_lower_arm": (0.047, 0.16),
    "mannequin_hand": (0.045, 0.05),
    "mannequin_thigh": (0.08, 0.27),
    "mannequin_shin": (0.062, 0.29),
    "mannequin_foot": (0.05, 0.13),
}


# ---------------------------------------------------------------------------------------------
# The rig: a flat, parent-first bone list (name, parent or None, head, tail), rig space.
# ---------------------------------------------------------------------------------------------

RIG_BONES = [
    ("pelvis", None, (0, 0, 0.95), (0, 0.1, 0.95)),
    ("spine", "pelvis", (0, 0, 1.05), (0, 0, 1.25)),
    ("chest", "spine", (0, 0, 1.25), (0, 0, 1.49)),
    ("neck", "chest", (0, 0, 1.49), (0, 0, 1.54)),
    ("head", "neck", (0, 0, 1.54), (0, 0, 1.66)),
    ("upper_arm_l", "chest", (-0.21, 0, 1.42), (-0.21, 0, 1.14)),
    ("lower_arm_l", "upper_arm_l", (-0.21, 0, 1.14), (-0.21, 0, 0.89)),
    ("hand_l", "lower_arm_l", (-0.21, 0, 0.89), (-0.21, 0, 0.82)),
    ("upper_arm_r", "chest", (0.21, 0, 1.42), (0.21, 0, 1.14)),
    ("lower_arm_r", "upper_arm_r", (0.21, 0, 1.14), (0.21, 0, 0.89)),
    ("hand_r", "lower_arm_r", (0.21, 0, 0.89), (0.21, 0, 0.82)),
    ("thigh_l", "pelvis", (-0.1, 0, 0.9), (-0.1, 0, 0.47)),
    ("shin_l", "thigh_l", (-0.1, 0, 0.47), (-0.1, 0, 0.06)),
    ("foot_l", "shin_l", (-0.1, 0, 0.06), (-0.1, 0.15, 0.06)),
    ("thigh_r", "pelvis", (0.1, 0, 0.9), (0.1, 0, 0.47)),
    ("shin_r", "thigh_r", (0.1, 0, 0.47), (0.1, 0, 0.06)),
    ("foot_r", "shin_r", (0.1, 0, 0.06), (0.1, 0.15, 0.06)),
]

BONE_HEAD = {name: head for name, _parent, head, _tail in RIG_BONES}
BONE_PARENT = {name: parent for name, parent, _head, _tail in RIG_BONES}


def write_rig(path=RIG_PATH):
    lines = [
        "# mannequin: a 1.8 m humanoid rig (joints are bones, parts are capsule meshes riding",
        "# them via BoneAttachment), facing +Y, origin at the feet. Bone names are a contract",
        "# (clips, IK, ragdolls and FootIK legs address them) -- see tools/gen_mannequin.py.",
        f"# {GENERATED}",
        "rig:",
        "  name: mannequin",
        "  bones:",
    ]
    for name, parent, head, tail in RIG_BONES:
        lines.append(f"    - name: {name}")
        if parent:
            lines.append(f"      parent: {parent}")
        lines.append(f"      {vec('head', head)}")
        lines.append(f"      {vec('tail', tail)}")
    lines.append("")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------------------------
# Bone rest basis (mirrors coopa/rig/rig_asset.h's bone_rest_matrix exactly) and quaternion math
# ---------------------------------------------------------------------------------------------

def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vnorm(v):
    ln = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / ln for c in v)


def vcross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def vdot(a, b):
    return sum(x * y for x, y in zip(a, b))


def mat_to_quat(x, y, z):
    """x, y, z: the basis's three columns (unit, right-handed). Standard matrix->quaternion."""
    m00, m10, m20 = x
    m01, m11, m21 = y
    m02, m12, m22 = z
    trace = m00 + m11 + m22
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        return (((m21 - m12) / s), ((m02 - m20) / s), ((m10 - m01) / s), 0.25 * s)
    if m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2
        return (0.25 * s, (m10 + m01) / s, (m02 + m20) / s, (m21 - m12) / s)
    if m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2
        return ((m10 + m01) / s, 0.25 * s, (m21 + m12) / s, (m02 - m20) / s)
    s = math.sqrt(1.0 + m22 - m00 - m11) * 2
    return ((m02 + m20) / s, (m21 + m12) / s, 0.25 * s, (m10 - m01) / s)


_BASIS_CACHE = {}


def bone_basis(name):
    """The bone's own rig-space rest-basis quaternion (x, y, z, w) -- bone_rest_matrix()."""
    if name in _BASIS_CACHE:
        return _BASIS_CACHE[name]
    head = BONE_HEAD[name]
    tail = next(t for n, _p, _h, t in RIG_BONES if n == name)
    y = vnorm(vsub(tail, head))
    seed = (0.0, 0.0, 1.0) if abs(vdot(y, (0.0, 0.0, 1.0))) < 0.999 else (1.0, 0.0, 0.0)
    x = vnorm(vcross(seed, y))
    z = vcross(x, y)
    q = mat_to_quat(x, y, z)
    _BASIS_CACHE[name] = q
    return q


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def qconj(q):
    return [-q[0], -q[1], -q[2], q[3]]


def qrot(q, v):
    """Rotates vector v by unit quaternion q (x, y, z, w)."""
    return qmul(qmul(q, [v[0], v[1], v[2], 0.0]), qconj(q))[:3]


def conjugate_quat(r_old, basis):
    """The pose-delta for `inverse(basis) * r_old * basis` -- see this file's CONJUGATION doc."""
    return qmul(qmul(qconj(basis), r_old), basis)


def quat_xyz(x_deg, y_deg=0.0, z_deg=0.0):
    """Rotation X first, then Y, then Z (matches the Transform's Euler order)."""
    def axis(i, deg):
        a = math.radians(deg) * 0.5
        q = [0.0, 0.0, 0.0, math.cos(a)]
        q[i] = math.sin(a)
        return q
    return qmul(axis(2, z_deg), qmul(axis(1, y_deg), axis(0, x_deg)))


def vec(label, v):
    return f"{label}: {{x: {num(v[0])}, y: {num(v[1])}, z: {num(v[2])}}}"


# ---------------------------------------------------------------------------------------------
# Shapes: capsule parts riding the rig's bones via BoneAttachment.
# ---------------------------------------------------------------------------------------------

BODY = "{albedo: {r: 0.62, g: 0.66, b: 0.72}, roughness: 0.55, metallic: 0.05}"
LIMB = "{albedo: {r: 0.5, g: 0.54, b: 0.6}, roughness: 0.55, metallic: 0.05}"
DARK = "{albedo: {r: 0.2, g: 0.22, b: 0.26}, roughness: 0.6}"
VISOR = "{albedo: {r: 0.95, g: 0.55, b: 0.15}, roughness: 0.35, emissive: {r: 0.4, g: 0.18, b: 0.04}}"

# (name, bone, mesh, material, position or None, rotation, scale or None)
SHAPES = [
    ("pelvis_shape", "pelvis", "mannequin_pelvis", BODY, None, (0, -90, 0), (1, 0.8, 1)),
    ("abdomen_shape", "spine", "mannequin_abdomen", BODY, (0, 0.06, 0), (-90, 90, 0), (1, 0.8, 1)),
    ("chest_shape", "chest", "mannequin_torso", BODY, (0, 0.08, 0), (-90, 90, 0), (1.25, 0.8, 1)),
    ("neck_shape", "neck", "mannequin_neck", DARK, None, (-90, 90, 0), None),
    ("head_shape", "head", "mannequin_head", BODY, (0, 0.11, 0), (-90, 90, 0), None),
    ("visor", "head", "cube", VISOR, (-0.085, 0.13, 0), (-90, 90, 0), (0.16, 0.06, 0.05)),
    ("upper_arm_l_shape", "upper_arm_l", "mannequin_upper_arm", LIMB, (0, 0.14, 0), (90, 90, 0), None),
    ("lower_arm_l_shape", "lower_arm_l", "mannequin_lower_arm", LIMB, (0, 0.125, 0), (90, 90, 0), None),
    ("hand_l_shape", "hand_l", "mannequin_hand", DARK, (0, 0.07, 0), (90, 90, 0), None),
    ("upper_arm_r_shape", "upper_arm_r", "mannequin_upper_arm", LIMB, (0, 0.14, 0), (90, 90, 0), None),
    ("lower_arm_r_shape", "lower_arm_r", "mannequin_lower_arm", LIMB, (0, 0.125, 0), (90, 90, 0), None),
    ("hand_r_shape", "hand_r", "mannequin_hand", DARK, (0, 0.07, 0), (90, 90, 0), None),
    ("thigh_l_shape", "thigh_l", "mannequin_thigh", LIMB, (0, 0.215, 0), (90, 90, 0), None),
    ("shin_l_shape", "shin_l", "mannequin_shin", LIMB, (0, 0.205, 0), (90, 90, 0), None),
    ("foot_l_shape", "foot_l", "mannequin_foot", DARK, (0, 0.06, 0.005), (-90, 0, 180), None),
    ("thigh_r_shape", "thigh_r", "mannequin_thigh", LIMB, (0, 0.215, 0), (90, 90, 0), None),
    ("shin_r_shape", "shin_r", "mannequin_shin", LIMB, (0, 0.205, 0), (90, 90, 0), None),
    ("foot_r_shape", "foot_r", "mannequin_foot", DARK, (0, 0.06, 0.005), (-90, 0, 180), None),
]


def emit_shape(name, bone, mesh, material, pos, rot, scale, indent=4):
    p = " " * indent
    out = [f"{p}- name: {name}", f"{p}  components:", f"{p}    - type: Transform"]
    if pos is not None:
        out.append(f"{p}      {vec('position', pos)}")
    out.append(f"{p}      {vec('rotation', rot)}")
    if scale is not None:
        out.append(f"{p}      {vec('scale', scale)}")
    out += [f"{p}    - type: BoneAttachment", f"{p}      bone: {bone}",
            f"{p}    - type: MeshRenderer", f"{p}      mesh_path: {mesh}", f"{p}      material: {material}",
            f"{p}  children: []"]
    return out


# ---------------------------------------------------------------------------------------------
# Ragdoll bones: (bone, shape, shape params, mass kg, joint). Shapes sit along each bone toward
# its child (limbs hang along -Z); joints connect to the nearest ancestor bone in this list and
# are measured from the bind pose (identity rest rotations in the OLD local-joint sense -- see
# CONJUGATION). Positive X swings a limb forward, so a knee bends in [-140, 0] and an elbow in
# [0, 140]. Hands and the neck stay out (they ride their parent bone): fewer, heavier bodies keep
# the chain stiff and calm. direction/center/axis are in the BONE'S OWN rest basis (bone-local
# +Y is the bone's own length axis), not the old world-aligned convention.
def _vec(v):
    return "{x: %s, y: %s, z: %s}" % tuple(num(c) for c in v)


def _cone(axis, swing, twist, anchor=None):
    j = [("joint", "cone_twist")]
    if anchor:
        j.append(("anchor", _vec(anchor)))
    return j + [("axis", _vec(axis)), ("swing", num(swing)), ("twist_min", num(-twist)), ("twist_max", num(twist))]


def _hinge(axis, lo, hi):
    return [("joint", "hinge"), ("axis", _vec(axis)), ("limit_min", num(lo)), ("limit_max", num(hi))]


RAGDOLL = [
    ("pelvis", "capsule", {"radius": 0.12, "height": 0.32, "direction": "x"}, 12.0, None),
    ("spine", "capsule", {"radius": 0.11, "height": 0.36, "direction": "z", "center": (0, 0.08, 0)}, 8.0,
     _cone((0, 1, 0), 25, 20)),
    ("chest", "capsule", {"radius": 0.13, "height": 0.44, "direction": "z", "center": (0, 0.1, 0)}, 12.0,
     _cone((0, 1, 0), 20, 20)),
    ("head", "sphere", {"radius": 0.12, "center": (0, 0.11, 0)}, 5.0,
     _cone((0, 1, 0), 35, 45, anchor=(0, -0.05, 0))),
]
for _s in "lr":
    RAGDOLL += [
        (f"upper_arm_{_s}", "capsule", {"radius": 0.055, "height": 0.28, "direction": "y", "center": (0, 0.14, 0)}, 2.5,
         _cone((0, 1, 0), 80, 45)),
        (f"lower_arm_{_s}", "capsule", {"radius": 0.04, "height": 0.36, "direction": "y", "center": (0, 0.17, 0)}, 2.0,
         _hinge((0, 0, -1), 0, 140)),
        (f"thigh_{_s}", "capsule", {"radius": 0.08, "height": 0.43, "direction": "y", "center": (0, 0.215, 0)}, 8.0,
         _cone((0, 1, 0), 60, 30)),
        (f"shin_{_s}", "capsule", {"radius": 0.062, "height": 0.41, "direction": "y", "center": (0, 0.205, 0)}, 4.0,
         _hinge((0, 0, -1), -140, 0)),
        (f"foot_{_s}", "box", {"size": (0.1, 0.24, 0.08), "center": (0, 0.06, 0.015)}, 1.0,
         _hinge((-1, 0, 0), -30, 40)),
    ]


def ragdoll_component():
    out = ["    - type: Ragdoll",
           "      mode: animated",
           "      blend_time: 0.6",
           "      recover_state: idle",
           "      drag: 0.05",
           "      angular_drag: 0.8",
           "      bones:"]
    for bone, shape, params, mass, joint in RAGDOLL:
        out.append(f"        - bone: {bone}")
        out.append(f"          shape: {shape}")
        for key in ("radius", "height", "direction"):
            if key in params:
                v = params[key]
                out.append(f"          {key}: {v if isinstance(v, str) else num(v)}")
        if "size" in params:
            out.append(f"          size: {_vec(params['size'])}")
        if "center" in params:
            out.append(f"          center: {_vec(params['center'])}")
        out.append(f"          mass: {num(mass)}")
        for key, value in joint or ():
            out.append(f"          {key}: {value}")
    return out


def write_object(path=OBJECT_PATH, extra_components=(), header_note=None, clip_dir="animations/mannequin",
                 extra_children=(), extra_animator_clip_dir=None):
    header = [
        "# mannequin: a 1.8 m humanoid rig (rigs/mannequin.yaml); capsule parts ride its bones via",
        "# BoneAttachment. Clips idle / walk / run / jump / ride. Bone names are a contract (clips,",
        "# FootIK and ragdolls address them) -- see tools/gen_mannequin.py.",
    ]
    if header_note == IK_HEADER_NOTE:
        header = [
            "# mannequin_ik: the same 1.8 m rig as mannequin.yaml (rigs/mannequin.yaml), but posed by",
            "# bone-space IK instead of keyframes: a SkeletalAnimator plays the ordinary mannequin clips",
            "# ONLY for the pelvis bone's height bob and root motion (IK overwrites every bone it targets,",
            "# at a later update order, so the animator's contribution to those bones never survives to",
            "# the final pose); a plain Animator drives the floating target/pole empties that steer the IK.",
        ]
    lines = list(header)
    if header_note and header_note != IK_HEADER_NOTE:
        lines.append(f"# {header_note}")
    lines += [
        f"# {GENERATED}",
        "object:",
        f"  name: {os.path.splitext(os.path.basename(path))[0]}",
        "  components:",
        "    - type: Transform",
        "      position: {x: 0, y: 0, z: 0}",
        "    - type: Rig",
        "      rig: rigs/mannequin.yaml",
        "    - type: SkeletalAnimator",
        "      auto_play: idle",
        "      default_crossfade: 0.2",
        "      states:",
        f"        - {{name: idle, clip: {clip_dir}/idle.yaml}}",
        f"        - {{name: walk, clip: {clip_dir}/walk.yaml}}",
        f"        - {{name: run, clip: {clip_dir}/run.yaml}}",
        f"        - {{name: jump, clip: {clip_dir}/jump.yaml, wrap: once}}",
        f"        - {{name: ride, clip: {clip_dir}/ride.yaml}}",
    ]
    if extra_animator_clip_dir:
        lines += [
            "    - type: Animator",
            "      auto_play: idle",
            "      default_crossfade: 0.2",
            "      states:",
            f"        - {{name: idle, clip: {extra_animator_clip_dir}/idle.yaml}}",
            f"        - {{name: walk, clip: {extra_animator_clip_dir}/walk.yaml}}",
            f"        - {{name: run, clip: {extra_animator_clip_dir}/run.yaml}}",
            f"        - {{name: jump, clip: {extra_animator_clip_dir}/jump.yaml, wrap: once}}",
            f"        - {{name: ride, clip: {extra_animator_clip_dir}/ride.yaml}}",
        ]
    lines += list(extra_components)
    lines.append("  children:")
    for name, bone, mesh, material, pos, rot, scale in SHAPES:
        lines += emit_shape(name, bone, mesh, material, pos, rot, scale)
    lines += list(extra_children)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------------------------
# Clips: generated in the OLD convention (each bone's local-to-parent rotation / the pelvis's
# absolute position, as if every joint had an identity rest rotation) because that is also what
# bake_ik_clip()'s forward kinematics needs; write_clip() converts to the Rig's pose-delta
# convention at serialization time -- see this file's CONJUGATION doc.
# ---------------------------------------------------------------------------------------------


def rot_track(bone_name, samples, easing="linear"):
    """samples: [(time, (x_deg, y_deg, z_deg))]; keeps keys in one quaternion hemisphere."""
    keys, prev = [], None
    for t, angles in samples:
        q = quat_xyz(*angles)
        if prev is not None and sum(a * b for a, b in zip(q, prev)) < 0:
            q = [-c for c in q]
        prev = q
        keys.append((t, [round(c, 6) for c in q]))
    return (bone_name, "rotation_quat", keys, easing)


def pos_track(bone_name, channel, samples, easing="linear"):
    return (bone_name, f"position.{channel}", [(t, v) for t, v in samples], easing)


FPS = 30  # the editor Timeline's frame grid (editor/app/ui/timeline.inl, kAnimFps): keys land on it


def on_frame(t):
    """`t` snapped to the nearest frame of the Timeline's grid, so every key sits on a tick."""
    return round(t * FPS) / FPS


CLIPS = {}  # name -> the keyframed clip's OLD-convention data, for bake_ik_clip()


def write_property_clip(name, length, tracks, wrap="loop", events=(), root_motion=None, anim_dir=IK_ANIM_DIR,
                        label="mannequin_ik"):
    """Writes an OLD-style property clip (`tracks: - object: <path> property: ...`) -- used only
    for mannequin_ik's target/pole empties, which are plain SceneObjects, not rig bones.
    events: [(time, name, string)]; root_motion: (object, translation, rotation) or None."""
    assert abs(length * FPS - round(length * FPS)) < 1e-6, f"{name}: length {length} is not a whole frame"
    lines = [f"# {label} {name}. {GENERATED}", "clip:", f"  name: {name}", f"  wrap: {wrap}",
             f"  length: {num(length)}"]
    if root_motion:
        obj, translation, rotation = root_motion
        lines.append(f"  root_motion: {{object: {obj}, translation: {translation}, rotation: {rotation}}}")
    if events:
        lines.append("  events:")
        for t, ev_name, ev_str in events:
            lines.append(f"    - {{time: {num(round(on_frame(t), 4))}, name: {ev_name}, string: {ev_str}}}")
    lines.append("  tracks:")
    for path, prop, keys, easing in tracks:
        lines.append(f"    - object: {path}")
        lines.append(f"      property: {prop}")
        lines.append("      keys:")
        frames = [round(t * FPS) for t, _ in keys]
        assert len(set(frames)) == len(frames), f"{name}: two {path} keys snap to one frame"
        for t, v in keys:
            ease = "" if easing == "linear" else f", easing: {easing}"
            val = fmt(v) if isinstance(v, (list, tuple)) else num(v)
            lines.append(f"        - {{time: {num(round(on_frame(t), 4))}, value: {val}{ease}}}")
    os.makedirs(anim_dir, exist_ok=True)
    with open(os.path.join(anim_dir, f"{name}.yaml"), "w") as fh:
        fh.write("\n".join(lines) + "\n")


def write_clip(name, length, tracks, wrap="loop", events=(), root_motion=None, anim_dir=ANIM_DIR, label="mannequin",
              skip_bones=frozenset()):
    """Writes a SKELETAL clip (`bones: - bone: <name> location:/rotation:`) from OLD-convention
    tracks (see this file's CONJUGATION doc): rotation keys are conjugated through the bone's own
    rest basis; the pelvis's position.x/y/z keys become one conjugated `location` curve.
    events: [(time, name, string)]; root_motion: (bone, translation, rotation) or None.
    Key and event times are snapped to whole frames (on_frame); `length` must already be one.
    `skip_bones`: bones present in `tracks` (kept in CLIPS[name] for bake_ik_clip's forward
    kinematics) but NOT written into this clip's own `bones:` list.

    Known residual: a handful of rotation keys involving a genuinely multi-axis old-convention
    angle (ride's thigh bones; one walk/run/jump angle that lands exactly on a 6th-decimal
    rounding boundary) differ from the shipped asset by 1 in the 6th decimal place of a
    quaternion W component (e.g. 0.794819 vs this code's 0.79482) -- verified mathematically
    that W is EXACTLY invariant under this conjugation regardless of basis, so this is not a
    formula error; reproducing the shipped value would require knowing the exact (and
    functionally irrelevant, ~1e-6) floating-point path the original conversion used."""
    CLIPS[name] = {"length": length, "tracks": tracks, "wrap": wrap, "events": events, "root_motion": root_motion}
    assert abs(length * FPS - round(length * FPS)) < 1e-6, f"{name}: length {length} is not a whole frame"
    lines = [f"# {label} {name}. {GENERATED}", "clip:", f"  name: {name}", f"  wrap: {wrap}",
             f"  length: {num(length)}"]
    if root_motion:
        bone, translation, rotation = root_motion
        lines.append(f"  root_motion: {{bone: {bone}, translation: {translation}, rotation: {rotation}}}")
    if events:
        lines.append("  events:")
        for t, ev_name, ev_str in events:
            lines.append(f"    - {{time: {num(round(on_frame(t), 4))}, name: {ev_name}, string: {ev_str}}}")

    # Group by bone, preserving first-seen order; merge position.x/.y/.z into one location curve.
    order = []
    rot_by_bone, pos_by_bone = {}, {}
    for bone, prop, keys, easing in tracks:
        if bone not in order:
            order.append(bone)
        if prop == "rotation_quat":
            rot_by_bone[bone] = (keys, easing)
        elif prop.startswith("position."):
            pos_by_bone.setdefault(bone, {})[prop.split(".")[1]] = (keys, easing)

    lines.append("  bones:")
    for bone in order:
        if bone in skip_bones:
            continue
        lines.append(f"    - bone: {bone}")
        if bone in pos_by_bone:
            basis = bone_basis(bone)
            head = BONE_HEAD[bone]
            chans = pos_by_bone[bone]
            # The output time grid is whichever channel has the MOST keys (location's own
            # cadence); a sparser channel (e.g. a 2-key linear root-motion ramp) is sampled
            # (linearly interpolated) at those times rather than requiring a key at each one.
            driver_ch = max(chans, key=lambda c: len(chans[c][0]))
            times = [t for t, _ in chans[driver_ch][0]]
            easing = chans[driver_ch][1]

            def old_at(ch, t):
                if ch not in chans:
                    return 0.0
                keys = chans[ch][0]
                if len(keys) == 1 or t <= keys[0][0]:
                    return keys[0][1]
                if t >= keys[-1][0]:
                    return keys[-1][1]
                for i in range(len(keys) - 1):
                    if keys[i][0] <= t <= keys[i + 1][0]:
                        span = keys[i + 1][0] - keys[i][0]
                        u = (t - keys[i][0]) / span if span > 0 else 0.0
                        return keys[i][1] + (keys[i + 1][1] - keys[i][1]) * u
                raise AssertionError(f"{name}: bone {bone} channel {ch} has no key covering t={t}")

            lines.append("      location:")
            for t in times:
                t4 = round(on_frame(t), 4)
                # The driver channel is sampled at its own exact (raw) key time; any OTHER
                # channel (e.g. a 2-key linear root-motion ramp) is sampled at the DISPLAY-
                # ROUNDED time instead -- a quirk this reproduces because the shipped asset's
                # own travel values match it exactly.
                old_pos = tuple(old_at(ch, t if ch == driver_ch else t4) for ch in "xyz")
                delta = tuple(old_pos[i] - head[i] for i in range(3))
                loc = clean0(qrot(qconj(basis), delta))
                ease = "" if easing == "linear" else f", easing: {easing}"
                lines.append(f"        - {{time: {num(t4)}, value: {fmt6(loc)}{ease}}}")
        if bone in rot_by_bone:
            basis = bone_basis(bone)
            keys, easing = rot_by_bone[bone]
            lines.append("      rotation:")
            frames = [round(t * FPS) for t, _ in keys]
            assert len(set(frames)) == len(frames), f"{name}: two {bone} rotation keys snap to one frame"
            for t, q_old in keys:
                q_pose = clean0([round(c, 6) for c in conjugate_quat(q_old, basis)])
                ease = "" if easing == "linear" else f", easing: {easing}"
                lines.append(f"        - {{time: {num(round(on_frame(t), 4))}, value: {fmt(q_pose)}{ease}}}")
    os.makedirs(anim_dir, exist_ok=True)
    with open(os.path.join(anim_dir, f"{name}.yaml"), "w") as fh:
        fh.write("\n".join(lines) + "\n")


def cycle(length, n, fn):
    """n + 1 samples of fn(phase in [0, 1]) over one loop, the last equal to the first."""
    return [(length * i / n, fn((i % n) / n)) for i in range(n + 1)]


def gait(name, length, stride, knee, arm_swing, elbow, lean, bob, pelvis_z, travel, n=12):
    """A walk/run cycle: legs in antiphase, arms against the legs, knees bent during swing.

    The pelvis travels `travel` metres along +Y per cycle -- the clip's ROOT MOTION (pelvis, xy):
    the SkeletalAnimator holds the pelvis in place and hands the travel to its owner (a
    CharacterController with use_root_motion moves by it; anything else just plays the gait in
    place). A footstep event fires as each foot plants: the left thigh is furthest forward at
    phase 0.25, the right at 0.75.
    """
    tau = 2 * math.pi

    def thigh(phase, side):
        return stride * math.sin(tau * phase + (0 if side == "l" else math.pi))

    def shin(phase, side):
        p = tau * phase + (0 if side == "l" else math.pi)
        return -(5 + knee * max(0.0, math.cos(p)) ** 1.5)

    times = [length * i / n for i in range(n + 1)]
    z_vals = [pelvis_z - bob * math.cos(2 * tau * (i / n)) for i in range(n + 1)]
    tracks = [pos_track("pelvis", "z", list(zip(times, z_vals))),
              pos_track("pelvis", "y", [(0.0, 0.0), (length, travel)])]
    tracks.append(rot_track("spine", cycle(length, n, lambda p: (-lean, 0, 0))))
    tracks.append(rot_track("chest", cycle(length, n, lambda p: (0, 0, 4 * math.sin(tau * p)))))
    tracks.append(rot_track("head", cycle(length, n, lambda p: (lean * 0.6, 0, -4 * math.sin(tau * p)))))
    for side in "lr":
        abduct = 7 if side == "l" else -7
        tracks.append(rot_track(f"thigh_{side}", cycle(length, n, lambda p, s=side: (thigh(p, s), 0, 0))))
        tracks.append(rot_track(f"shin_{side}", cycle(length, n, lambda p, s=side: (shin(p, s), 0, 0))))
        tracks.append(rot_track(f"foot_{side}", cycle(length, n,
                                                      lambda p, s=side: (-0.5 * (thigh(p, s) + shin(p, s)), 0, 0))))
        tracks.append(rot_track(f"upper_arm_{side}", cycle(length, n,
                                                           lambda p, s=side: (-arm_swing / stride * thigh(p, s), abduct, 0))))
        tracks.append(rot_track(f"lower_arm_{side}", cycle(length, n,
                                                           lambda p, s=side: (elbow + 0.3 * arm_swing / stride * max(0.0, -thigh(p, s)), 0, 0))))
    events = [(0.25 * length, "footstep", "left"), (0.75 * length, "footstep", "right")]
    write_clip(name, length, tracks, events=events, root_motion=("pelvis", "xy", "none"))


def idle():
    length, n, tau = 2.4, 8, 2 * math.pi
    breathe = lambda p: math.sin(tau * p)
    pelvis_head_z = BONE_HEAD["pelvis"][2]
    tracks = [
        pos_track("pelvis", "z", cycle(length, n, lambda p: pelvis_head_z - 0.006 * (1 - breathe(p)))),
        rot_track("chest", cycle(length, n, lambda p: (-1.5 * breathe(p), 0, 0)), "ease_in_out"),
        rot_track("head", cycle(length, n, lambda p: (1.0 * breathe(p), 0, 3 * math.sin(tau * p * 0.5 * 2))), "ease_in_out"),
    ]
    for side in "lr":
        sgn = 1 if side == "l" else -1
        tracks.append(rot_track(f"thigh_{side}", cycle(length, n, lambda p: (2, 0, 0))))
        tracks.append(rot_track(f"shin_{side}", cycle(length, n, lambda p: (-4, 0, 0))))
        tracks.append(rot_track(f"foot_{side}", cycle(length, n, lambda p: (2, 0, 0))))
        tracks.append(rot_track(f"upper_arm_{side}", cycle(length, n, lambda p, g=sgn: (2 * breathe(p), g * (7 + 1.5 * breathe(p)), 0)), "ease_in_out"))
        tracks.append(rot_track(f"lower_arm_{side}", cycle(length, n, lambda p: (12 + 2 * breathe(p), 0, 0)), "ease_in_out"))
    # The skeletal clip omits upper_arm_r/lower_arm_r (unlike every other clip here, which is
    # fully symmetric) -- a quirk of the shipped asset this generator reproduces rather than
    # "fixes"; bake_ik_clip() still needs the full symmetric data above for the mannequin_ik's
    # hand_target_r to swing with the breathing motion, so only the YAML OUTPUT skips them.
    write_clip("idle", length, tracks, skip_bones={"upper_arm_r", "lower_arm_r"})


def jump():
    # Once: a quick tuck into the air pose, held. Played while airborne.
    pelvis_head_z = BONE_HEAD["pelvis"][2]
    key = lambda pose: [(0.0, pose[0]), (0.18, pose[1]), (0.5, pose[1])]
    tracks = [
        pos_track("pelvis", "z", [(0.0, pelvis_head_z), (0.18, pelvis_head_z), (0.5, pelvis_head_z)], "ease_out"),
        rot_track("spine", key([(0, 0, 0), (-8, 0, 0)]), "ease_out"),
        rot_track("head", key([(0, 0, 0), (6, 0, 0)]), "ease_out"),
    ]
    for side in "lr":
        sgn = 1 if side == "l" else -1
        lead = side == "l"
        tracks.append(rot_track(f"thigh_{side}", key([(2, 0, 0), (55 if lead else 15, 0, 0)]), "ease_out"))
        tracks.append(rot_track(f"shin_{side}", key([(-4, 0, 0), (-75 if lead else -40, 0, 0)]), "ease_out"))
        tracks.append(rot_track(f"foot_{side}", key([(2, 0, 0), (15, 0, 0)]), "ease_out"))
        tracks.append(rot_track(f"upper_arm_{side}", key([(0, sgn * 7, 0), (35 if lead else -25, sgn * 35, 0)]), "ease_out"))
        tracks.append(rot_track(f"lower_arm_{side}", key([(12, 0, 0), (45, 0, 0)]), "ease_out"))
    write_clip("jump", 0.5, tracks, wrap="once")


def ride():
    """Seated in a saddle (loop): thighs forward and spread round a barrel, shins hanging back,
    hands held low in front, a slow breath. A Rider (toyengine/scene/rider.h) puts the root on
    the saddle and IK takes the feet to the stirrups and the hands to the reins; this is the pose
    those blend from, and all a rig without IK gets."""
    length, n, tau = 2.4, 8, 2 * math.pi
    breathe = lambda p: math.sin(tau * p)
    pelvis_head_z = BONE_HEAD["pelvis"][2]
    tracks = [
        pos_track("pelvis", "z", cycle(length, n, lambda p: pelvis_head_z - 0.004 * (1 - breathe(p)))),
        rot_track("spine", cycle(length, n, lambda p: (-4, 0, 0))),
        rot_track("chest", cycle(length, n, lambda p: (-1.5 * breathe(p), 0, 0)), "ease_in_out"),
        rot_track("head", cycle(length, n, lambda p: (4 + 1.0 * breathe(p), 0, 0)), "ease_in_out"),
    ]
    for side in "lr":
        sgn = 1 if side == "l" else -1
        tracks.append(rot_track(f"thigh_{side}", cycle(length, n, lambda p, g=sgn: (70, g * 28, 0))))
        tracks.append(rot_track(f"shin_{side}", cycle(length, n, lambda p: (-80, 0, 0))))
        tracks.append(rot_track(f"foot_{side}", cycle(length, n, lambda p: (12, 0, 0))))
        tracks.append(rot_track(f"upper_arm_{side}", cycle(length, n, lambda p, g=sgn: (25 + breathe(p), g * 10, 0)), "ease_in_out"))
        tracks.append(rot_track(f"lower_arm_{side}", cycle(length, n, lambda p: (55, 0, 0))))
    write_clip("ride", length, tracks)


# ---------------------------------------------------------------------------------------------
# The fully IK mannequin: forward kinematics of the keyframed clips (OLD identity-rest joint
# convention -- this is independent of the Rig system; it only drives floating target empties),
# baked into target/pole property tracks.
# ---------------------------------------------------------------------------------------------

def _joints():
    """bone name -> (rest position RELATIVE TO ITS PARENT'S HEAD, parent name or None) -- the
    OLD identity-rest-joint tree's convention `fk_pose` assumes (a joint's `pos` was its pivot
    offset from its parent's pivot, accumulated recursively), derived from RIG_BONES' absolute
    rig-space heads."""
    out = {}
    for name, parent, head, _tail in RIG_BONES:
        rel = head if parent is None else tuple(head[i] - BONE_HEAD[parent][i] for i in range(3))
        out[name] = (rel, parent)
    return out


JOINTS = _joints()


def ease(mode, t):
    """coopa::anim::apply_easing."""
    if mode == "step":
        return 0.0
    if mode == "ease_in":
        return t * t
    if mode == "ease_out":
        return t * (2.0 - t)
    if mode == "ease_in_out":
        return 2.0 * t * t if t < 0.5 else 1.0 - 2.0 * (1.0 - t) * (1.0 - t)
    return t


def sample_track(keys, easing, t):
    """The track's value at t as the Animator samples it: clamped ends, the left key's easing,
    lists blended per component (a quaternion is then normalised: nlerp)."""
    times = [on_frame(k) for k, _ in keys]
    vals = [v for _, v in keys]
    if t <= times[0]:
        return vals[0]
    if t >= times[-1]:
        return vals[-1]
    i = max(j for j in range(len(times)) if times[j] <= t)
    span = times[i + 1] - times[i]
    u = ease(easing, (t - times[i]) / span if span > 0 else 0.0)
    a, b = vals[i], vals[i + 1]
    if isinstance(a, list):
        return [x + (y - x) * u for x, y in zip(a, b)]
    return a + (b - a) * u


def fk_pose(clip, t):
    """World (= mannequin-root) position and rotation of every joint at time t, under the OLD
    identity-rest-joint convention. Root motion's translation is held at the clip's start, as the
    old Animator held it."""
    local_pos = {n: list(p) for n, (p, _) in JOINTS.items()}
    local_rot = {n: [0.0, 0.0, 0.0, 1.0] for n in JOINTS}
    held = set(clip["root_motion"][1]) if clip["root_motion"] else set()
    for bone, prop, keys, easing in clip["tracks"]:
        if prop == "rotation_quat":
            q = sample_track(keys, easing, t)
            ln = math.sqrt(sum(c * c for c in q)) or 1.0
            local_rot[bone] = [c / ln for c in q]
        elif prop.startswith("position."):
            ch = prop.split(".")[1]
            local_pos[bone]["xyz".index(ch)] = keys[0][1] if ch in held else sample_track(keys, easing, t)
    world = {}

    def walk(name):
        if name in world:
            return world[name]
        parent = JOINTS[name][1]
        if parent is None:
            world[name] = (local_pos[name], local_rot[name])
        else:
            pp, pr = walk(parent)
            off = qrot(pr, local_pos[name])
            world[name] = ([pp[i] + off[i] for i in range(3)], qmul(pr, local_rot[name]))
        return world[name]

    for n in JOINTS:
        walk(n)
    return world


def _pole(top, mid, end, fallback, dist=0.5):
    """A point the middle joint (knee / elbow) bends toward: off the top-end line, through mid."""
    axis = [end[i] - top[i] for i in range(3)]
    al = math.sqrt(sum(c * c for c in axis)) or 1.0
    axis = [c / al for c in axis]
    rel = [mid[i] - top[i] for i in range(3)]
    d = sum(rel[i] * axis[i] for i in range(3))
    perp = [rel[i] - axis[i] * d for i in range(3)]
    pl = math.sqrt(sum(c * c for c in perp))
    dirv = [c / pl for c in perp] if pl > 0.01 else list(fallback)
    return [mid[i] + dirv[i] * dist for i in range(3)]


LOOK_DISTANCE = 2.0


def ik_targets(world):
    """object name -> (position, rotation or None) for every IK target of the mannequin_ik.

    `world[bone][1]` is `fk_pose`'s OLD-convention world rotation (this file's CONJUGATION doc)
    -- a plain joint-chain composition with every bone's rest treated as identity, independent
    of the Rig system's actual (non-identity, per-bone) rest basis. A match_target_rotation IK
    component reads a target's rotation as that target's TRUE rig-space world rotation, so it
    must be converted: `model_i == old_world_i * rest_model_i` (CONJUGATION doc) says the bone's
    TRUE world rotation is `qmul(world[bone][1], bone_basis(bone))`. Expressed as the target's
    stored LOCAL rotation, that's additionally conjugated by whatever the target's actual scene
    PARENT contributes: identity for foot_target/knee_pole (plain children of the rig root), or
    `bone_basis("pelvis")` for the upper-body targets (children of `pelvis_socket`, itself a
    BoneAttachment to `pelvis` -- see bone_attachment.h's `current_world_matrix()`, which copies
    the bone's world rotation with no further twist). Left as the raw `world[bone][1]` alone (as
    this function used to), a match_target_rotation target hands its IK component the identity
    rest-basis assumption instead of the bone's real one -- invisible for a leaf end-effector
    (hand/foot) with no checked descendant, but a large, chain-amplified error for ChainIK's
    chest tip, whose neck/head children inherit and compound it (ik_character_test.cpp's head
    check; verified numerically against the running engine's own measured rotations).
    """
    pelvis_basis = bone_basis("pelvis")

    def true_world_rot(bone_name):
        return qmul(world[bone_name][1], bone_basis(bone_name))

    out = {}
    for s in "lr":
        out[f"foot_target_{s}"] = (world[f"foot_{s}"][0], true_world_rot(f"foot_{s}"))
        out[f"knee_pole_{s}"] = (_pole(world[f"thigh_{s}"][0], world[f"shin_{s}"][0], world[f"foot_{s}"][0], (0, 1, 0)), None)
        out[f"hand_target_{s}"] = (world[f"hand_{s}"][0], qmul(qconj(pelvis_basis), true_world_rot(f"hand_{s}")))
        out[f"elbow_pole_{s}"] = (_pole(world[f"upper_arm_{s}"][0], world[f"lower_arm_{s}"][0], world[f"hand_{s}"][0],
                                        (0, -1, 0)), None)
    out["spine_target"] = (world["chest"][0], qmul(qconj(pelvis_basis), true_world_rot("chest")))
    hp, hr = world["head"]
    fwd = qrot(hr, (0, 1, 0))
    out["look_target"] = ([hp[i] + fwd[i] * LOOK_DISTANCE for i in range(3)], None)
    return out


IK_TARGET_NAMES = [f"{k}_{s}" for k in ("foot_target", "knee_pole", "hand_target", "elbow_pole") for s in "lr"] + \
                  ["spine_target", "look_target"]
# Where each target hangs: the feet and knees from the mannequin root -- ground-relative, so a
# planted foot stays put however the body moves -- and the upper body's from the pelvis (via
# `pelvis_socket`, a BoneAttachment child), as the bones they stand for do: a pelvis lowered by
# FootIK carries the hands, chest and gaze with it.
UPPER_BODY_TARGETS = {f"{k}_{s}" for k in ("hand_target", "elbow_pole") for s in "lr"} | {"spine_target", "look_target"}


def target_path(obj):
    # "pelvis_socket/<target>": the scene's actual child object's name. The clip's object_path
    # is walked via SceneObject::find_descendant() per segment (Animator::resolve_object_), an
    # EXACT name match -- "pelvis/<target>" (the PATHS["pelvis"] naming from before the rig
    # split, when a joint named "pelvis" really was a SceneObject) no longer resolves to
    # anything post rig-split, silently leaving every upper-body target un-animated at its rest
    # transform all clip long. Caught by ik_character_test.cpp's head-position check once the
    # first full ctest sweep ran after the rig-split plan's Stage 1-6 (previously only narrow
    # per-stage suites ran, which never exercised this path).
    return f"pelvis_socket/{obj}" if obj in UPPER_BODY_TARGETS else obj


def bake_ik_clip(name):
    """Writes animations/mannequin_ik/<name>.yaml: the keyframed clip's pelvis tracks (and its
    root motion and events) as they are, and every IK target keyed on every frame from the clip's
    forward kinematics -- so the IK rig reproduces the keyframed motion. Property-clip format
    (the targets are plain SceneObjects, not rig bones)."""
    clip = CLIPS[name]
    frames = round(clip["length"] * FPS)
    poses = [fk_pose(clip, i / FPS) for i in range(frames + 1)]
    samples = [ik_targets(w) for w in poses]
    # Position, like rotation (see ik_targets()'s doc), is stored as the target's LOCAL offset
    # from its actual scene parent: identity for a plain root child (foot_target/knee_pole), or
    # conjugated through inverse(pelvis_basis) for an upper-body target under `pelvis_socket` --
    # the SAME formula ik_extra_children()'s REST positions already use and have verified
    # against the shipped asset (fk_pose's positions are rig-space, relative to the owner, same
    # as pelvis_p here, so the owner's own scene-placement rotation plays no part). Previously
    # written as the raw, unconjugated world-relative-to-pelvis offset: harmless while
    # target_path()'s "pelvis/<target>" alias (now fixed, see its own doc) silently kept this
    # whole animated track from ever reaching the target object at all, but wrong -- and
    # load-bearing -- the moment that path actually resolves.
    pelvis_basis = bone_basis("pelvis")
    # IK TARGETS ONLY. The source clip's own `pelvis` tracks are deliberately NOT carried over:
    # they address a bone by OBJECT path, and since the rig split a bone is not a SceneObject,
    # so nothing resolves them. The pelvis is posed by the SkeletalAnimator playing the shared
    # keyframed clip (animations/mannequin/<name>.yaml, which keys the pelvis BONE) -- copying
    # them here only produced dead tracks that read as if the Animator drove the body.
    tracks = []
    for obj in IK_TARGET_NAMES:
        def local(i):
            p = samples[i][obj][0]
            if obj not in UPPER_BODY_TARGETS:
                return p
            pelvis_p, pelvis_r = poses[i]["pelvis"]
            assert abs(pelvis_r[3]) > 0.999999, "the pelvis never rotates in these clips"
            world_offset = [p[k] - pelvis_p[k] for k in range(3)]
            return qrot(qconj(pelvis_basis), world_offset)
        pos_keys = [(i / FPS, [round(c, 5) for c in local(i)]) for i in range(frames + 1)]
        tracks.append((target_path(obj), "position", pos_keys, "linear"))
        if samples[0][obj][1] is not None:
            rot_keys, prev = [], None
            for i in range(frames + 1):
                q = samples[i][obj][1]
                if prev is not None and sum(a * b for a, b in zip(q, prev)) < 0:
                    q = [-c for c in q]
                prev = q
                rot_keys.append((i / FPS, [round(c, 6) for c in q]))
            tracks.append((target_path(obj), "rotation_quat", rot_keys, "linear"))
    # No root_motion either, for the same reason: `root_motion: {object: pelvis}` never resolves
    # post-split, so an Animator's apply_root_motion reading this clip is inert. Root motion for
    # a rig comes from the SkeletalAnimator's own clip (`root_motion: {bone: pelvis}`) -- see
    # assets_test.cpp's root_motion_consumers_have_a_skeletal_producer.
    write_property_clip(name, clip["length"], tracks, wrap=clip["wrap"], events=clip["events"],
                        anim_dir=IK_ANIM_DIR, label="mannequin_ik")


def ik_components():
    """The mannequin_ik's solvers: a spine chain, two-bone arms and legs, a look-at head."""
    out = ["    # Spine first (a chain: spine -> chest; the chest takes its target's rotation, twist and",
           "    # all, and the neck rides on it), then the limbs hanging from it, then the head --",
           "    # IkSystem's default order for equal priorities.",
           "    - type: ChainIK",
           "      root: spine",
           "      tip: chest",
           "      target: pelvis_socket/spine_target",
           "      match_target_rotation: true"]
    for s in "lr":
        out += ["    - type: TwoBoneIK",
                f"      upper: thigh_{s}",
                f"      lower: shin_{s}",
                f"      end: foot_{s}",
                f"      target: foot_target_{s}",
                f"      pole: knee_pole_{s}",
                "      soft_limit: 0.0",
                "      match_target_rotation: true",
                "    - type: TwoBoneIK",
                f"      upper: upper_arm_{s}",
                f"      lower: lower_arm_{s}",
                f"      end: hand_{s}",
                f"      target: pelvis_socket/hand_target_{s}",
                f"      pole: pelvis_socket/elbow_pole_{s}",
                "      soft_limit: 0.0",
                "      match_target_rotation: true"]
    out += ["    - type: LookAtIK",
            "      bone: head",
            "      target: pelvis_socket/look_target",
            "      max_angle: 89"]
    return out


def ik_extra_children():
    """pelvis_socket (a BoneAttachment to `pelvis`) holding the upper-body targets, and the
    feet's/knees' target empties as plain root children, all at their rest places.

    pelvis_socket's children are LOCAL to the pelvis BONE's own rest basis (BoneAttachment
    places them in the bone's anchor frame, which is rotated relative to rig space -- see this
    file's CONJUGATION doc), so a world-space offset from the pelvis is converted the same way a
    skeletal clip's location channel is: `inverse(pelvis_basis) * world_offset`."""
    rest = ik_targets(fk_pose({"tracks": [], "root_motion": None}, 0.0))
    pelvis_p = fk_pose({"tracks": [], "root_motion": None}, 0.0)["pelvis"][0]
    pelvis_basis = bone_basis("pelvis")
    out = ['    - name: pelvis_socket', '      components:', '        - type: Transform',
           '        - type: BoneAttachment', '          bone: pelvis', '      children:']
    for obj in IK_TARGET_NAMES:
        if obj not in UPPER_BODY_TARGETS:
            continue
        p = rest[obj][0]
        world_offset = tuple(p[k] - pelvis_p[k] for k in range(3))
        local = [round(c, 5) for c in qrot(qconj(pelvis_basis), world_offset)]
        out += ["        - name: " + obj, "          components:", "            - type: Transform",
                f"              {vec('position', local)}", "          children: []"]
    for obj in IK_TARGET_NAMES:
        if obj in UPPER_BODY_TARGETS:
            continue
        p = [round(c, 5) for c in rest[obj][0]]
        out += [f"    - name: {obj}", "      components:", "        - type: Transform",
                f"          {vec('position', p)}", "      children: []"]
    return out


def main():
    os.makedirs(MESH_DIR, exist_ok=True)
    for name, (r, length) in PART_MESHES.items():
        capsule_mesh(os.path.join(MESH_DIR, f"{name}.yaml"), r, length)
    write_rig()
    write_object()
    write_object(RAGDOLL_OBJECT_PATH, ragdoll_component(),
                 "With a Ragdoll: R (input_toggle) or Ragdoll::activate() goes limp, deactivate() recovers.")
    idle()
    # Travel per cycle ~ two foot strides (2 x 2 x leg 0.84 x sin(stride)): walk ~1.6 m/s, run ~5.3 m/s.
    gait("walk", 0.9, stride=26, knee=55, arm_swing=22, elbow=15, lean=3, bob=0.02, pelvis_z=0.94, travel=1.45)
    gait("run", 0.6, stride=44, knee=95, arm_swing=45, elbow=70, lean=12, bob=0.04, pelvis_z=0.9, travel=3.2)
    jump()
    ride()
    for clip in ("idle", "walk", "run", "jump", "ride"):
        bake_ik_clip(clip)
    write_object(IK_OBJECT_PATH, ik_components(), IK_HEADER_NOTE,
                 clip_dir="animations/mannequin", extra_children=ik_extra_children(),
                 extra_animator_clip_dir="animations/mannequin_ik")
    print("wrote", RIG_PATH, "+", OBJECT_PATH, "+", RAGDOLL_OBJECT_PATH, "+", IK_OBJECT_PATH, "+", len(PART_MESHES),
          "meshes + 5 clips + 5 IK clips")


if __name__ == "__main__":
    main()
