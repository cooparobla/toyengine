/**
 * @file mesh_ops_test.cpp
 * @brief Edit Mode's mesh operations on known shapes, with hand-counted results: extrude, inset, flip,
 * bevel, merge by distance, delete, subdivide, Catmull-Clark, triangulate, tris to quads,
 * dissolve, bridge, loop cut + edge slide, box UVs and selection growth. Every result must stay
 * closed and consistently wound.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/mesh_loops.h"
#include "editor/mesh/mesh_ops.h"
#include "editor/mesh/mesh_subdivide.h"
#include "editor/mesh/primitives.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("mesh_ops");

namespace toy::editor::testing {

COOPA_TEST(extrude_inset_and_flip_keep_the_mesh_closed) {
    EditMesh m = make_cube();
    MeshSelection sel;
    sel.mode = SelectMode::Face;
    // Select the +Z face.
    for (uint32_t f = 0; f < m.faces.size(); ++f) if (m.face_normal(f).z > 0.9f) sel.faces.insert(f);
    expect(sel.faces.size() == 1, "found the top face");
    extrude_faces(m, sel, 1.0f);
    expect(m.faces.size() == 10 && m.positions.size() == 12, "extrude adds 4 side quads and 4 vertices");
    expect(euler(m) == 2 && closed_and_consistent(m), "extruded cube stays closed and consistently wound");
    glm::vec3 lo, hi;
    m.bounds(lo, hi);
    expect(std::abs(hi.z - 1.5f) < 1e-5f, "the cap moved 1 unit up");

    inset_faces(m, sel, 0.25f);
    expect(m.faces.size() == 14 && closed_and_consistent(m), "inset of a quad adds 4 rim quads");

    const EditMesh before = m;
    MeshSelection none;
    flip_normals(m, none);
    expect(!(m == before), "flip changes winding");
    flip_normals(m, none);
    expect(m == before, "flipping twice is the identity");
}

COOPA_TEST(bevel_merge_by_distance_and_delete) {
    EditMesh m = make_cube();
    MeshSelection sel;
    sel.mode = SelectMode::Edge;
    sel.edges.insert(m.edges().front());
    bevel_edges(m, sel, 0.1f);
    expect(m.faces.size() == 7, "beveling one cube edge adds one strip face (got " + std::to_string(m.faces.size()) + ")");
    expect(euler(m) == 2 && closed_and_consistent(m), "the bevelled cube is still closed and consistent");

    // Merge by distance welds an unwelded copy back together.
    EditMesh soup;
    for (const auto& f : make_cube().faces) {
        Face nf;
        for (const auto& c : f.corners) {
            soup.positions.push_back(make_cube().positions[c.v]);
            nf.corners.push_back({static_cast<uint32_t>(soup.positions.size() - 1), c.uv});
        }
        soup.faces.push_back(nf);
    }
    MeshSelection all;
    const size_t removed = merge_by_distance(soup, all, 1e-4f);
    expect(soup.positions.size() == 8 && removed == 16, "merge by distance welds 24 corners to 8 vertices");
    expect(closed_and_consistent(soup), "the welded soup is a closed cube");

    EditMesh d = make_cube();
    MeshSelection top;
    top.mode = SelectMode::Face;
    top.faces.insert(0);
    delete_selection(d, top);
    expect(d.faces.size() == 5 && euler(d) == 1, "deleting a face opens the cube");
}

/** @brief Subdivide, Catmull-Clark, Triangulate, Tris to Quads, Dissolve, Grid, Bridge. */
COOPA_TEST(subdivide_triangulate_dissolve_and_bridge) {
    EditMesh c1 = make_cube();
    MeshSelection all;
    all.select_all(c1);
    subdivide(c1, all, 1);
    expect(c1.positions.size() == 26 && c1.faces.size() == 24 && closed_and_consistent(c1), "subdivide a cube once: 26 / 24, closed");
    expect(all.faces.size() == 24, "the new faces are selected");
    EditMesh c2 = make_cube();
    all.select_all(c2);
    subdivide(c2, all, 2);
    expect(c2.positions.size() == 56 && c2.faces.size() == 54 && closed_and_consistent(c2), "two cuts: 56 / 54, closed");
    EditMesh c3 = make_cube();
    MeshSelection one;
    one.faces = {1};
    subdivide(c3, one, 1);
    expect(closed_and_consistent(c3) && euler(c3) == 2, "subdividing one face keeps the mesh closed (neighbours gain the cuts)");
    EditMesh hex = make_cylinder(0.5f, 1.0f, 6, true);
    MeshSelection cap;
    cap.faces = {static_cast<uint32_t>(hex.faces.size() - 1)};
    subdivide(hex, cap, 2);
    expect(closed_and_consistent(hex) && euler(hex) == 2, "an n-gon with an odd segment count subdivides without holes");

    EditMesh cc = make_cube();
    catmull_clark(cc, all, 1);
    float maxlen = 0.0f;
    for (const auto& p : cc.positions) maxlen = std::max(maxlen, glm::length(p));
    expect(cc.positions.size() == 26 && cc.faces.size() == 24 && closed_and_consistent(cc) && maxlen < 0.8f,
           "Catmull-Clark: 26 / 24, closed, corners pulled in");

    EditMesh tc = make_cube();
    all.select_all(tc);
    triangulate(tc, all);
    expect(tc.faces.size() == 12 && closed_and_consistent(tc), "triangulate a cube: 12 triangles");
    tris_to_quads(tc, all);
    expect(tc.faces.size() == 6 && closed_and_consistent(tc), "tris to quads restores the 6 quads (got " + std::to_string(tc.faces.size()) + ")");
    EditMesh cyl = make_cylinder(0.5f, 1.0f, 16, true);
    MeshSelection capsel;
    capsel.faces = {static_cast<uint32_t>(cyl.faces.size() - 1)};
    triangulate(cyl, capsel);
    expect(capsel.faces.size() == 14 && closed_and_consistent(cyl), "a 16-gon cap becomes 14 triangles");

    EditMesh g = make_grid(2, 1);
    MeshSelection ds;
    ds.mode = SelectMode::Edge;
    dissolve_edges(g, ds, {make_edge(1, 4)});
    expect(g.faces.size() == 1 && g.positions.size() == 4, "dissolving a grid's middle edge leaves one quad (got " +
                                                               std::to_string(g.faces.size()) + " faces, " + std::to_string(g.positions.size()) + " verts)");
    EditMesh g2 = make_grid(2, 2);
    MeshSelection dv;
    dv.mode = SelectMode::Vertex;
    dv.verts = {4};
    dissolve_verts(g2, dv);
    expect(g2.faces.size() == 1 && g2.faces[0].corners.size() == 8, "dissolving a grid's centre vertex leaves one 8-gon");

    const EditMesh g35 = make_grid(3, 5);
    expect(g35.positions.size() == 24 && g35.faces.size() == 15, "grid 3 x 5: 24 verts, 15 quads");

    // Two facing squares (bottom facing down, top facing up) bridge into a closed box.
    EditMesh box = make_plane(1.0f, 1);
    MeshSelection tmp;
    flip_normals(box, [&] { MeshSelection a; a.faces = {0}; return a; }());
    append_mesh(box, make_plane(1.0f, 1), glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 1)), tmp);
    MeshSelection bs;
    bs.mode = SelectMode::Edge;
    for (const auto& e : box.edges()) bs.edges.insert(e);
    std::string err;
    const bool bridged = bridge_edge_loops(box, bs, &err);
    expect(bridged && box.faces.size() == 6 && closed_and_consistent(box) && euler(box) == 2,
           "bridging two facing squares closes a box (" + err + "; faces " + std::to_string(box.faces.size()) + ", closed " +
               std::to_string(closed_and_consistent(box)) + ")");
}

