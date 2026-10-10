/**
 * @file visibility.h
 * @brief CPU visibility math for the mesh draw path: frustum planes, world-space bounds,
 *        projected size, and LOD selection.
 *
 * Pure functions over glm types -- no Vulkan, no scene -- so every piece is unit-tested in
 * test.cpp's device-free math group. ToyRenderPipeline runs them once per renderer per view
 * (camera, each shadow cascade, each view of every shadowed point/spot light) to decide what
 * each pass draws.
 *
 * Conventions match the rest of the renderer: right-handed view space looking down -Z, and
 * clip-space depth in [0, 1] (GLM_FORCE_DEPTH_ZERO_TO_ONE, libcoopa/CMakeLists.txt). The
 * negative-height viewport the main passes use flips Y after clip space, so it changes
 * nothing here.
 */

#ifndef TOYENGINE_RENDER_VISIBILITY_H
#define TOYENGINE_RENDER_VISIBILITY_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toy {
namespace render {

/// A world-space axis-aligned box as centre + half-extent -- the form both the frustum test
/// and the bounding-sphere radius want, so nothing converts back and forth per view.
struct WorldBounds {
    glm::vec3 center{0.0f};
    glm::vec3 extent{0.0f};

    /// Radius of the sphere enclosing the box.
    float radius() const { return glm::length(extent); }
};

/**
 * @brief Object-space AABB [lo, hi] under an affine `model`, as the tightest world AABB
 *        enclosing the transformed box (Arvo's method: |M| applied to the half-extent).
 *
 * Exact for every affine matrix -- rotation, non-uniform scale, shear -- and identical to
 * transforming all 8 corners and taking their min/max, at a fraction of the cost.
 */
inline WorldBounds world_aabb(const glm::mat4& model, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 c = 0.5f * (lo + hi);
    const glm::vec3 e = 0.5f * (hi - lo);
    WorldBounds out;
    out.center = glm::vec3(model * glm::vec4(c, 1.0f));
    for (int row = 0; row < 3; ++row) {
        out.extent[row] = std::abs(model[0][row]) * e.x +
                          std::abs(model[1][row]) * e.y +
                          std::abs(model[2][row]) * e.z;
    }
    return out;
}

/**
 * @brief The six clip planes of a view-projection matrix, normalized, pointing inward.
 *
 * Gribb-Hartmann extraction for a [0, 1] depth range: the near plane is row 2 alone (z >= 0)
 * rather than row 3 + row 2 (the GL [-1, 1] form). Works unchanged for perspective,
 * orthographic (shadow cascades) and the 90-degree cube-face projections.
 */
struct Frustum {
    glm::vec4 planes[6];   // xyz = inward normal, w = distance; inside when dot(n, p) + w >= 0

