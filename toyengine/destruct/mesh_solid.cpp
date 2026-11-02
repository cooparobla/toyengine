#include <toyengine/destruct/mesh_solid.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace toy {
namespace destruct {

namespace {

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

} // namespace

float mesh_volume(const std::vector<Vertex>& v, const std::vector<std::uint32_t>& idx) {
    double total = 0.0;
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
        const glm::dvec3 a(v[idx[i]].position), b(v[idx[i + 1]].position), c(v[idx[i + 2]].position);
        total += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    return static_cast<float>(std::abs(total));
}

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

void centre_in_frame(std::vector<Vertex>& vertices, const glm::mat3& frame,
                     glm::vec3& out_half_extent, glm::vec3& out_centre) {
    const glm::mat3 to_frame = glm::transpose(frame);
    for (Vertex& v : vertices) {
        v.position = to_frame * v.position;
        v.normal   = to_frame * v.normal;
        v.tangent  = glm::vec4(to_frame * glm::vec3(v.tangent), v.tangent.w);
    }
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    if (vertices.empty()) { lo = glm::vec3(0.0f); hi = glm::vec3(0.0f); }
    const glm::vec3 local_centre = 0.5f * (lo + hi);
    for (Vertex& v : vertices) v.position -= local_centre;
    out_half_extent = 0.5f * (hi - lo);
    out_centre = frame * local_centre;
}

} // namespace destruct
} // namespace toy
