#!/usr/bin/env python3
"""Converts Blender scenes into a toyengine assets folder (meshes, materials, textures, prefabs, scenes).

Usage (system python, standard library only -- it drives Blender in background mode):

    python3 tools/blender_to_toy.py --out <assets_dir> level.blend props.blend [options]

    --out DIR          assets folder to write into (created if missing). Files the converter
                       produces overwrite whatever is already at the same path; nothing else in
                       the folder is touched or deleted.
    --blender PATH     Blender executable (default: $BLENDER, the macOS app bundle, then PATH).
    --no-modifiers     export base meshes instead of the modifier-evaluated result.
    --light-scale F    point/spot/area watts -> engine intensity (default 0.5).
    --sun-scale F      sun strength -> DirectionalLight intensity (default 1.0).

The .blend files are converted one after another in the order given, so when two files hold an
asset with the same name the LAST file wins. The same script also runs inside Blender directly:

    blender -b level.blend --python tools/blender_to_toy.py -- --out <assets_dir>

Output (everything under the `blender` tag folder; tags never appear in references):

    meshes/blender/<mesh>.yaml          one per mesh datablock (per object when modifiers apply)
    materials/blender/<material>.yaml   Principled BSDF -> PBR material
    textures/blender/<image>.png        images used by those materials (+ packed _mr / _mask maps)
    objects/blender/<collection>.yaml   a prefab per instanced collection
    objects/blender/<armature>.yaml     a rig per armature: bones, skinned meshes, Animator
    animations/blender/<rig>/<action>.yaml   one clip per action that animates the rig's bones
    scenes/blender/<scene>/scene.yaml   one per Blender scene, named after the .blend file

Blender and toyengine share conventions (Z-up, right-handed, metres, CCW front faces, cameras
and spots looking down local -Z, XYZ Euler == the engine's Rz*Ry*Rx), so transforms copy
straight across. Anything the engine has no equivalent for is skipped with a warning.

Armatures become rigs (see the create-animation skill): each bone is an empty object at its head
(the bone tree mirrors Blender's), meshes deformed by the armature become SkinnedMeshRenderers
whose vertex groups name those bones (bind pose = the rest pose), and objects parented to a bone
sit under its bone object. Every action that animates the armature's bones (assigned, in the NLA,
or merely stored in the file) is baked frame by frame -- constraints and drivers included -- into
a clip of bone position / rotation_quat / scale tracks, and the rig's Animator gets one state per
clip (auto-playing the assigned action). In the scene the armature is an instance of its rig;
everything it owns lives in the rig asset. Object-level animation is not converted.
"""

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys

try:
    import bpy  # only present when running inside Blender
    from mathutils import Matrix, Vector
    IN_BLENDER = True
except ImportError:
    IN_BLENDER = False

TAG = "blender"
LOG_PREFIX = "[toy]"

# Engine light limits (see .claude/skills/create-scene/reference/components.md).
MAX_POINT_LIGHTS = 16
MAX_SPOT_LIGHTS = 8


def sanitize(name):
    """Turns a Blender datablock name into a safe asset file name (`Cube.001` -> `Cube_001`)."""
    out = re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_")
    return out or "unnamed"


# --------------------------------------------------------------------------------------------
# YAML emitter (Blender ships without PyYAML)
# --------------------------------------------------------------------------------------------

_PLAIN = re.compile(r"^[A-Za-z_][A-Za-z0-9_./-]*$")
_RESERVED = {"true", "false", "yes", "no", "on", "off", "null", "y", "n", "~"}


def fmt_num(x):
    """A float with a decimal point (so it parses as a float), rounded to 6 places, no -0.0."""
    x = round(float(x), 6)
    if x == 0.0:
        x = 0.0
    return repr(x)


def _scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return fmt_num(v)
    s = str(v)
    if _PLAIN.match(s) and s.lower() not in _RESERVED:
        return s
    return json.dumps(s)  # a JSON string is a valid double-quoted YAML scalar


def _is_scalar(v):
    return isinstance(v, (bool, int, float, str))


def _is_flat(v):
    if isinstance(v, dict):
        return 0 < len(v) <= 6 and all(_is_scalar(x) for x in v.values())
    if isinstance(v, list):
        return all(_is_scalar(x) for x in v)
    return False


def _flow(v):
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{k}: {_scalar(x)}" for k, x in v.items()) + " }"
    return "[" + ", ".join(_scalar(x) for x in v) + "]"


def _emit(v, indent):
    pad = " " * indent
    lines = []
    if isinstance(v, dict):
        for k, x in v.items():
            if _is_scalar(x):
                lines.append(f"{pad}{k}: {_scalar(x)}")
            elif _is_flat(x):
                lines.append(f"{pad}{k}: {_flow(x)}")
            elif not x:
                lines.append(f"{pad}{k}: {'[]' if isinstance(x, list) else '{}'}")
            else:
                lines.append(f"{pad}{k}:")
                lines.extend(_emit(x, indent + 2))
    else:
        for x in v:
            if _is_scalar(x):
                lines.append(f"{pad}- {_scalar(x)}")
            elif _is_flat(x):
                lines.append(f"{pad}- {_flow(x)}")
            else:
                sub = _emit(x, indent + 2)
                sub[0] = pad + "- " + sub[0].lstrip()
                lines.extend(sub)
    return lines


def to_yaml(data, header=""):
    """Serialises nested dicts/lists to block YAML, with small maps and scalar lists in flow style."""
    text = "\n".join(_emit(data, 0)) + "\n"
    return header + text


# --------------------------------------------------------------------------------------------
# Driver: runs Blender in background mode once per .blend file
# --------------------------------------------------------------------------------------------

def find_blender(explicit):
    """Resolves the Blender executable from --blender, $BLENDER, the macOS app bundle or PATH."""
    candidates = [explicit, os.environ.get("BLENDER"),
                  "/Applications/Blender.app/Contents/MacOS/Blender", shutil.which("blender")]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def parse_args(argv):
    p = argparse.ArgumentParser(description="Convert .blend files into a toyengine assets folder.")
    p.add_argument("blends", nargs="*", help=".blend files, converted in order (last one wins on clashes)")
    p.add_argument("--out", required=True, help="assets folder to write into (created if missing)")
    p.add_argument("--blender", help="Blender executable")
    p.add_argument("--no-modifiers", action="store_true", help="export base meshes, not modifier results")
    p.add_argument("--light-scale", type=float, default=0.5, help="point/spot/area watts -> intensity")
    p.add_argument("--sun-scale", type=float, default=1.0, help="sun strength -> intensity")
    p.add_argument("--source-name", help=argparse.SUPPRESS)  # set by the driver for each .blend
    return p.parse_args(argv)


