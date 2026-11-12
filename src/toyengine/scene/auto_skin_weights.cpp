#include "toyengine/scene/auto_skin_weights.h"

#include <physxcoopa/geometry/mesh_bvh.h>
#include <physxcoopa/geometry/ray.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace toy {
namespace scene {

namespace {

constexpr float k_huge = 1e30f;

/** @brief Nearest point on segment a->b to `p`, and the parameter-clamped distance. */
float segment_distance(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, glm::vec3& closest) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = (len2 > 1e-12f) ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    closest = a + ab * t;
    return glm::length(p - closest);
}

/** @brief distance_to_bone(), also reporting the length of the segment that won. */
float bone_distance(const BoneSegments& bone, const glm::vec3& p, glm::vec3& closest, float& segment_length) {
    float best = k_huge;
    closest = p;
    segment_length = 0.0f;
    for (const auto& [a, b] : bone.segments) {
        glm::vec3 c;
        const float d = segment_distance(p, a, b, c);
        if (d < best) {
            best = d;
            closest = c;
            segment_length = glm::length(b - a);
        }
    }
    return best;
}

/** @brief A mesh's longest bounding-box side -- the scale every epsilon here is a fraction of. */
float mesh_extent(const std::vector<glm::vec3>& positions) {
    if (positions.empty()) return 1.0f;
    glm::vec3 lo(k_huge), hi(-k_huge);
    for (const glm::vec3& p : positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    const glm::vec3 d = hi - lo;
    const float e = std::max(d.x, std::max(d.y, d.z));
    return (e > 1e-6f) ? e : 1.0f;
}

/**
 * @class SurfaceOccluder
 * @brief "Can this vertex see that point?", answered against the mesh's own surface.
 *
 * The ray starts a hair INSIDE the surface (along the negated vertex normal, Blender's trick)
 * so a vertex can never occlude itself on the faces it belongs to, and stops a hair short of
 * the target so a bone grazing the skin is not hidden by it either.
 */
class SurfaceOccluder {
public:
    SurfaceOccluder(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& triangles,
                    const std::vector<glm::vec3>& normals, float epsilon)
        : positions_(positions), triangles_(triangles), normals_(normals), epsilon_(epsilon) {
        bvh_.build(positions, triangles);
    }

    bool visible(uint32_t vertex, const glm::vec3& target) const {
        const glm::vec3 origin = positions_[vertex] - normals_[vertex] * epsilon_;
        glm::vec3 dir = target - origin;
        const float len = glm::length(dir);
        if (len <= epsilon_) return true;
        coopa::physx::geometry::Ray ray;
        ray.origin = origin;
        ray.direction = dir / len;
        ray.max_distance = len - epsilon_;
        if (ray.max_distance <= 0.0f) return true;
        bool blocked = false;
        bvh_.raycast(ray, [&](uint32_t tri) {
            if (blocked) return;
            const uint32_t i = tri * 3u;
            float t = 0.0f;
            if (coopa::physx::geometry::ray_vs_triangle(ray, positions_[triangles_[i]], positions_[triangles_[i + 1u]],
                                                        positions_[triangles_[i + 2u]], t)) {
                blocked = true;
            }
        });
        return !blocked;
    }

private:
    const std::vector<glm::vec3>& positions_;
    const std::vector<uint32_t>& triangles_;
    const std::vector<glm::vec3>& normals_;
    float epsilon_;
    coopa::physx::geometry::MeshBVH bvh_;
};

/** @brief Area-weighted vertex normals, outward for a consistently wound mesh. */
std::vector<glm::vec3> vertex_normals(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& triangles) {
    std::vector<glm::vec3> n(positions.size(), glm::vec3(0.0f));
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        const uint32_t i0 = triangles[t], i1 = triangles[t + 1], i2 = triangles[t + 2];
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) continue;
        const glm::vec3 c = glm::cross(positions[i1] - positions[i0], positions[i2] - positions[i0]);
        n[i0] += c; n[i1] += c; n[i2] += c;
    }
    for (glm::vec3& v : n) {
        const float l = glm::length(v);
        v = (l > 1e-12f) ? v / l : glm::vec3(0.0f);
    }
    return n;
}

/** @brief Undirected edges of a triangle soup, per vertex -- the blur's neighbourhood. */
std::vector<std::vector<uint32_t>> vertex_neighbors(size_t count, const std::vector<uint32_t>& triangles) {
    std::vector<std::vector<uint32_t>> out(count);
    auto link = [&](uint32_t a, uint32_t b) {
        if (a >= count || b >= count || a == b) return;
        if (std::find(out[a].begin(), out[a].end(), b) == out[a].end()) out[a].push_back(b);
    };
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        const uint32_t i0 = triangles[t], i1 = triangles[t + 1], i2 = triangles[t + 2];
        link(i0, i1); link(i1, i0); link(i1, i2); link(i2, i1); link(i2, i0); link(i0, i2);
    }
    return out;
}

