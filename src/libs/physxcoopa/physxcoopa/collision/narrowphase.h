/**
 * @file narrowphase.h
 * @brief Per-shape-pair contact generation, dispatched by shape type.
 *
 * Dispatch table: Sphere-Sphere, Sphere-Box, Box-Box, Capsule pairs and TriangleMesh pairs.
 * Every pair has an exact analytic or SAT solution -- no GJK/EPA anywhere (see the plan's
 * "Design rationale").
 */

#ifndef PHYSXCOOPA_COLLISION_NARROWPHASE_H
#define PHYSXCOOPA_COLLISION_NARROWPHASE_H

#include <physxcoopa/collision/shape.h>
#include <physxcoopa/collision/manifold.h>
#include <physxcoopa/collision/sat.h>
#include <physxcoopa/collision/sat_hull.h>
#include <physxcoopa/collision/segment.h>
#include <physxcoopa/collision/mesh_contact.h>
#include <physxcoopa/dynamics/physics_material.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @brief Sphere-vs-sphere closest-point test.
 *
 * @param normal    Output: unit normal from A's center toward B's center.
 * @param penetration Output: overlap depth (positive when overlapping).
 * @param point     Output: contact point, on A's surface along `normal`.
 * @return True if the spheres overlap.
 */
inline bool sphere_vs_sphere(const geometry::Sphere& a, const geometry::Sphere& b,
                              glm::vec3& normal, float& penetration, glm::vec3& point) {
    glm::vec3 delta = b.center - a.center;
    float dist2 = glm::dot(delta, delta);
    float radius_sum = a.radius + b.radius;
    if (dist2 > radius_sum * radius_sum) return false;

    float dist = std::sqrt(dist2);
    normal = dist > util::k_epsilon ? delta / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    penetration = radius_sum - dist;
    point = a.center + normal * a.radius;
    return true;
}

/**
 * @brief Sphere-vs-box closest-point test, including the degenerate case where the sphere's
 *        center lies inside the box (handled via minimum-face-distance push-out, since the
 *        ordinary closest-point-on-surface distance is meaningless once the center is inside).
 *
 * @param normal      Output: unit normal from the box's surface toward the sphere's center.
 * @param penetration Output: overlap depth.
 * @param point       Output: contact point, on the box's surface.
 * @return True if the sphere overlaps the box.
 */
inline bool sphere_vs_box(const geometry::Sphere& sphere, const geometry::OBB& box,
                           glm::vec3& normal, float& penetration, glm::vec3& point) {
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    glm::vec3 d = sphere.center - box.center;
    glm::vec3 local(glm::dot(d, ax), glm::dot(d, ay), glm::dot(d, az));

    bool inside = std::abs(local.x) <= box.half_extents.x &&
                  std::abs(local.y) <= box.half_extents.y &&
                  std::abs(local.z) <= box.half_extents.z;

    glm::vec3 normal_local;
    glm::vec3 closest_local;

    if (!inside) {
        closest_local = glm::clamp(local, -box.half_extents, box.half_extents);
        glm::vec3 diff = local - closest_local;
        float dist = glm::length(diff);
        if (dist > sphere.radius) return false;
        penetration = sphere.radius - dist;
        normal_local = dist > util::k_epsilon ? diff / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    } else {
        glm::vec3 face_dist = box.half_extents - glm::abs(local);
        int axis = 0;
        if (face_dist.y < face_dist.x) axis = 1;
        if (face_dist.z < (axis == 0 ? face_dist.x : face_dist.y)) axis = 2;

        normal_local = glm::vec3(0.0f);
        normal_local[axis] = local[axis] >= 0.0f ? 1.0f : -1.0f;
        penetration = face_dist[axis] + sphere.radius;
        closest_local = local;
        closest_local[axis] = normal_local[axis] * box.half_extents[axis];
    }

    normal = ax * normal_local.x + ay * normal_local.y + az * normal_local.z;
    point = box.center + ax * closest_local.x + ay * closest_local.y + az * closest_local.z;
    return true;
}

