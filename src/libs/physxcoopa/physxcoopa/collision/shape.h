/**
 * @file shape.h
 * @brief Collision geometry attached to a body, plus the world-space instancing helpers
 *        narrowphase and broadphase use to place that geometry given a body's pose.
 */

#ifndef PHYSXCOOPA_COLLISION_SHAPE_H
#define PHYSXCOOPA_COLLISION_SHAPE_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/sphere.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/dynamics/physics_material.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @brief Which primitive a Shape's fields describe.
 *
 * ConvexHull is SAT-only, same as Box -- no GJK, no EPA, no randomized convergence. See
 * geometry/convex_hull.h's file doc for why that is the deliberate line, not an oversight.
 */
enum class ShapeType {
    Sphere,
    Box,
    Capsule,
    TriangleMesh,
    ConvexHull,
};

/**
 * @struct Shape
 * @brief Final, world-scale collision geometry for one body, plus its material/trigger/layer.
 *
 * Dimensions here are already world-scale (a BoxCollider's authored `size` multiplied by the
 * owning SceneObject's world scale) -- PhysicsWorld itself has no notion of scale, only
 * position and orientation; that baking happens at bind time (PhysicsSystem's reconcile
 * pass, or update_changed_shapes_() when a collider changes), not per-substep.
 *
 * A body created with no explicit shape (`enabled == false`, the default) participates in
 * dynamics but never in collision -- matching Unity's "Rigidbody with no Collider" case: it
 * falls under gravity but hits nothing.
 */
struct Shape {
    bool enabled = false;
    ShapeType type = ShapeType::Sphere;

    /** @brief Local offset from the body's origin (Unity's collider `center`), pre-rotation:
     *         `local_center` is positioned by the BODY's own rotation alone (standard
     *         hierarchical-transform convention -- world_pos = pos + rot * local_center), not
     *         by local_rotation below. */
    glm::vec3 local_center{0.0f};

    /** @brief The shape's own orientation relative to the body, composed ON TOP of the body's
     *         rotation for anything that orients the shape itself (world_rot = rot *
     *         local_rotation) -- but never for local_center's placement above, which uses `rot`
     *         alone. Identity by default (Sphere, being rotation-invariant, never reads it at
     *         all). Lets a single non-compound collider's shape sit at an angle relative to its
     *         own Transform (e.g. a capsule authored diagonally), and is the primitive compound
     *         colliders need for each child shape's own local orientation. */
    glm::quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};

    // --- Sphere ---
    float radius = 0.5f;

    // --- Box ---
    glm::vec3 half_extents{0.5f};

    // --- Capsule --- (long axis is local Z; permute via `capsule_axis` for X/Y)
    float capsule_radius = 0.5f;
    float capsule_half_height = 0.5f; /**< Cylindrical half-length, excluding the end caps. */
    int capsule_axis = 2;             /**< 0=X, 1=Y, 2=Z -- default Z given the Z-up world. */

    // --- TriangleMesh ---
    const geometry::TriangleMesh* mesh = nullptr; /**< Non-owning; lifetime owned by the
                                                        MeshCollider's AssetHandle. */
    /** @brief Uniform scale applied to the mesh's local-space vertices (MeshCollider takes the
     *         max component of the owning object's world scale, matching Sphere/Capsule's
     *         rule -- a mesh authored for one scale doesn't stay proportionally correct under
     *         non-uniform scale without per-vertex scaling, which v1 doesn't support). */
    float mesh_scale = 1.0f;

    // --- ConvexHull ---
    /** @brief Non-owning; lifetime owned by ConvexCollider's `std::shared_ptr<const ConvexHull>`
     *         (a runtime-built hull has no asset file to hold an AssetHandle to, unlike
     *         TriangleMesh's `mesh` above). */
    const geometry::ConvexHull* hull = nullptr;
    float hull_scale = 1.0f; /**< Uniform scale, same convention as `mesh_scale`. */

    const dynamics::PhysicsMaterial* material = nullptr; /**< nullptr -> PhysicsMaterial::default_material(). */
    bool is_trigger = false;
    uint32_t layer = 0;

    /** @brief The effective material for contacts against this shape. */
    const dynamics::PhysicsMaterial& material_or_default() const {
        return material ? *material : dynamics::PhysicsMaterial::default_material();
    }

    static Shape make_sphere(float r, const glm::vec3& center = glm::vec3(0.0f)) {
        Shape s;
        s.enabled = true;
        s.type = ShapeType::Sphere;
        s.radius = r;
        s.local_center = center;
        return s;
    }

    static Shape make_box(const glm::vec3& half_ext, const glm::vec3& center = glm::vec3(0.0f)) {
        Shape s;
        s.enabled = true;
        s.type = ShapeType::Box;
        s.half_extents = half_ext;
        s.local_center = center;
        return s;
    }

    static Shape make_capsule(float r, float half_height, int axis = 2, const glm::vec3& center = glm::vec3(0.0f)) {
        Shape s;
        s.enabled = true;
        s.type = ShapeType::Capsule;
        s.capsule_radius = r;
        s.capsule_half_height = half_height;
        s.capsule_axis = axis;
        s.local_center = center;
        return s;
    }

    static Shape make_mesh(const geometry::TriangleMesh* m, const glm::vec3& center = glm::vec3(0.0f)) {
        Shape s;
        s.enabled = true;
        s.type = ShapeType::TriangleMesh;
        s.mesh = m;
        s.local_center = center;
        return s;
    }

    static Shape make_hull(const geometry::ConvexHull* h, float scale = 1.0f,
                            const glm::vec3& center = glm::vec3(0.0f)) {
        Shape s;
        s.enabled = true;
        s.type = ShapeType::ConvexHull;
        s.hull = h;
        s.hull_scale = scale;
        s.local_center = center;
        return s;
    }
};