    static Frustum from_matrix(const glm::mat4& m) {
        // glm is column-major: row r of the matrix is (m[0][r], m[1][r], m[2][r], m[3][r]).
        auto row = [&m](int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
        const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        Frustum f;
        f.planes[0] = r3 + r0;   // left
        f.planes[1] = r3 - r0;   // right
        f.planes[2] = r3 + r1;   // bottom
        f.planes[3] = r3 - r1;   // top
        f.planes[4] = r2;        // near (z_clip >= 0)
        f.planes[5] = r3 - r2;   // far  (z_clip <= w)
        for (glm::vec4& p : f.planes) {
            const float len = glm::length(glm::vec3(p));
            if (len > 0.0f) p /= len;
        }
        return f;
    }

    /// True unless the box lies entirely outside at least one plane (the p-vertex test).
    /// Conservative: a box straddling a frustum corner outside every plane individually
    /// but not jointly reads as visible, which costs a draw and never drops one.
    bool intersects(const WorldBounds& b) const {
        for (const glm::vec4& p : planes) {
            const float r = b.extent.x * std::abs(p.x) + b.extent.y * std::abs(p.y) +
                            b.extent.z * std::abs(p.z);
            if (glm::dot(glm::vec3(p), b.center) + p.w < -r) return false;
        }
        return true;
    }
};

/**
 * @brief Projected height of a bounding sphere as a fraction of the viewport height.
 *
 * 1.0 = the sphere spans the full screen height. The LOD metric (independent of resolution,
 * and of FOV to first order) and the small-object shadow cull share it.
 *
 * @param center World-space sphere centre.
 * @param radius Sphere radius.
 * @param view   World -> view matrix (right-handed, looking down -Z).
 * @param proj   Projection. Perspective vs orthographic is read from proj[3][3]
 *               (1 for orthographic, 0 for perspective).
 */
inline float screen_height_fraction(const glm::vec3& center, float radius,
                                    const glm::mat4& view, const glm::mat4& proj) {
    const float p11 = std::abs(proj[1][1]);   // cot(fov/2), or 2/height for ortho
    if (proj[3][3] > 0.5f) {
        return radius * p11;   // orthographic: NDC half-height spans 1 -> fraction = r * p11
    }
    const float depth = -(view * glm::vec4(center, 1.0f)).z;
    // Inside or straddling the sphere: it covers the screen.
    if (depth <= radius) return 1.0e6f;
    return radius * p11 / depth;
}

/**
 * @brief Approximate diameter, in texels, of a bounding sphere drawn into a square shadow map.
 *
 * Works directly on a combined light view-projection (the only form the cascade matrices
 * exist in): the clip-space half-height a world radius maps to is radius * |row 1 of M|,
 * divided by clip w at the centre (1 for an orthographic cascade). The NDC diameter (2 *
 * that) spans `resolution` / 2 texels per unit. Returns a huge value when the sphere reaches
 * the projection centre (w <= radius), so a caster wrapped around a light is never culled.
 */
inline float projected_texels(const glm::mat4& view_proj, const glm::vec3& center, float radius,
                              float resolution) {
    const glm::vec3 row1(view_proj[0][1], view_proj[1][1], view_proj[2][1]);
    const glm::vec4 row3(view_proj[0][3], view_proj[1][3], view_proj[2][3], view_proj[3][3]);
    const float w = glm::dot(glm::vec3(row3), center) + row3.w;
    const float row3_len = glm::length(glm::vec3(row3));
    if (row3_len > 0.0f && w <= radius * row3_len) return 1.0e6f;   // perspective, too close
    return radius * glm::length(row1) / std::max(w, 1e-6f) * resolution;
}

/// One LOD level's switch threshold. Level 0 is always the full-detail mesh.
struct LodThreshold {
    float screen_size = 0.0f;   ///< Use this level while the object is at least this big.
};

/**
 * @brief Picks the LOD level for an object `size` (screen_height_fraction) big.
 *
 * Levels are ordered coarse-ward: level i is used while size >= thresholds[i] (level 0's
 * threshold is ignored -- it covers everything above level 1's). Returns -1 when size falls
 * below `cull_size` (0 disables culling).
 *
 * Hysteresis: moving to a COARSER level than `previous` requires size to drop
 * `hysteresis` (a fraction, e.g. 0.1) below that level's threshold, and moving finer
 * requires it to rise that far above -- so an object sitting on a threshold doesn't flicker
 * between two levels every frame. Pass previous = -2 for "no history".
 */
inline int select_lod(float size, const std::vector<LodThreshold>& thresholds, float cull_size,
                      int previous, float hysteresis) {
    const int n = static_cast<int>(thresholds.size());
    if (n <= 1) return (cull_size > 0.0f && size < cull_size) ? -1 : 0;

    auto pick = [&](float s) {
        if (cull_size > 0.0f && s < cull_size) return -1;
        int level = 0;
        for (int i = 1; i < n; ++i) {
            if (s < thresholds[i].screen_size) level = i;
        }
        return level;
    };

    const int raw = pick(size);
    if (previous < -1 || raw == previous) return raw;

    // Order levels by coarseness with "culled" (-1) coarsest of all.
    auto rank = [n](int level) { return level < 0 ? n : level; };

    // Re-pick as if the object were `hysteresis` further from the boundary than it is, and
    // move only as far as that pessimistic pick allows: a change sticks once the size has
    // cleared the threshold it crosses by the margin, while a jump across several levels
    // still moves as far as the margin permits.
    if (rank(raw) > rank(previous)) {
        const int cand = pick(size / (1.0f - hysteresis));   // as if bigger: resists coarsening
        return rank(cand) > rank(previous) ? cand : previous;
    }
    const int cand = pick(size / (1.0f + hysteresis));       // as if smaller: resists refining
    return rank(cand) < rank(previous) ? cand : previous;
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_VISIBILITY_H
