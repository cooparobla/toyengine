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
uint32_t add_vertex(EditMesh& m, glm::vec3 p);
void add_face(EditMesh& m, std::initializer_list<std::pair<uint32_t, glm::vec2>> corners, bool smooth = false);
constexpr float kPi = 3.14159265358979f;
} // namespace detail

/** @brief An axis-aligned box of `size`, centred on the origin, one quad per side. */
EditMesh make_cube(glm::vec3 size = glm::vec3(1.0f));

/** @brief A flat quad grid in XY facing +Z, `size` across, `xs` x `ys` quads. */
EditMesh make_grid(int xs, int ys, float size = 2.0f);

/** @brief A flat grid in XY facing +Z, `size` across, `subdivisions` quads per side. */
inline EditMesh make_plane(float size = 2.0f, int subdivisions = 1) { return make_grid(subdivisions, subdivisions, size); }

/** @brief A cylinder along Z, centred on the origin, with optional N-gon caps. */
EditMesh make_cylinder(float radius = 0.5f, float height = 1.0f, int segments = 16, bool caps = true);

/** @brief A UV sphere (smooth-shaded), poles on Z. */
EditMesh make_uv_sphere(float radius = 0.5f, int segments = 24, int rings = 12);

/** @brief The primitive names the editor's Create menu offers, in order. */
const std::vector<std::string>& primitive_names();

/**
 * @brief A primitive's parameters -- what Blender's "Adjust Last Operation" panel edits
 *        after Add > Mesh. Each kind reads the fields that apply to it.
 */
struct PrimitiveParams {
    std::string kind = "Cube";
    float size = 2.0f;          ///< Cube edge, plane / grid width.
    float radius = 0.5f;        ///< Cylinder, sphere.
    float depth = 1.0f;         ///< Cylinder height.
    int x_subdivisions = 10;    ///< Grid.
    int y_subdivisions = 10;    ///< Grid.
    int segments = 16;          ///< Cylinder, sphere (around).
    int rings = 12;             ///< Sphere.
    bool caps = true;           ///< Cylinder.

    /** @brief The defaults for a kind (Blender's: a 2 m cube / plane, a 10 x 10 grid...). */
    static PrimitiveParams defaults(const std::string& kind);
    bool operator==(const PrimitiveParams& o) const;
};

EditMesh make_primitive(const PrimitiveParams& p);

inline EditMesh make_primitive(const std::string& name) { return make_primitive(PrimitiveParams::defaults(name)); }

/**
 * @brief Appends `src` (transformed by `xf`) to `dst` as new, separate geometry -- Shift+A in
 *        Edit Mode -- and selects it (Face mode).
 */
template <class Selection>
inline void append_mesh(EditMesh& dst, const EditMesh& src, const glm::mat4& xf, Selection& sel) {
    const uint32_t base = static_cast<uint32_t>(dst.positions.size());
    for (const auto& p : src.positions) dst.positions.push_back(glm::vec3(xf * glm::vec4(p, 1.0f)));
    sel.clear();
    for (const auto& f : src.faces) {
        Face nf = f;
        for (auto& c : nf.corners) c.v += base;
        sel.faces.insert(static_cast<uint32_t>(dst.faces.size()));
        dst.faces.push_back(std::move(nf));
    }
    dst.sync_vertex_data();   // the added vertices join no vertex groups
}

} // namespace toy::editor

#endif // TOYEDITOR_MESH_PRIMITIVES_H
