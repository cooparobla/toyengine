---
name: create-object
description: Create a reusable toyengine object asset (prefab) -- assets/objects/<name>.yaml holding an object tree with components (props, lamps, campfires, rigs, tile sets) -- and place instances of it in scenes with per-instance overrides. Use when asked to make a prefab, reusable object, prop, kit piece or "object asset", to turn part of a scene into one, or to instance/override one in a scene.
---

# Create an object asset (prefab)

An object asset is a single object tree in its own file that scenes instantiate with
`prefab:`. UI screens use the same mechanism (create-ui). Format reference:
`libs/libcoopa/coopa/scene/scene_inherit.h` (merge rules, documented at the top of the file).
Component keys: [../create-scene/reference/components.md](../create-scene/reference/components.md).

Examples: `assets/objects/props/campfire.yaml` (particles + flickering light + props, inline
materials), `robot_arm.yaml` (rig: empty joint objects with child `cube` visuals, Animator on
the root), `tentacle.yaml` (skinned mesh rig).

## File

```yaml
# <name> -- <what it is, how big, where its origin is>.
object:
  name: <name>
  components:
    - type: Transform
      position: { x: 0.0, y: 0.0, z: 0.0 }
      rotation: { x: 0.0, y: 0.0, z: 0.0 }
      scale: { x: 1.0, y: 1.0, z: 1.0 }
  children:
    - name: body
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 0.5 }
          rotation: { x: 0.0, y: 0.0, z: 0.0 }
          scale: { x: 1.0, y: 1.0, z: 1.0 }
        - type: MeshRenderer
          mesh_path: cube
          material: materials/wood_crate
      children: []
```

Shared assets go in `assets/objects/<tag>/` (e.g. `props`; `prefab: objects/<name>` finds it in
any tag folder); a scene-only one in `assets/scenes/<scene>/objects/`.
Lowercase snake_case file and object names. `type: Foo` entries, never `!Foo` tags.

## Placing it

```yaml
- name: crate_01
  prefab: objects/crate              # extension optional; "objects/rig#arm" picks a child
  components:
    - type: Transform                # REPLACES the prefab root's Transform
      position: { x: 3.0, y: 1.0, z: 0.0 }
      rotation: { x: 0.0, y: 0.0, z: 45.0 }
      scale: { x: 1.0, y: 1.0, z: 1.0 }
  children:
    - name: body                     # matched by name: deep-merges into the prefab's child
      components:
        - type: MeshRenderer         # matched by type: only the keys given change
          material: { base: materials/wood_crate, albedo: { r: 0.6, g: 0.4, b: 0.3 } }
```

Merge rules: components match by `type` (add `id:` when a type repeats), children by `name`;
maps deep-merge, lists and scalars replace; unmatched entries are appended; `remove: true` on a
component or child deletes it. Prefabs can nest (no cycles; depth limit 32). Paths inside the
prefab (meshes, materials) resolve against the prefab file's folder first, then `assets/`.

## Procedure

1. **Origin and scale first.** Put the root at the object's natural pivot -- the ground contact
   point for props (so `z: 0` places it on the floor), the hinge for a door, the base for a rig.
   Model in metres, Z-up.
2. **Keep the root clean.** The root usually holds only a Transform (plus behaviour components
   like Animator, Rigidbody, KinematicMover). Visuals go on children, so an instance's root
   Transform override and scaling of visuals never fight. Rigs: joints are empty objects, the
   visible shapes are their children (scaling a shape then never scales a joint).
3. **Name every child meaningfully** -- instances override by child name, animation tracks
   address children by path, and UI code finds widgets by name.
4. **Materials**: shared materials by path; small one-off looks inline. Avoid copying a shared
   material into the prefab.
5. **Physics**: a dynamic prop needs Rigidbody + collider on the same object (usually the
   root). Colliders take `material: <physics name>`.
6. **Document it** in the file header (what, size, origin, any overridable children) and, for a
   shared asset, mention it in `assets/README.md`'s layout/objects notes if it is a new kind.
7. The editor can also make one from a selection (Make Object Asset); its output is the same
   format.

## Verify

Place one or more instances (with an override) in a scene -- a scratch copy of a test scene is
fine -- and:

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>
./build/toyengine_tests caml_roundtrips_every_asset
```

Look at the frame: the object sits on the ground at its placed position, the override took
effect, nothing is missing (a misspelt component type is silently dropped). Check the `.log`.
