#include <toyengine/destruct/fracture.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace toy {
namespace destruct {

namespace {

/// Distance under which two cut-loop endpoints are the same point. Relative to the mesh, so a
/// 2 cm prop and a 20 m statue both weld their loops at the same proportion of their size.
constexpr float k_weld_fraction = 1e-4f;

/// A corner's signed distance from the plane; negative is kept.
float plane_distance(const glm::vec3& p, const glm::vec3& n, float d) { return glm::dot(n, p) - d; }

/** @brief The vertex where the segment a->b crosses the plane, every attribute interpolated. */
Vertex cross_at(const Vertex& a, const Vertex& b, float da, float db) {
    const float t = std::clamp(da / (da - db), 0.0f, 1.0f);
    Vertex v;
    v.position = glm::mix(a.position, b.position, t);
    v.normal   = glm::normalize(glm::mix(a.normal, b.normal, t));
    v.uv       = glm::mix(a.uv, b.uv, t);
    v.tangent  = glm::mix(a.tangent, b.tangent, t);
    return v;
}

/** @brief Two axes spanning the plane with normal `n`, for laying out cap UVs. */
void plane_basis(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 up = std::abs(n.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    u = glm::normalize(glm::cross(up, n));
    v = glm::cross(n, u);
}

/// Quantised position, so the two ends of a shared cut edge hash to one key.
struct GridKey {
    std::int64_t x, y, z;
    bool operator==(const GridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct GridKeyHash {
    std::size_t operator()(const GridKey& k) const {
        return static_cast<std::size_t>(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
    }
};
GridKey key_of(const glm::vec3& p, float weld) {
    const float inv = 1.0f / std::max(weld, 1e-9f);
    return GridKey{static_cast<std::int64_t>(std::llround(p.x * inv)),
                   static_cast<std::int64_t>(std::llround(p.y * inv)),
                   static_cast<std::int64_t>(std::llround(p.z * inv))};
}

/**
 * @brief Caps the opening a clip left: chains the cut segments into loops and fans each one.
 *
 * The segments come out of the clip in no particular order, so they are chained by their
 * endpoints (welded on a grid, since the same corner is computed independently by each of the
 * two triangles that share the cut edge and the two results differ in the last bit). A loop
 * that will not close -- a stray segment from a degenerate triangle, a mesh that was not solid
 * where it was cut -- is dropped rather than guessed at.
 */
void cap_opening(const std::vector<std::pair<Vertex, Vertex>>& segments, const glm::vec3& normal,
                 float weld, std::vector<Vertex>& out_vertices, std::vector<std::uint32_t>& out_indices) {
    if (segments.size() < 3) return;

    // Endpoint -> the segments touching it. A closed cut has exactly two per point.
    std::unordered_map<GridKey, std::vector<std::size_t>, GridKeyHash> at_point;
    at_point.reserve(segments.size() * 2);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        at_point[key_of(segments[i].first.position, weld)].push_back(i);
        at_point[key_of(segments[i].second.position, weld)].push_back(i);
    }

    std::vector<std::uint8_t> used(segments.size(), 0);
    glm::vec3 u, v;
    plane_basis(normal, u, v);

    for (std::size_t start = 0; start < segments.size(); ++start) {
        if (used[start]) continue;
        std::vector<Vertex> loop;
        std::size_t current = start;
        Vertex from = segments[start].first;
        Vertex to   = segments[start].second;
        used[start] = 1;
        loop.push_back(from);
        const GridKey closing = key_of(from.position, weld);

        // Walk segment to segment until we arrive back where we started.
        for (std::size_t guard = 0; guard < segments.size(); ++guard) {
            loop.push_back(to);
            const GridKey here = key_of(to.position, weld);
            if (here == closing) break;
            std::size_t next = segments.size();
            const auto it = at_point.find(here);
            if (it != at_point.end()) {
                for (std::size_t candidate : it->second) {
                    if (used[candidate]) continue;
                    next = candidate;
                    break;
                }
            }
            if (next == segments.size()) { loop.clear(); break; }   // open chain: not a cap
            used[next] = 1;
            const bool forward = key_of(segments[next].first.position, weld) == here;
            to = forward ? segments[next].second : segments[next].first;
            current = next;
        }
        (void)current;
        if (loop.size() < 4) continue;   // first point repeated at the end, so a triangle is 4

        loop.pop_back();                 // drop the repeat
        glm::vec3 centre(0.0f);
        for (const Vertex& p : loop) centre += p.position;
        centre /= static_cast<float>(loop.size());

        // Order the ring by angle about the plane's normal: the walk above can arrive at a
        // ring's points in either rotational direction, and a fan over an unordered ring self-
        // intersects. (Convex rings, which is what a plane cut through a solid leaves.)
        std::sort(loop.begin(), loop.end(), [&](const Vertex& a, const Vertex& b) {
            const glm::vec3 da = a.position - centre, db = b.position - centre;
            return std::atan2(glm::dot(da, v), glm::dot(da, u)) <
                   std::atan2(glm::dot(db, v), glm::dot(db, u));
        });

        // The kept half is `dot(n, p) <= d`, so the material is BEHIND the plane and the new
        // face looks along +n, away from it. (Getting this backwards lights every cut face
        // from inside the piece, which renders as solid black -- the first thing a wrong
        // fracture looks like.) The ring is sorted anticlockwise about +n below, which is the
        // winding a front face wants from that side.
        const glm::vec3 cap_normal = normal;
        const auto emit = [&](const glm::vec3& p) {
            Vertex out;
            out.position = p;
            out.normal   = cap_normal;
            out.uv       = glm::vec2(glm::dot(p, u), glm::dot(p, v));
            out.tangent  = glm::vec4(u, 1.0f);
            out_vertices.push_back(out);
            return static_cast<std::uint32_t>(out_vertices.size() - 1);
        };
        const std::uint32_t hub = emit(centre);
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const std::uint32_t a = emit(loop[i].position);
            const std::uint32_t b = emit(loop[(i + 1) % loop.size()].position);
            out_indices.push_back(hub);
            out_indices.push_back(a);
            out_indices.push_back(b);
        }
    }
}


/**
 * @brief Eigenvectors of a symmetric 3x3, by cyclic Jacobi rotation -- the piece's principal
 *        axes, as the columns of the returned matrix.
 *
 * Jacobi rather than a closed-form cubic: it is a dozen lines, it never needs a special case
 * for repeated eigenvalues (a cube's covariance has three of them), and the vectors come out
 * orthonormal to machine precision, which is what a rotation has to be.
 */
glm::mat3 jacobi_eigenvectors(glm::mat3 a) {
    glm::mat3 v(1.0f);
    for (int sweep = 0; sweep < 12; ++sweep) {
        float off = std::abs(a[1][0]) + std::abs(a[2][0]) + std::abs(a[2][1]);
        if (off < 1e-10f) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[q][p]) < 1e-12f) continue;
                const float theta = (a[q][q] - a[p][p]) / (2.0f * a[q][p]);
                const float t = (theta >= 0.0f ? 1.0f : -1.0f) /
                                (std::abs(theta) + std::sqrt(theta * theta + 1.0f));
                const float c = 1.0f / std::sqrt(t * t + 1.0f);
                const float s = t * c;
                glm::mat3 rot(1.0f);
                rot[p][p] = c;  rot[q][q] = c;
                rot[q][p] = s;  rot[p][q] = -s;
                a = glm::transpose(rot) * a * rot;
                v = v * rot;
            }
        }
    }
    return v;
}