def drive(args):
    """Runs the exporter inside Blender for each .blend file and prints a combined summary."""
    if not args.blends:
        print("error: no .blend files given", file=sys.stderr)
        return 2
    missing = [b for b in args.blends if not os.path.isfile(b)]
    if missing:
        print("error: not found: " + ", ".join(missing), file=sys.stderr)
        return 2
    blender = find_blender(args.blender)
    if not blender:
        print("error: Blender not found -- pass --blender or set $BLENDER", file=sys.stderr)
        return 2

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    script = os.path.abspath(__file__)
    failures, written, warnings = [], [], []

    for blend in args.blends:
        blend = os.path.abspath(blend)
        print(f"== {os.path.basename(blend)}")
        cmd = [blender, "-b", "--factory-startup", blend, "--python-exit-code", "1",
               "--python", script, "--", "--out", out,
               "--light-scale", str(args.light_scale), "--sun-scale", str(args.sun_scale)]
        if args.no_modifiers:
            cmd.append("--no-modifiers")
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace")
        for line in proc.stdout.splitlines():
            if line.startswith(LOG_PREFIX):
                msg = line[len(LOG_PREFIX):].strip()
                print("  " + msg)
                if msg.startswith("wrote "):
                    written.append(msg[6:])
                elif msg.startswith("warning:"):
                    warnings.append(f"{os.path.basename(blend)}: {msg}")
        if proc.returncode != 0:
            failures.append(blend)
            tail = "\n".join(proc.stdout.splitlines()[-25:])
            print(f"  FAILED (exit {proc.returncode}); Blender output tail:\n{tail}")

    overwritten = len(written) - len(set(written))
    print(f"\n{len(set(written))} files written to {out}"
          + (f" ({overwritten} written by more than one .blend; the later one won)" if overwritten else ""))
    if warnings:
        print(f"{len(warnings)} warnings (see above)")
    if failures:
        print("FAILED: " + ", ".join(os.path.basename(f) for f in failures))
        return 1
    return 0


# --------------------------------------------------------------------------------------------
# Exporter: runs inside Blender
# --------------------------------------------------------------------------------------------

