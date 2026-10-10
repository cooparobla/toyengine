/**
 * @file mesh_topology_test.cpp
 * @brief MeshTopology adjacency and the loops / rings Alt+click and Ctrl+Alt+click select, on cubes,
 * grids and subdivided cubes.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/mesh_loops.h"
#include "editor/mesh/mesh_subdivide.h"
#include "editor/mesh/mesh_topology.h"
#include "editor/mesh/primitives.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("mesh_topology");

namespace toy::editor::testing {

/** @brief MeshTopology adjacency; edge rings and loops on cubes, grids and spheres. */
COOPA_TEST(edge_loops_and_rings_on_cubes_and_grids) {
    const EditMesh cube = make_cube();
    const MeshTopology t(cube);
    bool two = t.edges.size() == 12;
    for (uint32_t e = 0; e < t.edges.size(); ++e) two = two && t.faces_of(e).size() == 2;
    expect(two, "a cube has 12 edges, each with 2 faces");
    expect(t.find_edge(4, 0) == t.find_edge(0, 4) && t.find_edge(0, 4) != MeshTopology::kNone, "find_edge ignores direction");
    const EdgePath ring = edge_ring(t, t.find_edge(0, 4));
    expect(ring.closed && ring.edges.size() == 4 && ring.faces.size() == 4, "the ring of a cube's vertical edge: 4 edges, closed");
    expect(edge_loop(t, t.find_edge(0, 4)).edges.size() == 1, "a cube's loops stop at its valence-3 corners");

    EditMesh sc = make_cube();
    MeshSelection all;
    all.select_all(sc);
    subdivide(sc, all, 1);
    const MeshTopology ts(sc);
    uint32_t eq = MeshTopology::kNone;
    for (uint32_t e = 0; e < ts.edges.size(); ++e) {
        const auto& [a, b] = ts.edges[e];
        if (std::abs(sc.positions[a].z) < 1e-5f && std::abs(sc.positions[b].z) < 1e-5f) { eq = e; break; }
    }
    const EdgePath equator = edge_loop(ts, eq);
    expect(equator.closed && equator.edges.size() == 8, "a subdivided cube's equator loop: 8 edges, closed (got " +
                                                            std::to_string(equator.edges.size()) + ")");
    const EditMesh grid = make_grid(2, 4);
    const MeshTopology tg(grid);
    const EdgePath open = edge_loop(tg, tg.find_edge(4, 7));   // x = 0, y rows 1-2: interior vertical edge
    expect(!open.closed && open.edges.size() == 4, "an interior grid loop runs boundary to boundary (got " +
                                                       std::to_string(open.edges.size()) + ")");
    MeshSelection sel;
    sel.mode = SelectMode::Edge;
    select_edge_loop(grid, sel, make_edge(4, 7), false);
    expect(sel.edges.size() == 4, "Alt+click selects the loop");
    select_edge_loop(grid, sel, make_edge(4, 7), true);
    expect(sel.edges.empty(), "Shift+Alt+click on a selected loop deselects it");
    select_edge_ring(grid, sel, make_edge(4, 7), false);
    expect(sel.edges.size() == 3, "Ctrl+Alt+click selects the ring across the grid (got " + std::to_string(sel.edges.size()) + ")");
}

} // namespace toy::editor::testing