/**
 * @brief The piece's principal axes, from the covariance of its vertices -- one of the frames
 *        best_frame() tries.
 */
glm::mat3 principal_axes(const std::vector<Vertex>& vertices) {
    if (vertices.size() < 3) return glm::mat3(1.0f);
    glm::vec3 mean(0.0f);
    for (const Vertex& v : vertices) mean += v.position;
    mean /= static_cast<float>(vertices.size());

    glm::mat3 cov(0.0f);
    for (const Vertex& v : vertices) {
        const glm::vec3 d = v.position - mean;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i][j] += d[i] * d[j];
    }
    cov /= static_cast<float>(vertices.size());

    glm::mat3 axes = jacobi_eigenvectors(cov);
    for (int i = 0; i < 3; ++i) axes[i] = glm::vec4(glm::normalize(glm::vec3(axes[i])), 0.0f);
    if (glm::determinant(axes) < 0.0f) axes[2] = -axes[2];
    return axes;
}

/** @brief Half-extent of `vertices` measured in `frame`'s axes, and the box's volume. */
glm::vec3 extent_in(const std::vector<Vertex>& vertices, const glm::mat3& frame, float& volume) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    const glm::mat3 to_frame = glm::transpose(frame);
    for (const Vertex& v : vertices) {
        const glm::vec3 p = to_frame * v.position;
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec3 half = 0.5f * (hi - lo);
    volume = 8.0f * half.x * half.y * half.z;
    return half;
}

