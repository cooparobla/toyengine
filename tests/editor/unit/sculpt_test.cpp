/**
 * @file sculpt_test.cpp
 * @brief Sculpt brushes on a flat grid with known answers: Draw raises only inside the brush, Ctrl
 * inverts, Smooth flattens a spike, X symmetry is exact, Grab follows the cursor, and the vertex
 * grid query matches brute force. Also Normal orientation of a face.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/mesh_mirror.h"
#include "editor/mesh/mesh_ops.h"
#include "editor/mesh/primitives.h"
#include "editor/mesh/sculpt.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("sculpt");

namespace toy::editor::testing {

/** @brief Normal orientation and the sculpt brushes. */
COOPA_TEST(brushes_symmetry_grab_and_vertex_grid_query) {
    EditMesh cube = make_cube();
    MeshSelection top;
    top.faces = {1};
    const glm::mat3 nb = normal_basis(cube, top);
    expect(glm::distance(nb[2], glm::vec3(0, 0, 1)) < 1e-5f && std::abs(glm::dot(nb[0], nb[2])) < 1e-5f,
           "Normal orientation of the top face: Z up, X in the face");

    expect(sculpt_falloff(0.0f) == 1.0f && sculpt_falloff(1.0f) == 0.0f && sculpt_falloff(0.5f) == 0.5f, "falloff ends");
    EditMesh g = make_grid(16, 16, 2.0f);
    SculptCache cache;
    cache.build(g);
    VertexGrid grid;
    grid.build(g, 0.5f);
    std::vector<uint32_t> moved;
    const uint32_t centre = 8 * 17 + 8;
    SculptDab d;
    d.center = glm::vec3(0);
    d.radius = 0.5f;
    d.strength = 1.0f;
    sculpt_dab(g, cache, grid, d, moved);
    bool outside_still = true;
    for (uint32_t v = 0; v < g.positions.size(); ++v) {
        if (glm::length(glm::vec2(g.positions[v])) > 0.5f + 1e-4f) outside_still &= g.positions[v].z == 0.0f;
    }
    expect(g.positions[centre].z > 0.0f && outside_still, "Draw raises the centre and leaves the rest alone");
    d.invert = true;
    const float z1 = g.positions[centre].z;
    sculpt_dab(g, cache, grid, d, moved);
    expect(g.positions[centre].z < z1, "Ctrl (invert) pushes back in");

    EditMesh spike = make_grid(16, 16, 2.0f);
    spike.positions[centre].z = 1.0f;
    cache.build(spike);
    grid.build(spike, 0.5f);
    d = SculptDab{};
    d.center = glm::vec3(0, 0, 1.0f);   // the brush is a sphere: centred on the spike
    d.radius = 0.6f;
    d.strength = 1.0f;
    d.brush = SculptBrush::Smooth;
    sculpt_dab(spike, cache, grid, d, moved);
    expect(spike.positions[centre].z < 0.5f, "Smooth flattens a spike");

    EditMesh sym = make_grid(16, 16, 2.0f);
    cache.build(sym);
    grid.build(sym, 0.3f);
    d = SculptDab{};
    d.center = glm::vec3(0.5f, 0, 0);
    d.radius = 0.3f;
    d.strength = 1.0f;
    MirrorSettings axes;
    axes.axis[0] = true;
    for_each_symmetric(d, axes, [&](const SculptDab& md) { sculpt_dab(sym, cache, grid, md, moved); });
    const uint32_t right = 8 * 17 + 12, left = 8 * 17 + 4;   // x = +-0.5
    expect(sym.positions[right].z > 0.0f && std::abs(sym.positions[right].z - sym.positions[left].z) < 1e-6f,
           "X symmetry raises both sides the same");

    // The grid query matches brute force.
    std::vector<uint32_t> q;
    grid.query(sym, glm::vec3(0.1f, -0.2f, 0), 0.37f, q);
    size_t brute = 0;
    for (const auto& p : sym.positions) brute += glm::length(p - glm::vec3(0.1f, -0.2f, 0)) <= 0.37f;
    expect(q.size() == brute, "VertexGrid::query finds exactly the vertices in range");

    EditMesh gm = make_grid(16, 16, 2.0f);
    grid.build(gm, 0.5f);
    const SculptGrab gr = sculpt_grab_begin(gm, grid, glm::vec3(0), 0.5f, 0.5f);
    sculpt_grab_apply(gm, gr, glm::vec3(0, 0, 0.3f), moved);
    expect(std::abs(gm.positions[centre].z - 0.3f) < 1e-5f, "Grab moves the centre with the cursor");
}

} // namespace toy::editor::testing