class Exporter:
    """Writes every scene of the open .blend file into the assets folder."""

    def __init__(self, args):
        import numpy as np
        self.np = np
        self.out = os.path.abspath(args.out)
        self.apply_modifiers = not args.no_modifiers
        self.light_scale = args.light_scale
        self.sun_scale = args.sun_scale
        self.blend_name = os.path.basename(bpy.data.filepath) or "untitled.blend"
        self.unit = 1.0
        # Per-run caches: Blender datablock -> engine reference, and asset name -> owner key so
        # two different datablocks that sanitise to the same name don't silently overwrite.
        self.mesh_refs, self.material_refs, self.image_refs, self.prefab_refs = {}, {}, {}, {}
        self.rig_refs = {}
        self.taken = {"meshes": {}, "materials": {}, "textures": {}, "objects": {}, "scenes": {},
                      "animations": {}}
        self.prefabs_in_progress = set()
        self.rigs_in_progress = []  # stack of armatures whose rig is being written
        self.foreign = self._scan_foreign_assets()
        self._find_rig_ownership()

    def _find_rig_ownership(self):
        """Maps each mesh deformed by an armature to it (skin_of), and each object a rig owns --
        its skins and the armature's descendants -- to that armature (owner_rig)."""
        self.skin_of, self.owner_rig = {}, {}
        for obj in bpy.data.objects:
            if obj.type != "MESH":
                continue
            arms = [m.object for m in obj.modifiers
                    if m.type == "ARMATURE" and m.object and m.object.type == "ARMATURE"]
            if arms:
                if len(set(arms)) > 1:
                    self.warn(f"'{obj.name}' is deformed by several armatures; only '{arms[0].name}' is used")
                self.skin_of[obj] = arms[0]
        for obj in bpy.data.objects:
            p = obj.parent
            while p is not None and p.type != "ARMATURE":
                p = p.parent
            if p is not None:
                self.owner_rig[obj] = p  # the nearest armature ancestor owns it
        for skin, arm in self.skin_of.items():
            if skin not in self.owner_rig:
                self.owner_rig[skin] = arm

    def walkable(self, obj, members):
        """False when obj belongs to a rig whose armature is also in `members` (it's in the rig asset)."""
        arm = self.owner_rig.get(obj)
        return arm is None or arm not in members

    # ---- logging / files ---------------------------------------------------------------------

    def log(self, msg):
        print(f"{LOG_PREFIX} {msg}", flush=True)

    def warn(self, msg):
        self.log(f"warning: {msg}")

    def header(self, what):
        return (f"# {what} -- converted from {self.blend_name} by tools/blender_to_toy.py.\n"
                f"# Re-running the converter overwrites this file.\n")

    def _scan_foreign_assets(self):
        """Asset names already present under --out in tag folders other than `blender/`."""
        found = {}
        for kind, ext in (("meshes", ".yaml"), ("materials", ".yaml"), ("textures", ".png"),
                          ("objects", ".yaml")):
            root = os.path.join(self.out, kind)
            names = {}
            for dirpath, _, files in os.walk(root):
                rel = os.path.relpath(dirpath, root)
                if rel == TAG or rel.startswith(TAG + os.sep):
                    continue
                for f in files:
                    if f.endswith(ext):
                        names[f[: -len(ext)]] = os.path.join(kind, rel, f)
            found[kind] = names
        return found

    def claim(self, kind, base, key):
        """Reserves a unique asset name of `kind` for the datablock `key`, suffixing on clashes."""
        name, n = base, 2
        while name in self.taken[kind] and self.taken[kind][name] != key:
            name = f"{base}_{n}"
            n += 1
        if name != base:
            self.warn(f"{kind[:-1]} name '{base}' is used by two datablocks; second one saved as '{name}'")
        self.taken[kind][name] = key
        if name in self.foreign.get(kind, {}):
            self.warn(f"{kind[:-1]} '{name}' also exists at {self.foreign[kind][name]}; asset names must be "
                      f"unique per type, so references to it are ambiguous")
        return name

    def write_text(self, rel, text):
        path = os.path.join(self.out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        self.log(f"wrote {rel}")

    # ---- entry -------------------------------------------------------------------------------

    def run(self):
        stem = sanitize(os.path.splitext(self.blend_name)[0])
        scenes = list(bpy.data.scenes)
        for scene in scenes:
            name = stem if len(scenes) == 1 else f"{stem}_{sanitize(scene.name)}"
            self.export_scene(scene, name)

    # ---- scenes and objects ------------------------------------------------------------------

    def export_scene(self, scene, scene_name):
        self.unit = scene.unit_settings.scale_length or 1.0
        view_layer = scene.view_layers[0]
        for vl in scene.view_layers:
            if vl.use:
                view_layer = vl
                break
        depsgraph = view_layer.depsgraph
        depsgraph.update()
        self.depsgraph = depsgraph
        self.scene = scene
        self.counts = {"SUN": 0, "POINT": 0, "SPOT": 0}

        # Objects in collections excluded from the view layer (typically the sources of
        # collection instances) are left out of the scene; they still export as prefabs.
        visible = set(view_layer.objects)
        roots = [o for o in scene.objects if o in visible and (o.parent is None or o.parent not in visible)
                 and self.walkable(o, visible)]
        roots.sort(key=lambda o: o.name)
        root_objects = [self.export_object(o, None, visible) for o in roots]

        if self.counts["SUN"] > 1:
            self.warn(f"scene '{scene.name}' has {self.counts['SUN']} sun lights; only the first stays active")
        if self.counts["POINT"] > MAX_POINT_LIGHTS:
            self.warn(f"scene '{scene.name}' has {self.counts['POINT']} point lights; the engine uses {MAX_POINT_LIGHTS}")
        if self.counts["SPOT"] > MAX_SPOT_LIGHTS:
            self.warn(f"scene '{scene.name}' has {self.counts['SPOT']} spot lights; the engine uses {MAX_SPOT_LIGHTS}")

        scene_name = self.claim("scenes", scene_name, scene.name)
        data = {"format": "blender",
                "scene": {"scene_name": scene_name, "root_objects": root_objects}}
        header = self.header(f"Scene '{scene.name}'") + f"# Run: ./build/toyengine {scene_name}\n"
        self.write_text(os.path.join("scenes", TAG, scene_name, "scene.yaml"), to_yaml(data, header))

    def transform(self, obj, parent_matrix):
        """Engine Transform for obj relative to parent_matrix (None = world)."""
        m = obj.matrix_world if parent_matrix is None else parent_matrix.inverted_safe() @ obj.matrix_world
        return self.transform_from_matrix(m, obj.name)

    def transform_from_matrix(self, m, label):
        m3 = m.to_3x3()
        cols = [m3.col[i].normalized() for i in range(3)]
        if max(abs(cols[0].dot(cols[1])), abs(cols[1].dot(cols[2])), abs(cols[0].dot(cols[2]))) > 1e-3:
            self.warn(f"'{label}' is sheared (non-uniform parent scale with rotation); the engine can't represent it")
        if m3.determinant() < 0:
            self.warn(f"'{label}' is mirrored (negative scale); faces may render inside-out")
        loc, rot, sca = m.decompose()
        e = rot.to_euler("XYZ")
        return {"type": "Transform",
                "position": {"x": loc.x * self.unit, "y": loc.y * self.unit, "z": loc.z * self.unit},
                "rotation": {"x": round(math.degrees(e.x), 4), "y": round(math.degrees(e.y), 4),
                             "z": round(math.degrees(e.z), 4)},
                "scale": {"x": sca.x, "y": sca.y, "z": sca.z}}

    def is_active(self, obj):
        if obj.hide_render:
            return False
        colls = obj.users_collection
        return not colls or not all(c.hide_render for c in colls)

    def export_object(self, obj, parent, members, parent_matrix=None):
        """Builds the scene/prefab node for obj and its children within `members`.

        parent_matrix overrides the parent's world matrix (prefab roots pass the collection's
        instance-offset frame); otherwise the transform is relative to obj.parent.
        """
        if parent_matrix is None and parent is not None:
            parent_matrix = parent.matrix_world
        node = {"name": obj.name}
        if not self.is_active(obj):
            node["active"] = False
        components = [self.transform(obj, parent_matrix)]

        t = obj.type
        if t == "MESH" and self.skin_of.get(obj) in self.rigs_in_progress:
            components.extend(self.export_skinned_renderer(obj, self.skin_of[obj]))
        elif t in ("MESH", "CURVE", "SURFACE", "META", "FONT"):
            renderer = self.export_mesh_renderer(obj)
            if renderer:
                components.append(renderer)
        elif t == "LIGHT":
            light, active = self.export_light(obj)
            if light:
                components.append(light)
            if not active:
                node["active"] = False
        elif t == "CAMERA":
            components.append(self.export_camera(obj))
        elif t == "EMPTY":
            if obj.instance_type == "COLLECTION" and obj.instance_collection:
                ref = self.export_prefab(obj.instance_collection)
                if ref:
                    node["prefab"] = ref
            elif obj.instance_type != "NONE":
                self.warn(f"'{obj.name}': {obj.instance_type} instancing is not supported; exported as an empty")
        elif t == "ARMATURE":
            ref = self.export_rig(obj)
            if ref:
                node["prefab"] = ref
        else:
            self.warn(f"'{obj.name}': object type {t} is not supported; exported as an empty")

        action = obj.animation_data.action if obj.animation_data else None
        if action and t != "ARMATURE":
            self.warn(f"'{obj.name}': animation (action '{action.name}') is not converted")
        elif action and any(not fc.data_path.startswith("pose.bones[") for fc in self.action_fcurves(action)):
            self.warn(f"'{obj.name}': action '{action.name}' also animates the armature object itself; "
                      f"only bone animation is converted")

        node["components"] = components
        children = sorted((c for c in obj.children if c in members and self.walkable(c, members)),
                          key=lambda c: c.name)
        node["children"] = [self.export_object(c, obj, members) for c in children]
        return node

    def export_prefab(self, coll):
        """Writes coll as objects/blender/<name>.yaml (once) and returns its reference."""
        if coll in self.prefab_refs:
            return self.prefab_refs[coll]
        if coll in self.prefabs_in_progress:
            self.warn(f"collection '{coll.name}' instances itself; recursion cut")
            return None
        self.prefabs_in_progress.add(coll)
        members = set(coll.all_objects)
        roots = sorted((o for o in members if (o.parent is None or o.parent not in members)
                        and self.walkable(o, members)), key=lambda o: o.name)
        frame = Matrix.Translation(coll.instance_offset)  # the prefab root sits at the instance offset
        children = [self.export_object(o, None, members, parent_matrix=frame) for o in roots]
        name = self.claim("objects", sanitize(coll.name), coll.name)
        data = {"format": "toyengine-object",
                "object": {"name": coll.name,
                           "components": [self.identity_transform()],
                           "children": children}}
        self.write_text(os.path.join("objects", TAG, name + ".yaml"),
                        to_yaml(data, self.header(f"Collection '{coll.name}' as a prefab")))
        self.prefabs_in_progress.discard(coll)
        ref = f"objects/{name}"
        self.prefab_refs[coll] = ref
        return ref

    # ---- rigs and animation ------------------------------------------------------------------

    @staticmethod
    def bone_name(bone):
        """A bone's object name: its Blender name minus '/', which clip tracks use as the path separator."""
        return bone.name.replace("/", "_")

    def bone_local(self, bone):
        """A rest bone's matrix relative to its parent bone (root bones: armature space)."""
        return bone.matrix_local if bone.parent is None else bone.parent.matrix_local.inverted_safe() @ bone.matrix_local

    def export_rig(self, arm):
        """Writes the armature as objects/blender/<name>.yaml (once) plus its clips; returns its reference.

        The rig root sits at the armature object's origin: bones and every object the rig owns
        are placed in armature space, so a scene instance carries the armature's transform.
        """
        if arm in self.rig_refs:
            return self.rig_refs[arm]
        if arm in self.rigs_in_progress:
            self.warn(f"armature '{arm.name}' contains itself; recursion cut")
            return None
        name = self.claim("objects", sanitize(arm.name), ("rig", arm.name))
        ref = f"objects/{name}"
        self.rig_refs[arm] = ref

        # Rest pose while the tree is built: bone-parented children and skins are placed, and
        # skins evaluated, undeformed -- the engine's bind pose is the rig's pose at start.
        self.rigs_in_progress.append(arm)
        pose_position = arm.data.pose_position
        try:
            arm.data.pose_position = "REST"
            self.depsgraph.update()
            children = self.rig_tree(arm)
        finally:
            arm.data.pose_position = pose_position
            self.depsgraph.update()
            self.rigs_in_progress.pop()

        components = [self.identity_transform()]
        states, auto_play = self.export_clips(arm, name)
        if states:
            components.append({"type": "Animator", "auto_play": auto_play, "states": states})
        data = {"format": "toyengine-object",
                "object": {"name": arm.name, "components": components, "children": children}}
        self.write_text(os.path.join("objects", TAG, name + ".yaml"),
                        to_yaml(data, self.header(f"Armature '{arm.name}' as a rig")))
        return ref

    @staticmethod
    def identity_transform():
        return {"type": "Transform", "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "rotation": {"x": 0.0, "y": 0.0, "z": 0.0}, "scale": {"x": 1.0, "y": 1.0, "z": 1.0}}

    def rig_tree(self, arm):
        """The rig root's children: the bone tree, then the skins and other objects the rig owns."""
        bones = arm.data.bones
        bone_nodes = {}

        def bone_node(b):
            node = {"name": self.bone_name(b),
                    "components": [self.transform_from_matrix(self.bone_local(b), f"{arm.name}: bone {b.name}")]}
            bone_nodes[b.name] = node
            node["children"] = [bone_node(c) for c in sorted(b.children, key=lambda c: c.name)]
            return node

        children = [bone_node(b) for b in sorted((b for b in bones if b.parent is None), key=lambda b: b.name)]
        members = {o for o, a in self.owner_rig.items() if a is arm}
        tops = sorted((o for o in members if o.parent is arm or o.parent not in members), key=lambda o: o.name)
        for o in tops:
            on_bone = (o.parent is arm and o.parent_type == "BONE" and o.parent_bone in bone_nodes
                       and self.skin_of.get(o) is not arm)
            if on_bone:
                frame = arm.matrix_world @ bones[o.parent_bone].matrix_local
                bone_nodes[o.parent_bone]["children"].append(self.export_object(o, None, members, parent_matrix=frame))
            else:
                children.append(self.export_object(o, None, members, parent_matrix=arm.matrix_world))
        return children

    def export_skinned_renderer(self, obj, arm):
        """MeshRenderer (material only) + SkinnedMeshRenderer for a mesh the armature deforms."""
        mesh_name = self.export_mesh(obj, skin_arm=arm)
        if not mesh_name:
            return []
        mats = [s.material for s in obj.material_slots]
        if len(mats) > 1:
            self.warn(f"'{obj.name}': skinned meshes take one material; using the first of {len(mats)} slots")
        material = self.export_material(mats[0]) if mats and mats[0] else "materials/default"
        return [{"type": "MeshRenderer", "material": material},
                {"type": "SkinnedMeshRenderer", "mesh_path": mesh_name}]

    @staticmethod
    def action_fcurves(action):
        """Every F-curve of an action, layered (Blender 4.4+ slotted actions) or legacy."""
        layers = getattr(action, "layers", None)
        if layers:
            return [fc for layer in layers for strip in layer.strips
                    for bag in getattr(strip, "channelbags", ()) for fc in bag.fcurves]
        return list(getattr(action, "fcurves", ()))

    _BONE_PATH = re.compile(r'^pose\.bones\["((?:[^"\\]|\\.)*)"\]')

    def action_bones(self, action):
        """Names of the bones an action's F-curves animate."""
        names = set()
        for fc in self.action_fcurves(action):
            m = self._BONE_PATH.match(fc.data_path)
            if m:
                names.add(re.sub(r"\\(.)", r"\1", m.group(1)))
        return names

    def rig_actions(self, arm):
        """The armature's actions: its assigned one, its NLA strips', then any action in the file
        that animates its bones and isn't assigned to some other object."""
        found = []
        ad = arm.animation_data
        if ad:
            if ad.action:
                found.append(ad.action)
            for track in ad.nla_tracks:
                for strip in track.strips:
                    if strip.action and strip.action not in found:
                        found.append(strip.action)
        claimed = set()
        for o in bpy.data.objects:
            if o is not arm and o.animation_data:
                if o.animation_data.action:
                    claimed.add(o.animation_data.action)
                claimed.update(s.action for t in o.animation_data.nla_tracks for s in t.strips if s.action)
        names = {b.name for b in arm.data.bones}
        for action in sorted(bpy.data.actions, key=lambda a: a.name):
            if action not in found and action not in claimed and self.action_bones(action) & names:
                found.append(action)
        return found

    def bake_action(self, arm, action):
        """Samples every pose bone's parent-relative (position, quat xyzw, scale) at each whole
        frame of the action's range, with only that action playing. Returns a list of frames."""
        scene = self.scene
        had_anim = arm.animation_data is not None
        ad = arm.animation_data_create()
        saved_action, saved_slot = ad.action, getattr(ad, "action_slot", None)
        saved_mutes = [t.mute for t in ad.nla_tracks]
        saved_frame, saved_sub = scene.frame_current, scene.frame_subframe
        saved_pose = arm.data.pose_position
        start, end = action.frame_range
        f0, f1 = int(math.floor(start + 1e-4)), int(math.ceil(end - 1e-4))
        frames = []
        try:
            for t in ad.nla_tracks:
                t.mute = True
            ad.action = action
            if hasattr(ad, "action_slot") and ad.action_slot is None and len(action.slots):
                slots = [s for s in action.slots if s.target_id_type in ("OBJECT", "UNSPECIFIED")]
                if slots:
                    ad.action_slot = slots[0]
            arm.data.pose_position = "POSE"
            for f in range(f0, max(f1, f0) + 1):
                scene.frame_set(f)
                pose = arm.evaluated_get(self.depsgraph).pose
                sample = {}
                for pb in pose.bones:
                    m = pb.matrix if pb.parent is None else pb.parent.matrix.inverted_safe() @ pb.matrix
                    loc, rot, sca = m.decompose()
                    sample[pb.name] = ((loc.x * self.unit, loc.y * self.unit, loc.z * self.unit),
                                       (rot.x, rot.y, rot.z, rot.w), (sca.x, sca.y, sca.z))
                frames.append(sample)
        finally:
            ad.action = saved_action
            if saved_action is not None and saved_slot is not None:
                ad.action_slot = saved_slot
            for t, mute in zip(ad.nla_tracks, saved_mutes):
                t.mute = mute
            arm.data.pose_position = saved_pose
            if not had_anim:
                arm.animation_data_clear()
            scene.frame_set(saved_frame, subframe=saved_sub)
        return frames

    def export_clips(self, arm, rig_name):
        """Bakes and writes every action of the rig; returns (Animator states, auto_play state)."""
        actions = self.rig_actions(arm)
        if not actions:
            return [], None
        r = self.scene.render
        fps = r.fps / (r.fps_base or 1.0)
        bones = arm.data.bones
        paths = {}
        for b in bones:  # parents come before children in Blender's bone list
            paths[b.name] = (paths[b.parent.name] + "/" if b.parent else "") + self.bone_name(b)
        rest = {}
        for b in bones:
            loc, rot, sca = self.bone_local(b).decompose()
            rest[b.name] = ((loc.x * self.unit, loc.y * self.unit, loc.z * self.unit),
                            (rot.x, rot.y, rot.z, rot.w), (sca.x, sca.y, sca.z))

        baked = [(a, self.bake_action(arm, a)) for a in actions]

        # A channel that moves in any clip is keyed in every clip, so switching clips always
        # drives it (a clip that leaves it alone would leave it wherever the last one put it).
        def differs(ch, a, b):
            if ch == 1:
                return abs(sum(x * y for x, y in zip(a, b))) < 1.0 - 1e-6
            return max(abs(x - y) for x, y in zip(a, b)) > 1e-5
        moving = set()
        for _, frames in baked:
            for sample in frames:
                for bname, vals in sample.items():
                    for ch in range(3):
                        if (bname, ch) not in moving and bname in rest and differs(ch, vals[ch], rest[bname][ch]):
                            moving.add((bname, ch))
        channels = sorted(moving, key=lambda k: (paths[k[0]], k[1]))

        states, auto_play = [], None
        assigned = arm.animation_data.action if arm.animation_data else None
        for action, frames in baked:
            if not frames:
                continue
            claimed = self.claim("animations", f"{rig_name}/{sanitize(action.name)}", (arm.name, action.name))
            clip = claimed.split("/", 1)[1]
            text = self.clip_yaml(clip, frames, fps, channels, paths)
            self.write_text(os.path.join("animations", TAG, rig_name, clip + ".yaml"),
                            self.header(f"Action '{action.name}' on armature '{arm.name}'") + text)
            states.append({"name": clip, "clip": f"animations/{rig_name}/{clip}.yaml"})
            if auto_play is None or action == assigned:
                auto_play = clip
        return states, auto_play

    @staticmethod
    def reduce_keys(values, tol):
        """Indices of the keys to keep: a key is dropped when interpolating linearly between its
        kept neighbours reproduces it (and every key in between) within tol."""
        n = len(values)
        if n <= 2:
            return list(range(n))

        def near(a, b):
            return max(abs(x - y) for x, y in zip(a, b)) <= tol

        if all(near(v, values[0]) for v in values):
            return [0, n - 1]
        keep, a, j = [0], 0, 2
        while j < n:
            va, vj = values[a], values[j]
            ok = all(near(values[k], [x + (y - x) * (k - a) / (j - a) for x, y in zip(va, vj)])
                     for k in range(a + 1, j))
            if ok:
                j += 1
            else:
                keep.append(j - 1)
                a, j = j - 1, j + 1
        keep.append(n - 1)
        return keep

    def clip_yaml(self, clip, frames, fps, channels, paths):
        """One clip: a position / rotation_quat / scale track per moving bone channel, keyed per frame."""
        props = ("position", "rotation_quat", "scale")
        lines = ["clip:", f"  name: {_scalar(clip)}", "  wrap: loop"]
        if len(frames) > 1:
            lines.append(f"  length: {fmt_num((len(frames) - 1) / fps)}")
        lines.append("  tracks:" if channels else "  tracks: []")
        for bname, ch in channels:
            values = [list(f[bname][ch]) for f in frames]
            if ch == 1:  # nlerp takes the short way only between keys in the same hemisphere
                for i in range(1, len(values)):
                    if sum(x * y for x, y in zip(values[i], values[i - 1])) < 0.0:
                        values[i] = [-x for x in values[i]]
            lines.append(f"    - object: {_scalar(paths[bname])}")
            lines.append(f"      property: {props[ch]}")
            lines.append("      keys:")
            for i in self.reduce_keys(values, 1e-4):
                lines.append(f"        - {{ time: {fmt_num(i / fps)}, value: [{', '.join(fmt_num(x) for x in values[i])}] }}")
        return "\n".join(lines) + "\n"

    # ---- lights and cameras ------------------------------------------------------------------

    def export_light(self, obj):
        """Returns (component, active)."""
        L = obj.data
        color = {"r": L.color[0], "g": L.color[1], "b": L.color[2]}
        shadows = bool(getattr(L, "use_shadow", True))
        if L.type == "SUN":
            self.counts["SUN"] += 1
            d = (obj.matrix_world.to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized()
            comp = {"type": "DirectionalLight", "direction": {"x": d.x, "y": d.y, "z": d.z},
                    "color": color, "intensity": L.energy * self.sun_scale, "cast_shadows": shadows}
            return comp, self.counts["SUN"] == 1

        if getattr(L, "use_custom_distance", False):
            rng = L.cutoff_distance * self.unit
        else:
            rng = min(max(math.sqrt(max(L.energy, 0.0)), 1.0), 50.0) * self.unit
        base = {"color": color, "intensity": L.energy * self.light_scale, "range": rng, "cast_shadows": shadows}
        if L.type == "POINT":
            self.counts["POINT"] += 1
            return {"type": "PointLight", **base}, True
        if L.type == "SPOT":
            self.counts["SPOT"] += 1
            outer = math.degrees(L.spot_size) * 0.5
            return {"type": "SpotLight", **base, "inner_angle": outer * (1.0 - L.spot_blend),
                    "outer_angle": outer}, True
        if L.type == "AREA":
            self.counts["SPOT"] += 1
            self.warn(f"'{obj.name}': area light approximated as a wide spot light")
            return {"type": "SpotLight", **base, "inner_angle": 40.0, "outer_angle": 80.0}, True
        self.warn(f"'{obj.name}': light type {L.type} not supported")
        return None, True

    def export_camera(self, obj):
        cam = obj.data
        r = self.scene.render
        aspect = (r.resolution_x * r.pixel_aspect_x) / max(r.resolution_y * r.pixel_aspect_y, 1e-6)
        fit = cam.sensor_fit
        if fit == "AUTO":
            fit = "HORIZONTAL" if aspect >= 1.0 else "VERTICAL"
            sensor = cam.sensor_width
        else:
            sensor = cam.sensor_width if fit == "HORIZONTAL" else cam.sensor_height

        comp = {"type": "Camera", "main": obj == self.scene.camera}
        if cam.type == "ORTHO":
            half = cam.ortho_scale * 0.5 * self.unit
            comp["projection"] = "Orthographic"
            comp["orthographic_size"] = half / aspect if fit == "HORIZONTAL" else half
        else:
            if cam.type != "PERSP":
                self.warn(f"'{obj.name}': {cam.type} camera exported as perspective")
            half_angle = math.atan(sensor / (2.0 * cam.lens))
            if fit == "HORIZONTAL":
                half_angle = math.atan(math.tan(half_angle) / aspect)
            comp["projection"] = "Perspective"
            comp["fov"] = math.degrees(2.0 * half_angle)
            comp["lens"] = cam.lens
            comp["sensor_width"] = cam.sensor_width
        comp["near_clip_plane"] = cam.clip_start * self.unit
        comp["far_clip_plane"] = cam.clip_end * self.unit
        if abs(cam.shift_x) > 1e-6 or abs(cam.shift_y) > 1e-6:
            self.warn(f"'{obj.name}': camera lens shift is not supported")
        return comp

    # ---- meshes ------------------------------------------------------------------------------

    def export_mesh_renderer(self, obj):
        mesh_name = self.export_mesh(obj)
        if not mesh_name:
            return None
        slots = [s.material for s in obj.material_slots]
        refs = [self.export_material(m) if m else "materials/default" for m in slots] or ["materials/default"]
        comp = {"type": "MeshRenderer", "mesh_path": mesh_name}
        if len(refs) == 1:
            comp["material"] = refs[0]
        else:
            comp["materials"] = refs
        return comp

    def export_mesh(self, obj, skin_arm=None):
        """Writes the object's mesh (once per datablock) and returns its mesh_path key.

        With skin_arm, the mesh is that armature's skin: evaluated in the rest pose (the caller
        has set it) and written with its vertex-group weights for the armature's deform bones.
        """
        evaluated = obj.type != "MESH" or (self.apply_modifiers and any(m.show_render for m in obj.modifiers))
        key = ("obj", obj.name) if evaluated else ("mesh", obj.data.name)
        groups = None
        if skin_arm is not None:
            evaluated, key = True, ("skin", obj.name)
            deform = {b.name: b for b in skin_arm.data.bones if b.use_deform}
            groups = {vg.index: self.bone_name(deform[vg.name]) for vg in obj.vertex_groups if vg.name in deform}
        if key in self.mesh_refs:
            return self.mesh_refs[key]
        if skin_arm is not None:
            if not groups:
                self.warn(f"'{obj.name}': no vertex group names a deform bone of '{skin_arm.name}'; "
                          f"the mesh won't follow the rig")
            if any(m.type == "ARMATURE" and not m.use_vertex_groups for m in obj.modifiers):
                self.warn(f"'{obj.name}': armature modifier deforms by envelopes; only vertex groups are converted")
            if obj.data.shape_keys:
                self.warn(f"'{obj.name}': shape keys are not converted; the skin uses its evaluated rest shape")

        src = obj.evaluated_get(self.depsgraph) if evaluated else obj
        me = bpy.data.meshes.new_from_object(src, preserve_all_data_layers=True, depsgraph=self.depsgraph)
        try:
            if not me.polygons:
                self.warn(f"'{obj.name}': mesh has no faces; no renderer exported")
                self.mesh_refs[key] = None
                return None
            name = self.claim("meshes", sanitize(key[1]), key)
            slot_names = [sanitize(s.material.name) if s.material else f"slot{i}"
                          for i, s in enumerate(obj.material_slots)]
            if skin_arm is not None:
                slot_names = []  # skinned meshes draw with one material
            text = self.mesh_yaml(me, slot_names, obj.name, groups)
        finally:
            bpy.data.meshes.remove(me)
        self.write_text(os.path.join("meshes", TAG, name + ".yaml"), text)
        self.mesh_refs[key] = name
        return name

    def mesh_yaml(self, me, slot_names, label, groups=None):
        """One entry per face corner (loop): positions, corner normals, UVs, tangents, and with
        `groups` (vertex-group index -> bone name) skin weights."""
        np = self.np
        # The engine fan-triangulates polygons, which breaks on concave n-gons, and
        # calc_tangents() rejects n-gons, so anything above a quad is triangulated first.
        if any(p.loop_total > 4 for p in me.polygons):
            import bmesh
            bm = bmesh.new()
            bm.from_mesh(me)
            ngons = [f for f in bm.faces if len(f.verts) > 4]
            bmesh.ops.triangulate(bm, faces=ngons, quad_method="BEAUTY", ngon_method="BEAUTY")
            bm.to_mesh(me)
            bm.free()

        n_loops = len(me.loops)
        co = np.empty(len(me.vertices) * 3, np.float64)
        me.vertices.foreach_get("co", co)
        vidx = np.empty(n_loops, np.int64)
        me.loops.foreach_get("vertex_index", vidx)
        pos = co.reshape(-1, 3)[vidx] * self.unit

        normals = np.empty(n_loops * 3, np.float64)
        if hasattr(me, "corner_normals"):  # Blender 4.1+
            me.corner_normals.foreach_get("vector", normals)
        else:
            me.calc_normals_split()
            me.loops.foreach_get("normal", normals)
        normals = normals.reshape(-1, 3)

        uvs = tangents = None
        uv_layer = me.uv_layers.active
        if uv_layer:
            uvs = np.empty(n_loops * 2, np.float64)
            uv_layer.uv.foreach_get("vector", uvs) if hasattr(uv_layer, "uv") else uv_layer.data.foreach_get("uv", uvs)
            uvs = uvs.reshape(-1, 2)
            if uvs.size and (uvs.min() < -1e-3 or uvs.max() > 1.0 + 1e-3):
                self.warn(f"'{label}': UVs outside 0..1 -- engine textures clamp (no tiling)")
            # Blender's v=0 is the image's bottom row; the engine samples PNG rows top-down.
            uvs[:, 1] = 1.0 - uvs[:, 1]
            try:
                me.calc_tangents(uvmap=uv_layer.name)
                t = np.empty(n_loops * 3, np.float64)
                me.loops.foreach_get("tangent", t)
                sign = np.empty(n_loops, np.float64)
                me.loops.foreach_get("bitangent_sign", sign)
                # Engine bitangent = cross(N, T) * w and must point image-up, which is
                # Blender's +v -- exactly what bitangent_sign describes.
                tangents = np.hstack([t.reshape(-1, 3), sign.reshape(-1, 1)])
            except RuntimeError as e:
                self.warn(f"'{label}': tangents not exported ({e}); the engine will derive them")

        starts = np.empty(len(me.polygons), np.int64)
        totals = np.empty(len(me.polygons), np.int64)
        me.polygons.foreach_get("loop_start", starts)
        me.polygons.foreach_get("loop_total", totals)
        mat_idx = np.empty(len(me.polygons), np.int64)
        me.polygons.foreach_get("material_index", mat_idx)

        def rows(arr):
            arr = np.round(arr, 6)
            arr[arr == 0] = 0.0  # drop -0.0
            return "\n".join("  - [" + ", ".join(repr(x) for x in r) + "]" for r in arr.tolist())

        out = [self.header(f"Mesh for '{label}'") + "vertices:\n" + rows(pos)]
        out.append("normals:\n" + rows(normals))
        if uvs is not None:
            out.append("uvs:\n" + rows(uvs))
        if tangents is not None:
            out.append("tangents:\n" + rows(tangents))
        out.append("faces:\n" + "\n".join(
            "  - [" + ", ".join(str(i) for i in range(s, s + n)) + "]" for s, n in zip(starts.tolist(), totals.tolist())))
        if len(slot_names) > 1:
            out.append("material_slots: [" + ", ".join(slot_names) + "]")
            out.append("face_materials: [" + ", ".join(str(min(i, len(slot_names) - 1)) for i in mat_idx.tolist()) + "]")
        if groups:
            # The engine skins with each vertex's 4 strongest groups; normalise those to 1.
            per_vertex, unweighted = [], 0
            for v in me.vertices:
                infl = sorted(((g.weight, groups[g.group]) for g in v.groups
                               if g.group in groups and g.weight > 0.0), reverse=True)[:4]
                total = sum(w for w, _ in infl)
                if total <= 0.0:
                    per_vertex.append("{}")
                    unweighted += 1
                else:
                    per_vertex.append("{ " + ", ".join(f"{_scalar(n)}: {fmt_num(w / total)}" for w, n in infl) + " }")
            if unweighted:
                self.warn(f"'{label}': {unweighted} vertices have no deform-bone weight; they won't move with the rig")
            out.append("weights:\n" + "\n".join("  - " + per_vertex[i] for i in vidx.tolist()))
        return "\n".join(out) + "\n"

    # ---- materials ---------------------------------------------------------------------------

    def export_material(self, mat):
        if mat in self.material_refs:
            return self.material_refs[mat]
        name = self.claim("materials", sanitize(mat.name), mat.name)
        ref = f"materials/{name}"
        self.material_refs[mat] = ref
        data = self.material_data(mat, name)
        self.write_text(os.path.join("materials", TAG, name + ".yaml"),
                        to_yaml(data, self.header(f"Material '{mat.name}'")))
        return ref

    @staticmethod
    def _input(node, *names):
        for n in names:
            if n in node.inputs:
                return node.inputs[n]
        return None

    def _surface_node(self, mat):
        tree = mat.node_tree
        out = None
        for n in tree.nodes:
            if n.type == "OUTPUT_MATERIAL" and n.target in ("ALL", "EEVEE") and n.is_active_output:
                out = n
                break
        if out is None:
            out = next((n for n in tree.nodes if n.type == "OUTPUT_MATERIAL"), None)
        if out and out.inputs["Surface"].is_linked:
            node = out.inputs["Surface"].links[0].from_node
            while node.type == "REROUTE" and node.inputs[0].is_linked:
                node = node.inputs[0].links[0].from_node
            return node
        return None

    def trace_image(self, socket, label):
        """Follows socket back to an Image Texture node.

        Returns (image, channel) with channel one of "RGB", "R", "G", "B", "A", or None when
        the socket is unlinked or fed by something other than a simple image chain.
        """
        if socket is None or not socket.is_linked:
            return None
        link = socket.links[0]
        node, out_sock, channel = link.from_node, link.from_socket, None
        while True:
            if node.type == "REROUTE":
                if not node.inputs[0].is_linked:
                    return None
                link = node.inputs[0].links[0]
                node, out_sock = link.from_node, link.from_socket
            elif node.type in ("SEPRGB", "SEPARATE_COLOR"):
                channel = {"R": "R", "Red": "R", "G": "G", "Green": "G", "B": "B", "Blue": "B"}.get(out_sock.name)
                if getattr(node, "mode", "RGB") != "RGB" or channel is None or not node.inputs[0].is_linked:
                    self.warn(f"{label}: unsupported Separate Color setup; using the constant value")
                    return None
                link = node.inputs[0].links[0]
                node, out_sock = link.from_node, link.from_socket
            elif node.type == "TEX_IMAGE":
                if node.image is None:
                    return None
                vec = node.inputs["Vector"]
                if vec.is_linked and vec.links[0].from_node.type == "MAPPING":
                    self.warn(f"{label}: Mapping node ignored -- engine textures don't tile or transform")
                if out_sock.name == "Alpha":
                    channel = "A"
                return node.image, channel or "RGB"
            else:
                self.warn(f"{label}: '{node.bl_label}' node in the texture chain is not supported; "
                          f"using the constant value")
                return None

    def material_data(self, mat, name):
        label = f"material '{mat.name}'"
        d = {}
        node = self._surface_node(mat) if mat.use_nodes and mat.node_tree else None

        if node is None or node.type != "BSDF_PRINCIPLED":
            if node is not None and node.type == "BSDF_DIFFUSE":
                c = node.inputs["Color"].default_value
                d["albedo"] = {"r": c[0], "g": c[1], "b": c[2]}
                d["metallic"] = 0.0
                d["roughness"] = node.inputs["Roughness"].default_value
                return d
            if node is not None and node.type == "EMISSION":
                c = node.inputs["Color"].default_value
                d["albedo"] = {"r": 0.0, "g": 0.0, "b": 0.0}
                d["emissive"] = {"r": c[0], "g": c[1], "b": c[2]}
                d["emissive_strength"] = node.inputs["Strength"].default_value
                return d
            if mat.use_nodes:
                self.warn(f"{label}: surface is not a Principled BSDF; using the viewport display colour")
            c = mat.diffuse_color
            d["albedo"] = {"r": c[0], "g": c[1], "b": c[2]}
            d["metallic"] = float(mat.metallic)
            d["roughness"] = float(mat.roughness)
            if c[3] < 1.0:
                d["alpha"] = c[3]
                d["alpha_mode"] = "BLEND"
            return d

        bsdf = node
        base = self._input(bsdf, "Base Color")
        metal = self._input(bsdf, "Metallic")
        rough = self._input(bsdf, "Roughness")
        alpha = self._input(bsdf, "Alpha")
        normal = self._input(bsdf, "Normal")
        em_col = self._input(bsdf, "Emission Color", "Emission")
        em_str = self._input(bsdf, "Emission Strength")
        transmission = self._input(bsdf, "Transmission Weight", "Transmission")
        ior = self._input(bsdf, "IOR")

        # Base colour.
        base_img = self.trace_image(base, label + " base color")
        if base_img:
            d["albedo"] = {"r": 1.0, "g": 1.0, "b": 1.0}
            d["texture_albedo"] = self.export_image(base_img[0])
        else:
            if base.is_linked:
                self.warn(f"{label}: base color input not converted; using its default value")
            c = base.default_value
            d["albedo"] = {"r": c[0], "g": c[1], "b": c[2]}

        # Metallic / roughness, packed glTF-style (G = roughness, B = metallic).
        m_img = self.trace_image(metal, label + " metallic")
        r_img = self.trace_image(rough, label + " roughness")
        d["metallic"] = 1.0 if m_img else float(metal.default_value)
        d["roughness"] = 1.0 if r_img else float(rough.default_value)
        if m_img or r_img:
            d["texture_metallic_roughness"] = self.export_mr(name, m_img, r_img)

        # Normal map.
        if normal is not None and normal.is_linked:
            nnode = normal.links[0].from_node
            if nnode.type == "NORMAL_MAP":
                if nnode.space != "TANGENT":
                    self.warn(f"{label}: {nnode.space} normal map not supported (tangent space only)")
                else:
                    n_img = self.trace_image(nnode.inputs["Color"], label + " normal")
                    if n_img:
                        d["texture_normal"] = self.export_image(n_img[0])
            else:
                self.warn(f"{label}: '{nnode.bl_label}' on the Normal input is not supported (use a Normal Map node)")

        # Emission.
        if em_col is not None and em_str is not None:
            c = em_col.default_value
            if em_col.is_linked:
                self.warn(f"{label}: textured emission not supported; using the constant colour")
            if em_str.default_value > 0.0 and max(c[0], c[1], c[2]) > 0.0:
                d["emissive"] = {"r": c[0], "g": c[1], "b": c[2]}
                d["emissive_strength"] = float(em_str.default_value)

        # Transparency.
        method = getattr(mat, "surface_render_method", None)  # Blender 4.2+: DITHERED / BLENDED
        blended = method == "BLENDED" or getattr(mat, "blend_method", "") == "BLEND"
        a_img = self.trace_image(alpha, label + " alpha")
        if a_img:
            img, channel = a_img
            if channel == "A":
                d["texture_alpha_mask"] = self.export_image(img)
            else:
                d["texture_alpha_mask"] = self.export_mask(name, img, channel)
            if blended:
                d["alpha_mode"] = "BLEND"
            else:
                d["alpha_mode"] = "CUTOUT"
                d["alpha_cutoff"] = float(getattr(mat, "alpha_threshold", 0.5))
                d["cull_backfaces"] = False
        elif alpha is not None and alpha.default_value < 0.999:
            d["alpha"] = float(alpha.default_value)
            d["alpha_mode"] = "BLEND"

        if transmission is not None and not transmission.is_linked and transmission.default_value > 0.5:
            d["alpha_mode"] = "BLEND"
            d["refraction"] = True
            if ior is not None:
                d["ior"] = float(ior.default_value)
        return d

    # ---- textures ----------------------------------------------------------------------------

    def _pixels(self, img):
        """(h, w, 4) float array of the image as stored (sRGB-encoded for byte colour images)."""
        np = self.np
        w, h = img.size
        if w == 0 or h == 0:
            return None
        px = np.empty(w * h * 4, np.float32)
        img.pixels.foreach_get(px)
        px = px.reshape(h, w, 4)
        if img.is_float and img.colorspace_settings.name not in ("Non-Color", "Raw", "Linear Rec.709"):
            # Float images hold linear values; a colour PNG is sRGB-encoded.
            rgb = np.clip(px[..., :3], 0.0, 1.0)
            px[..., :3] = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1 / 2.4) - 0.055)
        return px

    def _save_png(self, px, rel, non_color):
        """Writes an (h, w, 4) float array as a PNG at rel (under --out)."""
        np = self.np
        h, w = px.shape[:2]
        path = os.path.join(self.out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = bpy.data.images.new("__toy_export", w, h, alpha=True)
        try:
            if non_color:
                tmp.colorspace_settings.name = "Non-Color"
            tmp.pixels.foreach_set(np.clip(px, 0.0, 1.0).astype(np.float32).ravel())
            tmp.filepath_raw = path
            tmp.file_format = "PNG"
            tmp.save()
        finally:
            bpy.data.images.remove(tmp)
        self.log(f"wrote {rel}")

    def export_image(self, img):
        """Writes img to textures/blender/<name>.png (once) and returns its reference."""
        if img in self.image_refs:
            return self.image_refs[img]
        base = sanitize(os.path.splitext(img.name)[0])
        name = self.claim("textures", base, img.name)
        rel = os.path.join("textures", TAG, name + ".png")
        ref = f"textures/{name}.png"
        self.image_refs[img] = ref

        if img.source not in ("FILE", "GENERATED"):
            self.warn(f"image '{img.name}': {img.source} images are not supported")
            return ref
        src = bpy.path.abspath(img.filepath, library=img.library) if img.filepath else ""
        if img.source == "FILE" and not img.packed_file and src.lower().endswith(".png") and os.path.isfile(src):
            dst = os.path.join(self.out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.abspath(src) != os.path.abspath(dst):
                shutil.copyfile(src, dst)
            self.log(f"wrote {rel}")
            return ref
        px = self._pixels(img)
        if px is None:
            self.warn(f"image '{img.name}' has no pixel data (missing file?)")
            return ref
        self._save_png(px, rel, img.colorspace_settings.name == "Non-Color")
        return ref

    def _channel(self, img, channel, shape):
        """One channel of img as an (h, w) array, resampled (nearest) to shape."""
        np = self.np
        px = self._pixels(img)
        if px is None:
            return None
        idx = {"RGB": 0, "R": 0, "G": 1, "B": 2, "A": 3}[channel]
        ch = px[..., idx]
        if ch.shape != shape:
            ys = np.arange(shape[0]) * ch.shape[0] // shape[0]
            xs = np.arange(shape[1]) * ch.shape[1] // shape[1]
            ch = ch[ys][:, xs]
        return ch

    def export_mr(self, mat_name, m_img, r_img):
        """Packs metallic/roughness sources into <material>_mr.png (G = roughness, B = metallic)."""
        np = self.np
        first = (r_img or m_img)[0]
        w, h = first.size
        name = self.claim("textures", f"{mat_name}_mr", ("mr", mat_name))
        rel = os.path.join("textures", TAG, name + ".png")
        if w == 0 or h == 0:
            self.warn(f"image '{first.name}' has no pixel data (missing file?)")
            return f"textures/{name}.png"
        px = np.ones((h, w, 4), np.float32)
        for img_ch, dst in ((r_img, 1), (m_img, 2)):
            if img_ch:
                ch = self._channel(img_ch[0], img_ch[1], (h, w))
                if ch is not None:
                    px[..., dst] = ch
        self._save_png(px, rel, True)
        return f"textures/{name}.png"

    def export_mask(self, mat_name, img, channel):
        """Writes a white image whose alpha is the given channel of img (<material>_mask.png)."""
        np = self.np
        w, h = img.size
        name = self.claim("textures", f"{mat_name}_mask", ("mask", mat_name))
        rel = os.path.join("textures", TAG, name + ".png")
        ch = self._channel(img, channel, (h, w)) if w and h else None
        if ch is None:
            self.warn(f"image '{img.name}' has no pixel data (missing file?)")
            return f"textures/{name}.png"
        px = np.ones((h, w, 4), np.float32)
        px[..., 3] = ch
        self._save_png(px, rel, True)
        return f"textures/{name}.png"


def main():
    if IN_BLENDER:
        argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
        args = parse_args(argv)
        Exporter(args).run()
        return 0
    return drive(parse_args(sys.argv[1:]))


if __name__ == "__main__":
    code = main()
    if not IN_BLENDER:
        sys.exit(code)