/** @brief A right-handed frame from a face normal and any direction across it. */
glm::mat3 frame_from(const glm::vec3& normal, const glm::vec3& across) {
    glm::vec3 n = glm::normalize(normal);
    glm::vec3 u = across - n * glm::dot(across, n);
    if (glm::length(u) < 1e-5f) {
        u = std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        u = u - n * glm::dot(u, n);
    }
    u = glm::normalize(u);
    return glm::mat3(u, glm::cross(n, u), n);
}

/**
 * @brief The frame to box a piece in: the smallest box among a handful of candidates.
 *
 * Not PCA alone, which is the usual first guess and is measurably wrong here: PCA minimises
 * variance, not box volume, and on these shards it loses to the plain world axes (26.9 against
 * 20.5 over a broken cube -- fracture_test measures exactly this). What a fracture piece does
 * have is large flat faces, because every one of them is a cut plane or a face of the source
 * mesh, and a box that lies along the biggest face is usually the tight one. So: try the world
 * axes, try PCA, try a frame per large face, and keep whichever boxes smallest.
 *
 * Axes come back ordered longest first, which is what fit_boxes() splits along.
 */
glm::mat3 best_frame(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    if (vertices.size() < 4) return glm::mat3(1.0f);

    std::vector<glm::mat3> candidates;
    candidates.push_back(glm::mat3(1.0f));                       // the world axes
    candidates.push_back(principal_axes(vertices));              // PCA

    // The largest faces, by area: a shard's box usually wants to lie along one of them.
    struct Face { float area; glm::vec3 normal, across; };
    std::vector<Face> faces;
    faces.reserve(indices.size() / 3);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const glm::vec3 a = vertices[indices[t]].position;
        const glm::vec3 b = vertices[indices[t + 1]].position;
        const glm::vec3 c = vertices[indices[t + 2]].position;
        const glm::vec3 cross = glm::cross(b - a, c - a);
        const float area = 0.5f * glm::length(cross);
        if (area > 1e-9f) faces.push_back({area, cross / (2.0f * area), b - a});
    }
    std::partial_sort(faces.begin(), faces.begin() + std::min<std::size_t>(faces.size(), 6), faces.end(),
                      [](const Face& a, const Face& b) { return a.area > b.area; });
    for (std::size_t f = 0; f < faces.size() && f < 6; ++f) {
        candidates.push_back(frame_from(faces[f].normal, faces[f].across));
    }

    glm::mat3 best(1.0f);
    float best_volume = std::numeric_limits<float>::max();
    for (const glm::mat3& frame : candidates) {
        float volume = 0.0f;
        const glm::vec3 half = extent_in(vertices, frame, volume);
        (void)half;
        if (volume < best_volume) {
            best_volume = volume;
            best = frame;
        }
    }

    // Longest axis first, right-handed: fit_boxes() halves along axis 0, and a reflection is
    // not a rotation.
    float unused = 0.0f;
    const glm::vec3 half = extent_in(vertices, best, unused);
    int order[3] = {0, 1, 2};
    std::sort(std::begin(order), std::end(order), [&](int a, int b) { return half[a] > half[b]; });
    glm::mat3 frame(glm::vec3(best[order[0]]), glm::vec3(best[order[1]]), glm::vec3(best[order[2]]));
    if (glm::determinant(frame) < 0.0f) frame[2] = -frame[2];
    return frame;
}

/**
 * @brief Splits a piece's box into up to `max_boxes` boxes that hug its geometry.
 *
 * Halves along the longest axis while the part fills less of its box than `fill` asks, fitting
 * each half to the vertices that actually fall in it -- so a wedge becomes a staircase of two
 * or three boxes instead of one box with a hollow corner. The fit is per PART, not per plane,
 * which is what makes two boxes worth more than one cut.
 */
