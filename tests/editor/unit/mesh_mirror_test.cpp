/**
 * @file mesh_mirror_test.cpp
 * @brief Symmetry and Mirror: mirror maps pair vertices across local or global axes, on-plane vertices
 * stay on the plane, Mirror keeps faces outward, and sculpt dabs mirror across world axes.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/mesh_mirror.h"
#include "editor/mesh/mesh_ops.h"
#include "editor/mesh/primitives.h"
#include "editor/mesh/sculpt.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("mesh_mirror");

namespace toy::editor::testing {

COOPA_TEST(mirror_maps_and_mirror_in_local_and_global_axes) {
    // Local X: moving the +X face's vertices moves their -X partners the mirrored way.
    EditMesh m = make_cube();
    MeshSelection sel;
    sel.mode = SelectMode::Vertex;
    for (uint32_t v = 0; v < m.positions.size(); ++v) if (m.positions[v].x > 0) sel.verts.insert(v);
    MirrorSettings sx;
    sx.axis[0] = true;
    const MirrorMap map = build_mirror_map(m, sel.affected_vertices(m), sx, glm::mat4(1.0f));
    expect(map.pairs.size() == 4, "local X pairs the four +X vertices with the four -X ones");
    const EditMesh before = m;
    transform_selection(m, sel, glm::translate(glm::mat4(1.0f), glm::vec3(0.25f, 0.1f, 0)));
    apply_mirror_map(m, map);
    bool mirrored = true;
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        if (before.positions[v].x > 0) continue;
        const glm::vec3 want = before.positions[v] + glm::vec3(-0.25f, 0.1f, 0);
        mirrored &= glm::length(m.positions[v] - want) < 1e-5f;
    }
    expect(mirrored, "the -X side moves by the mirrored delta");

    // On-plane vertices stay on the plane.
    EditMesh g = make_grid(2, 2, 2.0f);
    MeshSelection gs;
    gs.mode = SelectMode::Vertex;
    for (uint32_t v = 0; v < g.positions.size(); ++v) if (std::abs(g.positions[v].x) < 1e-6f) gs.verts.insert(v);
    const MirrorMap gmap = build_mirror_map(g, gs.affected_vertices(g), sx, glm::mat4(1.0f));
    transform_selection(g, gs, glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 0, 0.2f)));
    apply_mirror_map(g, gmap);
    bool on_plane = !gs.verts.empty();
    for (uint32_t v : gs.verts) on_plane &= std::abs(g.positions[v].x) < 1e-5f && std::abs(g.positions[v].z - 0.2f) < 1e-5f;
    expect(on_plane, "vertices on the mirror plane slide along it but stay on it");

    // Global X on a mesh turned 90 degrees about Z: the world's X is the mesh's Y.
    const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
    EditMesh c = make_cube();
    MeshSelection cs;
    cs.mode = SelectMode::Vertex;
    for (uint32_t v = 0; v < c.positions.size(); ++v) if (c.positions[v].y > 0) cs.verts.insert(v);
    MirrorSettings gx = sx;
    gx.global = true;
    expect(build_mirror_map(c, cs.affected_vertices(c), gx, turned).pairs.size() == 4, "global X pairs across the mesh's local Y");
    expect(build_mirror_map(c, cs.affected_vertices(c), sx, turned).pairs.empty(), "local X pairs nothing for a +Y selection");

    // Mirror: an asymmetric cube flipped along X keeps closed, outward-facing faces.
    EditMesh a = make_cube();
    a.positions[7] += glm::vec3(0.3f, 0, 0);   // (+,+,+) corner pulled out
    MeshSelection all;
    all.mode = SelectMode::Face;
    for (uint32_t f = 0; f < a.faces.size(); ++f) all.faces.insert(f);
    mirror_selection(a, all, mirror_plane_matrix(0, false, glm::vec3(0.0f), glm::mat4(1.0f)));
    expect(std::abs(a.positions[7].x + 0.8f) < 1e-5f, "Mirror X flips the pulled corner to -X");
    glm::vec3 centre(0.0f);
    for (const auto& p : a.positions) centre += p;
    centre /= static_cast<float>(a.positions.size());
    bool outward = closed_and_consistent(a);
    for (uint32_t f = 0; f < a.faces.size(); ++f) outward &= glm::dot(a.face_normal(f), a.face_center(f) - centre) > 0.0f;
    expect(outward, "mirrored faces still face outward");
    // Global mirror through a pivot, on the turned mesh: world X flips local Y about the pivot.
    EditMesh b = make_cube();
    mirror_selection(b, all, mirror_plane_matrix(0, true, glm::vec3(0, 0.5f, 0), turned));
    expect(std::abs(b.positions[0].y - 1.5f) < 1e-5f && std::abs(b.positions[0].x + 0.5f) < 1e-5f,
           "a global Mirror on a rotated mesh flips along the world axis through the pivot");

    // Sculpt: a global mirror image of a dab on the turned mesh lands at local -Y.
    SculptDab d;
    d.center = glm::vec3(0.1f, 0.4f, 0.0f);
    d.radius = 0.1f;
    std::vector<glm::vec3> centres;
    for_each_symmetric(d, mirror_images(gx, turned), [&](const SculptDab& md) { centres.push_back(md.center); });
    expect(centres.size() == 2 && glm::length(centres[1] - glm::vec3(0.1f, -0.4f, 0.0f)) < 1e-5f,
           "a global X sculpt dab mirrors across the world axis");
}

} // namespace toy::editor::testing
