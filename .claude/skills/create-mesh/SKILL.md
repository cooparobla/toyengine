---
name: create-mesh
description: Create a toyengine mesh asset -- assets/meshes/<name>.yaml (vertices, normals, uvs, tangents, faces, material slots, vertex groups for skinning, LOD sidecars, collider meshes) -- by hand for simple shapes or with a Python generator for procedural ones. Use when asked to make, model, generate or fix a mesh / model / geometry / shape, add a collision mesh, add LODs, or when a face renders wrong (untextured, black, inside-out, faceted).
---

# Create a mesh

Mesh files are per-corner vertex arrays plus polygon index lists, in Blender's export style.
Parser: `libs/gfxcoopa/gfxcoopa/engine/data/mesh.h` (`parse_unwelded_`, fan triangulation,
tangent generation, slots, LODs). Helper library: `tools/mesh_yaml.py`. Generators to copy
from: `tools/gen_tile_side_meshes.py` (uses `mesh_yaml`), `tools/gen_particle_demo_assets.py`
(lathe, flat shading, triangulating writer), `tools/gen_water_grid_mesh.py` (grid),
`tools/gen_animation_demo_scene.py` (skinned tube with weights).

Check `assets/README.md` "Shared meshes" first: `cube`, `plane`, `sphere`... and a scaled
Transform often beat a new mesh.

## Format

```yaml
vertices:  [[x, y, z], ...]          # one entry per CORNER; every array below is indexed the same
normals:   [[x, y, z], ...]          # always write them (missing = (0,1,0))
uvs:       [[u, v], ...]             # every corner needs a real UV (missing = (0,0))
tangents:  [[x, y, z, w], ...]       # optional: omit the key and they're generated from UVs;
                                     # if written, never parallel to the normal; w = handedness
colors:    []                        # vertex paint data (editor only; the renderer ignores it)
weights:   [{}, {}, ...]             # one map per corner: { group_name: weight }; {} = none
faces:     [[i0, i1, i2, i3], ...]   # tris, quads or convex planar n-gons; CCW seen from outside
material_slots: [stone, moss]        # optional submeshes...
face_materials: [0, 0, 1, ...]       # ...slot index per face (parallel to faces)
```

Rules (each one has caused a real bug here):
- **All per-corner arrays have the same length** as `vertices`.
- **Winding is counter-clockwise viewed from outside**; the right-hand-rule normal points out.
  Clockwise faces are back-face culled away (inside-out mesh).
- **No duplicate or overlapping faces.** A stray coplanar face with zero UVs made the kitchen_sink test scene's
  pillar top render untextured.
- **Hard edges and UV seams = duplicated corners** with different normals/UVs (`cube.yaml`: 24
  corners, 4 per face). Corners weld at load only when position, normal, UV and tangent are all
  identical, so a smooth surface shares values.
- **Wrap-around seams** (cylinders, lathes, tubes) need a duplicated column at u = 1. Don't copy
  the `(s + 1) % segments` pattern in `gen_particle_demo_assets.lathe()` for textured meshes --
  its last strip's UVs run backwards.
- n-gons are fan-triangulated from their first index: concave or non-planar n-gons break; write
  triangles for anything tricky.
- **One file per use type.** The asset cache refuses the same file loaded as two types: a
  MeshCollider, WaterBody or SkinnedMeshRenderer needs its own copy
  (`<name>_collider.yaml`), even with identical data.
- Round coordinates to ~5 decimals so editor saves don't churn the file.
- Z-up, metres. Put the origin where the object should be placed from (base centre for props)
  and document it.

## LODs

A sidecar `<mesh>.lod.yaml` next to the mesh (overrides any inline `lods:`):

```yaml
lods:
  - { ratio: 0.5, screen_size: 0.12 }        # auto-simplified to 50% below 12% of screen height
  - { mesh: <name>_low, screen_size: 0.05 }  # or a hand-made sibling mesh
cull_screen_size: 0.004                      # 0/absent = never culled
```

`screen_size` must decrease down the list. Example: `assets/meshes/primitives/sphere.lod.yaml`.

## Skinning

Vertex groups in `weights:` name the bone objects (the 4 strongest per corner are used,
normalise them to 1). The rig object tree holds bones as ordinary child objects; the skin object
has a MeshRenderer without `mesh_path` plus `SkinnedMeshRenderer { mesh_path: <name> }`. See
`assets/meshes/animation/tentacle.yaml` + `assets/objects/animation/tentacle.yaml`, and create-animation.

## Procedure

1. **Simple shape (< ~30 corners)**: write the YAML by hand, mirroring `cube.yaml` /
   `tests/fixtures/scenes/kitchen_sink/meshes/pillar.yaml`.
2. **Anything procedural**: write a generator `tools/gen_<thing>.py` (standard library only --
   system python has no numpy) using `tools/mesh_yaml.py`:
   ```python
   import mesh_yaml
   mesh_yaml.write_mesh(path, verts, faces, uvs, normals=None, tangents=None)
   # smooth normals by default; for flat shading unweld first (see flat_shaded() in
   # gen_particle_demo_assets.py); pass mesh_yaml.perpendicular_tangents(normals) if any
   # normal can point along +-X
   ```
   Commit the generator alongside the mesh so it can be regenerated.
3. **Location**: shared -> `assets/meshes/<tag>/<name>.yaml` (tag folder such as `props`,
   `primitives`; references use only the name, see `assets/README.md` "Tags") and a row in `assets/README.md`'s Shared
   meshes table (shape, size, origin); scene-only -> `assets/scenes/<scene>/meshes/<name>.yaml`
   (same-named scene mesh shadows the shared one).
4. **Collision**: a separate `<name>_collider.yaml` (only `vertices`/`faces` are used) on a
   `MeshCollider { mesh_path: <name>_collider, convex: false }`; non-convex = static only.

## Verify

Put it in a scene with a textured material (e.g. `materials/prototype_grid` or `brick`) so UV
problems show, then:

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>
.claude/skills/create-scene/scripts/render_scene.sh <scene> debug_view=normals save_low_res=true
./build/toyengine_tests caml_roundtrips_every_asset
```

Look for: missing/see-through faces (winding), faceting where it should be smooth or vice
versa (normals/welding), stretched or untextured faces (UVs), z-fighting (duplicate faces).
Check other `debug_view` names in `assets/config.yaml`.