std::vector<FractureBox> fit_boxes(const std::vector<Vertex>& vertices, float volume,
                                   const glm::vec3& half_extent, float fill, int max_boxes) {
    std::vector<FractureBox> boxes;
    const auto box_of = [&](const std::vector<const Vertex*>& set) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Vertex* v : set) { lo = glm::min(lo, v->position); hi = glm::max(hi, v->position); }
        FractureBox b;
        b.centre = 0.5f * (lo + hi);
        b.half_extent = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));
        return b;
    };
    std::vector<const Vertex*> all;
    all.reserve(vertices.size());
    for (const Vertex& v : vertices) all.push_back(&v);

    const float whole = 8.0f * half_extent.x * half_extent.y * half_extent.z;
    if (max_boxes <= 1 || whole <= 0.0f || volume >= fill * whole) {
        boxes.push_back(box_of(all));
        return boxes;
    }

    // Breadth-first halving: split the part whose box wastes the most space, until the budget
    // runs out or every part is tight enough.
    struct Part { std::vector<const Vertex*> points; FractureBox box; float waste; };
    const auto waste_of = [](const FractureBox& b, float share) {
        return 8.0f * b.half_extent.x * b.half_extent.y * b.half_extent.z - share;
    };
    std::vector<Part> parts;
    parts.push_back({all, box_of(all), 0.0f});
    parts[0].waste = waste_of(parts[0].box, volume);

    while (static_cast<int>(parts.size()) < max_boxes) {
        // The worst-fitting part, and the axis it is longest along.
        std::size_t worst = 0;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            if (parts[i].waste > parts[worst].waste) worst = i;
        }
        if (parts[worst].waste <= 0.0f || parts[worst].points.size() < 8) break;
        const FractureBox& box = parts[worst].box;
        const int axis = box.half_extent.x >= box.half_extent.y
                             ? (box.half_extent.x >= box.half_extent.z ? 0 : 2)
                             : (box.half_extent.y >= box.half_extent.z ? 1 : 2);
        const float cut = box.centre[axis];

        std::vector<const Vertex*> lo_points, hi_points;
        for (const Vertex* v : parts[worst].points) {
            (v->position[axis] <= cut ? lo_points : hi_points).push_back(v);
        }
        if (lo_points.size() < 4 || hi_points.size() < 4) break;

        // The share of the volume each half carries, by point count -- enough to rank which
        // part to split next; the exact split volume would cost another clip per part.
        const float total = static_cast<float>(parts[worst].points.size());
        const float lo_share = volume * static_cast<float>(lo_points.size()) / total;
        const float hi_share = volume * static_cast<float>(hi_points.size()) / total;
        Part lo_part{lo_points, box_of(lo_points), 0.0f};
        Part hi_part{hi_points, box_of(hi_points), 0.0f};
        lo_part.waste = waste_of(lo_part.box, lo_share);
        hi_part.waste = waste_of(hi_part.box, hi_share);
        parts[worst] = std::move(lo_part);
        parts.push_back(std::move(hi_part));
    }

    boxes.reserve(parts.size());
    for (const Part& p : parts) boxes.push_back(p.box);
    return boxes;
}

/** @brief Bounding box of a vertex array. */
void bounds_of(const std::vector<Vertex>& v, glm::vec3& lo, glm::vec3& hi) {
    lo = glm::vec3(std::numeric_limits<float>::max());
    hi = glm::vec3(std::numeric_limits<float>::lowest());
    for (const Vertex& x : v) {
        lo = glm::min(lo, x.position);
        hi = glm::max(hi, x.position);
    }
    if (v.empty()) { lo = glm::vec3(0.0f); hi = glm::vec3(0.0f); }
}

/** @brief Signed volume of a closed triangle mesh (the divergence theorem over its faces). */
float mesh_volume(const std::vector<Vertex>& v, const std::vector<std::uint32_t>& idx) {
    double total = 0.0;
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
        const glm::dvec3 a(v[idx[i]].position), b(v[idx[i + 1]].position), c(v[idx[i + 2]].position);
        total += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    return static_cast<float>(std::abs(total));
}

} // namespace

