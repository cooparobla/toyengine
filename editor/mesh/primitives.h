/**
 * @file primitives.h
 * @brief Parametric starting shapes for the mesh editor (Z-up, counter-clockwise front faces,
 *        matching every mesh file the engine ships).
 */

#ifndef TOYEDITOR_MESH_PRIMITIVES_H
#define TOYEDITOR_MESH_PRIMITIVES_H

#include "edit_mesh.h"

#include <cmath>
#include <string>

namespace toy::editor {

namespace detail {
inline uint32_t add_vertex(EditMesh& m, glm::vec3 p) { m.positions.push_back(p); return static_cast<uint32_t>(m.positions.size() - 1); }
inline void add_face(EditMesh& m, std::initializer_list<std::pair<uint32_t, glm::vec2>> corners, bool smooth = false) {
    Face f;
    f.smooth = smooth;
    for (const auto& c : corners) f.corners.push_back(Corner{c.first, c.second});
    m.faces.push_back(std::move(f));
}
constexpr float kPi = 3.14159265358979f;
} // namespace detail

/** @brief An axis-aligned box of `size`, centred on the origin, one quad per side. */
inline EditMesh make_cube(glm::vec3 size = glm::vec3(1.0f)) {
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

/** @brief A flat grid in XY facing +Z, `size` across, `subdivisions` quads per side. */
inline EditMesh make_plane(float size = 2.0f, int subdivisions = 1) {
    EditMesh m;
    const int n = std::max(1, subdivisions);
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back({(x / float(n) - 0.5f) * size, (y / float(n) - 0.5f) * size, 0.0f});
    auto idx = [&](int x, int y) { return static_cast<uint32_t>(y * (n + 1) + x); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            detail::add_face(m, {{idx(x, y), {x / float(n), y / float(n)}},
                                 {idx(x + 1, y), {(x + 1) / float(n), y / float(n)}},
                                 {idx(x + 1, y + 1), {(x + 1) / float(n), (y + 1) / float(n)}},
                                 {idx(x, y + 1), {x / float(n), (y + 1) / float(n)}}});
    return m;
}

/** @brief A cylinder along Z, centred on the origin, with optional N-gon caps. */
inline EditMesh make_cylinder(float radius = 0.5f, float height = 1.0f, int segments = 16, bool caps = true) {
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

/** @brief A UV sphere (smooth-shaded), poles on Z. */
inline EditMesh make_uv_sphere(float radius = 0.5f, int segments = 24, int rings = 12) {
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

/**
 * @brief The terrain tile-side template: the +Z face of the unit cube [0,1]^3 (see
 *        toyengine/world/tile_mesh_library.h, which rotates it onto all six faces).
 */
inline EditMesh make_tile_side() {
    EditMesh m;
    m.positions = {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    detail::add_face(m, {{0, {0, 0}}, {1, {1, 0}}, {2, {1, 1}}, {3, {0, 1}}});
    return m;
}

/** @brief The primitive names the editor's Create menu offers, in order. */
inline const std::vector<std::string>& primitive_names() {
    static const std::vector<std::string> names = {"Cube", "Plane", "Cylinder", "Sphere", "Tile Side"};
    return names;
}

inline EditMesh make_primitive(const std::string& name) {
    if (name == "Plane") return make_plane();
    if (name == "Cylinder") return make_cylinder();
    if (name == "Sphere") return make_uv_sphere();
    if (name == "Tile Side") return make_tile_side();
    return make_cube();
}

} // namespace toy::editor

#endif // TOYEDITOR_MESH_PRIMITIVES_H