/**
 * @struct Csr
 * @brief A symmetric sparse matrix in compressed-row form, with its diagonal kept separately
 *        for the Jacobi preconditioner.
 */
struct Csr {
    std::vector<uint32_t> row_begin;   /**< size n + 1 */
    std::vector<uint32_t> col;
    std::vector<float>    val;
    std::vector<float>    diag;
    size_t n = 0;

    void multiply(const std::vector<float>& x, std::vector<float>& out) const {
        out.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            double sum = static_cast<double>(diag[i]) * x[i];
            for (uint32_t k = row_begin[i]; k < row_begin[i + 1]; ++k) {
                sum += static_cast<double>(val[k]) * x[col[k]];
            }
            out[i] = static_cast<float>(sum);
        }
    }
};

double dot(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) s += static_cast<double>(a[i]) * b[i];
    return s;
}

/**
 * @brief Jacobi-preconditioned conjugate gradient for `A x = b`, starting from x = 0.
 * @return False if the relative residual never reached `tolerance` within `max_iterations`.
 */
bool solve_cg(const Csr& A, const std::vector<float>& b, std::vector<float>& x, int max_iterations, float tolerance) {
    const size_t n = A.n;
    x.assign(n, 0.0f);
    const double b_norm = std::sqrt(dot(b, b));
    if (!(b_norm > 0.0)) return true;   // an all-zero right-hand side is solved by x = 0

    std::vector<float> r = b, z(n), p(n), ap(n);
    for (size_t i = 0; i < n; ++i) z[i] = r[i] / A.diag[i];
    p = z;
    double rz = dot(r, z);
    const double target = tolerance * b_norm;

    for (int it = 0; it < max_iterations; ++it) {
        A.multiply(p, ap);
        const double p_ap = dot(p, ap);
        if (!(std::abs(p_ap) > 1e-30)) return false;
        const double alpha = rz / p_ap;
        for (size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(x[i] + alpha * p[i]);
            r[i] = static_cast<float>(r[i] - alpha * ap[i]);
        }
        if (std::sqrt(dot(r, r)) <= target) return true;
        for (size_t i = 0; i < n; ++i) z[i] = r[i] / A.diag[i];
        const double rz_next = dot(r, z);
        if (!(std::abs(rz) > 1e-30)) return false;
        const double beta = rz_next / rz;
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<float>(z[i] + beta * p[i]);
        rz = rz_next;
        if (!std::isfinite(rz)) return false;
    }
    return false;
}

/** @brief Blender's heat_limit_weight(): fade out below `limit`, gone at half of it. */
float limit_weight(float w, float limit) {
    const float end = limit * 0.5f;
    if (w < end) return 0.0f;
    if (w < limit) return (w - end) / (limit - end) * limit;
    return w;
}

} // namespace

float distance_to_bone(const BoneSegments& bone, const glm::vec3& p, glm::vec3* closest) {
    glm::vec3 c;
    float len = 0.0f;
    const float d = bone_distance(bone, p, c, len);
    if (closest) *closest = c;
    return d;
}

