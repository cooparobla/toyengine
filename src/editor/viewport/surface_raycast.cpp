#include "editor/viewport/surface_raycast.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "editor/viewport/gizmo.h"

namespace toy::editor {

SurfaceRaycaster::SurfaceRaycaster(glm::vec3 origin, glm::vec3 dir, float max_t)
    : origin_(origin), max_t_(max_t) {
    const float len = glm::length(dir);
    dir_ = len > 1e-6f ? dir / len : glm::vec3(0.0f, 0.0f, -1.0f);
}

void SurfaceRaycaster::take_(const SurfaceHit& hit) {
    if (hit.t < 0.0f || hit.t > max_t_) return;
    if (best_ && best_->t <= hit.t) return;
    best_ = hit;
    // Always face the viewer: a two-sided triangle test reports the winding normal, and half
    // the time that points away -- which would plant everything upside down on a back face.
    if (glm::dot(best_->normal, dir_) > 0.0f) best_->normal = -best_->normal;
    const float len = glm::length(best_->normal);
    best_->normal = len > 1e-6f ? best_->normal / len : glm::vec3(0.0f, 0.0f, 1.0f);
}

void SurfaceRaycaster::consider_mesh(ObjectId id, const EditMesh& mesh, const TriangleBVH& bvh,
                                     const glm::mat4& world) {
    if (bvh.empty()) return;
    const float det = glm::determinant(world);
    if (std::abs(det) < 1e-12f) return;   // a degenerate (zero-scaled) object has no surface
    const glm::mat4 inv = glm::inverse(world);
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(origin_, 1.0f));
    const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir_, 0.0f));
    const auto hit = bvh.raycast(mesh, lo, ld);
    if (!hit) return;
    SurfaceHit out;
    out.point = glm::vec3(world * glm::vec4(hit->p, 1.0f));
    // The inverse transpose, so a non-uniform scale does not shear the normal off the surface.
    out.normal = glm::vec3(glm::transpose(inv) * glm::vec4(hit->normal, 0.0f));
    out.object = id;
    out.t = glm::dot(out.point - origin_, dir_);
    take_(out);
}

void SurfaceRaycaster::consider_terrain(ObjectId id, const toy::world::TerrainFieldView& view, float step) {
    if (!view) return;
    const float march = std::max(step, 0.05f);
    // Only ever downward-ish rays matter here; a ray running level along the ground would need
    // a DDA over the grid to answer honestly, and no brush casts one.
    auto below = [&](const glm::vec3& p) {
        return view.contains(p) ? p.z - view.height(p) : std::numeric_limits<float>::max();
    };

    float t0 = 0.0f;
    float d0 = below(origin_);
    if (d0 <= 0.0f && d0 != std::numeric_limits<float>::max()) {
        // The ray starts underground: there is no downward crossing to find.
        return;
    }
    const float limit = std::min(max_t_, 1e5f);
    for (float t = march; t <= limit; t += march) {
        const glm::vec3 p = origin_ + dir_ * t;
        const float d = below(p);
        if (d == std::numeric_limits<float>::max()) {
            // Outside the terrain's footprint: keep marching, it may come back over it.
            t0 = t;
            d0 = d;
            continue;
        }
        if (d0 == std::numeric_limits<float>::max() || d0 <= 0.0f) {
            t0 = t;
            d0 = d;
            continue;
        }
        if (d > 0.0f) {
            t0 = t;
            d0 = d;
            continue;
        }
        // Crossed between t0 (above) and t (below): bisect to well under a millimetre.
        float lo = t0, hi = t;
        for (int i = 0; i < 24; ++i) {
            const float mid = 0.5f * (lo + hi);
            const float dm = below(origin_ + dir_ * mid);
            if (dm == std::numeric_limits<float>::max() || dm > 0.0f) lo = mid;
            else hi = mid;
        }
        const glm::vec3 p_hit = origin_ + dir_ * hi;
        SurfaceHit out;
        out.point = glm::vec3(glm::vec2(p_hit), view.height(p_hit));
        out.normal = view.normal(p_hit);
        out.object = id;
        out.t = hi;
        take_(out);
        return;
    }
}

void SurfaceRaycaster::consider_point(ObjectId id, const glm::vec3& point, const glm::vec3& normal) {
    SurfaceHit out;
    out.point = point;
    out.normal = normal;
    out.object = id;
    out.t = glm::dot(point - origin_, dir_);
    take_(out);
}

std::optional<glm::vec3> ray_plane(glm::vec3 origin, glm::vec3 dir, float plane_z) {
    if (std::abs(dir.z) < 1e-6f) return std::nullopt;
    const float t = (plane_z - origin.z) / dir.z;
    if (t < 0.0f) return std::nullopt;
    return origin + dir * t;
}

} // namespace toy::editor