/**
 * @brief Sphere-vs-sphere signed-separation query for the CCD speculative-contact path --
 *        companion to sphere_vs_sphere() above, which only ever reports an overlapping pair.
 *        Trivial for spheres (their surfaces ARE their distance field), unlike Box-Box's SAT
 *        extension in collision/sat.h.
 *
 * @return False if the spheres are already overlapping (that's sphere_vs_sphere()'s case) or
 *         degenerate (coincident centers).
 */
inline bool sphere_vs_sphere_separation(const geometry::Sphere& a, const geometry::Sphere& b,
                                         glm::vec3& normal, float& separation, glm::vec3& point) {
    glm::vec3 delta = b.center - a.center;
    float dist = glm::length(delta);
    float radius_sum = a.radius + b.radius;
    if (dist <= radius_sum || dist < util::k_epsilon) return false;
    normal = delta / dist;
    separation = dist - radius_sum;
    point = a.center + normal * a.radius;
    return true;
}

/**
 * @brief Sphere-vs-box signed-separation query for the CCD speculative-contact path --
 *        companion to sphere_vs_box() above. Only handles the sphere-center-outside-the-box
 *        case (sphere_vs_box()'s own `!inside` branch) -- the inside case always means
 *        overlapping already, never speculative.
 *
 * @return False if the sphere is already overlapping the box (inside, or within `radius` of the
 *         surface -- that's sphere_vs_box()'s case).
 */
inline bool sphere_vs_box_separation(const geometry::Sphere& sphere, const geometry::OBB& box,
                                      glm::vec3& normal, float& separation, glm::vec3& point) {
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    glm::vec3 d = sphere.center - box.center;
    glm::vec3 local(glm::dot(d, ax), glm::dot(d, ay), glm::dot(d, az));

    bool inside = std::abs(local.x) <= box.half_extents.x && std::abs(local.y) <= box.half_extents.y &&
                  std::abs(local.z) <= box.half_extents.z;
    if (inside) return false;

    glm::vec3 closest_local = glm::clamp(local, -box.half_extents, box.half_extents);
    glm::vec3 diff = local - closest_local;
    float dist = glm::length(diff);
    if (dist <= sphere.radius) return false;

    separation = dist - sphere.radius;
    glm::vec3 normal_local = diff / dist;
    normal = ax * normal_local.x + ay * normal_local.y + az * normal_local.z;
    point = box.center + ax * closest_local.x + ay * closest_local.y + az * closest_local.z;
    return true;
}

/**
 * @brief Generates (or refuses) a contact manifold for one pair of bodies, dispatching on
 *        shape type. `out` is fully overwritten; `out.a`/`out.b` are set even on failure.
 *
 * @param allow_speculative When true, a not-yet-touching-but-close pair still produces a
 *        SINGLE-point speculative manifold (ContactManifold::add_speculative_point(), see its
 *        doc) instead of returning false, for Sphere-Sphere, Sphere-Box, Box-Sphere, and Box-Box
 *        pairs -- the pairs cheap enough to support (Box-Box via collision/sat.h's separated-axis
 *        path; the sphere ones are near-trivial distance math on top of what
 *        sphere_vs_sphere()/sphere_vs_box() above already compute). Capsule-involving and
 *        TriangleMesh pairs ignore the flag -- meshes are static or kinematic and mesh CCD is
 *        deferred;
 *        capsule speculative support is a tractable follow-up (closest_points_segment_obb/
 *        closest_points_segment_segment in collision/segment.h are already distance-based).
 *        Defaults to false.
 *
 * @return True if the shapes overlap (or, with `allow_speculative`, are merely close and
 *         closing fast for a supported pair) and `out` now holds a valid manifold.
 */