bool clip_mesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
               const glm::vec3& normal, float distance, bool cap,
               std::vector<Vertex>& out_vertices, std::vector<std::uint32_t>& out_indices) {
    out_vertices.clear();
    out_indices.clear();
    if (indices.size() < 3) return false;

    glm::vec3 lo, hi;
    bounds_of(vertices, lo, hi);
    const float weld = std::max(k_weld_fraction * glm::length(hi - lo), 1e-7f);

    std::vector<std::pair<Vertex, Vertex>> cut;
    const auto push = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
        const std::uint32_t base = static_cast<std::uint32_t>(out_vertices.size());
        out_vertices.push_back(a);
        out_vertices.push_back(b);
        out_vertices.push_back(c);
        out_indices.push_back(base);
        out_indices.push_back(base + 1);
        out_indices.push_back(base + 2);
    };

    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Vertex& v0 = vertices[indices[t]];
        const Vertex& v1 = vertices[indices[t + 1]];
        const Vertex& v2 = vertices[indices[t + 2]];
        const float d0 = plane_distance(v0.position, normal, distance);
        const float d1 = plane_distance(v1.position, normal, distance);
        const float d2 = plane_distance(v2.position, normal, distance);
        const bool in0 = d0 <= 0.0f, in1 = d1 <= 0.0f, in2 = d2 <= 0.0f;
        const int inside = static_cast<int>(in0) + static_cast<int>(in1) + static_cast<int>(in2);
        if (inside == 0) continue;
        if (inside == 3) { push(v0, v1, v2); continue; }

        // Sutherland-Hodgman over the triangle: walk its edges, keeping inside corners and the
        // crossings, then fan the resulting 3- or 4-gon. The two crossings are the cut edge.
        const Vertex* tri[3] = {&v0, &v1, &v2};
        const float dist[3] = {d0, d1, d2};
        std::vector<Vertex> poly;
        Vertex crossings[2];
        int crossed = 0;
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            const bool ii = dist[i] <= 0.0f, ij = dist[j] <= 0.0f;
            if (ii) poly.push_back(*tri[i]);
            if (ii != ij) {
                const Vertex x = cross_at(*tri[i], *tri[j], dist[i], dist[j]);
                poly.push_back(x);
                if (crossed < 2) crossings[crossed++] = x;
            }
        }
        for (std::size_t i = 2; i < poly.size(); ++i) push(poly[0], poly[i - 1], poly[i]);
        if (crossed == 2) cut.emplace_back(crossings[0], crossings[1]);
    }

    if (out_indices.empty()) return false;
    if (cap) cap_opening(cut, normal, weld, out_vertices, out_indices);
    return true;
}

