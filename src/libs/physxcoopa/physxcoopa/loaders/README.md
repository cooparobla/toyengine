# physxcoopa/loaders (`coopa::physx::loaders`)

`coopa::asset::AssetManager` integration for collision geometry that doesn't come from a
primitive shape.

| File | Purpose |
|---|---|
| [`triangle_mesh_loader.h`](triangle_mesh_loader.h) | `TriangleMeshLoader : TypedAssetLoader<TriangleMesh, TriangleMesh>` — decodes the same Blender-exported `vertices:`/`faces:` YAML gfxcoopa's `MeshLoader` reads. `decode_typed()` does the entire pipeline off-thread (no GPU step exists for physics): fan-triangulates each face polygon, welds vertices via a spatial hash (epsilon relative to the mesh's own bounds diagonal), drops triangles degenerate after welding, computes per-triangle face normals, and builds the edge-adjacency table (`(min_v,max_v) -> owning triangles`, flagging boundary vs non-manifold edges) `mesh_contact.h`'s internal-edge correction depends on. `finalize_typed()` returns its argument unchanged. |
| [`physics_material_loader.h`](physics_material_loader.h) | `PhysicsMaterialLoader : TypedAssetLoader<PhysicsMaterial, PhysicsMaterial>` — decodes `physics_materials/<name>.yaml` (`dynamic_friction`, `static_friction`, `restitution`, `friction_combine`, `restitution_combine`) into a shared `dynamics::PhysicsMaterial`. Unknown combine-mode names fall back to the default. |

## Usage Example

```cpp
assets.register_loader<coopa::physx::geometry::TriangleMesh>(
    std::make_unique<coopa::physx::loaders::TriangleMeshLoader>());
auto mesh = assets.load<coopa::physx::geometry::TriangleMesh>("meshes/terrain.yaml", ctx.scene_dir);
```
