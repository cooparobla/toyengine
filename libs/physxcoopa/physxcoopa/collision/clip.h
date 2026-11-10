/**
 * @file clip.h
 * @brief Sutherland-Hodgman polygon clipping against a plane, plus OBB face/side-plane
 *        extraction -- the machinery box-box SAT uses to build a face-face manifold.
 */

#ifndef PHYSXCOOPA_COLLISION_CLIP_H
#define PHYSXCOOPA_COLLISION_CLIP_H

#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace coopa {
namespace physx {
namespace collision {

/** @brief A half-space: points `p` with `dot(normal, p) <= d` are kept by clip_polygon(). */
struct ClipPlane {
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float d = 0.0f;
};

/** @brief Maximum polygon size clip_polygon() can produce (an input quad can gain at most
 *         one vertex per clip plane, and box-box clips against 4 planes: 4+4=8, rounded up). */
inline constexpr int k_max_clip_vertices = 9;

/**
 * @brief Clips a polygon against one half-space (Sutherland-Hodgman), preserving winding.
 *
 * @param in       Input polygon vertices, in order.
 * @param in_count Number of input vertices.
 * @param plane    Half-space to clip against.
 * @param out      Output buffer, capacity >= k_max_clip_vertices.
 * @param out_count Output: number of vertices written to `out`.
 */
inline void clip_polygon(const glm::vec3* in, int in_count, const ClipPlane& plane,
                          glm::vec3* out, int& out_count) {
    out_count = 0;
    if (in_count == 0) return;

    for (int i = 0; i < in_count; ++i) {
        const glm::vec3& current = in[i];
        const glm::vec3& prev = in[(i - 1 + in_count) % in_count];

        float d_current = glm::dot(plane.normal, current) - plane.d;
        float d_prev = glm::dot(plane.normal, prev) - plane.d;

        bool current_inside = d_current <= 0.0f;
        bool prev_inside = d_prev <= 0.0f;

        if (current_inside != prev_inside) {
            // Edge crosses the plane -- insert the intersection point.
            float t = d_prev / (d_prev - d_current);
            if (out_count < k_max_clip_vertices) out[out_count++] = prev + t * (current - prev);
        }
        if (current_inside) {
            if (out_count < k_max_clip_vertices) out[out_count++] = current;
        }
    }
}

/**
 * @brief Returns the 4 world-space corners of one face of an OBB, plus its outward normal.
 *
 * @param box   Box to query.
 * @param axis  Which local axis the face is perpendicular to (0=X, 1=Y, 2=Z).
 * @param sign  +1 for the face on the positive side of that axis, -1 for the negative side.
 * @param corners Output: 4 corners, in a consistent (if not guaranteed CCW) winding order.
 * @param normal  Output: outward unit face normal.
 */
inline void obb_face(const geometry::OBB& box, int axis, float sign, glm::vec3 corners[4], glm::vec3& normal) {
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    glm::vec3 axes[3] = {ax, ay, az};
    glm::vec3 he = box.half_extents;

    const glm::vec3& main_axis = axes[axis];
    const glm::vec3& u = axes[(axis + 1) % 3];
    const glm::vec3& v = axes[(axis + 2) % 3];
    float hu = he[(axis + 1) % 3];
    float hv = he[(axis + 2) % 3];

    glm::vec3 face_center = box.center + main_axis * (sign * he[axis]);
    corners[0] = face_center - u * hu - v * hv;
    corners[1] = face_center + u * hu - v * hv;
    corners[2] = face_center + u * hu + v * hv;
    corners[3] = face_center - u * hu + v * hv;
    normal = main_axis * sign;
}

/**
 * @brief Returns the 4 side clip planes bounding one face of an OBB -- the planes the
 *        incident face gets clipped against to produce a face-face contact manifold.
 *
 * @param box   Reference box.
 * @param axis  The reference face's axis (0=X, 1=Y, 2=Z).
 * @param planes Output: exactly 4 planes.
 */
inline void obb_side_planes(const geometry::OBB& box, int axis, ClipPlane planes[4]) {
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    glm::vec3 axes[3] = {ax, ay, az};
    glm::vec3 he = box.half_extents;

    const glm::vec3& u = axes[(axis + 1) % 3];
    const glm::vec3& v = axes[(axis + 2) % 3];
    float hu = he[(axis + 1) % 3];
    float hv = he[(axis + 2) % 3];

    planes[0] = ClipPlane{u, glm::dot(u, box.center) + hu};
    planes[1] = ClipPlane{-u, glm::dot(-u, box.center) + hu};
    planes[2] = ClipPlane{v, glm::dot(v, box.center) + hv};
    planes[3] = ClipPlane{-v, glm::dot(-v, box.center) + hv};
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_CLIP_H