std::vector<FracturePiece> fracture_mesh(const std::vector<Vertex>& vertices,
                                         const std::vector<std::uint32_t>& indices,
                                         const FractureSettings& settings) {
    std::vector<FracturePiece> out;
    if (indices.size() < 3 || vertices.empty() || settings.pieces < 1) return out;
    if (settings.pieces == 1) {
        FracturePiece whole;
        whole.vertices = vertices;
        whole.indices  = indices;
        glm::vec3 lo, hi;
        bounds_of(whole.vertices, lo, hi);
        whole.centre = 0.5f * (lo + hi);
        whole.half_extent = 0.5f * (hi - lo);
        whole.volume = mesh_volume(whole.vertices, whole.indices);
        for (Vertex& v : whole.vertices) v.position -= whole.centre;
        whole.boxes = fit_boxes(whole.vertices, whole.volume, whole.half_extent,
                                glm::clamp(settings.collider_fill, 0.0f, 1.0f),
                                std::max(1, settings.max_boxes));
        out.push_back(std::move(whole));
        return out;
    }

    glm::vec3 lo, hi;
    bounds_of(vertices, lo, hi);
    const glm::vec3 size = hi - lo;

    // Sites on a jittered grid: a pure random scatter clumps, and clumped sites make slivers --
    // chunks with no volume that fall through the world and cost a body each. `irregularity`
    // is how far off its cell a site may wander, so 0 is an even break and 1 is a wild one.
    std::mt19937 rng(settings.seed ? settings.seed : 1u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const int per_axis = std::max(1, static_cast<int>(std::ceil(std::cbrt(static_cast<double>(settings.pieces)))));
    std::vector<glm::vec3> sites;
    sites.reserve(static_cast<std::size_t>(settings.pieces));
    for (int z = 0; z < per_axis && static_cast<int>(sites.size()) < settings.pieces; ++z) {
        for (int y = 0; y < per_axis && static_cast<int>(sites.size()) < settings.pieces; ++y) {
            for (int x = 0; x < per_axis && static_cast<int>(sites.size()) < settings.pieces; ++x) {
                const glm::vec3 cell((x + 0.5f) / per_axis, (y + 0.5f) / per_axis, (z + 0.5f) / per_axis);
                const glm::vec3 jitter(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f);
                const float spread = glm::clamp(settings.irregularity, 0.0f, 1.0f) / static_cast<float>(per_axis);
                sites.push_back(lo + size * glm::clamp(cell + jitter * spread * 2.0f, glm::vec3(0.01f), glm::vec3(0.99f)));
            }
        }
    }

    const int neighbours = settings.neighbours > 0
        ? std::min<int>(settings.neighbours, static_cast<int>(sites.size()) - 1)
        : static_cast<int>(sites.size()) - 1;

    std::vector<std::pair<float, std::size_t>> order;
    std::vector<Vertex> cell_v, work_v;
    std::vector<std::uint32_t> cell_i, work_i;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        // Nearest sites first: each clip can only shrink the cell, and the closest planes cut
        // the most, so the cell collapses to nothing (and the loop exits) as early as possible.
        order.clear();
        for (std::size_t j = 0; j < sites.size(); ++j) {
            if (j != i) {
                const glm::vec3 delta = sites[i] - sites[j];
                order.emplace_back(glm::dot(delta, delta), j);
            }
        }
        std::partial_sort(order.begin(), order.begin() + neighbours, order.end());

        cell_v = vertices;
        cell_i = indices;
        bool alive = true;
        for (int k = 0; k < neighbours && alive; ++k) {
            const glm::vec3& other = sites[order[static_cast<std::size_t>(k)].second];
            const glm::vec3 axis = other - sites[i];
            const float len = glm::length(axis);
            if (len < 1e-6f) continue;
            const glm::vec3 n = axis / len;
            // The bisector: everything closer to `i` than to `other` is on the negative side.
            const float d = glm::dot(n, 0.5f * (sites[i] + other));
            alive = clip_mesh(cell_v, cell_i, n, d, settings.cap_interiors, work_v, work_i);
            cell_v.swap(work_v);
            cell_i.swap(work_i);
        }
        if (!alive || cell_i.size() < 3) continue;

        FracturePiece piece;
        piece.volume = mesh_volume(cell_v, cell_i);
        if (piece.volume <= 0.0f) continue;
        piece.vertices = std::move(cell_v);
        piece.indices  = std::move(cell_i);

        // Into the shard's own frame: the vertices (and their normals and tangents, which are
        // directions in the same space) are turned so the long axis is x. Everything after
        // this -- the box, the split, the collider -- is measured there.
        const glm::mat3 frame = best_frame(piece.vertices, piece.indices);
        const glm::mat3 to_piece = glm::transpose(frame);
        for (Vertex& v : piece.vertices) {
            v.position = to_piece * v.position;
            v.normal   = to_piece * v.normal;
            v.tangent  = glm::vec4(to_piece * glm::vec3(v.tangent), v.tangent.w);
        }
        glm::vec3 plo, phi;
        bounds_of(piece.vertices, plo, phi);
        const glm::vec3 local_centre = 0.5f * (plo + phi);
        for (Vertex& v : piece.vertices) v.position -= local_centre;

        piece.half_extent = 0.5f * (phi - plo);
        piece.orientation = glm::quat_cast(frame);
        // Back out to the source mesh's space: the piece's origin is its box centre, turned.
        piece.centre      = frame * local_centre;
        piece.boxes       = fit_boxes(piece.vertices, piece.volume, piece.half_extent,
                                      glm::clamp(settings.collider_fill, 0.0f, 1.0f),
                                      std::max(1, settings.max_boxes));
        out.push_back(std::move(piece));
    }

    // Largest first: a caller with a chunk budget keeps the pieces that read as debris and
    // drops the crumbs.
    std::sort(out.begin(), out.end(), [](const FracturePiece& a, const FracturePiece& b) {
        return a.volume > b.volume;
    });
    return out;
}

} // namespace destruct
} // namespace toy