/** @brief Instances a Sphere shape at a body's current world pose. */
inline geometry::Sphere world_sphere(const Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    geometry::Sphere out;
    out.center = pos + rot * s.local_center;
    out.radius = s.radius;
    return out;
}

/** @brief Instances a Box shape (as an OBB) at a body's current world pose. */
inline geometry::OBB world_obb(const Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    geometry::OBB out;
    out.center = pos + rot * s.local_center;
    out.half_extents = s.half_extents;
    out.orientation = rot * s.local_rotation;
    return out;
}

/** @brief Instances a Capsule shape at a body's current world pose. */
inline geometry::Capsule world_capsule(const Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    glm::vec3 axis_local(0.0f);
    axis_local[s.capsule_axis] = 1.0f;
    glm::vec3 axis_world = (rot * s.local_rotation) * axis_local;
    glm::vec3 center = pos + rot * s.local_center;

    geometry::Capsule out;
    out.a = center - axis_world * s.capsule_half_height;
    out.b = center + axis_world * s.capsule_half_height;
    out.radius = s.capsule_radius;
    return out;
}

/**
 * @brief World-space AABB enclosing a shape at a body's current pose, used by broadphase.
 *
 * @param s   Shape (must have `enabled == true`).
 * @param pos Body world position.
 * @param rot Body world orientation.
 * @return Tight world-space AABB.
 */
inline geometry::AABB world_bounds(const Shape& s, const glm::vec3& pos, const glm::quat& rot) {
    switch (s.type) {
        case ShapeType::Sphere: {
            geometry::Sphere sph = world_sphere(s, pos, rot);
            geometry::AABB out;
            out.min = sph.center - glm::vec3(sph.radius);
            out.max = sph.center + glm::vec3(sph.radius);
            return out;
        }
        case ShapeType::Box:
            return world_obb(s, pos, rot).bounds();
        case ShapeType::Capsule: {
            geometry::Capsule cap = world_capsule(s, pos, rot);
            geometry::AABB out;
            out.min = glm::min(cap.a, cap.b) - glm::vec3(cap.radius);
            out.max = glm::max(cap.a, cap.b) + glm::vec3(cap.radius);
            return out;
        }
        case ShapeType::TriangleMesh: {
            if (!s.mesh) return geometry::AABB{};
            glm::mat4 m(glm::mat3_cast(rot * s.local_rotation) * s.mesh_scale);
            m[3] = glm::vec4(pos + rot * s.local_center, 1.0f);
            return s.mesh->bounds().transform(m);
        }
        case ShapeType::ConvexHull: {
            if (!s.hull) return geometry::AABB{};
            return s.hull->bounds(pos + rot * s.local_center, rot * s.local_rotation, s.hull_scale);
        }
    }
    return geometry::AABB{};
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_SHAPE_H