/** @brief Loop Cut: counts, closure, UVs, terminal triangles, and Edge Slide. */
COOPA_TEST(loop_cut_and_edge_slide) {
    EditMesh m = make_cube();
    MeshSelection sel;
    LoopCutResult r = loop_cut(m, sel, make_edge(0, 4), 1);
    expect(r.ok && m.positions.size() == 12 && m.faces.size() == 10, "one cut around a cube: 12 verts, 10 faces");
    expect(closed_and_consistent(m) && euler(m) == 2, "the cut cube stays closed and consistently wound");
    bool mid = true, uv_mid = false;
    for (uint32_t v = 8; v < 12; ++v) mid = mid && std::abs(m.positions[v].z) < 1e-5f;
    for (const auto& f : m.faces) for (const auto& c : f.corners) if (c.v >= 8 && (std::abs(c.uv.x - 0.5f) < 1e-5f || std::abs(c.uv.y - 0.5f) < 1e-5f)) uv_mid = true;
    expect(mid && uv_mid, "new vertices sit halfway, with halfway UVs");
    expect(sel.mode == SelectMode::Edge && sel.edges.size() == 4, "the new loop is selected");

    // Edge Slide: t = +-1 puts the loop on either rail.
    EditMesh base = m;
    apply_edge_slide(m, r.rails, 1.0f);
    const float z_up = m.positions[8].z;
    m = base;
    apply_edge_slide(m, r.rails, -1.0f);
    expect(std::abs(std::abs(z_up) - 0.5f) < 1e-5f && std::abs(m.positions[8].z + z_up) < 1e-5f, "slide moves to either end");
    m = base;
    auto rails = edge_slide_rails(m, sel.edges);
    expect(rails && rails->size() == 4, "G G builds a rail for each loop vertex");
    if (rails) {
        apply_edge_slide(m, *rails, 0.5f);
        bool moved = true;
        for (uint32_t v = 8; v < 12; ++v) moved = moved && std::abs(std::abs(m.positions[v].z) - 0.25f) < 1e-5f;
        expect(moved, "sliding halfway moves the loop to z = +-0.25");
    }
    std::set<Edge> branch = {make_edge(0, 1), make_edge(0, 2), make_edge(0, 4)};
    std::string err;
    expect(!edge_slide_rails(make_cube(), branch, &err) && !err.empty(), "a branching selection can't slide");

    EditMesh m3 = make_cube();
    expect(loop_cut(m3, sel, make_edge(0, 4), 3).ok && m3.faces.size() == 18 && m3.positions.size() == 20 && closed_and_consistent(m3),
           "three cuts: 20 verts, 18 faces, closed");

    EditMesh sph = make_uv_sphere(0.5f, 8, 4);
    const LoopCutResult rs = loop_cut(sph, sel, make_edge(1, 2), 1);   // a ring-1 edge: the ring runs pole to pole
    size_t cap_quads = 0;
    for (const auto& f : sph.faces) {
        bool touches_pole = false;
        for (const auto& c : f.corners) touches_pole |= c.v == 0 || c.v == static_cast<uint32_t>(sph.positions.size() - 1) - 0 ? false : false;
        (void)touches_pole;
    }
    for (size_t f = 0; f < sph.faces.size(); ++f) {
        for (const auto& c : sph.faces[f].corners) {
            if (std::abs(std::abs(sph.positions[c.v].z) - 0.5f) < 1e-5f && sph.faces[f].corners.size() == 4) { ++cap_quads; break; }
        }
    }
    expect(rs.ok && closed_and_consistent(sph) && euler(sph) == 2, "a cut through a sphere's caps stays closed");
    expect(cap_quads == 2, "the two cap triangles it crosses become quads (got " + std::to_string(cap_quads) + ")");
}

COOPA_TEST(box_uvs_and_select_linked) {
    EditMesh m = make_cube();
    uv_box_project(m, MeshSelection{}, 1.0f);
    bool in_range = true;
    for (const auto& f : m.faces) for (const auto& c : f.corners) in_range &= std::abs(c.uv.x) <= 0.5001f && std::abs(c.uv.y) <= 0.5001f;
    expect(in_range, "box UVs of a unit cube span its faces at 1 UV/unit");
    MeshSelection sel;
    sel.mode = SelectMode::Vertex;
    sel.verts.insert(0);
    select_linked(m, sel);
    expect(sel.verts.size() == 8 && sel.faces.size() == 6, "select linked grows to the whole cube");
    convert_selection(m, sel, SelectMode::Face);
    expect(sel.mode == SelectMode::Face && sel.faces.size() == 6, "converting to face mode keeps coverage");
}

} // namespace toy::editor::testing
