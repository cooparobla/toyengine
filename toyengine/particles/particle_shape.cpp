#include <toyengine/particles/particle_shape.h>

namespace toy {
namespace particles {

void MeshSurface::build(const std::vector<glm::vec3>& positions, const std::vector<glm::vec3>& normals,
           const std::vector<glm::vec3>& tangents, const std::vector<uint32_t>& indices) {
    tris_.clear();
    face_cdf_.clear();
    verts_.clear();
    edges_.clear();
    edge_cdf_.clear();
    total_area_ = 0.0f;

    const size_t corner_count = indices.empty() ? positions.size() : indices.size();
    auto idx = [&](size_t c) -> uint32_t { return indices.empty() ? static_cast<uint32_t>(c) : indices[c]; };
    const bool has_n = normals.size() == positions.size();
    const bool has_t = tangents.size() == positions.size();

    // Distinct vertices (welded by quantized position) and unique edges between them.
    std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> weld;
    auto key = [](const glm::vec3& p) {
        return std::make_tuple(static_cast<int64_t>(std::llround(p.x * 1e4f)),
                               static_cast<int64_t>(std::llround(p.y * 1e4f)),
                               static_cast<int64_t>(std::llround(p.z * 1e4f)));
    };
    auto vertex_id = [&](uint32_t i) {
        auto [it, inserted] = weld.try_emplace(key(positions[i]), static_cast<uint32_t>(verts_.size()));
        if (inserted) {
            Vert v;
            v.p = positions[i];
            v.n = has_n ? normals[i] : glm::vec3(0.0f);
            verts_.push_back(v);
        } else if (has_n) {
            verts_[it->second].n += normals[i];
        }
        return it->second;
    };
    std::map<std::pair<uint32_t, uint32_t>, bool> seen_edges;

    for (size_t c = 0; c + 2 < corner_count; c += 3) {
        const uint32_t i0 = idx(c), i1 = idx(c + 1), i2 = idx(c + 2);
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) continue;
        Tri t;
        t.p[0] = positions[i0]; t.p[1] = positions[i1]; t.p[2] = positions[i2];
        const glm::vec3 cr = glm::cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        const float area = 0.5f * glm::length(cr);
        if (area <= 1e-12f) continue;
        t.face_n = cr / (2.0f * area);
        for (int k = 0; k < 3; ++k) {
            const uint32_t ii = (k == 0) ? i0 : (k == 1) ? i1 : i2;
            t.n[k] = has_n && glm::dot(normals[ii], normals[ii]) > 1e-12f ? glm::normalize(normals[ii]) : t.face_n;
        }
        const glm::vec3 tan = has_t ? tangents[i0] : (t.p[1] - t.p[0]);
        t.tangent = glm::dot(tan, tan) > 1e-12f ? glm::normalize(tan) : glm::vec3(1.0f, 0.0f, 0.0f);
        tris_.push_back(t);
        total_area_ += area;
        face_cdf_.push_back(total_area_);

        const uint32_t v[3] = {vertex_id(i0), vertex_id(i1), vertex_id(i2)};
        if (!has_n) for (uint32_t k : v) verts_[k].n += t.face_n * area;
        for (int k = 0; k < 3; ++k) {
            uint32_t a = v[k], b = v[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            if (a == b || !seen_edges.emplace(std::make_pair(a, b), true).second) continue;
            edges_.push_back({a, b});
        }
    }
    for (Vert& v : verts_) {
        const float len = glm::length(v.n);
        v.n = len > 1e-8f ? v.n / len : glm::vec3(0.0f, 0.0f, 1.0f);
    }
    float total_len = 0.0f;
    for (const auto& e : edges_) {
        total_len += glm::distance(verts_[e.first].p, verts_[e.second].p);
        edge_cdf_.push_back(total_len);
    }
    bmin_ = glm::vec3(1e30f);
    bmax_ = glm::vec3(-1e30f);
    for (const Vert& v : verts_) { bmin_ = glm::min(bmin_, v.p); bmax_ = glm::max(bmax_, v.p); }
}

EmitSample MeshSurface::sample(MeshEmitFrom from, float u, Rng& rng) const {
    EmitSample s;
    if (tris_.empty()) return s;
    switch (from) {
        case MeshEmitFrom::Vertices: {
            const size_t i = std::min(static_cast<size_t>(u * static_cast<float>(verts_.size())), verts_.size() - 1);
            s.position = verts_[i].p;
            s.normal = verts_[i].n;
            s.tangent = any_perpendicular_(s.normal);
            break;
        }
        case MeshEmitFrom::Edges: {
            if (edges_.empty()) return s;
            const size_t i = search_(edge_cdf_, u * edge_cdf_.back());
            const Vert& a = verts_[edges_[i].first];
            const Vert& b = verts_[edges_[i].second];
            const float f = rng.next01();
            s.position = glm::mix(a.p, b.p, f);
            s.normal = glm::normalize(glm::mix(a.n, b.n, f) + glm::vec3(0.0f, 0.0f, 1e-6f));
            s.tangent = glm::normalize(b.p - a.p);
            break;
        }
        case MeshEmitFrom::Faces:
        default: {
            const size_t i = search_(face_cdf_, u * total_area_);
            const Tri& t = tris_[i];
            // Uniform point in a triangle: (1 - sqrt(r1), sqrt(r1)(1 - r2), sqrt(r1) r2).
            const float r1 = std::sqrt(rng.next01());
            const float r2 = rng.next01();
            const float b0 = 1.0f - r1, b1 = r1 * (1.0f - r2), b2 = r1 * r2;
            s.position = t.p[0] * b0 + t.p[1] * b1 + t.p[2] * b2;
            const glm::vec3 n = t.n[0] * b0 + t.n[1] * b1 + t.n[2] * b2;
            const float len = glm::length(n);
            s.normal = len > 1e-6f ? n / len : t.face_n;
            s.tangent = t.tangent;
            break;
        }
    }
    s.direction = s.normal;
    return s;
}

std::vector<glm::vec4> MeshSurface::export_faces() const {
    std::vector<glm::vec4> out;
    out.reserve(tris_.size() * 8);
    for (size_t i = 0; i < tris_.size(); ++i) {
        const Tri& t = tris_[i];
        out.emplace_back(t.p[0], face_cdf_[i]);
        out.emplace_back(t.p[1], 0.0f);
        out.emplace_back(t.p[2], 0.0f);
        for (int k = 0; k < 3; ++k) out.emplace_back(t.n[k], 0.0f);
        out.emplace_back(t.face_n, 0.0f);
        out.emplace_back(t.tangent, 0.0f);
    }
    return out;
}

size_t MeshSurface::search_(const std::vector<float>& cdf, float x) {
    const auto it = std::upper_bound(cdf.begin(), cdf.end(), x);
    return std::min(static_cast<size_t>(it - cdf.begin()), cdf.size() - 1);
}

glm::vec3 MeshSurface::any_perpendicular_(const glm::vec3& n) {
    const glm::vec3 a = std::abs(n.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(a, n));
}

EmitSample sample_analytic_shape(const ShapeSettings& s, Rng& rng) {
    EmitSample out;
    const float arc = glm::radians(std::clamp(s.arc_deg, 0.0f, 360.0f));
    auto shell_radius = [&](float r, float dims) {
        // Uniform in the volume between r*(1 - thickness) and r: invert the r^dims CDF.
        const float inner = std::clamp(1.0f - s.radius_thickness, 0.0f, 1.0f);
        const float lo = std::pow(inner, dims);
        return r * std::pow(lo + (1.0f - lo) * rng.next01(), 1.0f / dims);
    };
    switch (s.type) {
        case EmitShape::Point:
            out.direction = glm::vec3(0.0f, 0.0f, 1.0f);
            break;
        case EmitShape::Sphere:
        case EmitShape::Hemisphere: {
            glm::vec3 d = rng.unit_vector();
            if (s.type == EmitShape::Hemisphere) d.z = std::abs(d.z);
            if (arc < 6.2831f) {
                const float a = rng.next01() * arc;
                const float r = std::sqrt(std::max(0.0f, 1.0f - d.z * d.z));
                d = glm::vec3(r * std::cos(a), r * std::sin(a), d.z);
            }
            out.position = d * shell_radius(s.radius, 3.0f);
            out.direction = d;
            out.normal = d;
            break;
        }
        case EmitShape::Cone: {
            // Born on the base disc, leaving along a direction tilted outward in proportion to
            // how far from the centre it was born -- Unity's cone, so the spray fans evenly.
            const float a = rng.next01() * arc;
            const float rr = shell_radius(1.0f, 2.0f);
            const glm::vec2 radial(std::cos(a), std::sin(a));
            out.position = glm::vec3(radial * rr * s.radius, 0.0f);
            const float tilt = glm::radians(std::clamp(s.angle_deg, 0.0f, 89.0f)) * rr;
            out.direction = glm::vec3(radial * std::sin(tilt), std::cos(tilt));
            if (s.radius <= 1e-4f) {
                // A point cone: spread uniformly over the solid angle instead.
                const float cos_max = std::cos(glm::radians(std::clamp(s.angle_deg, 0.0f, 179.0f)));
                const float z = cos_max + (1.0f - cos_max) * rng.next01();
                const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                out.direction = glm::vec3(radial * r, z);
            }
            out.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            break;
        }
        case EmitShape::Box:
            out.position = glm::vec3(rng.signed01(), rng.signed01(), rng.signed01()) * (s.box * 0.5f);
            out.direction = glm::vec3(0.0f, 0.0f, 1.0f);
            break;
        case EmitShape::Circle: {
            const float a = rng.next01() * arc;
            const glm::vec2 radial(std::cos(a), std::sin(a));
            out.position = glm::vec3(radial * shell_radius(s.radius, 2.0f), 0.0f);
            out.direction = glm::vec3(radial, 0.0f);
            out.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            out.tangent = glm::vec3(-radial.y, radial.x, 0.0f);
            break;
        }
        case EmitShape::Edge:
            out.position = glm::vec3(rng.signed01() * 0.5f * s.length, 0.0f, 0.0f);
            out.direction = glm::vec3(0.0f, 0.0f, 1.0f);
            break;
        case EmitShape::Mesh:
            break;
    }
    return out;
}

void finish_shape_sample(const ShapeSettings& s, EmitSample& e, Rng& rng) {
    e.position += s.offset;
    if (s.type == EmitShape::Mesh) e.position += e.normal * s.normal_offset;
    if (s.random_direction > 0.0f) {
        const glm::vec3 d = glm::mix(e.direction, rng.unit_vector(), std::clamp(s.random_direction, 0.0f, 1.0f));
        const float len = glm::length(d);
        e.direction = len > 1e-5f ? d / len : e.direction;
    }
}

} // namespace particles
} // namespace toy
