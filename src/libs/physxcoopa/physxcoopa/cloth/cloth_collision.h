/**
 * @file cloth_collision.h
 * @brief Projects a cloth particle out of a rigid collision::Shape, reusing the engine's existing
 *        closest-point primitives rather than introducing a second geometry layer.
 *
 * Cloth collision is a one-sided point query, not a manifold: a particle is a point, so there are
 * no contact features to clip, no tangent basis to build and no impulses to warm start. That is
 * why this does NOT go through collision/narrowphase.h -- ContactManifold caps at four points
 * (collision/manifold.h), and a 25x25 sheet resting on a sphere produces hundreds of
 * simultaneous contacts that could not be expressed in that container even in principle.
 *
 * Every branch below delegates to a primitive that already exists and is already tested:
 *   Sphere        analytic centre distance
 *   Box           geometry::closest_point_on_obb        (geometry/obb.h)
 *   Capsule       geometry::closest_point_on_segment    (geometry/capsule.h)
 *   TriangleMesh  geometry::MeshBVH + collision::closest_point_on_triangle (collision/mesh_contact.h)
 *   ConvexHull    collision::hull_vs_sphere, treating the particle as a zero-mass probe sphere
 *                 of radius `thickness` (collision/sat_hull.h) -- same single-most-violated-face
 *                 approximation that function already documents, never swept (like TriangleMesh;
 *                 not among project_particle()'s two-pose Box/Capsule cases).
 */

#ifndef PHYSXCOOPA_CLOTH_CLOTH_COLLISION_H
#define PHYSXCOOPA_CLOTH_CLOTH_COLLISION_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

#include <physxcoopa/collision/mesh_contact.h>
#include <physxcoopa/collision/sat_hull.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/util/math.h>

namespace coopa {
namespace physx {
namespace cloth {

/**
 * @struct ClothCollider
 * @brief A snapshot of one rigid shape at the pose the cloth should collide against.
 *
 * A snapshot, rather than a (BodyId, shape index) pair resolved on demand, for two reasons: the
 * cloth solver runs several substeps against the same frozen rigid state, so re-resolving would
 * repeat the same lookup 4x per particle; and it keeps cloth_solver.h free of any dependency on
 * PhysicsWorld's storage layout, which is what lets the solver be unit-tested standalone.
 */
struct ClothCollider {
    const collision::Shape* shape = nullptr; /**< Non-owning; valid for the duration of one step. */
    glm::vec3 position{0.0f};                /**< Owning body's world position. */
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; /**< Owning body's world orientation. */

    /**
     * @brief World-space displacement the collider crosses during the step, beyond `position`.
     *
     * `position` is therefore the pose at the START of the step and `position + sweep` the pose
     * the frame draws. Every caller that knows its colliders moved must fill this, and both of
     * this engine's do: PhysicsWorld::step_cloths_() from each body's velocity, and toyengine's
     * ClothMeshSystem from the rig's bone poses at the previous solve.
     *
     * It is load-bearing twice over. The obvious one is tunnelling -- a shin travels several
     * centimetres per frame and a particle would otherwise pass straight through it. The less
     * obvious one is that solve_cloth() advances the pose across its SUBSTEPS from this, instead
     * of holding the shape at the far end of the displacement for all of them; without that, a
     * body moving 4 m/s arrives 6.7 cm inside the sheet on substep one and the positional
     * correction that removes it is read back as 16 m/s of cloth (see stage 7).
     *
     * Projection is exact for a sphere (a swept sphere IS a capsule); a box or capsule is probed
     * at both poses, END LAST (see project_particle()). A TriangleMesh is never swept.
     */
    glm::vec3 sweep{0.0f};

    /** @brief Orientation at the end of `sweep`, for shapes whose rotation changes their extent.
     *         Ignored for spheres (rotationally symmetric) and meshes (never swept). */
    glm::quat end_orientation{1.0f, 0.0f, 0.0f, 0.0f};

