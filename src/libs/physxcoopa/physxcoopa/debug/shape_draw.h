/**
 * @file shape_draw.h
 * @brief Wireframe of one collision::Shape at a world pose, into a DebugDraw -- the drawing
 *        PhysicsWorld::debug_draw() does for its live bodies, usable for any shape: e.g. an
 *        editor drawing collider COMPONENTS straight from their transforms, with or without a
 *        running simulation.
 */

#ifndef PHYSXCOOPA_DEBUG_SHAPE_DRAW_H
#define PHYSXCOOPA_DEBUG_SHAPE_DRAW_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <physxcoopa/collision/shape.h>
#include <physxcoopa/collision/sat_hull.h>
#include <physxcoopa/debug/debug_draw.h>

namespace coopa {
namespace physx {
namespace debug {

/**
 * @brief Appends `shape`'s wireframe, the shape belonging to a body at `position` / `orientation`
 *        (the shape's own local centre and rotation are applied on top, as for a live body).
 */
inline void add_shape(DebugDraw& out, const collision::Shape& shape, const glm::vec3& position,
                      const glm::quat& orientation, uint32_t color) {
    switch (shape.type) {
        case collision::ShapeType::Sphere:
            out.add_sphere(collision::world_sphere(shape, position, orientation), color);
            break;
        case collision::ShapeType::Box:
            out.add_obb(collision::world_obb(shape, position, orientation), color);
            break;
        case collision::ShapeType::Capsule:
            out.add_capsule(collision::world_capsule(shape, position, orientation), color);
            break;
        case collision::ShapeType::TriangleMesh: {
            if (!shape.mesh) break;
            glm::mat4 transform(glm::mat3_cast(orientation * shape.local_rotation) * shape.mesh_scale);
            transform[3] = glm::vec4(position + orientation * shape.local_center, 1.0f);
            out.add_mesh(*shape.mesh, transform, color);
            break;
        }
        case collision::ShapeType::ConvexHull: {
            // No add_hull() on DebugDraw -- a hull's edges (already deduplicated by
            // build_convex_hull()'s face merging) are just world-space line segments, so drawing
            // them directly is simpler than adding a new DebugDraw method for one caller.
            if (!shape.hull) break;
            const collision::PosedHull posed = collision::world_posed_hull(shape, position, orientation);
            for (const glm::uvec2& edge : posed.hull->edges)
                out.add_line(posed.world_vertex(edge.x), posed.world_vertex(edge.y), color);
            break;
        }
    }
}

} // namespace debug
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DEBUG_SHAPE_DRAW_H
