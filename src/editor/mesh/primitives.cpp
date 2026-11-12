#include "editor/mesh/primitives.h"

namespace toy {
namespace editor {
namespace detail {

uint32_t add_vertex(EditMesh& m, glm::vec3 p) { m.positions.push_back(p); return static_cast<uint32_t>(m.positions.size() - 1); }

void add_face(EditMesh& m, std::initializer_list<std::pair<uint32_t, glm::vec2>> corners, bool smooth) {
    Face f;
    f.smooth = smooth;
    for (const auto& c : corners) f.corners.push_back(Corner{c.first, c.second});
    m.faces.push_back(std::move(f));
}

} // namespace detail
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {

EditMesh make_cube(glm::vec3 size) {
    EditMesh m;
    const glm::vec3 h = size * 0.5f;
    for (int i = 0; i < 8; ++i) {
        m.positions.push_back({(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
    }
    using detail::add_face;
    const glm::vec2 a{0, 0}, b{1, 0}, c{1, 1}, d{0, 1};
    add_face(m, {{0, a}, {2, b}, {3, c}, {1, d}});   // -Z (bottom)
    add_face(m, {{4, a}, {5, b}, {7, c}, {6, d}});   // +Z (top)
    add_face(m, {{0, a}, {1, b}, {5, c}, {4, d}});   // -Y
    add_face(m, {{3, a}, {2, b}, {6, c}, {7, d}});   // +Y
    add_face(m, {{2, a}, {0, b}, {4, c}, {6, d}});   // -X
    add_face(m, {{1, a}, {3, b}, {7, c}, {5, d}});   // +X
    return m;
}

EditMesh make_grid(int xs, int ys, float size) {
    EditMesh m;
    const int nx = std::max(1, xs), ny = std::max(1, ys);
    for (int y = 0; y <= ny; ++y)
        for (int x = 0; x <= nx; ++x)
            m.positions.push_back({(x / float(nx) - 0.5f) * size, (y / float(ny) - 0.5f) * size, 0.0f});
    auto idx = [&](int x, int y) { return static_cast<uint32_t>(y * (nx + 1) + x); };
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            detail::add_face(m, {{idx(x, y), {x / float(nx), y / float(ny)}},
                                 {idx(x + 1, y), {(x + 1) / float(nx), y / float(ny)}},
                                 {idx(x + 1, y + 1), {(x + 1) / float(nx), (y + 1) / float(ny)}},
                                 {idx(x, y + 1), {x / float(nx), (y + 1) / float(ny)}}});
    return m;
}

EditMesh make_cylinder(float radius, float height, int segments, bool caps) {
    EditMesh m;
    const int s = std::max(3, segments);
    for (int i = 0; i < s; ++i) {
        const float a = 2.0f * detail::kPi * i / s;
        m.positions.push_back({radius * std::cos(a), radius * std::sin(a), -height * 0.5f});
    }
    for (int i = 0; i < s; ++i) {
        const float a = 2.0f * detail::kPi * i / s;
        m.positions.push_back({radius * std::cos(a), radius * std::sin(a), height * 0.5f});
    }
    for (int i = 0; i < s; ++i) {
        const uint32_t b0 = i, b1 = (i + 1) % s, t0 = s + i, t1 = s + (i + 1) % s;
        const float u0 = i / float(s), u1 = (i + 1) / float(s);
        detail::add_face(m, {{b0, {u0, 0}}, {b1, {u1, 0}}, {t1, {u1, 1}}, {t0, {u0, 1}}}, true);
    }
    if (caps) {
        Face bottom, top;
        for (int i = s - 1; i >= 0; --i) {
            const float a = 2.0f * detail::kPi * i / s;
            bottom.corners.push_back({static_cast<uint32_t>(i), {0.5f + 0.5f * std::cos(a), 0.5f - 0.5f * std::sin(a)}});
        }
        for (int i = 0; i < s; ++i) {
            const float a = 2.0f * detail::kPi * i / s;
            top.corners.push_back({static_cast<uint32_t>(s + i), {0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)}});
        }
        m.faces.push_back(bottom);
        m.faces.push_back(top);
    }
    return m;
}

EditMesh make_uv_sphere(float radius, int segments, int rings) {
    EditMesh m;
    const int s = std::max(3, segments), r = std::max(2, rings);
    const uint32_t south = detail::add_vertex(m, {0, 0, -radius});
    for (int j = 1; j < r; ++j) {
        const float phi = detail::kPi * j / r - detail::kPi * 0.5f;
        for (int i = 0; i < s; ++i) {
            const float th = 2.0f * detail::kPi * i / s;
            m.positions.push_back({radius * std::cos(phi) * std::cos(th), radius * std::cos(phi) * std::sin(th), radius * std::sin(phi)});
        }
    }
    const uint32_t north = detail::add_vertex(m, {0, 0, radius});
    auto ring = [&](int j, int i) { return static_cast<uint32_t>(1 + (j - 1) * s + ((i % s) + s) % s); };
    for (int i = 0; i < s; ++i) {
        const float u0 = i / float(s), u1 = (i + 1) / float(s);
        detail::add_face(m, {{south, {(u0 + u1) * 0.5f, 0}}, {ring(1, i + 1), {u1, 1.0f / r}}, {ring(1, i), {u0, 1.0f / r}}}, true);
        for (int j = 1; j < r - 1; ++j) {
            const float v0 = j / float(r), v1 = (j + 1) / float(r);
            detail::add_face(m, {{ring(j, i), {u0, v0}}, {ring(j, i + 1), {u1, v0}}, {ring(j + 1, i + 1), {u1, v1}}, {ring(j + 1, i), {u0, v1}}}, true);
        }
        detail::add_face(m, {{ring(r - 1, i), {u0, (r - 1) / float(r)}}, {ring(r - 1, i + 1), {u1, (r - 1) / float(r)}}, {north, {(u0 + u1) * 0.5f, 1}}}, true);
    }
    return m;
}

const std::vector<std::string>& primitive_names() {
    static const std::vector<std::string> names = {"Cube", "Plane", "Grid", "Cylinder", "Sphere"};
    return names;
}

PrimitiveParams PrimitiveParams::defaults(const std::string& kind) {
    PrimitiveParams p;
    p.kind = kind;
    if (kind == "Cube") p.size = 1.0f;
    if (kind == "Sphere") p.segments = 24;
    return p;
}

bool PrimitiveParams::operator==(const PrimitiveParams& o) const {
    return kind == o.kind && size == o.size && radius == o.radius && depth == o.depth && x_subdivisions == o.x_subdivisions &&
           y_subdivisions == o.y_subdivisions && segments == o.segments && rings == o.rings && caps == o.caps;
}

EditMesh make_primitive(const PrimitiveParams& p) {
    if (p.kind == "Plane") return make_plane(p.size, 1);
    if (p.kind == "Grid") return make_grid(p.x_subdivisions, p.y_subdivisions, p.size);
    if (p.kind == "Cylinder") return make_cylinder(p.radius, p.depth, p.segments, p.caps);
    if (p.kind == "Sphere") return make_uv_sphere(p.radius, p.segments, p.rings);
    return make_cube(glm::vec3(p.size));
}

} // namespace editor
} // namespace toy
