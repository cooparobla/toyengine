# physxcoopa/debug (`coopa::physx::debug`)

Pure-data debug visualization. No Vulkan, no gfxcoopa dependency — `PhysicsWorld::debug_draw()`
(`world.h`) fills a `DebugDraw` with plain line segments; the consuming engine renders them
however it likes.

| File | Purpose |
|---|---|
| [`debug_draw.h`](debug_draw.h) | `DebugLine`, `DebugDrawFlags` (`Colliders`/`BVH`/`Contacts`/`Joints`/`All`), `DebugDraw` (`add_aabb()`/`add_obb()`/`add_sphere()`/`add_capsule()`/`add_mesh()` wireframe builders). Colored by state: white awake, green sleeping, yellow trigger (overrides sleep color), red contact normal, magenta joint anchors/axes, blue BVH nodes. |

## Usage Example

```cpp
coopa::physx::debug::DebugDraw draw;
world.debug_draw(draw, coopa::physx::debug::DebugDrawFlags::Colliders | coopa::physx::debug::DebugDrawFlags::Contacts);
for (const auto& line : draw.lines) {
    my_line_renderer.draw(line.a, line.b, line.color);
}
```