AutoWeights compute_auto_weights(const std::vector<glm::vec3>& positions,
                                 const std::vector<uint32_t>& triangles,
                                 const std::vector<BoneSegments>& bones,
                                 const AutoWeightSettings& settings) {
    AutoWeights out;
    out.weights.resize(positions.size());
    if (positions.empty() || bones.empty()) return out;
    for (const BoneSegments& b : bones) out.groups.push_back(b.name);

    const size_t n = positions.size();
    const size_t bone_count = bones.size();
    const float extent = mesh_extent(positions);

    // Distance from every vertex to every bone, plus the length of the segment that won (the
    // envelope's reach) -- both methods start here.
    std::vector<float> distance(n * bone_count, k_huge);
    std::vector<glm::vec3> nearest(n * bone_count);
    std::vector<float> bone_reach(bone_count, 0.0f);
    for (size_t b = 0; b < bone_count; ++b) {
        for (const auto& [head, tail] : bones[b].segments) bone_reach[b] = std::max(bone_reach[b], glm::length(tail - head));
        if (bone_reach[b] < 1e-6f) bone_reach[b] = extent * 0.1f;   // a bone with no length still reaches a little
        bone_reach[b] *= std::max(settings.envelope_falloff, 1e-3f);
        for (size_t v = 0; v < n; ++v) {
            glm::vec3 c;
            float seg_len = 0.0f;
            distance[v * bone_count + b] = bone_distance(bones[b], positions[v], c, seg_len);
            nearest[v * bone_count + b] = c;
        }
    }

    // A usable surface to conduct heat along: at least one triangle with area.
    double total_area = 0.0;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        if (triangles[t] >= n || triangles[t + 1] >= n || triangles[t + 2] >= n) continue;
        total_area += 0.5 * glm::length(glm::cross(positions[triangles[t + 1]] - positions[triangles[t]],
                                                   positions[triangles[t + 2]] - positions[triangles[t]]));
    }
    const bool heat = settings.method == AutoWeightMethod::Heat && total_area > 1e-12;
    out.fell_back_to_envelope = settings.method == AutoWeightMethod::Heat && !heat;

    std::vector<float> dense(n * bone_count, 0.0f);

    if (heat) {
        // --- heat sources: which bones a vertex is nearest to AND can see -------------------
        const std::vector<glm::vec3> normals = vertex_normals(positions, triangles);
        std::unique_ptr<SurfaceOccluder> occluder;
        if (settings.visibility) {
            occluder = std::make_unique<SurfaceOccluder>(positions, triangles, normals, extent * 1e-4f);
        }
        std::vector<float> heat_h(n, 0.0f), heat_p(n, 0.0f);
        std::vector<std::vector<int>> closest(n);
        for (size_t v = 0; v < n; ++v) {
            float mindist = k_huge;
            for (size_t b = 0; b < bone_count; ++b) mindist = std::min(mindist, distance[v * bone_count + b]);
            // Blender's tolerance: every bone within (1 + weight_limit) of the nearest one
            // shares the heat, so a vertex sitting on a joint is fed by both its bones.
            const float cutoff = mindist * (1.0f + settings.weight_limit);
            for (size_t b = 0; b < bone_count; ++b) {
                if (distance[v * bone_count + b] > cutoff) continue;
                if (occluder && !occluder->visible(static_cast<uint32_t>(v), nearest[v * bone_count + b])) continue;
                closest[v].push_back(static_cast<int>(b));
            }
            if (closest[v].empty()) continue;   // nearest bone hidden: no source here, only conduction
            const float d = std::max(mindist, 1e-4f * extent);
            heat_p[v] = 1.0f / static_cast<float>(closest[v].size());
            heat_h[v] = static_cast<float>(closest[v].size()) / (d * d);
        }

        // --- the cotangent stiffness matrix and the vertex areas -----------------------------
        std::vector<float> area(n, 0.0f);
        std::unordered_map<uint64_t, float> edge_weight;
        edge_weight.reserve(triangles.size());
        auto edge_key = [](uint32_t a, uint32_t b) {
            return (a < b) ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a;
        };
        for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
            const uint32_t i[3] = {triangles[t], triangles[t + 1], triangles[t + 2]};
            if (i[0] >= n || i[1] >= n || i[2] >= n) continue;
            const float a2 = glm::length(glm::cross(positions[i[1]] - positions[i[0]], positions[i[2]] - positions[i[0]]));
            if (a2 < 1e-14f) continue;
            const float tri_area = 0.5f * a2;
            for (int c = 0; c < 3; ++c) area[i[c]] += tri_area / 3.0f;
            // The weight of the edge opposite corner c is cot(angle at c) / 2, and
            // cot(angle) = dot(u, v) / |cross(u, v)| = dot(u, v) / (2 * area).
            for (int c = 0; c < 3; ++c) {
                const glm::vec3 u = positions[i[(c + 1) % 3]] - positions[i[c]];
                const glm::vec3 v = positions[i[(c + 2) % 3]] - positions[i[c]];
                edge_weight[edge_key(i[(c + 1) % 3], i[(c + 2) % 3])] += glm::dot(u, v) / (2.0f * a2);
            }
        }

        std::vector<std::vector<std::pair<uint32_t, float>>> rows(n);
        for (const auto& [key, w] : edge_weight) {
            // Clamped non-negative: see this file's header for why the solver needs it.
            if (w <= 0.0f) continue;
            const uint32_t a = static_cast<uint32_t>(key >> 32), b = static_cast<uint32_t>(key & 0xFFFFFFFFu);
            rows[a].push_back({b, w});
            rows[b].push_back({a, w});
        }

        Csr m;
        m.n = n;
        m.row_begin.resize(n + 1, 0);
        m.diag.assign(n, 0.0f);
        // The area-normalised Laplace-Beltrami row, multiplied back by its area so the matrix
        // is symmetric: (sum_j w_ij (x_i - x_j)) + area_i * H_i * x_i = area_i * H_i * p_i.
        for (size_t i = 0; i < n; ++i) {
            float diagonal = area[i] * heat_h[i];
            for (const auto& [j, w] : rows[i]) diagonal += w;
            // Insurance for a vertex no triangle and no source touches: keep the matrix
            // positive definite so CG cannot divide by zero on it.
            m.diag[i] = (diagonal > 0.0f) ? diagonal : 1.0f;
            m.row_begin[i + 1] = m.row_begin[i] + static_cast<uint32_t>(rows[i].size());
        }
        m.col.reserve(m.row_begin[n]);
        m.val.reserve(m.row_begin[n]);
        for (size_t i = 0; i < n; ++i) {
            for (const auto& [j, w] : rows[i]) { m.col.push_back(j); m.val.push_back(-w); }
        }

        // --- one solve per bone ---------------------------------------------------------------
        std::vector<float> rhs(n), solution;
        for (size_t b = 0; b < bone_count; ++b) {
            for (size_t v = 0; v < n; ++v) {
                const bool source = std::find(closest[v].begin(), closest[v].end(), static_cast<int>(b)) != closest[v].end();
                rhs[v] = source ? area[v] * heat_h[v] * heat_p[v] : 0.0f;
            }
            if (!solve_cg(m, rhs, solution, settings.cg_iterations, settings.cg_tolerance)) {
                out.unsolved.push_back(bones[b].name);
                continue;
            }
            for (size_t v = 0; v < n; ++v) dense[v * bone_count + b] = glm::clamp(solution[v], 0.0f, 1.0f);
        }
    } else {
        for (size_t v = 0; v < n; ++v) {
            for (size_t b = 0; b < bone_count; ++b) {
                const float t = 1.0f - distance[v * bone_count + b] / bone_reach[b];
                dense[v * bone_count + b] = (t > 0.0f) ? t * t : 0.0f;
            }
        }
    }

    // --- post pass: fade out the faintest, blur, keep the strongest few, normalise ------------
    for (float& w : dense) w = limit_weight(w, settings.weight_limit);

    if (settings.smooth_iterations > 0) {
        const std::vector<std::vector<uint32_t>> neighbors = vertex_neighbors(n, triangles);
        const float f = glm::clamp(settings.smooth_factor, 0.0f, 1.0f);
        std::vector<float> next(dense.size());
        for (int it = 0; it < settings.smooth_iterations; ++it) {
            for (size_t v = 0; v < n; ++v) {
                if (neighbors[v].empty()) {
                    for (size_t b = 0; b < bone_count; ++b) next[v * bone_count + b] = dense[v * bone_count + b];
                    continue;
                }
                for (size_t b = 0; b < bone_count; ++b) {
                    float sum = 0.0f;
                    for (uint32_t j : neighbors[v]) sum += dense[j * bone_count + b];
                    const float mean = sum / static_cast<float>(neighbors[v].size());
                    next[v * bone_count + b] = dense[v * bone_count + b] * (1.0f - f) + mean * f;
                }
            }
            dense.swap(next);
        }
    }

    const size_t keep = (settings.max_influences > 0) ? static_cast<size_t>(settings.max_influences) : bone_count;
    std::vector<std::pair<int, float>> scratch;
    for (size_t v = 0; v < n; ++v) {
        scratch.clear();
        for (size_t b = 0; b < bone_count; ++b) {
            const float w = dense[v * bone_count + b];
            if (w > 0.0f) scratch.push_back({static_cast<int>(b), w});
        }
        std::sort(scratch.begin(), scratch.end(), [](const auto& a, const auto& b) {
            return (a.second != b.second) ? a.second > b.second : a.first < b.first;
        });
        if (scratch.size() > keep) scratch.resize(keep);
        if (scratch.empty() && settings.fill_unweighted) {
            size_t best = 0;
            for (size_t b = 1; b < bone_count; ++b) {
                if (distance[v * bone_count + b] < distance[v * bone_count + best]) best = b;
            }
            scratch.push_back({static_cast<int>(best), 1.0f});
        }
        if (settings.normalize && !scratch.empty()) {
            float sum = 0.0f;
            for (const auto& [b, w] : scratch) sum += w;
            if (sum > 1e-12f) for (auto& [b, w] : scratch) w /= sum;
        }
        out.weights[v] = scratch;
    }
    return out;
}

} // namespace scene
} // namespace toy