inline bool generate_contacts(
    dynamics::BodyId id_a, const Shape& shape_a, const glm::vec3& pos_a, const glm::quat& rot_a,
    dynamics::BodyId id_b, const Shape& shape_b, const glm::vec3& pos_b, const glm::quat& rot_b,
    ContactManifold& out, bool allow_speculative = false) {

    out = ContactManifold{};
    out.a = id_a;
    out.b = id_b;

    bool hit = false;

    if (shape_a.type == ShapeType::Sphere && shape_b.type == ShapeType::Sphere) {
        geometry::Sphere A = world_sphere(shape_a, pos_a, rot_a);
        geometry::Sphere B = world_sphere(shape_b, pos_b, rot_b);
        glm::vec3 n, p;
        float pen;
        if (sphere_vs_sphere(A, B, n, pen, p)) {
            out.normal = n;
            out.add_point(p, pen, 0);
            hit = true;
        } else if (allow_speculative && sphere_vs_sphere_separation(A, B, n, pen, p)) {
            out.normal = n;
            out.add_speculative_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Sphere && shape_b.type == ShapeType::Box) {
        geometry::Sphere A = world_sphere(shape_a, pos_a, rot_a);
        geometry::OBB B = world_obb(shape_b, pos_b, rot_b);
        glm::vec3 n_box_to_sphere, p;
        float pen;
        if (sphere_vs_box(A, B, n_box_to_sphere, pen, p)) {
            out.normal = -n_box_to_sphere; // A(sphere) -> B(box) is the opposite of box->sphere
            out.add_point(p, pen, 0);
            hit = true;
        } else if (allow_speculative && sphere_vs_box_separation(A, B, n_box_to_sphere, pen, p)) {
            out.normal = -n_box_to_sphere;
            out.add_speculative_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Box && shape_b.type == ShapeType::Sphere) {
        geometry::OBB A = world_obb(shape_a, pos_a, rot_a);
        geometry::Sphere B = world_sphere(shape_b, pos_b, rot_b);
        glm::vec3 n_box_to_sphere, p;
        float pen;
        if (sphere_vs_box(B, A, n_box_to_sphere, pen, p)) {
            out.normal = n_box_to_sphere; // A(box) -> B(sphere) is box->sphere directly
            out.add_point(p, pen, 0);
            hit = true;
        } else if (allow_speculative && sphere_vs_box_separation(B, A, n_box_to_sphere, pen, p)) {
            out.normal = n_box_to_sphere;
            out.add_speculative_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Box && shape_b.type == ShapeType::Box) {
        geometry::OBB A = world_obb(shape_a, pos_a, rot_a);
        geometry::OBB B = world_obb(shape_b, pos_b, rot_b);
        hit = generate_box_box_contacts(A, B, out, allow_speculative); // fills normal + points; a/b set below
    } else if (shape_a.type == ShapeType::Capsule && shape_b.type == ShapeType::Capsule) {
        geometry::Capsule A = world_capsule(shape_a, pos_a, rot_a);
        geometry::Capsule B = world_capsule(shape_b, pos_b, rot_b);
        glm::vec3 n, p;
        float pen;
        if (capsule_vs_capsule(A, B, n, pen, p)) {
            out.normal = n;
            out.add_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Capsule && shape_b.type == ShapeType::Sphere) {
        geometry::Capsule A = world_capsule(shape_a, pos_a, rot_a);
        geometry::Sphere B = world_sphere(shape_b, pos_b, rot_b);
        glm::vec3 n, p;
        float pen;
        if (capsule_vs_sphere(A, B, n, pen, p)) {
            out.normal = n;
            out.add_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Sphere && shape_b.type == ShapeType::Capsule) {
        geometry::Sphere A = world_sphere(shape_a, pos_a, rot_a);
        geometry::Capsule B = world_capsule(shape_b, pos_b, rot_b);
        glm::vec3 n, p;
        float pen;
        if (capsule_vs_sphere(B, A, n, pen, p)) {
            out.normal = -n; // capsule_vs_sphere's normal points capsule->sphere = B->A here
            out.add_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Capsule && shape_b.type == ShapeType::Box) {
        geometry::Capsule A = world_capsule(shape_a, pos_a, rot_a);
        geometry::OBB B = world_obb(shape_b, pos_b, rot_b);
        hit = capsule_vs_box(A, B, out); // normal points box->capsule = B->A; flip below
        if (hit) out.normal = -out.normal;
    } else if (shape_a.type == ShapeType::Box && shape_b.type == ShapeType::Capsule) {
        geometry::OBB A = world_obb(shape_a, pos_a, rot_a);
        geometry::Capsule B = world_capsule(shape_b, pos_b, rot_b);
        hit = capsule_vs_box(B, A, out); // normal already points box(A)->capsule(B)
    } else if (shape_a.type == ShapeType::ConvexHull && shape_b.type == ShapeType::ConvexHull) {
        PosedHull A = world_posed_hull(shape_a, pos_a, rot_a);
        PosedHull B = world_posed_hull(shape_b, pos_b, rot_b);
        hit = generate_hull_hull_contacts(A, B, out);
    } else if (shape_a.type == ShapeType::ConvexHull && shape_b.type == ShapeType::Box) {
        PosedHull A = world_posed_hull(shape_a, pos_a, rot_a);
        geometry::OBB B = world_obb(shape_b, pos_b, rot_b);
        hit = generate_hull_box_contacts(A, B, out); // normal already points hull(a) -> box(b)
    } else if (shape_a.type == ShapeType::Box && shape_b.type == ShapeType::ConvexHull) {
        geometry::OBB A = world_obb(shape_a, pos_a, rot_a);
        PosedHull B = world_posed_hull(shape_b, pos_b, rot_b);
        hit = generate_hull_box_contacts(B, A, out); // generated as B(hull) -> A(box); flip to A -> B
        if (hit) out.normal = -out.normal;
    } else if (shape_a.type == ShapeType::ConvexHull && shape_b.type == ShapeType::Sphere) {
        PosedHull A = world_posed_hull(shape_a, pos_a, rot_a);
        geometry::Sphere B = world_sphere(shape_b, pos_b, rot_b);
        glm::vec3 n, p;
        float pen;
        if (hull_vs_sphere(A, B, n, pen, p)) {
            out.normal = n; // already hull(a) -> sphere(b)
            out.add_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::Sphere && shape_b.type == ShapeType::ConvexHull) {
        geometry::Sphere A = world_sphere(shape_a, pos_a, rot_a);
        PosedHull B = world_posed_hull(shape_b, pos_b, rot_b);
        glm::vec3 n, p;
        float pen;
        if (hull_vs_sphere(B, A, n, pen, p)) {
            out.normal = -n; // hull_vs_sphere's normal points hull(b) -> sphere(a); flip to a -> b
            out.add_point(p, pen, 0);
            hit = true;
        }
    } else if (shape_a.type == ShapeType::ConvexHull && shape_b.type == ShapeType::Capsule) {
        PosedHull A = world_posed_hull(shape_a, pos_a, rot_a);
        geometry::Capsule B = world_capsule(shape_b, pos_b, rot_b);
        hit = hull_vs_capsule(A, B, out); // already hull(a) -> capsule(b)
    } else if (shape_a.type == ShapeType::Capsule && shape_b.type == ShapeType::ConvexHull) {
        geometry::Capsule A = world_capsule(shape_a, pos_a, rot_a);
        PosedHull B = world_posed_hull(shape_b, pos_b, rot_b);
        hit = hull_vs_capsule(B, A, out);
        if (hit) out.normal = -out.normal; // hull(b) -> capsule(a); flip to a -> b
    } else if (shape_a.type != ShapeType::TriangleMesh && shape_b.type == ShapeType::TriangleMesh) {
        hit = generate_mesh_contacts(shape_a, pos_a, rot_a, shape_b, pos_b, rot_b, out);
        if (hit) out.normal = -out.normal; // generate_mesh_contacts's normal points mesh(b)->shape(a); flip to a->b
    } else if (shape_a.type == ShapeType::TriangleMesh && shape_b.type != ShapeType::TriangleMesh) {
        hit = generate_mesh_contacts(shape_b, pos_b, rot_b, shape_a, pos_a, rot_a, out); // normal points mesh(a)->shape(b) = a->b already
    } else {
        return false; // mesh-mesh: not supported
    }

    if (!hit) return false;
    out.a = id_a;
    out.b = id_b;

    const dynamics::PhysicsMaterial& mat_a = shape_a.material_or_default();
    const dynamics::PhysicsMaterial& mat_b = shape_b.material_or_default();
    out.friction = dynamics::combine(mat_a.dynamic_friction, mat_b.dynamic_friction,
                                      mat_a.friction_combine, mat_b.friction_combine);
    out.restitution = dynamics::combine(mat_a.restitution, mat_b.restitution,
                                         mat_a.restitution_combine, mat_b.restitution_combine);
    out.is_trigger = shape_a.is_trigger || shape_b.is_trigger;
    out.valid = true;
    return true;
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_NARROWPHASE_H