    /**
     * @brief The owning body's velocity, so a contact can be resolved in the surface's frame.
     *
     * Friction opposes RELATIVE sliding, and the inelastic normal clamp kills the velocity a
     * contact approaches at -- both of which are nonsense measured in the world frame against a
     * moving body. A sheet lying on a ball driven at 4 m/s has 4 m/s of world velocity and zero
     * relative to the ball; braking it toward world-zero is a 2.4 m/s-per-substep tangential
     * force scrubbing the fabric off the back of the ball, which is exactly what it did.
     *
     * Angular velocity is carried separately because the surface velocity at a contact is
     * `linear + cross(angular, p - position)` -- a spinning drum drags cloth around it even
     * though its centre never moves.
     */
    glm::vec3 linear_velocity{0.0f};
    glm::vec3 angular_velocity{0.0f}; /**< Radians/second about `position`. */

    /** @brief Surface velocity at world point `p`, for friction and the normal clamp. */
    glm::vec3 velocity_at(const glm::vec3& p) const {
        return linear_velocity + glm::cross(angular_velocity, p - position);
    }

    geometry::AABB bounds;                   /**< World AABB over BOTH poses, for the cheap reject. */

    /** @brief True if this collider moves far enough between cloth solves to be worth sweeping. */
    bool is_swept() const { return glm::dot(sweep, sweep) > util::k_epsilon * util::k_epsilon; }
};

namespace detail {

/**
 * @brief Pushes `p` out to `thickness` above a shape's surface at ONE pose, if it is closer.
 *
 * The per-pose worker behind project_particle(); see that function for how the two are combined
 * for a swept collider. `sweep` is consumed only by the Sphere branch, which can express its own
 * swept volume exactly (as a capsule) and therefore never needs a second pose.
 *
 * Handles the interior case explicitly for every primitive: a particle that has tunnelled inside
 * a shape must be pushed out along the shortest exit direction, not left where it is. The naive
 * "closest point minus particle" normal degenerates to zero exactly when the particle is on the
 * surface and points the WRONG WAY when it is inside, so each branch below derives its normal
 * from the shape's own geometry (centre-to-point for a sphere, axis-to-point for a capsule,
 * minimum-penetration face for a box, triangle normal for a mesh) rather than from the
 * difference vector.
 *
 * @param s            Shape to project out of.
 * @param pose_pos      Owning body's world position at this pose.
 * @param pose_rot      Owning body's world orientation at this pose.
 * @param sweep         Swept displacement (Sphere only); pass a zero vector for a static test.
 * @param motion_start  Particle position at the start of the substep (TriangleMesh only), or
 *                      nullptr when no motion is known.
 * @param thickness    Standoff distance kept between the particle and the surface, metres.
 * @param p            [in,out] Particle position; written only when a correction is applied.
 * @param normal_out   [out] Unit outward surface normal at the contact; untouched on no-hit.
 * @param depth_out    [out] Correction distance actually applied, metres; untouched on no-hit.
 * @return True if the particle was inside the thickened surface and has been moved.
 */
inline bool project_at_pose_(const collision::Shape& s, const glm::vec3& pose_pos,
                             const glm::quat& pose_rot, const glm::vec3& sweep,
                             float thickness, glm::vec3& p, glm::vec3& normal_out,
                             float& depth_out, const glm::vec3* motion_start) {
    const bool swept = glm::dot(sweep, sweep) > util::k_epsilon * util::k_epsilon;

    switch (s.type) {
        case collision::ShapeType::Sphere: {
            const geometry::Sphere sphere = collision::world_sphere(s, pose_pos, pose_rot);
            // A swept sphere IS a capsule, so this needs no new geometry -- the closest point on the
            // motion segment plays the role the centre plays when still, and a sphere's rotation
            // cannot change its extent, so no second orientation is needed either. When the sweep is
            // zero the segment degenerates to the centre and this is bit-identical to the static
            // case (closest_point_on_segment returns `a` for a zero-length segment).
            const glm::vec3 axis_a = sphere.center;
            const glm::vec3 axis_b = sphere.center + sweep;
            const glm::vec3 on_axis = swept
                ? geometry::closest_point_on_segment(p, axis_a, axis_b)
                : axis_a;
            glm::vec3 d = p - on_axis;
            const float len = glm::length(d);
            const float target = sphere.radius + thickness;
            if (len >= target) return false;
            // Dead centre: any direction is equally correct, so pick the world up axis rather
            // than dividing by zero.
            glm::vec3 n = (len > util::k_epsilon) ? d / len : glm::vec3(0.0f, 0.0f, 1.0f);
            depth_out = target - len;
            p = on_axis + n * target;
            normal_out = n;
            return true;
        }

        case collision::ShapeType::Capsule: {
            const geometry::Capsule cap = collision::world_capsule(s, pose_pos, pose_rot);
            const glm::vec3 on_axis = geometry::closest_point_on_segment(p, cap.a, cap.b);
            glm::vec3 d = p - on_axis;
            const float len = glm::length(d);
            const float target = cap.radius + thickness;
            if (len >= target) return false;
            glm::vec3 n = d;
            if (len > util::k_epsilon) {
                n = d / len;
            } else {
                // On the axis itself: push out perpendicular to the axis, in any consistent
                // direction (orthonormal_basis picks one deterministically).
                glm::vec3 axis = util::safe_normalize(cap.b - cap.a);
                glm::vec3 t1, t2;
                util::orthonormal_basis(axis, t1, t2);
                n = t1;
            }
            depth_out = target - len;
            p = on_axis + n * target;
            normal_out = n;
            return true;
        }

        case collision::ShapeType::Box: {
            const geometry::OBB box = collision::world_obb(s, pose_pos, pose_rot);
            glm::vec3 ax, ay, az;
            box.axes(ax, ay, az);
            const glm::vec3 d = p - box.center;
            const glm::vec3 local(glm::dot(d, ax), glm::dot(d, ay), glm::dot(d, az));
            const glm::vec3 he = box.half_extents;

            const bool inside = std::abs(local.x) <= he.x && std::abs(local.y) <= he.y &&
                                std::abs(local.z) <= he.z;
            glm::vec3 local_target;
            glm::vec3 local_normal;
            if (inside) {
                // Exit along the axis with the least remaining penetration -- the standard
                // minimum-translation choice, and the only one that does not drag a particle
                // across the width of the box.
                const glm::vec3 slack(he.x - std::abs(local.x), he.y - std::abs(local.y),
                                      he.z - std::abs(local.z));
                int axis = (slack.x <= slack.y && slack.x <= slack.z) ? 0 : (slack.y <= slack.z ? 1 : 2);
                const float sign = (local[axis] >= 0.0f) ? 1.0f : -1.0f;
                local_target = local;
                local_target[axis] = sign * (he[axis] + thickness);
                local_normal = glm::vec3(0.0f);
                local_normal[axis] = sign;
                depth_out = slack[axis] + thickness;
            } else {
                local_target = glm::clamp(local, -he, he);
                const glm::vec3 delta = local - local_target;
                const float len = glm::length(delta);
                // The tolerance is load-bearing, and it is about COST rather than correctness.
                // This branch's placement is written in world space (`box.center + ax*x + ...`)
                // and re-measured in box space (three dot products with the axes), and that round
                // trip is not exact: a particle left sitting at the standoff comes back as
                // `thickness * (1 - 3e-7)`, a bare `>=` calls it a hit, and the correction it then
                // applies is ~3e-9 m. Harmless on its own -- but solve_cloth()'s re-sweep breaks
                // out only when a pass corrects NOTHING, so every particle resting on a box
                // consumed all k_collider_passes passes, every substep, forever. A flag lying on
                // a box collider paid 4x the collision stage for 3 nanometres.
                //
                // Measured by cloth_collision/projection_is_idempotent_so_the_collider_re_sweep_
                // breaks_out, which is the only reason this was visible at all: it has no
                // behavioural tell, since the sheet is in exactly the right place either way.
                // The sphere and capsule branches are already exact here, their test being radial
                // against `radius + thickness` rather than a clamp-and-subtract.
                if (len >= thickness - util::k_epsilon) return false;
                local_normal = (len > util::k_epsilon) ? delta / len : glm::vec3(0.0f, 0.0f, 1.0f);
                depth_out = thickness - len;
                local_target = local_target + local_normal * thickness;
            }

            p = box.center + ax * local_target.x + ay * local_target.y + az * local_target.z;
            normal_out = glm::normalize(ax * local_normal.x + ay * local_normal.y + az * local_normal.z);
            return true;
        }

        case collision::ShapeType::TriangleMesh: {
            if (!s.mesh || s.mesh->empty()) return false;

            // Work in the mesh's own local frame: one inverse transform of the query point beats
            // transforming every candidate triangle, and the BVH is built in local space anyway.
            const glm::quat rot = pose_rot * s.local_rotation;
            const glm::quat inv_rot = glm::inverse(rot);
            const glm::vec3 mesh_origin = pose_pos + pose_rot * s.local_center;
            const float scale = (s.mesh_scale > util::k_epsilon) ? s.mesh_scale : 1.0f;
            const glm::vec3 local_p = (inv_rot * (p - mesh_origin)) / scale;
            const float local_thickness = thickness / scale;

            // --- Swept crossing test -------------------------------------------------------
            // A triangle soup has no cheap "inside" to test against, so unlike the sphere, box and
            // capsule branches this one cannot recover a particle that has ended up deep on the
            // wrong side: it would be further than `thickness` from every triangle and the
            // proximity query below would find nothing at all. The fix is to never let it get
            // there -- catch the crossing itself, which is unambiguous and needs no watertight
            // mesh (ray-parity inside tests do, and game meshes routinely are not).
            //
            // The length gate is what makes this affordable. A particle being held `thickness`
            // clear of a surface cannot cross it without moving further than `thickness` in one
            // substep, and a settled sheet moves microns per substep -- so the raycast never runs
            // on resting cloth, only on particles genuinely fast enough to tunnel. Ungated this
            // would be a BVH traversal per particle per substep (~2500 per frame for a 25x25
            // sheet) for a case that almost never happens.
            if (motion_start) {
                const glm::vec3 local_start = (inv_rot * (*motion_start - mesh_origin)) / scale;
                const glm::vec3 seg = local_p - local_start;
                const float seg_len = glm::length(seg);
                if (seg_len > local_thickness) {
                    geometry::Ray ray;
                    ray.origin = local_start;
                    ray.direction = seg / seg_len;
                    ray.max_distance = seg_len;

                    float best_t = seg_len;
                    uint32_t crossed_tri = 0;
                    bool crossed = false;
                    s.mesh->raycast(ray, [&](uint32_t tri) {
                        glm::vec3 v0, v1, v2;
                        s.mesh->triangle_vertices(tri, v0, v1, v2);
                        float t = 0.0f;
                        // ray_vs_triangle is single-sided (it only hits the face the winding normal
                        // points toward), so the reversed winding is what catches a particle
                        // arriving from the back. A collision surface is not a one-way gate.
                        if ((geometry::ray_vs_triangle(ray, v0, v1, v2, t) ||
                             geometry::ray_vs_triangle(ray, v0, v2, v1, t)) && t < best_t) {
                            best_t = t;
                            crossed_tri = tri;
                            crossed = true;
                        }
                    });

                    if (crossed) {
                        const glm::vec3 local_hit = ray.origin + ray.direction * best_t;
                        // Which side is "out" is READ FROM THE MOTION, not guessed from a normal --
                        // so this carries none of the internal-edge ambiguity that forces
                        // mesh_contact.h to consult TriangleAdjacency for rigid contacts.
                        glm::vec3 face = util::safe_normalize(s.mesh->triangle_normal(crossed_tri));
                        if (glm::dot(local_start - local_hit, face) < 0.0f) face = -face;

                        const glm::vec3 local_fixed = local_hit + face * local_thickness;
                        depth_out = glm::length(local_p - local_fixed) * scale;
                        p = mesh_origin + rot * (local_fixed * scale);
                        normal_out = rot * face;
                        return true;
                    }
                }
            }

            // --- Proximity test ------------------------------------------------------------
            geometry::AABB query;
            query.min = local_p - glm::vec3(local_thickness);
            query.max = local_p + glm::vec3(local_thickness);

            float best_d2 = local_thickness * local_thickness;
            // A particle already held AT the standoff must come back as "nothing to correct", or
            // solve_cloth()'s re-sweep never breaks out of its k_collider_passes loop -- the same
            // cost-only bug the Box branch's rejection tolerance documents, and this is the branch
            // heightmap terrain uses, so a sheet resting on terrain was the worst case for it.
            // Compared against squared distances, hence the shrunk radius rather than an epsilon
            // on the comparison.
            const float contact_limit = std::max(0.0f, local_thickness - util::k_epsilon);
            const float contact_d2 = contact_limit * contact_limit;
            glm::vec3 best_point(0.0f);
            glm::vec3 best_delta(0.0f);
            uint32_t best_tri = 0;
            bool hit = false;
            // Whether the particle is on the FRONT side of at least one triangle it is in contact
            // range of. Behind every one of them means it has genuinely penetrated the surface;
            // behind some but in front of others means it is sitting in a concave pocket and is
            // perfectly fine where it is. Deciding from the single nearest triangle instead would
            // teleport a particle in an inside corner straight through the wall.
            bool any_front = false;

            s.mesh->query(query, [&](uint32_t tri) {
                glm::vec3 v0, v1, v2;
                s.mesh->triangle_vertices(tri, v0, v1, v2);
                const glm::vec3 closest = collision::closest_point_on_triangle(local_p, v0, v1, v2);
                const glm::vec3 delta = local_p - closest;
                const float d2 = glm::dot(delta, delta);
                if (d2 > local_thickness * local_thickness) return; // bounds overlapped, surface didn't

                // >= 0 so a particle lying exactly ON the surface counts as outside, leaving the
                // existing behaviour for that case untouched.
                if (glm::dot(delta, s.mesh->triangle_normal(tri)) >= 0.0f) any_front = true;

                // `>= contact_d2` is the idempotence guard above; `> best_d2` keeps the nearest.
                if (d2 >= contact_d2 || d2 > best_d2) return;
                best_d2 = d2;
                best_point = closest;
                best_delta = delta;
                best_tri = tri;
                hit = true;
            });

            if (!hit) return false;

            glm::vec3 best_normal;
            if (!any_front) {
                // Penetrating: exit through the nearest triangle's FRONT face. Deriving the
                // direction from (particle - closest point) here -- which is what an outside
                // particle uses -- would point back into the solid and cement the penetration at
                // exactly `thickness` on the wrong side.
                best_normal = util::safe_normalize(s.mesh->triangle_normal(best_tri));
            } else if (best_d2 > util::k_epsilon * util::k_epsilon) {
                // The separation direction itself, which -- unlike the face normal -- is also the
                // right exit direction in the triangle's edge and vertex regions.
                best_normal = best_delta / std::sqrt(best_d2);
            } else {
                // Exactly on the surface: the separation direction is undefined, so fall back to
                // the face normal, signed to push the particle off the side it is on.
                const glm::vec3 face = util::safe_normalize(s.mesh->triangle_normal(best_tri));
                best_normal = (glm::dot(best_delta, face) < 0.0f) ? -face : face;
            }

            const glm::vec3 local_fixed = best_point + best_normal * local_thickness;
            // The distance actually travelled. Identical to (thickness - distance) for an outside
            // particle, and correct for a penetrating one, where the correction spans the surface.
            depth_out = glm::length(local_p - local_fixed) * scale;
            p = mesh_origin + rot * (local_fixed * scale);
            normal_out = rot * best_normal;
            return true;
        }

        case collision::ShapeType::ConvexHull: {
            if (!s.hull) return false;
            const collision::PosedHull posed = collision::world_posed_hull(s, pose_pos, pose_rot);
            const geometry::Sphere probe{p, thickness};
            glm::vec3 point;
            float pen = 0.0f;
            if (!collision::hull_vs_sphere(posed, probe, normal_out, pen, point)) return false;
            // A probe sphere exactly TANGENT to the hull is a touch, not a penetration, and
            // hull_vs_sphere() reports it as a hit with `pen` at zero. Returning true for it would
            // stop solve_cloth()'s re-sweep from ever breaking out -- the same cost-only trap the
            // Box and TriangleMesh branches carry a tolerance for. Cheaper to express here than
            // there, since this branch is handed the penetration directly.
            if (pen <= util::k_epsilon) return false;
            // Same convention as every other branch here: final position = (point on the actual
            // shape's surface) + normal * thickness.
            p = point + normal_out * thickness;
            depth_out = pen;
            return true;
        }
    }
    return false;
}

} // namespace detail

/**
 * @brief Pushes `p` out of a collider -- including the volume it sweeps before the cloth is next
 *        solved -- to `thickness` above its surface, if it is closer than that.
 *
 * Dispatch:
 *   - **Sphere**, and anything not moving: one call to project_at_pose_(). A swept sphere is a
 *     capsule, which that function models exactly, so nothing extra is needed.
 *   - **Box / Capsule while moving**: projected against the start pose and then against the end
 *     pose, in that order. The exact swept volume of a rotating box is a hull no closest-point
 *     routine here can express, and two samples bound it closely enough for a sub-frame
 *     displacement -- a sheet does not resolve the difference.
 *
 *     **The end pose must be projected LAST, not merely preferred when it is deeper.** The end
 *     pose is the one the frame draws, so it is the only one whose surface the result has to be
 *     outside of; resolving a deeper start-pose overlap and stopping there leaves the particle
 *     clear of where a limb WAS and inside where it now is. On a fast swinging limb that reads as
 *     cloth intermittently sinking into the leg, only on the frames of peak speed. Chaining the
 *     two instead keeps the swept volume's push (the start pose moves the particle ahead of the
 *     limb) while making the drawn pose authoritative.
 *   - **TriangleMesh**: never swept (non-convex mesh colliders are static-only in this engine), so
 *     it falls through the single-call path with whatever sweep it was given ignored.
 *
 * @param collider     Shape snapshot to project out of, including its sweep.
 * @param thickness    Standoff distance kept between the particle and the surface, metres.
 * @param motion_start Particle position at the start of this substep, enabling the swept crossing
 *                     test against a TriangleMesh (the one shape with no interior case of its own
 *                     -- see project_at_pose_'s TriangleMesh branch). nullptr disables it, which is
 *                     what a caller with no motion information should pass.
 * @param p          [in,out] Particle position; written only when a correction is applied.
 * @param normal_out [out] Unit outward surface normal at the contact; untouched on no-hit.
 * @param depth_out  [out] Correction distance actually applied, metres; untouched on no-hit.
 * @return True if the particle was inside the thickened (swept) surface and has been moved.
 */
inline bool project_particle(const ClothCollider& collider, float thickness,
                             glm::vec3& p, glm::vec3& normal_out, float& depth_out,
                             const glm::vec3* motion_start = nullptr) {
    if (!collider.shape || !collider.shape->enabled) return false;
    const collision::Shape& s = *collider.shape;

    const bool two_pose = collider.is_swept() &&
                          (s.type == collision::ShapeType::Box ||
                           s.type == collision::ShapeType::Capsule);
    if (!two_pose) {
        return detail::project_at_pose_(s, collider.position, collider.orientation, collider.sweep,
                                         thickness, p, normal_out, depth_out, motion_start);
    }

    glm::vec3 moved = p, n_start(0.0f);
    float d_start = 0.0f;
    const bool hit_start = detail::project_at_pose_(s, collider.position, collider.orientation,
                                                    glm::vec3(0.0f), thickness, moved, n_start,
                                                    d_start, motion_start);

    glm::vec3 n_end(0.0f);
    float d_end = 0.0f;
    const bool hit_end = detail::project_at_pose_(s, collider.position + collider.sweep,
                                                  collider.end_orientation, glm::vec3(0.0f),
                                                  thickness, moved, n_end, d_end, motion_start);

    if (!hit_start && !hit_end) return false;
    p = moved;
    // The end pose's contact describes the surface the particle now rests on, so friction and
    // the caller's barycentric redistribution follow it whenever it applied.
    normal_out = hit_end ? n_end : n_start;
    depth_out = hit_end ? d_end : d_start;
    return true;
}

} // namespace cloth
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_CLOTH_CLOTH_COLLISION_H
