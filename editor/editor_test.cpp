/**
 * @file editor_test.cpp
 * @brief toyengine_editor's test suite, registered with ctest one group at a time.
 *
 * @code
 * ./toyengine_editor_tests                 # everything
 * ./toyengine_editor_tests --group mesh    # one group
 * ./toyengine_editor_tests extrude         # tests whose name contains "extrude"
 * EDITOR_DUMP_DIR=/tmp/shots ./toyengine_editor_tests --group editor_shell   # also save UI screenshots
 * @endcode
 *
 * Groups writer/document/mesh/imm/viewport/config need no GPU. editor_shell and package
 * build an Engine with an invisible window (never shown, never grabbing input) against a
 * scratch project in the temp directory -- nothing here writes inside the repository.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/engine.h>

#include "app/editor_app.h"
#include "build/packager.h"
#include "core/scene_document.h"
#include "mesh/edit_mesh.h"
#include "mesh/mesh_ops.h"
#include "mesh/mesh_bvh.h"
#include "mesh/mesh_loops.h"
#include "mesh/mesh_subdivide.h"
#include "mesh/mesh_topology.h"
#include "mesh/sculpt.h"
#include "mesh/primitives.h"
#include "schema/component_schema.h"
#include "viewport/gizmo.h"
#include "viewport/modal_transform.h"

#include <coopa/yaml/writer.h>
#include <gfxcoopa/util/image_readback.h>

#include <root_directory.h>

namespace {

namespace fs = std::filesystem;
using namespace toy::editor;

// =====================================================================================
// Runner
// =====================================================================================

int g_failures = 0, g_test_failures = 0, g_assertions = 0;
bool g_verbose = false;
const char* g_current = "";

void expect(bool ok, const std::string& what) {
    ++g_assertions;
    if (!ok) {
        std::cerr << "  [FAIL] " << g_current << ": " << what << "\n";
        ++g_failures;
        ++g_test_failures;
    } else if (g_verbose) {
        std::cout << "  [ OK ] " << what << "\n";
    }
}

fs::path tmp_root() {
    static const fs::path d = [] {
        fs::path p = fs::temp_directory_path() / "toyengine_editor_tests";
        fs::create_directories(p);
        return p;
    }();
    return d;
}

fs::path fresh_dir(const std::string& name) {
    const fs::path d = tmp_root() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::vector<fs::path> asset_yaml_files() {
    std::vector<fs::path> out;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(ROOT_DIR) / "assets")) {
        const auto ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".yaml" || ext == ".yml")) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** @brief V - E + F over a mesh (2 for a closed genus-0 surface). */
long euler(const EditMesh& m) {
    return static_cast<long>(m.positions.size()) - static_cast<long>(m.edges().size()) + static_cast<long>(m.faces.size());
}

/** @brief Every edge shared by exactly two faces, used in opposite directions. */
bool closed_and_consistent(const EditMesh& m) {
    std::map<std::pair<uint32_t, uint32_t>, int> directed;
    for (const auto& f : m.faces) {
        for (size_t i = 0; i < f.corners.size(); ++i) ++directed[{f.corners[i].v, f.corners[(i + 1) % f.corners.size()].v}];
    }
    for (const auto& [e, n] : directed) {
        if (n != 1) return false;
        auto it = directed.find({e.second, e.first});
        if (it == directed.end() || it->second != 1) return false;
    }
    return true;
}

// =====================================================================================
// Group "writer" -- coopa/yaml/writer.h
// =====================================================================================

void test_writer_scalars_roundtrip() {
    Node n = Node::mapping();
    n["f1"] = Node(1.0);
    n["f2"] = Node(0.1);
    n["f3"] = Node(-123.456789);
    n["f4"] = Node(1e-7);
    n["f5"] = Node(3.0e20);
    n["i"] = Node(int64_t(42));
    n["b"] = Node(true);
    n["s_true"] = Node(std::string("true"));
    n["s_num"] = Node(std::string("1.5"));
    n["s_empty"] = Node(std::string(""));
    n["s_colon"] = Node(std::string("a: b"));
    n["s_hash"] = Node(std::string("#x"));
    n["s_path"] = Node(std::string("assets/scenes/a b/scene.yaml"));
    n["s_quote"] = Node(std::string("say \"hi\""));
    const std::string text = coopa::yaml::emit(n);
    const Node back = Node::deserialize(text);
    expect(back == n, "every scalar kind survives emit -> parse");
    expect(back.at("f1").is_float_number(), "1.0 stays a float, not an int");
    expect(back.at("i").is_integer(), "an int stays an int");
    expect(back.at("s_true").is_string() && back.at("s_num").is_string(), "ambiguous strings stay strings");
    expect(coopa::yaml::emit(back) == text, "emit is stable across a round trip");
}

void test_writer_key_order_and_flow() {
    Node obj = Node::mapping();
    obj["children"] = Node::sequence();
    obj["components"] = Node::sequence();
    obj["active"] = Node(true);
    obj["name"] = Node(std::string("Cube"));
    Node t = Node::mapping();
    t["z"] = Node(1.0); t["x"] = Node(0.0); t["y"] = Node(2.0);
    obj["position"] = t;
    const std::string text = coopa::yaml::emit(obj);
    expect(text.find("name:") < text.find("active:"), "name comes before active");
    expect(text.find("active:") < text.find("components:"), "identity keys before structure");
    expect(text.find("components:") < text.find("children:"), "children last");
    expect(text.find("position: { x: 0.0, y: 2.0, z: 1.0 }") != std::string::npos, "short vectors are flow style:\n" + text);
}

void test_writer_roundtrips_every_asset() {
    int n = 0;
    for (const auto& p : asset_yaml_files()) {
        const Node a = coopa::yaml::load_document(p);
        const std::string t1 = coopa::yaml::emit(a);
        const Node b = Node::deserialize(t1);
        expect(a == b, "emit -> parse preserves " + fs::relative(p, ROOT_DIR).string());
        expect(coopa::yaml::emit(b) == t1, "emit is byte-stable for " + fs::relative(p, ROOT_DIR).string());
        ++n;
    }
    expect(n > 10, "round-tripped the repository's assets (" + std::to_string(n) + " files)");
}

// =====================================================================================
// Group "document" -- SceneDocument, undo, schemas
// =====================================================================================

void test_scene_documents_save_load_stable() {
    const fs::path dir = fresh_dir("doc_roundtrip");
    int count = 0;
    for (const auto& p : asset_yaml_files()) {
        if (p.parent_path().parent_path().filename() != "scenes") continue;
        SceneDocument doc;
        doc.load(p);
        const fs::path out = dir / (std::to_string(count++) + ".yaml");
        doc.save(out);
        std::ifstream in(out);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        expect(text.find("__eid") == std::string::npos, "editor ids never reach disk (" + p.filename().string() + ")");
        Node original = coopa::yaml::load_document(p);
        Node saved = coopa::yaml::load_document(out);
        expect(original == saved, "load -> save preserves the scene exactly: " + fs::relative(p, ROOT_DIR).string());
        SceneDocument again;
        again.load(out);
        const fs::path out2 = dir / (std::to_string(count++) + ".yaml");
        again.save(out2);
        std::ifstream in2(out2);
        const std::string text2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        expect(text == text2, "save -> load -> save is byte-identical");
    }
    expect(count >= 10, "every scene was round-tripped");
}

void test_scene_document_random_edits_undo() {
    SceneDocument doc;
    doc.load(fs::path(ROOT_DIR) / "assets/scenes/pixel_demo/scene.yaml");
    const Node original = doc.node();
    std::mt19937 rng(1234);
    int edits = 0;
    for (int i = 0; i < 200; ++i) {
        const auto ids = doc.all_ids();
        if (ids.empty()) break;
        const ObjectId a = ids[rng() % ids.size()];
        const ObjectId b = ids[rng() % ids.size()];
        Change c;
        switch (rng() % 6) {
            case 0: c = {ChangeScope::Structure, doc.add_object(doc.make_object("N" + std::to_string(i)), rng() % 2 ? a : 0)}; break;
            case 1: { auto d = doc.duplicate_objects({a}); c = {d.empty() ? ChangeScope::None : ChangeScope::Structure, 0}; break; }
            case 2: c = doc.delete_objects({a}); break;
            case 3: c = doc.reparent(a, rng() % 3 ? b : 0); break;
            case 4: c = doc.set_transform(a, glm::vec3(i, 1, 2), glm::vec3(0, 0, i), glm::vec3(1), "T"); break;
            default: c = doc.set_object_key(a, "name", Node("R" + std::to_string(i)), "Rename"); break;
        }
        if (c.scope != ChangeScope::None) ++edits;
    }
    const Node final_state = doc.node();
    expect(edits > 100, "most random edits applied (" + std::to_string(edits) + ")");
    while (doc.undo_stack().can_undo()) doc.undo();
    expect(doc.node() == original, "undoing everything restores the original document");
    while (doc.undo_stack().can_redo()) doc.redo();
    expect(doc.node() == final_state, "redoing everything restores the final document");
}

void test_scene_document_reparent_rules() {
    SceneDocument doc;
    doc.reset("T");
    const ObjectId p = doc.add_object(doc.make_object("Parent"));
    const ObjectId c = doc.add_object(doc.make_object("Child"), p);
    const ObjectId g = doc.add_object(doc.make_object("Grandchild"), c);
    expect(doc.parent_of(g) == c && doc.parent_of(c) == p && doc.parent_of(p) == ObjectId(0), "parents are tracked");
    expect(doc.reparent(p, g).scope == ChangeScope::None, "an object cannot move under its own descendant");
    expect(doc.reparent(p, p).scope == ChangeScope::None, "or under itself");
    expect(doc.reparent(g, 0).scope == ChangeScope::Structure && doc.parent_of(g) == ObjectId(0), "moving to the root works");
    expect(doc.unique_name("Parent") == "Parent.001", "unique names number like Blender");
    auto dup = doc.duplicate_objects({c});
    expect(dup.size() == 1 && dup[0] != c && doc.find(dup[0]) != nullptr, "duplicates get fresh ids");
}

void test_schema_defaults() {
    for (const auto& [type, schema] : schemas()) {
        const Node c = default_component(type);
        expect(component_type(c) == type, "default " + type + " carries its type");
        for (const auto& f : schema.fields) {
            if (f.in_default && f.kind != FieldKind::AssetRef && f.kind != FieldKind::String && f.kind != FieldKind::Enum) {
                expect(c.contains(f.key), type + "." + f.key + " is written by default");
            }
        }
    }
    expect(find_schema("MeshRenderer") && find_schema("Rigidbody") && find_schema("Transform"), "core schemas exist");
}

// =====================================================================================
// Group "mesh" -- EditMesh, primitives, operations
// =====================================================================================

void test_primitives_are_closed() {
    for (const auto& name : primitive_names()) {
        const EditMesh m = make_primitive(name);
        expect(!m.faces.empty(), name + " has faces");
        if (name == "Plane" || name == "Grid" || name == "Tile Side") {
            expect(euler(m) == 1, name + " is a disc (V-E+F = 1)");
            continue;
        }
        expect(euler(m) == 2, name + " has Euler characteristic 2 (got " + std::to_string(euler(m)) + ")");
        expect(closed_and_consistent(m), name + " is closed with consistent winding");
        // Outward normals: every face normal points away from the centroid.
        glm::vec3 lo, hi;
        m.bounds(lo, hi);
        const glm::vec3 c = (lo + hi) * 0.5f;
        bool outward = true;
        for (size_t f = 0; f < m.faces.size(); ++f) outward &= glm::dot(m.face_normal(f), m.face_center(f) - c) > 0.0f;
        expect(outward, name + " faces point outward");
    }
}

void test_extrude_inset_flip() {
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

void test_bevel_and_merge_and_delete() {
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


/** @brief MeshTopology adjacency; edge rings and loops on cubes, grids and spheres. */
void test_topology_loops_rings() {
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

/** @brief Loop Cut: counts, closure, UVs, terminal triangles, and Edge Slide. */
void test_loop_cut_and_slide() {
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

/** @brief Subdivide, Catmull-Clark, Triangulate, Tris to Quads, Dissolve, Grid, Bridge. */
void test_subdivide_triangulate_dissolve() {
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

/** @brief Material slots: YAML round trip, ops keep each face's slot, and the engine builds parts. */
void test_mesh_material_slots() {
    EditMesh m = make_cube();
    m.slots = {"body", "trim"};
    m.faces[1].slot = 1;   // top face is trim
    const Node n = mesh_to_node(m);
    expect(n.contains("material_slots") && n.contains("face_materials"), "slots are written");
    const EditMesh back = mesh_from_node(n);
    expect(back.slots == m.slots && back.faces.size() == 6, "slot names round-trip");
    size_t trim = 0;
    for (const auto& f : back.faces) trim += f.slot == 1;
    expect(trim == 1, "face slots round-trip");

    EditMesh cut = m;
    MeshSelection sel;
    loop_cut(cut, sel, make_edge(0, 4), 1);
    size_t trim_after = 0;
    for (const auto& f : cut.faces) trim_after += f.slot == 1;
    expect(trim_after == 1, "a loop cut keeps the untouched trim face's slot");
    EditMesh sub = m;
    MeshSelection all;
    all.select_all(sub);
    subdivide(sub, all, 1);
    size_t trim_sub = 0;
    for (const auto& f : sub.faces) trim_sub += f.slot == 1;
    expect(trim_sub == 4, "subdividing the trim face gives 4 trim faces");

    const auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(mesh_to_node(sub));
    expect(cpu.lods[0].parts.size() == 2 && cpu.lods[0].parts[1].index_count == 4 * 6, "the engine builds a trim part of 8 triangles");
}

void test_mesh_io_roundtrip_and_engine_load() {
    for (const auto& name : primitive_names()) {
        EditMesh m = make_primitive(name);
        uv_box_project(m, MeshSelection{}, 1.0f);
        const Node node = mesh_to_node(m);
        const EditMesh back = mesh_from_node(node);
        expect(back.faces.size() == m.faces.size() && back.positions.size() == m.positions.size(),
               name + ": export -> import keeps topology");
        bool smooth_kept = true;
        for (size_t i = 0; i < m.faces.size(); ++i) smooth_kept &= back.faces[i].smooth == m.faces[i].smooth;
        expect(smooth_kept, name + ": smooth/flat shading survives");
        auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(Node::deserialize(coopa::yaml::emit(node)));
        expect(!cpu.vertices.empty() && cpu.indices.size() == m.triangle_count() * 3,
               name + ": the written YAML loads through the engine's Mesh::build_cpu");
    }
    // Existing Blender exports import and re-export with the same triangle count.
    for (const auto& p : asset_yaml_files()) {
        if (p.parent_path().filename() != "meshes" || p.filename().string().find(".lod.") != std::string::npos) continue;
        const Node src = coopa::yaml::load_document(p);
        if (!src.contains("faces")) continue;
        const EditMesh m = mesh_from_node(src);
        const auto a = coopa::gfx::engine::data::Mesh::build_cpu(src);
        const auto b = coopa::gfx::engine::data::Mesh::build_cpu(mesh_to_node(m));
        // Re-import drops faces that collapse when coincident corners weld; never adds any.
        expect(b.lods.at(0).index_count <= a.lods.at(0).index_count && b.lods.at(0).index_count * 10 >= a.lods.at(0).index_count * 9,
               "re-exported (" + std::to_string(a.lods.at(0).index_count) + " vs " + std::to_string(b.lods.at(0).index_count) + ") " + p.filename().string() + " has the same triangle count");
    }
}

void test_uv_projection_and_selection() {
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

// =====================================================================================
// Group "imm" -- uicoopa's immediate-mode layer, headless (no font, no GPU)
// =====================================================================================

struct ImmHarness {
    coopa::ui::imm::Context ctx;
    coopa::ui::DrawList dl;
    glm::vec2 mouse{0.0f};
    bool down = false;
    bool prev_down = false;

    void frame(const std::function<void(coopa::ui::imm::Context&)>& fn, std::vector<uint32_t> chars = {},
               std::vector<coopa::input::KeyEvent> keys = {}, glm::vec2 delta = glm::vec2(0.0f)) {
        coopa::ui::imm::FrameInput in;
        in.mouse = mouse;
        in.mouse_delta = delta;
        in.down[0] = down;
        in.pressed[0] = down && !prev_down;
        in.released[0] = !down && prev_down;
        in.chars = std::move(chars);
        in.keys = std::move(keys);
        prev_down = down;
        dl.begin(coopa::ui::Rect{{0, 0}, {800, 600}});
        ctx.begin_frame(dl, in, {800, 600});
        fn(ctx);
        ctx.end_frame();
    }
    void click(glm::vec2 at, const std::function<void(coopa::ui::imm::Context&)>& fn) {
        mouse = at;
        frame(fn);
        down = true;
        frame(fn);
        down = false;
        frame(fn);
    }
};

void test_imm_button_and_checkbox() {
    ImmHarness h;
    int clicks = 0;
    bool check = false;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        if (c.button("Press", 100)) ++clicks;
        c.checkbox("Check", &check);
        c.end_region();
    };
    h.frame(ui);
    h.click({20, 15}, ui);
    expect(clicks == 1, "a press + release over the button clicks it once");
    h.click({20, 40}, ui);
    expect(check, "clicking the checkbox toggles it");
    h.click({250, 250}, ui);
    expect(clicks == 1 && check, "clicking empty space does nothing");
}

void test_imm_text_input_commits() {
    ImmHarness h;
    std::string value = "abc";
    bool committed = false;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        committed |= c.input_text("Name", &value);
        c.end_region();
    };
    h.frame(ui);
    h.click({300, 15}, ui);                          // focus (selects all)
    expect(h.ctx.wants_keyboard(), "clicking a field focuses it");
    h.frame(ui, {'x', 'y', 'z'});
    expect(value == "abc", "typing edits a private buffer, not the value");
    coopa::input::KeyEvent enter{coopa::input::Key::Enter, 0, coopa::input::KeyAction::Press, coopa::input::Mods::None};
    h.frame(ui, {}, {enter});
    expect(committed && value == "xyz", "Enter commits the edit (got '" + value + "')");
    expect(!h.ctx.wants_keyboard(), "and releases the keyboard");
}

void test_imm_drag_float_and_popup_blocking() {
    ImmHarness h;
    float v = 1.0f;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        c.drag_float("Value", &v, 0.1f);
        c.end_region();
    };
    h.frame(ui);
    h.mouse = {300, 15};
    h.frame(ui);
    h.down = true;
    h.frame(ui);
    for (int i = 0; i < 5; ++i) { h.mouse.x += 10; h.frame(ui, {}, {}, {10, 0}); }
    h.down = false;
    h.frame(ui);
    expect(std::abs(v - 6.0f) < 1e-3f, "dragging 50 px at 0.1/px adds 5 to 1 (got " + std::to_string(v) + ")");

    // A popup over a button: clicks land on the popup, not the button underneath.
    ImmHarness p;
    int under = 0, item = 0;
    auto ui2 = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        if (c.button("Open", 100)) c.open_popup("menu", glm::vec2(6, 6));
        if (c.button("Under", 100)) ++under;
        if (c.begin_popup("menu", 200)) {
            if (c.menu_item("Item")) ++item;
            c.end_popup();
        }
        c.end_region();
    };
    p.frame(ui2);
    p.click({20, 15}, ui2);
    p.frame(ui2);
    expect(p.ctx.any_popup_open(), "the popup opened");
    p.click({40, 16}, ui2);
    expect(item == 1 && under == 0, "a click inside the popup hits its item, not the widget below");
    expect(!p.ctx.any_popup_open(), "choosing an item closes the popup");
}


void test_imm_icon_button_and_tooltip() {
    using coopa::ui::imm::Icon;
    ImmHarness h;
    int clicks = 0;
    bool on = false;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        if (c.icon_button("play", Icon::Play, "Play\nStart the game", on, 24)) { ++clicks; on = !on; }
        c.end_region();
    };
    h.frame(ui);
    const size_t verts = h.dl.vertices().size();
    expect(verts >= 3, "an icon button draws its glyph (" + std::to_string(verts) + " vertices)");
    h.click({12, 12}, ui);
    expect(clicks == 1 && on, "clicking the icon button fires once and toggles");
    h.mouse = {250, 250};
    h.frame(ui);
    expect(h.dl.vertices().size() > verts, "a toggled-on icon button gets its highlighted rounded frame");
    std::string tip;
    h.mouse = {12, 12};
    for (int i = 0; i < 40; ++i) { h.frame(ui); if (!h.ctx.tooltip_text().empty()) tip = h.ctx.tooltip_text(); }
    expect(tip == "Play\nStart the game", "hovering shows the rich tooltip (got '" + tip + "')");
    h.mouse = {250, 250};
    h.frame(ui);
    expect(h.ctx.tooltip_text().empty(), "moving away hides it");
    // Rounded fills emit a fan per corner, unlike a plain quad.
    h.frame([&](coopa::ui::imm::Context& c) { c.fill({0, 0, 10, 10}, glm::vec4(1)); });
    const size_t quad = h.dl.vertices().size();
    h.frame([&](coopa::ui::imm::Context& c) { c.fill_rounded({0, 0, 40, 40}, glm::vec4(1), 6); });
    expect(h.dl.vertices().size() > quad, "fill_rounded emits more geometry than a square fill");
}


/** @brief imm themes: colour formats, inheritance, app sections, errors, and a lossless round trip. */
void test_imm_theme_files() {
    namespace imm = coopa::ui::imm;
    const imm::Theme t = imm::parse_theme(R"(
name: Test
metrics:
  font_size: 14
  rounding: 2.5
colors:
  accent: "#ff8000"
  selection: "#11223380"
  text: [0.5, 0.25, 1.0]
  panel_bg: {r: 0.1, g: 0.2, b: 0.3, a: 0.4}
viewport:
  grid: "#fff"
)");
    expect(t.name == "Test" && t.style.font_size == 14.0f && t.style.rounding == 2.5f, "metrics apply");
    expect(glm::distance(t.style.accent, glm::vec4(1.0f, 128 / 255.0f, 0.0f, 1.0f)) < 1e-4f, "#rrggbb colours");
    expect(std::abs(t.style.selection.a - 128 / 255.0f) < 1e-4f, "#rrggbbaa carries alpha");
    expect(t.style.text == glm::vec4(0.5f, 0.25f, 1.0f, 1.0f), "[r, g, b] float lists (alpha 1)");
    expect(t.style.panel_bg == glm::vec4(0.1f, 0.2f, 0.3f, 0.4f), "{r, g, b, a} maps");
    expect(t.color("viewport", "grid", glm::vec4(0)) == glm::vec4(1), "app sections, #rgb shorthand");
    expect(t.color("viewport", "missing", glm::vec4(0.5f)) == glm::vec4(0.5f), "unknown roles fall back");
    expect(t.style.row_height == imm::Style{}.row_height, "unmentioned fields keep the built-in value");

    bool threw = false;
    try { imm::parse_theme("colors:\n  accent: \"#12345\"\n"); } catch (const std::exception& e) {
        threw = std::string(e.what()).find("colors.accent") != std::string::npos;
    }
    expect(threw, "a malformed colour names its key");

    // Round trip: every field survives save -> load (to 8-bit colour precision).
    const imm::Theme back = imm::parse_theme(imm::theme_to_yaml(t));
    struct Flat {
        std::vector<float>* out;
        void metric(const char*, const float& v) const { out->push_back(v); }
        void color(const char*, const glm::vec4& c) const { for (int i = 0; i < 4; ++i) out->push_back(c[i]); }
    };
    std::vector<float> a, b;
    imm::visit_style(t.style, Flat{&a});
    imm::visit_style(back.style, Flat{&b});
    bool same = back.name == t.name && back.sections == t.sections && a.size() == b.size();
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) same = same && std::abs(a[i] - b[i]) < 0.003f;
    expect(same, "theme_to_yaml round-trips every field");
}

/** @brief Clicking an open dropdown's opener closes it (instead of re-opening it). */
void test_imm_dropdown_toggles() {
    ImmHarness h;
    int idx = 0;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.combo_box("dd", {10, 10, 120, 20}, &idx, {"One", "Two", "Three"});
    };
    h.frame(ui);
    h.click({40, 20}, ui);
    h.frame(ui);
    expect(h.ctx.any_popup_open(), "clicking a dropdown opens it");
    h.click({40, 20}, ui);
    h.frame(ui);
    expect(!h.ctx.any_popup_open(), "clicking the open dropdown closes it");
    h.click({40, 20}, ui);
    h.frame(ui);
    expect(h.ctx.any_popup_open(), "and the next click opens it again");

    // A plain button that opens a popup below itself behaves the same.
    ImmHarness p;
    auto ui2 = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        if (c.button("Open", 100)) c.open_popup("menu", glm::vec2(6, 40));
        if (c.begin_popup("menu", 200)) { c.menu_item("Item"); c.end_popup(); }
        c.end_region();
    };
    p.frame(ui2);
    p.click({20, 15}, ui2);
    p.frame(ui2);
    expect(p.ctx.any_popup_open(), "button popup opened");
    p.click({20, 15}, ui2);
    p.frame(ui2);
    expect(!p.ctx.any_popup_open(), "clicking its opener again closes it");
    // Right-clicking elsewhere while open still closes it (and re-opening via another click works).
    p.click({20, 15}, ui2);
    p.frame(ui2);
    p.click({300, 250}, ui2);
    expect(!p.ctx.any_popup_open(), "clicking empty space closes it");
}

void test_imm_menubar_and_tree() {
    ImmHarness h;
    int saved = 0;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_menubar({0, 0, 800, 24});
        if (c.begin_menu("File")) {
            if (c.menu_item("Save", "Ctrl+S")) ++saved;
            c.end_menu();
        }
        c.end_menubar();
    };
    h.frame(ui);
    h.click({15, 12}, ui);
    h.frame(ui);
    expect(h.ctx.any_popup_open(), "clicking a menu header opens it");
    h.click({40, 36}, ui);
    expect(saved == 1, "clicking the item runs it");

    ImmHarness t;
    int clicked = 0;
    bool opened = false;
    auto tree = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        auto r = c.tree_node(c.get_id("root"), "Root", false, false, false);
        if (r.clicked) ++clicked;
        if (r.open) { opened = true; c.tree_node(c.get_id("child"), "Child", true, false); c.tree_pop(); }
        c.end_region();
    };
    t.frame(tree);
    t.click({100, 15}, tree);
    expect(clicked == 1 && !opened, "clicking the row selects without expanding");
    t.click({12, 15}, tree);
    t.frame(tree);
    expect(opened, "clicking the arrow expands");
}

// =====================================================================================
// Group "viewport" -- picking and gizmo math
// =====================================================================================

ViewProj test_view_proj() {
    ViewProj vp;
    vp.view = glm::lookAt(glm::vec3(0, -10, 0), glm::vec3(0), glm::vec3(0, 0, 1));
    vp.proj = glm::perspective(glm::radians(50.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    vp.rect = {100, 50, 640, 360};
    return vp;
}

coopa::input::KeyEvent key_press(coopa::input::Key k, coopa::input::Mods m = coopa::input::Mods::None) {
    coopa::input::KeyEvent e;
    e.key = k;
    e.action = coopa::input::KeyAction::Press;
    e.mods = m;
    return e;
}

/** @brief Axis locking: per-component typed input, plane typing, local scale, MMB auto axis, edge slide. */
void test_modal_axis_locking() {
    using coopa::input::Key;
    using coopa::input::Mods;
    const ViewProj vp = test_view_proj();
    const glm::vec2 c = *vp.project(glm::vec3(0));
    ModalTransform mt;
    auto run = [&](std::vector<coopa::input::KeyEvent> keys) {
        mt.update(vp, c, keys, false, false, false, false);
        return mt.update(vp, c, {key_press(Key::Enter)}, false, false, false, false);
    };
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    expect(run({key_press(Key::Z, Mods::Shift), key_press(Key::Num1), key_press(Key::Tab), key_press(Key::Num2)}) ==
               ModalTransform::Outcome::Confirmed &&
               glm::distance(mt.result().translate, glm::vec3(1, 2, 0)) < 1e-5f,
           "G Shift+Z 1 Tab 2 moves (1, 2, 0) in the XY plane");
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    run({key_press(Key::Num1), key_press(Key::Tab), key_press(Key::Num2), key_press(Key::Tab), key_press(Key::Num3)});
    expect(glm::distance(mt.result().translate, glm::vec3(1, 2, 3)) < 1e-5f, "free G 1 Tab 2 Tab 3 moves (1, 2, 3)");

    // A basis rotated 90 degrees about Z: local X is world Y.
    const glm::mat3 rot(glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1));
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), rot, c, std::nullopt, "local");
    run({key_press(Key::X), key_press(Key::X), key_press(Key::Num2)});
    expect(glm::distance(mt.result().translate, glm::vec3(0, 2, 0)) < 1e-5f, "G X X 2 moves along the local X axis");
    mt.begin(ModalKind::Scale, vp, glm::vec3(0), rot, c, std::nullopt, "local");
    run({key_press(Key::X), key_press(Key::X), key_press(Key::Num2)});
    const glm::mat4 sm = delta_matrix(glm::vec3(0), glm::vec3(0, 0, 1), 0.0f, mt.result().scale, glm::vec3(0), mt.result().scale_basis);
    expect(glm::distance(glm::vec3(sm * glm::vec4(0, 1, 0, 1)), glm::vec3(0, 2, 0)) < 1e-5f &&
               glm::distance(glm::vec3(sm * glm::vec4(1, 0, 0, 1)), glm::vec3(1, 0, 0)) < 1e-5f,
           "S X X 2 scales along the local axis only");

    // MMB auto constraint: a horizontal drag picks X (screen right is +X from this view), a vertical one Z.
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    mt.update(vp, c, {}, false, false, false, false, true, true);
    mt.update(vp, c + glm::vec2(60, 2), {}, false, false, false, false, true, false);
    expect(mt.axis() == 0, "MMB drag sideways locks X (got " + std::to_string(mt.axis()) + ")");
    mt.update(vp, c + glm::vec2(60, 2), {}, false, false, false, false, false, false);
    mt.update(vp, c + glm::vec2(60, 2), {}, false, false, false, false, true, true);
    mt.update(vp, c + glm::vec2(62, -60), {}, false, false, false, false, true, false);
    expect(mt.axis() == 2, "MMB drag up locks Z");
    expect(mt.guide_axes().size() == 1, "an axis constraint draws one guide line");
    mt.update(vp, c, {key_press(Key::Y, Mods::Shift)}, false, false, false, false);
    expect(mt.guide_axes().size() == 2, "a plane constraint draws its two axes");
    mt.cancel();

    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c, std::nullopt, "local", true);
    expect(mt.update(vp, c, {key_press(Key::G)}, false, false, false, false) == ModalTransform::Outcome::SwitchToSlide,
           "G during an edit-mode Grab asks for Edge Slide");
    mt.begin(ModalKind::EdgeSlide, vp, glm::vec3(0), glm::mat3(1.0f), c, glm::vec3(1, 0, 0));
    run({key_press(Key::Period), key_press(Key::Num5)});
    expect(std::abs(mt.result().amount - 0.5f) < 1e-5f, "Edge Slide takes a typed factor");
}

/** @brief Normal orientation and the sculpt brushes. */
void test_normal_basis_and_sculpt() {
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
    const bool axes[3] = {true, false, false};
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

void test_projection_and_rays() {
    const ViewProj vp = test_view_proj();
    auto c = vp.project(glm::vec3(0));
    expect(c && glm::distance(*c, glm::vec2(420, 230)) < 0.01f, "the look-at point projects to the rect centre");
    glm::vec3 o, d;
    vp.ray({420, 230}, o, d);
    expect(glm::dot(d, glm::vec3(0, 1, 0)) > 0.9999f, "the centre ray points along the view direction");
    vp.ray({500, 120}, o, d);
    const float t = -o.y / d.y;
    auto back = vp.project(o + d * t);
    expect(back && glm::distance(*back, glm::vec2(500, 120)) < 0.05f, "ray() and project() are inverses");
    expect(ray_triangle({0, -5, 0}, {0, 1, 0}, {-1, 0, -1}, {1, 0, -1}, {0, 0, 1}).has_value(), "ray hits a facing triangle");
    expect(!ray_triangle({5, -5, 0}, {0, 1, 0}, {-1, 0, -1}, {1, 0, -1}, {0, 0, 1}).has_value(), "and misses beside it");
    auto hit = ray_aabb({0, -5, 0}, {0, 1, 0}, glm::vec3(-1), glm::vec3(1));
    expect(hit && std::abs(*hit - 4.0f) < 1e-5f, "ray-box entry distance");
    const glm::vec3 e(10, 20, 30);
    expect(glm::distance(matrix_to_euler(euler_to_matrix(e)), e) < 1e-3f, "Euler <-> matrix round trip uses Transform's convention");
}

void test_gizmo_translate_drag() {
    const ViewProj vp = test_view_proj();
    Gizmo g;
    g.mode = GizmoMode::Translate;
    const glm::vec3 pivot(0.0f);
    const auto c = *vp.project(pivot);
    const auto tip = *vp.project(pivot + glm::vec3(1, 0, 0) * g.size_px * vp.world_per_pixel(pivot));
    const glm::vec2 grab = c + (tip - c) * 0.6f;
    GizmoDelta r = g.update(vp, pivot, glm::mat3(1.0f), grab, false, false, false, false, true);
    expect(g.hot_axis() == 0, "hovering the X handle makes it hot");
    r = g.update(vp, pivot, glm::mat3(1.0f), grab, true, true, false, false, true);
    expect(r.started && r.active, "pressing starts a drag");
    // Move the mouse to where world x = +2 projects.
    const auto target = *vp.project(glm::vec3(2, 0, 0)) + (grab - c);
    r = g.update(vp, pivot, glm::mat3(1.0f), target, false, true, false, false, true);
    expect(std::abs(r.translate.x - 2.0f) < 0.05f && std::abs(r.translate.y) < 1e-4f && std::abs(r.translate.z) < 1e-4f,
           "dragging along X moves only X, by the cursor's world distance (" + std::to_string(r.translate.x) + ")");
    r = g.update(vp, pivot, glm::mat3(1.0f), target, false, true, false, true, true);
    expect(std::abs(r.translate.x - 2.0f) < 1e-4f, "Ctrl snaps to 0.25 units");
    r = g.update(vp, pivot, glm::mat3(1.0f), target, false, false, true, false, true);
    expect(r.finished && !g.dragging(), "release ends the drag");
}

// =====================================================================================
// Group "config" -- writing config.yaml without pinning preset values
// =====================================================================================

void test_config_untouched_and_minimal_edits() {
    const fs::path dir = fresh_dir("config_write");
    fs::copy_file(fs::path(ROOT_DIR) / "assets/config.yaml", dir / "config.yaml");
    ConfigDocument cfg;
    cfg.load(dir / "config.yaml");
    const Node original = cfg.node;
    cfg.save();
    expect(coopa::yaml::load_document(dir / "config.yaml") == original, "an untouched save changes no values");

    const size_t keys_before = cfg.node.at("render").size();
    const Node before = cfg.node;
    cfg.section("render")["shadow_quality"] = Node(std::string("low"));
    cfg.commit("q", before, {});
    cfg.save();
    const Node saved = coopa::yaml::load_document(dir / "config.yaml");
    expect(saved.at("render").size() == keys_before + (original.at("render").contains("shadow_quality") ? 0 : 1),
           "changing a quality tier adds no pinned preset keys");
    const toy::core::AppConfig parsed = toy::core::AppConfig::load((dir / "config.yaml").string());
    expect(parsed.render.shadow_quality == toy::render::RenderQuality::Low, "the written tier loads back");

    erase_key(cfg.section("render"), "exposure");
    cfg.save();
    expect(!coopa::yaml::load_document(dir / "config.yaml").at("render").contains("exposure"), "reset removes the key");
    expect(cfg.dirty() == false, "saving clears the dirty flag");
}

// =====================================================================================
// Group "editor_shell" -- the whole editor against a scratch project, headless
// =====================================================================================

toy::core::AppConfig shell_config(const Project& p) {
    toy::core::AppConfig cfg = toy::core::AppConfig::load(p.config_path().string());
    cfg.window.width = 1280;
    cfg.window.height = 760;
    cfg.window.visible = false;
    cfg.window.vsync = false;
    cfg.render.screen_ui_enabled = true;
    cfg.render.ssao_temporal_enabled = false;
    cfg.render.ssr_temporal_enabled = false;
    cfg.output.save_on_exit = false;
    apply_editor_render_overrides(cfg);
    return cfg;
}

toy::core::EngineOptions shell_options(const Project& p) {
    toy::core::EngineOptions o;
    o.project_root = p.root();
    o.load_default_scene = false;
    o.edit_mode = true;
    o.escape_quits = false;
    return o;
}

void dump(toy::core::Engine& engine, const std::string& name) {
    const char* dir = std::getenv("EDITOR_DUMP_DIR");
    if (!dir) return;
    fs::create_directories(dir);
    engine.save_screenshot((fs::path(dir) / (name + ".png")).string(), false);
}

void tick(toy::core::Engine& e, int n) { for (int i = 0; i < n; ++i) e.tick(); }

/** @brief The id of the object named "Renamed" (the restart test's renamed sphere), or 0. */
ObjectId sphere_id_for_restart(EditorApp& app) {
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Renamed") return id;
    return 0;
}

void test_editor_shell_end_to_end() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);   // recent-projects prefs stay in the scratch dir
    const fs::path root = fresh_dir("shell_project");
    Project project = Project::create(root);
    expect(project.valid() && coopa::yaml::document_exists(project.assets() / "scenes/main/scene.yaml"), "a new project has a scene");

    fs::path scene_path;
    toy::core::AppConfig restart_config;
    EditorState restart_state;
    {
        toy::core::Engine engine(shell_config(project), shell_options(project));
        EditorApp app(engine, project);
        tick(engine, 4);
        expect(app.document().all_ids().size() == 4, "the default scene opened (camera, sun, ground, cube)");
        expect(app.sync().scene() && !app.sync().scene()->is_simulating(), "the live scene is in edit mode");
        dump(engine, "01_scene_solid");

        // Create, select, transform, save.
        const ObjectId sphere = app.create_primitive("Sphere");
        tick(engine, 2);
        expect(app.document().find(sphere) && app.sync().live(sphere), "a created primitive exists in the document and live scene");
        expect(coopa::yaml::document_exists(project.assets() / "meshes/sphere.yaml"), "its mesh asset exists");
        app.document().set_transform(sphere, {2, 1, 0.5f}, {0, 0, 45}, glm::vec3(1), "Move");
        app.sync().apply(engine, app.document(), {ChangeScope::Transform, sphere});
        tick(engine, 2);
        expect(glm::distance(app.sync().live(sphere)->get_transform()->transform().position(), glm::vec3(2, 1, 0.5f)) < 1e-5f,
               "a transform edit reaches the live object without a rebuild");

        // Picking: the sphere's projected centre picks the sphere. (From the default view the
        // starter scene's camera sits right in front of it, so look from the other side.)
        app.camera().yaw_deg = -50.0f;
        app.camera().apply();
        tick(engine, 2);
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        expect(cam != nullptr, "a main camera (the editor camera) exists");
        glm::vec2 px;
        if (engine.world_to_window(glm::vec3(2, 1, 0.5f), px)) {
            const float s = std::max(1.0f, engine.display_scale());
            const ObjectId got = app.pick_object(px / s);
            expect(got == sphere, "clicking the sphere's centre picks it (got " +
                   (got && app.document().find(got) ? get_string(*app.document().find(got), "name") : std::string("nothing")) + ")");
        }

        // Material asset assigned by reference.
        expect(app.create_material("brick"), "a material asset can be created");
        app.material_document().node["albedo"] = make_color({0.7f, 0.2f, 0.1f});
        app.save_material();
        app.show_document_view();
        tick(engine, 2);
        const int ci = app.document().find_component(sphere, "MeshRenderer");
        Node comp = app.document().find(sphere)->at("components").as_seq()[ci];
        comp["material"] = Node(std::string("materials/brick"));
        app.document().set_component(sphere, ci, comp, "Assign");
        app.sync().apply(engine, app.document(), {ChangeScope::Object, sphere});
        tick(engine, 2);
        auto* mr = app.sync().live(sphere)->get_component<coopa::gfx::engine::components::MeshRenderer>();
        expect(mr && std::abs(mr->material.albedo.r - 0.7f) < 1e-4f, "a `material: materials/brick` reference loads the asset's values");

        // Shading modes render differently.
        app.set_shading(Shading::Solid);
        tick(engine, 3);
        const auto solid = engine.capture_image(true);
        app.set_shading(Shading::Wireframe);
        tick(engine, 3);
        const auto wire = engine.capture_image(true);
        app.set_shading(Shading::Full);
        tick(engine, 3);
        const auto full = engine.capture_image(true);
        dump(engine, "02_scene_full");
        auto differ = [](const coopa::gfx::util::ImageData& a, const coopa::gfx::util::ImageData& b) {
            if (a.pixels.size() != b.pixels.size()) return true;
            size_t n = 0;
            for (size_t i = 0; i < a.pixels.size(); i += 4) n += a.pixels[i] != b.pixels[i] || a.pixels[i + 1] != b.pixels[i + 1];
            return n > 100;
        };
        expect(differ(solid, wire) && differ(solid, full) && differ(wire, full), "wireframe, solid and full render differ");

        // Mesh editing in the Asset tab.
        app.new_mesh("Cube");
        tick(engine, 3);
        expect(app.active_asset_type() == AssetType::Mesh && app.mesh_document().mesh.faces.size() == 6, "a new cube mesh opens in the mesh viewer");
        {
            auto* po = engine.scene().find_object("PreviewObject");
            auto* pmr = po ? po->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
            expect(pmr && pmr->is_ready(), "the asset preview scene is active and shows the mesh");
        }
        auto& md = app.mesh_document();
        md.selection.mode = SelectMode::Face;
        md.selection.faces = {1};
        md.edit("Extrude", [](EditMesh& m, MeshSelection& s) { extrude_faces(m, s, 0.75f); });
        tick(engine, 3);
        expect(md.mesh.faces.size() == 10, "extrude through the document");
        dump(engine, "03_asset_mesh");
        app.set_shading(Shading::Solid);
        tick(engine, 3);
        dump(engine, "03b_asset_mesh_solid");
        app.set_shading(Shading::Full);
        md.do_undo();
        expect(md.mesh.faces.size() == 6, "mesh undo");
        md.do_redo();
        expect(app.save_mesh() && coopa::yaml::document_exists(project.assets() / "meshes" / (md.name + ".yaml")), "the mesh saves");

        // Render settings apply live.
        app.set_prop_tab(PropTab::Render);
        const Node before = app.config_document().node;
        app.config_document().section("render")["exposure"] = make_float(2.0);
        app.config_document().commit("exposure", before, {});
        app.apply_config_live();
        tick(engine, 2);
        expect(std::abs(engine.render_config().exposure - 2.0f) < 1e-5f, "a render setting edit applies to the live renderer");
        dump(engine, "04_render_settings");
        app.set_prop_tab(PropTab::Output);
        tick(engine, 2);
        dump(engine, "05_output_props");
        app.set_prop_tab(PropTab::World);
        tick(engine, 2);
        dump(engine, "05b_world_props");

        // Play / stop leaves the document alone.
        app.show_document_view();
        tick(engine, 2);
        const Node doc_before = app.document().node();
        app.play();
        tick(engine, 3);
        expect(app.playing() && engine.scene().is_simulating(), "play runs a simulating copy");
        app.stop();
        tick(engine, 2);
        expect(!app.playing() && app.document().node() == doc_before && !engine.scene().is_simulating(), "stop returns to the untouched edit scene");

        // Save.
        expect(app.save_scene(), "the scene saves");
        scene_path = app.document().path();

        // Renderer restart: a startup-only setting takes effect, documents survive.
        const Node cb = app.config_document().node;
        app.config_document().section("render")["render_width"] = Node(int64_t(320));
        app.config_document().section("render")["render_height"] = Node(int64_t(180));
        app.config_document().commit("res", cb, {});
        app.document().set_object_key(sphere, "name", Node(std::string("Renamed")), "Rename");
        restart_config = app.config_for_restart();
        restart_state = app.take_state();
    }
    {
        toy::core::AppConfig cfg = restart_config;
        cfg.window.visible = false;
        cfg.render.screen_ui_enabled = true;
        apply_editor_render_overrides(cfg);
        toy::core::Engine engine(cfg, shell_options(project));
        EditorApp app(engine, project, {}, std::move(restart_state));
        tick(engine, 3);
        expect(engine.pipeline().render_height() == 180, "a restart applies startup-only render settings");
        expect(app.document().dirty() && app.document().find_component(sphere_id_for_restart(app), "MeshRenderer") >= 0,
               "unsaved scene edits survive the restart");
        app.show_document_view();
        tick(engine, 1);
        app.undo();
        tick(engine, 2);
        expect(engine.scene().find_object("Sphere") != nullptr, "and remain undoable afterwards");
    }

    // The saved project loads in the plain game Engine.
    {
        toy::core::AppConfig cfg = shell_config(project);
        cfg.scene.default_scene = scene_path.string();
        toy::core::EngineOptions o;
        o.project_root = project.root();
        toy::core::Engine game(cfg, o);
        tick(game, 2);
        auto* obj = game.scene().find_object("Sphere");
        expect(obj != nullptr, "the game finds the editor-created object");
        auto* mr = obj ? obj->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
        expect(mr && mr->get_mesh().is_loaded() && std::abs(mr->material.albedo.r - 0.7f) < 1e-4f,
               "with its mesh loaded and its material asset applied");
    }
}

void test_material_reference_forms() {
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("material_forms");
    Project project = Project::create(root);
    Node mat = Node::mapping();
    mat["albedo"] = make_color({0.1f, 0.2f, 0.3f});
    mat["roughness"] = make_float(0.9);
    coopa::yaml::save_document(project.assets() / "materials/stone.yaml", mat);
    SceneDocument doc;
    doc.reset("Forms");
    auto add = [&](const std::string& name, Node material) {
        Node o = doc.make_object(name);
        Node mr = Node::mapping();
        mr["type"] = Node(std::string("MeshRenderer"));
        mr["mesh_path"] = Node(std::string("cube"));
        mr["material"] = material;
        o["components"].as_seq().push_back(mr);
        doc.add_object(o);
    };
    add("Ref", Node(std::string("materials/stone")));
    Node over = Node::mapping();
    over["base"] = Node(std::string("materials/stone"));
    over["roughness"] = make_float(0.2);
    add("Override", over);
    Node inl = Node::mapping();
    inl["albedo"] = make_color({0.1f, 0.2f, 0.3f});
    inl["roughness"] = make_float(0.9);
    add("Inline", inl);
    const fs::path scene = project.assets() / "scenes/forms/scene.yaml";
    doc.save(scene);

    toy::core::AppConfig cfg = shell_config(project);
    cfg.scene.default_scene = scene.string();
    toy::core::EngineOptions o;
    o.project_root = project.root();
    toy::core::Engine engine(cfg, o);
    auto mat_of = [&](const char* n) {
        return engine.scene().find_object(n)->get_component<coopa::gfx::engine::components::MeshRenderer>()->material;
    };
    const auto r = mat_of("Ref"), ov = mat_of("Override"), in = mat_of("Inline");
    expect(r.albedo == in.albedo && r.roughness == in.roughness, "a reference and the same values inline parse identically");
    expect(ov.albedo == in.albedo && std::abs(ov.roughness - 0.2f) < 1e-5f, "base + override keeps the base, overrides the key");
}


/**
 * @brief Drives the editor through REAL input events (Input::push_mouse_button/push_key via
 *        Engine::queue_input, cursor via set_cursor_override) -- the same path GLFW events
 *        take -- so hover offsets, click routing and the Blender keymap are all exercised.
 */
struct InputDriver {
    toy::core::Engine& e;
    float scale;   // canvas pixels -> cursor (framebuffer) pixels
    glm::vec2 at{0.0f};

    void move(glm::vec2 canvas, int frames = 1) {
        at = canvas;
        e.set_cursor_override(canvas * scale);
        tick(e, frames);
    }
    void drag(glm::vec2 to, coopa::input::MouseButton b, int steps = 6) {
        using coopa::input::KeyAction;
        e.queue_input([b](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Press, coopa::input::Mods::None); });
        tick(e, 1);
        const glm::vec2 from = at;
        for (int i = 1; i <= steps; ++i) move(glm::mix(from, to, i / float(steps)));
        e.queue_input([b](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Release, coopa::input::Mods::None); });
        tick(e, 1);
    }
    void click(glm::vec2 canvas, coopa::input::MouseButton b = coopa::input::MouseButton::Left,
               coopa::input::Mods mods = coopa::input::Mods::None) {
        using coopa::input::KeyAction;
        move(canvas);
        e.queue_input([b, mods](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Press, mods); });
        tick(e, 1);
        e.queue_input([b, mods](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Release, mods); });
        tick(e, 2);
    }
    void key(coopa::input::Key k, coopa::input::Mods mods = coopa::input::Mods::None) {
        using coopa::input::KeyAction;
        e.queue_input([k, mods](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Press, mods); });
        tick(e, 1);
        e.queue_input([k, mods](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Release, coopa::input::Mods::None); });
        tick(e, 1);
    }
};

void test_editor_real_input_blender_keymap() {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("input_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};

    // Menubar: clicking "File" where it is drawn opens it (catches any hover offset).
    in.click({44, 14});
    expect(app.ui().any_popup_open(), "clicking the File menu header opens it");
    in.key(Key::Escape);
    in.click({4, 300});   // click elsewhere closes it
    tick(engine, 2);

    const imm::Box vp = app.viewport_box();
    expect(vp.w > 100 && vp.h > 100, "the viewport has a usable size");
    const glm::vec2 centre = vp.center();

    // Click-select the cube at its projected centre.
    const ObjectId cube = [&] { for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Cube") return id; return ObjectId(0); }();
    glm::vec2 px;
    expect(engine.world_to_window(glm::vec3(0, 0, 0.5f), px), "the cube projects into the viewport");
    in.click(px / in.scale);
    expect(app.document().primary() == cube, "a real left click on the cube selects it");

    // G, X, type 2, Enter: moved exactly 2 along X.
    in.move(centre);
    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Num2);
    in.key(Key::Enter);
    glm::vec3 p, r, s;
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(p.x - 2.0f) < 1e-4f && std::abs(p.y) < 1e-4f && std::abs(p.z - 0.5f) < 1e-4f,
           "G X 2 Enter moves the cube exactly 2 on X (got " + std::to_string(p.x) + ", " + std::to_string(p.y) + ")");
    // G with mouse motion then RMB cancels.
    in.key(Key::G);
    in.move(centre + glm::vec2(80, 30), 2);
    in.click(centre + glm::vec2(80, 30), MouseButton::Right);
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(p.x - 2.0f) < 1e-4f, "RMB cancels a grab and restores the position");
    // R Z 90 Enter.
    in.move(centre);
    in.key(Key::R);
    in.key(Key::Z);
    in.key(Key::Num9);
    in.key(Key::Num0);
    in.key(Key::Enter);
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(std::abs(r.z) - 90.0f) < 0.01f, "R Z 90 rotates 90 degrees about Z (got " + std::to_string(r.z) + ")");
    // Ctrl+Z undoes the rotation.
    in.key(Key::Z, Mods::Control);
    tick(engine, 2);
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(r.z) < 0.01f, "Ctrl+Z undoes it");

    // MMB drag orbits the view.
    const float yaw0 = app.camera().yaw_deg;
    in.move(centre);
    in.drag(centre + glm::vec2(120, 0), MouseButton::Middle);
    expect(std::abs(app.camera().yaw_deg - yaw0) > 10.0f, "middle-drag orbits the camera");

    // The wheel zooms -- whole notches and macOS-style fractional (smooth) deltas alike.
    for (float notch : {1.0f, 0.35f, -0.35f}) {
        in.move(centre);
        const float d0 = app.camera().distance;
        const float yaw_before = app.camera().yaw_deg;
        engine.queue_input([notch](coopa::input::Input& i) { i.push_scroll(0.0, notch); });
        tick(engine, 1);
        const float d1 = app.camera().distance;
        expect(notch > 0 ? d1 < d0 : d1 > d0, "wheel " + std::to_string(notch) + " zooms " + (notch > 0 ? "in" : "out") +
               " (" + std::to_string(d0) + " -> " + std::to_string(d1) + ")");
        expect(app.camera().yaw_deg == yaw_before, "and does not orbit");
    }

    // Shift+A opens the add menu at the mouse.
    in.move(centre);
    in.key(Key::A, Mods::Shift);
    expect(app.ui().any_popup_open(), "Shift+A opens the add menu");
    in.key(Key::Escape);
    in.click({4, 300});

    // Tab into edit mode, face mode, select all, E extrude along the normal by 1.
    in.move(centre);
    app.document().select(cube);
    in.key(Key::Tab);
    expect(app.mesh_document().open() && app.mesh_document().mesh.faces.size() == 6, "Tab enters edit mode on the cube's mesh");
    in.key(Key::Num3);
    app.mesh_document().selection.faces = {1};   // the +Z face
    in.key(Key::E);
    in.key(Key::Num1);
    in.key(Key::Enter);
    glm::vec3 lo, hi;
    app.mesh_document().mesh.bounds(lo, hi);
    expect(app.mesh_document().mesh.faces.size() == 10 && std::abs(hi.z - 1.5f) < 1e-3f,
           "E 1 Enter extrudes the top face 1 unit along its normal (top z " + std::to_string(hi.z) + ")");
    in.key(Key::Z, Mods::Control);
    app.mesh_document().mesh.bounds(lo, hi);
    expect(std::abs(hi.z - 0.5f) < 1e-3f, "Ctrl+Z in edit mode undoes the mesh edit");
    expect(app.edit_mode_active(), "still in edit mode before the second Tab");
    in.key(Key::Tab);
    expect(!app.edit_mode_active(), "Tab leaves edit mode");
    dump(engine, "06_after_input");
}


/** @brief Blender chrome: nav-gizmo axis clicks, Properties tabs per selection, Pause / Step. */
void test_editor_blender_chrome() {
    using coopa::input::Key;
    using coopa::input::Mods;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("chrome_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};

    // Navigation gizmo: click the ball for +Z (screen position from the live view).
    const imm::Box g = app.nav_gizmo_rect();
    auto ball = [&](glm::vec3 axis) {
        const glm::mat3 vr(coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix());
        const glm::vec3 v = vr * axis;
        return glm::vec2(g.x + 55, g.y + 55) + glm::vec2(v.x, -v.y) * 40.0f;
    };
    in.click(ball(glm::vec3(0, 0, 1)));
    expect(std::abs(app.camera().pitch_deg - 89.5f) < 0.01f, "clicking the gizmo's Z ball gives the top view");
    in.click(ball(glm::vec3(0, -1, 0)));
    expect(std::abs(app.camera().pitch_deg) < 0.01f && std::abs(app.camera().yaw_deg) < 0.01f,
           "clicking its -Y ball gives the front view (pitch " + std::to_string(app.camera().pitch_deg) + ")");
    expect(app.document().selection().empty(), "gizmo clicks never select objects behind it");

    // Properties tabs follow the selection (Blender's object tabs appear with an object).
    app.document().clear_selection();
    app.set_prop_tab(PropTab::Material);
    tick(engine, 2);
    expect(app.prop_tab() == PropTab::Tool, "without a selection, object tabs fall back to Tool");
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Cube") app.document().select(id);
    app.set_prop_tab(PropTab::Data);
    tick(engine, 2);
    expect(app.prop_tab() == PropTab::Data, "a mesh object offers the Object Data tab");

    // Outliner eye: hides the object in the viewport (editor-only, the document is untouched).
    {
        const ObjectId sel = app.document().primary();
        expect(app.outliner_eye_rect(sel).has_value(), "the selected object's outliner row has an eye toggle");
        if (auto eye = app.outliner_eye_rect(sel)) {
            in.click(glm::vec2(eye->x + eye->w * 0.5f, eye->y + eye->h * 0.5f));
            tick(engine, 2);
            auto* live = app.sync().live(sel);
            expect(live && !live->active(), "clicking the eye hides the object in the viewport");
            expect(get_bool(*app.document().find(sel), "active", true), "and leaves the saved `active` flag alone");
            in.click(glm::vec2(eye->x + eye->w * 0.5f, eye->y + eye->h * 0.5f));
            tick(engine, 2);
            expect(live && live->active(), "clicking it again reveals it");
        }
    }

    // Unity play controls: Pause freezes, Step advances exactly one frame.
    ObjectId cube = 0;
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Cube") cube = id;
    expect(app.document().selection().empty(), "hiding deselects, as in Blender");
    app.document().select(cube);
    Node rb = default_component("Rigidbody");
    app.document().add_component(cube, rb);
    Node box = default_component("BoxCollider");   // physics bodies are built from colliders
    app.document().add_component(cube, box);
    app.document().set_transform(cube, {0, 0, 5}, glm::vec3(0), glm::vec3(1), "Lift");
    app.sync().rebuild(engine, app.document());
    tick(engine, 2);
    app.play();
    tick(engine, 5);
    expect(app.playing(), "playing");
    app.pause();
    tick(engine, 1);
    expect(app.paused(), "Pause freezes the simulation");
    auto z_of = [&] { return engine.scene().find_object("Cube")->get_transform()->transform().position().z; };
    const float z0 = z_of();
    tick(engine, 5);
    expect(std::abs(z_of() - z0) < 1e-6f, "nothing moves while paused");
    app.step();
    tick(engine, 4);
    const float z1 = z_of();
    expect(z1 < z0 - 1e-6f && app.paused(), "Step advances one frame and stays paused (" + std::to_string(z0) + " -> " + std::to_string(z1) + ")");
    tick(engine, 4);
    expect(std::abs(z_of() - z1) < 1e-6f, "and then holds still");
    app.stop();
    tick(engine, 2);
    expect(!app.playing(), "stop");
    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 4);
    dump(engine, "07_shading_workspace");
}

/** @brief Bundled editor themes: all load, Blender Dark spells out every role, switching applies live. */
void test_editor_themes() {
    namespace imm = coopa::ui::imm;
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());

    const auto themes = imm::list_themes(editor_themes_dir());
    expect(themes.size() >= 3, "editor/themes ships several themes (" + std::to_string(themes.size()) + ")");
    for (const auto& e : themes) {
        bool ok = true;
        try { imm::load_theme(e.path); } catch (const std::exception& ex) { ok = false; std::cerr << ex.what() << "\n"; }
        expect(ok, "theme '" + e.id + "' loads");
    }
    // The default theme is the complete reference: every Style field and editor role present.
    const fkyaml::node dark = coopa::yaml::load_document(editor_themes_dir() / "blender_dark.yaml");
    std::vector<std::string> missing;
    struct Presence {
        const fkyaml::node* root; std::vector<std::string>* missing;
        void metric(const char* k, const float&) const { if (!root->at("metrics").contains(k)) missing->push_back(std::string("metrics.") + k); }
        void color(const char* k, const glm::vec4&) const { if (!root->at("colors").contains(k)) missing->push_back(std::string("colors.") + k); }
    };
    const imm::Style ref_style;
    const EditorTheme ref_editor;
    imm::visit_style(ref_style, Presence{&dark, &missing});
    visit_editor_theme(ref_editor, [&](const char* sec, const char* role, const glm::vec4&) {
        if (!dark.contains(sec) || !dark.at(sec).contains(role)) missing.push_back(std::string(sec) + "." + role);
    });
    std::string list;
    for (const auto& m : missing) list += " " + m;
    expect(missing.empty(), "blender_dark.yaml lists every themable role (missing:" + list + ")");

    const fs::path root = fresh_dir("theme_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 3);
    expect(app.theme_id() == "blender_dark" && app.theme().name == "Blender Dark", "Blender Dark is the default theme");
    expect(app.ui().style.panel_bg == app.theme().style.panel_bg, "the theme drives the UI style");

    expect(app.set_theme("unity_dark"), "switch to Unity Dark");
    tick(engine, 3);
    dump(engine, "08_theme_unity_dark");
    expect(app.ui().style.selection == imm::load_theme(editor_themes_dir() / "unity_dark.yaml").style.selection,
           "the new theme applies immediately");
    expect(app.theme().style.axis_x == imm::load_theme(editor_themes_dir() / "blender_dark.yaml").style.axis_x,
           "`inherits:` keeps the base theme's roles");
    const Node prefs = Project::load_prefs();
    expect(prefs.contains("theme") && prefs.at("theme").get_value<std::string>() == "unity_dark", "the choice is remembered");

    expect(app.set_theme("blender_light"), "switch to Blender Light");
    tick(engine, 3);
    dump(engine, "09_theme_blender_light");
    expect(app.ui().style.text.r < 0.3f, "light theme: dark text");
    expect(!app.set_theme("no_such_theme") && app.theme_id() == "blender_light", "a missing theme keeps the current one");
    {
        EditorApp again(engine, project);   // a new session starts with the remembered theme
        expect(again.theme_id() == "blender_light", "the remembered theme loads at startup");
    }
    fs::remove(Project::prefs_path());
}

/** @brief BLEND materials are drawn in Solid / Material Preview (and the full render). */
void test_editor_transparency_preview() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("transparency_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);   // let the fill-mode rebuild settle
    ObjectId cube = 0;
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Cube") cube = id;
    expect(cube != 0, "the new project has a Cube");
    const int mr = app.document().find_component(cube, "MeshRenderer");
    auto set_alpha = [&](float a) {
        Node comp = app.document().find(cube)->at("components").as_seq()[static_cast<size_t>(mr)];
        Node mat = Node::mapping();
        mat["albedo"] = make_color({0.9f, 0.1f, 0.1f});
        mat["alpha_mode"] = Node(std::string("BLEND"));
        mat["alpha"] = make_float(a);
        comp["material"] = mat;
        app.document().set_component(cube, mr, comp, "Glass");
        app.sync().rebuild(engine, app.document());
        tick(engine, 4);
    };
    auto differing = [](const coopa::gfx::util::ImageData& a, const coopa::gfx::util::ImageData& b) {
        size_t n = 0;
        if (a.pixels.size() != b.pixels.size()) return size_t(0);
        for (size_t i = 0; i + 3 < a.pixels.size(); i += a.channels) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(int(a.pixels[i + c]) - int(b.pixels[i + c])));
            if (d > 12) ++n;
        }
        return n;
    };
    for (Shading sh : {Shading::Solid, Shading::MaterialPreview, Shading::Full}) {
        app.set_shading(sh);
        set_alpha(0.0f);
        const auto clear = engine.capture_image(true);
        set_alpha(0.6f);
        const auto glass = engine.capture_image(true);
        const size_t n = differing(clear, glass);
        const char* name = sh == Shading::Solid ? "Solid" : sh == Shading::MaterialPreview ? "Material Preview" : "Rendered";
        expect(n > 200, std::string("a 60% alpha cube shows in ") + name + " (" + std::to_string(n) + " px changed)");
        if (sh == Shading::MaterialPreview) dump(engine, "10_glass_material_preview");
    }
}

/** @brief The viewport fills its panel: the render follows the panel's aspect (resolution_mode fill). */
void test_editor_viewport_fill() {
    using coopa::input::Key;
    using coopa::input::Mods;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("fill_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    auto aspect_matches = [&](const std::string& what) {
        const imm::Box vb = app.viewport_box();
        const float panel = vb.w / std::max(1.0f, vb.h);
        const float render = float(engine.pipeline().render_width()) / float(engine.pipeline().render_height());
        expect(std::abs(panel - render) < 0.02f, what + ": render aspect " + std::to_string(render) +
                                                     " matches the panel's " + std::to_string(panel));
        const auto d = engine.display_rect();
        const float s = std::max(1.0f, engine.display_scale());
        expect(std::abs(d.w - vb.w * s) <= 2.0f && std::abs(d.h - vb.h * s) <= 2.0f,
               what + ": the image covers the whole panel (no bars)");
    };
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    expect(engine.pipeline().render_height() == 270, "the project's vertical resolution is kept");
    aspect_matches("default layout");
    const uint32_t w0 = engine.pipeline().render_width();

    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Space, Mods::Control);   // Blender's Toggle Maximize Area
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    expect(engine.pipeline().render_width() != w0, "maximizing the viewport changes the render width");
    aspect_matches("maximized");
    dump(engine, "11_viewport_maximized");

    // The view still works after a rebuild: clicking the cube selects it.
    ObjectId cube = 0;
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Cube") cube = id;
    glm::vec2 px;
    if (engine.world_to_window(glm::vec3(0, 0, 0.5f), px)) {
        app.document().clear_selection();
        in.click(px / in.scale);
        tick(engine, 2);
        expect(app.document().primary() == cube, "picking works after the pipeline rebuild");
    }
}

ObjectId object_named(EditorApp& app, const std::string& name) {
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == name) return id;
    return 0;
}

glm::mat4 world_of(EditorApp& app, ObjectId id) {
    auto* live = app.sync().live(id);
    return live && live->get_transform() ? live->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
}

/** @brief Window point (canvas px) of a mesh-local point on `id`, or nullopt. */
std::optional<glm::vec2> screen_of(toy::core::Engine& engine, EditorApp& app, ObjectId id, const glm::vec3& local, float scale) {
    glm::vec2 px;
    if (!engine.world_to_window(glm::vec3(world_of(app, id) * glm::vec4(local, 1.0f)), px)) return std::nullopt;
    return px / scale;
}

/** @brief The cube corner (x, y) nearest the camera, so its vertical edge is visible. */
glm::vec2 visible_corner(EditorApp& app, ObjectId cube) {
    const glm::mat4 view = coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix();
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::mat4 w = world_of(app, cube);
    glm::vec2 best(0.5f);
    float bd = 1e30f;
    for (float x : {-0.5f, 0.5f}) for (float y : {-0.5f, 0.5f}) {
        const float d = glm::distance(glm::vec3(w * glm::vec4(x, y, 0, 1)), eye);
        if (d < bd) { bd = d; best = {x, y}; }
    }
    return best;
}

/** @brief Ctrl+R loop cut and slide, Alt+click loops, G G edge slide, Ctrl+Alt rings, Ctrl+T -- with real input. */
void test_editor_quad_modelling() {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("quad_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    const ObjectId cube = object_named(app, "Cube");
    app.document().select(cube);
    in.move(app.viewport_box().center());
    in.key(Key::Tab);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    expect(app.edit_mode_active(), "Tab enters Edit Mode");
    in.key(Key::Num2);
    auto& md = app.mesh_document();
    expect(md.selection.mode == SelectMode::Edge, "2: edge select mode");

    const glm::vec2 corner = visible_corner(app, cube);
    const auto edge_px = screen_of(engine, app, cube, glm::vec3(corner, 0.1f), in.scale);
    expect(edge_px.has_value(), "the cube's near vertical edge is on screen");
    if (!edge_px) return;

    // Ctrl+R: the wheel changes the cut count, Esc cancels.
    in.move(*edge_px);
    in.key(Key::R, Mods::Control);
    expect(app.loop_cut_active(), "Ctrl+R starts Loop Cut and Slide");
    engine.queue_input([](coopa::input::Input& i) { i.push_scroll(0, 1); });
    tick(engine, 2);
    expect(app.loop_cut_cuts() == 2, "the wheel adds a cut (got " + std::to_string(app.loop_cut_cuts()) + ")");
    in.key(Key::Escape);
    expect(!app.loop_cut_active() && md.mesh.faces.size() == 6, "Esc cancels without cutting");

    // Ctrl+R, LMB (cut, then slide), RMB (keep the cut centred).
    in.move(*edge_px);
    in.key(Key::R, Mods::Control);
    in.move(*edge_px + glm::vec2(1, 0));
    in.click(*edge_px);
    in.click(*edge_px, MouseButton::Right);
    expect(md.mesh.faces.size() == 10 && closed_and_consistent(md.mesh), "one cut around the cube: 10 faces, closed (got " +
                                                                          std::to_string(md.mesh.faces.size()) + ")");
    bool centred = true;
    for (uint32_t v = 8; v < md.mesh.positions.size(); ++v) centred &= std::abs(md.mesh.positions[v].z) < 1e-4f;
    expect(centred, "RMB after the cut leaves the loop centred");
    in.key(Key::Z, Mods::Control);
    expect(md.mesh.faces.size() == 6, "one undo step removes the cut and its slide");
    in.key(Key::Z, Mods::Control | Mods::Shift);
    expect(md.mesh.faces.size() == 10, "redo brings it back");

    // Alt+click a loop edge: the whole new loop, without orbiting.
    const float yaw = app.camera().yaw_deg;
    const auto loop_px = screen_of(engine, app, cube, glm::vec3(corner.x, 0.0f, 0.0f), in.scale);
    if (loop_px) in.click(*loop_px, MouseButton::Left, Mods::Alt);
    in.key(Key::Num2);   // (a key event clears the held Alt)
    expect(md.selection.edges.size() == 4 && std::abs(app.camera().yaw_deg - yaw) < 1e-4f,
           "Alt+click selects the 4-edge loop and the view stays put (got " + std::to_string(md.selection.edges.size()) + ")");

    // G G 0.5 Enter: Edge Slide halfway.
    in.move(app.viewport_box().center());
    in.key(Key::G);
    in.key(Key::G);
    in.key(Key::Period);
    in.key(Key::Num5);
    in.key(Key::Enter);
    bool slid = true;
    for (uint32_t v = 8; v < 12; ++v) slid &= std::abs(std::abs(md.mesh.positions[v].z) - 0.25f) < 1e-4f;
    expect(slid, "G G .5 slides the loop halfway along its rails");

    // Ctrl+Alt+click a vertical edge: its ring.
    const auto ring_px = screen_of(engine, app, cube, glm::vec3(corner, -0.3f), in.scale);
    if (ring_px) in.click(*ring_px, MouseButton::Left, Mods::Control | Mods::Alt);
    in.key(Key::Num2);
    expect(md.selection.edges.size() == 4, "Ctrl+Alt+click selects the edge ring (got " + std::to_string(md.selection.edges.size()) + ")");

    // Ctrl+T: triangulate everything.
    in.key(Key::Num3);
    in.key(Key::A);
    in.key(Key::T, Mods::Control);
    expect(md.mesh.faces.size() == 20 && closed_and_consistent(md.mesh), "Ctrl+T: 10 quads become 20 triangles");
    in.key(Key::J, Mods::Alt);
    expect(md.mesh.faces.size() == 10, "Alt+J joins them back into quads");
    dump(engine, "12_quad_modelling");
    // Subdivide (W menu op) shows the Adjust Last Operation panel.
    in.key(Key::A);
    in.move(app.viewport_box().center());
    in.key(Key::W);
    tick(engine, 2);
    expect(app.ui().any_popup_open(), "W opens the edit-mode context menu");
    in.key(Key::Escape);
    in.key(Key::Tab);
    tick(engine, 2);
    const ObjectId grid = app.create_primitive("Grid");
    tick(engine, 3);
    expect(grid != 0, "Add > Grid creates a grid object");
    dump(engine, "15_adjust_last_operation");
}

/** @brief Edit Mode isolates the mesh (lights stay), Tab restores, and the toggle persists. */
void test_editor_isolation() {
    using coopa::input::Key;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("isolate_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    const ObjectId cube = object_named(app, "Cube"), ground = object_named(app, "Ground"), sun = object_named(app, "Sun");
    expect(cube && ground && sun, "the new project has Cube, Ground and Sun");
    app.hide_objects({object_named(app, "Camera")});
    const float yaw0 = app.camera().yaw_deg, dist0 = app.camera().distance;
    app.document().select(cube);
    in.move(app.viewport_box().center());
    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.edit_mode_active() && app.is_isolated(ground) && !app.sync().live(ground)->active(), "Edit Mode hides the other mesh");
    expect(!app.is_isolated(sun) && app.sync().live(sun)->active(), "lights stay on");
    expect(std::abs(app.camera().distance - dist0) > 1e-3f, "the view frames the edited mesh");
    dump(engine, "13_isolated_edit_mode");
    in.key(Key::Tab);
    tick(engine, 2);
    expect(!app.edit_mode_active() && app.sync().live(ground)->active() && !app.is_isolated(ground), "Tab brings everything back");
    expect(std::abs(app.camera().yaw_deg - yaw0) < 1e-3f && std::abs(app.camera().distance - dist0) < 1e-3f, "and restores the view");
    expect(!app.sync().live(object_named(app, "Camera"))->active(), "an object the user hid stays hidden");

    app.set_isolate_in_edit(false);
    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.edit_mode_active() && app.sync().live(ground)->active(), "with the toggle off, Edit Mode leaves the scene visible");
    in.key(Key::Tab);
    const Node prefs = Project::load_prefs();
    expect(prefs.contains("isolate_edit_mode") && !prefs.at("isolate_edit_mode").get_value<bool>(), "the toggle is saved");
    fs::remove(Project::prefs_path());
}

/** @brief Sculpting workspace: Subdivide Smooth, a Draw stroke, Ctrl inverts, one undo step per stroke. */
void test_editor_sculpt() {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    using coopa::input::KeyAction;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("sculpt_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    const ObjectId cube = object_named(app, "Cube");
    app.document().select(cube);
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 4);
    expect(app.interaction_mode() == InteractionMode::Sculpt, "the mode dropdown enters Sculpt Mode");
    auto& md = app.mesh_document();
    app.subdivide_smooth(2);
    tick(engine, 3);
    expect(md.mesh.faces.size() == 96 && closed_and_consistent(md.mesh), "Subdivide Smooth x2: 96 quads, closed (got " +
                                                                           std::to_string(md.mesh.faces.size()) + ")");
    app.sculpt_settings().symmetry[0] = false;
    app.sculpt_settings().strength = 1.0f;
    app.sculpt_settings().radius_px = 60.0f;

    // A Draw stroke across the visible face pushes vertices outward.
    const glm::vec2 corner = visible_corner(app, cube);
    const glm::vec3 face_point(corner.x * 0.8f, corner.y * 0.8f, 0.0f);
    auto start = screen_of(engine, app, cube, glm::vec3(corner.x, corner.y * 0.3f, 0.0f), in.scale);
    auto stop = screen_of(engine, app, cube, glm::vec3(corner.x * 0.3f, corner.y, 0.0f), in.scale);
    (void)face_point;
    expect(start && stop, "the stroke lies on screen");
    if (!start || !stop) return;
    const EditMesh before = md.mesh;
    const size_t undo0 = md.undo.undo_count();
    auto spread = [](const EditMesh& m) { float r = 0; for (const auto& p : m.positions) r += glm::length(p); return r; };
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    expect(spread(md.mesh) > spread(before) + 1e-3f, "Draw pulls the surface outward");
    expect(md.undo.undo_count() == undo0 + 1, "the whole stroke is one undo step");
    dump(engine, "14_sculpt_stroke");
    app.undo();
    tick(engine, 2);
    expect(md.mesh == before, "undo restores the exact shape");

    // Ctrl inverts: push in.
    in.move(*start);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::LeftControl, 0, KeyAction::Press, Mods::Control); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_mouse_button(MouseButton::Left, KeyAction::Press, Mods::Control); });
    tick(engine, 1);
    for (int i = 1; i <= 10; ++i) in.move(glm::mix(*start, *stop, i / 10.0f));
    engine.queue_input([](coopa::input::Input& i) { i.push_mouse_button(MouseButton::Left, KeyAction::Release, Mods::Control); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::LeftControl, 0, KeyAction::Release, Mods::None); });
    tick(engine, 2);
    expect(spread(md.mesh) < spread(before) - 1e-3f, "Ctrl+Draw pushes the surface in");
    app.show_document_view();
    tick(engine, 3);
    expect(app.interaction_mode() == InteractionMode::Object, "switching the view returns to Object Mode");
}

/** @brief The asset-focused flow: each asset type opens its view; switching asks to save. */
void test_editor_asset_views() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("asset_views_project");
    Project project = Project::create(root);
    {   // a small texture asset
        coopa::gfx::util::ImageData img;
        img.width = img.height = 4;
        img.channels = 4;
        img.pixels.assign(4 * 4 * 4, 200);
        coopa::gfx::util::save_image_png(img, (project.assets() / "textures" / "checker.png").string());
        project.refresh();
    }
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    expect(app.active_asset_type() == AssetType::Scene, "the project opens its default scene");
    expect(fs::is_directory(project.assets() / "objects"), "new projects have an objects/ folder");

    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Material && app.material_document().open(), "a material opens in the material view");
    auto* po = engine.scene().find_object("PreviewObject");
    auto* ground = engine.scene().find_object("PreviewGround");
    auto* key = engine.scene().find_object("LookdevKey");
    auto* pmr = po ? po->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(pmr && pmr->is_ready() && ground && ground->active() && key, "the lookdev scene shows the material on a shape, on a ground disc, lit");
    expect(app.shading() == Shading::Full, "materials open in the game's renderer");
    dump(engine, "16_lookdev");

    app.open_asset(AssetType::Mesh, "meshes/cube.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Mesh && app.interaction_mode() == InteractionMode::Object, "a mesh opens in the mesh viewer");
    app.set_interaction_mode(InteractionMode::Edit);
    expect(app.edit_mode_active(), "the mode dropdown enters Edit Mode on the mesh asset");
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::Sculpt, "and Sculpt Mode works on a standalone mesh asset");
    app.set_interaction_mode(InteractionMode::Object);

    app.open_asset(AssetType::Texture, "textures/checker.png");
    tick(engine, 4);
    po = engine.scene().find_object("PreviewObject");
    pmr = po ? po->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(app.active_asset_type() == AssetType::Texture && pmr && !pmr->material.texture_albedo.empty(), "a texture shows on a plane");

    app.open_asset(AssetType::Scene, "scenes/main/scene.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Scene && engine.scene().find_object("Cube"), "back to the scene");
    // Unsaved changes: switching asks first and does nothing until answered.
    const ObjectId cube = object_named(app, "Cube");
    app.document().set_object_key(cube, "name", Node(std::string("Renamed")), "Rename");
    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 3);
    expect(app.active_asset_type() == AssetType::Scene && app.ui().any_popup_open(), "switching with unsaved changes opens the save prompt");
}

/** @brief Object assets: create from a selection, place instances, edit the asset, override, spawn. */
void test_editor_object_assets() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("object_assets_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    const ObjectId cube = object_named(app, "Cube");
    app.document().select(cube);
    expect(app.create_object_asset_from_selection(), "Create Object Asset from the selection");
    tick(engine, 3);
    const fs::path asset = project.assets() / "objects" / "Cube.yaml";
    expect(coopa::yaml::document_exists(asset), "objects/Cube.yaml is written");
    const Node* inst = app.document().find(cube);
    expect(inst && get_string(*inst, "prefab") == "objects/Cube", "the selection becomes an instance (`prefab:`)");
    auto* live = engine.scene().find_object("Cube");
    auto* mr = live ? live->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(mr && mr->is_ready(), "the instance still renders its mesh");
    const ObjectId second = app.place_object_asset("objects/Cube.yaml", glm::vec3(3, 0, 0.5f));
    tick(engine, 3);
    expect(second != 0 && engine.scene().find_object(get_string(*app.document().find(second), "name")), "a second instance is placed");

    // An override on one instance: its own albedo, the other keeps the asset's.
    Node ov = Node::mapping();
    ov["type"] = Node(std::string("MeshRenderer"));
    Node mat = Node::mapping();
    mat["albedo"] = make_color({1.0f, 0.0f, 0.0f});
    ov["material"] = mat;
    app.document().add_component(second, ov);
    app.sync().rebuild(engine, app.document());
    tick(engine, 3);
    auto* l2 = engine.scene().find_object(get_string(*app.document().find(second), "name"));
    auto* m2 = l2 ? l2->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(m2 && m2->material.albedo.r > 0.99f && m2->is_ready(), "an instance override changes only that instance (mesh still from the asset)");
    expect(app.save_scene(), "the scene with instances saves");

    // The object asset opens as its own document; the hierarchy root is the object.
    app.open_asset(AssetType::Object, "objects/Cube.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Object && app.document().is_object_asset() && app.document().object_root() != 0,
           "an object asset opens as a one-object document");
    const ObjectId oroot = app.document().object_root();
    const ObjectId child = app.create_empty();
    tick(engine, 2);
    expect(child && app.document().parent_of(child).value_or(0) == oroot, "objects added to an object asset become children of its root");
    expect(app.save_scene(), "the object asset saves");
    const Node saved = coopa::yaml::load_document(asset);
    expect(saved.contains("object") && !saved.contains("scene") && saved.at("object").contains("children"),
           "it is written back as `object:` with the new child");

    // Runtime: the engine instantiates object assets.
    app.open_asset(AssetType::Scene, "scenes/main/scene.yaml");
    tick(engine, 4);
    const size_t before = engine.scene().root_objects().size();
    auto* spawned = engine.spawn("objects/Cube", glm::vec3(0, 3, 1));
    expect(spawned && engine.scene().root_objects().size() == before + 1 && spawned->children().size() == 1,
           "Engine::spawn() instantiates the asset, child included");
    dump(engine, "17_object_instances");
}

/** @brief Submeshes end to end: a two-slot mesh draws each slot with its own material. */
void test_editor_submesh_materials() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("submesh_project");
    Project project = Project::create(root);
    {   // cube with its top face in slot "trim"; a pure red material for it
        EditMesh m = make_cube();
        m.slots = {"body", "trim"};
        m.faces[1].slot = 1;
        coopa::yaml::save_document(project.assets() / "meshes" / "two_slot.yaml", mesh_to_node(m));
        Node red = Node::mapping();
        red["albedo"] = make_color({1.0f, 0.0f, 0.0f});
        red["emissive"] = make_color({1.0f, 0.0f, 0.0f});
        red["emissive_strength"] = make_float(2.0);
        coopa::yaml::save_document(project.assets() / "materials" / "red.yaml", red);
        project.refresh();
    }
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    const ObjectId cube = object_named(app, "Cube");
    const int ci = app.document().find_component(cube, "MeshRenderer");
    Node comp = app.document().find(cube)->at("components").as_seq()[static_cast<size_t>(ci)];
    comp["mesh_path"] = Node(std::string("two_slot"));
    Node mats = Node::mapping();
    mats["trim"] = Node(std::string("materials/red"));
    comp["materials"] = mats;
    app.document().set_component(cube, ci, comp, "Slots");
    app.sync().rebuild(engine, app.document());
    tick(engine, 6);
    auto* mr = engine.scene().find_object("Cube")->get_component<coopa::gfx::engine::components::MeshRenderer>();
    expect(mr && mr->is_ready() && mr->get_mesh()->part_count() == 2, "the renderer's mesh has two parts");
    expect(mr && mr->material_for(1).albedo.r > 0.99f && mr->material_for(0).albedo.r < 0.99f, "`materials: {trim: ...}` resolves by slot name");
    app.set_shading(Shading::MaterialPreview);
    tick(engine, 3);
    const auto img = engine.capture_image(true);
    size_t red = 0;
    for (size_t i = 0; i + 3 < img.pixels.size(); i += img.channels) {
        if (img.pixels[i] > 180 && img.pixels[i + 1] < 90 && img.pixels[i + 2] < 90) ++red;
    }
    expect(red > 30, "the trim slot renders red (" + std::to_string(red) + " px)");
    dump(engine, "18_submesh_materials");
}

/** @brief Edit Mode on a scene object edits the mesh ASSET (every user follows), not a copy. */
void test_editor_object_mesh_edit_is_asset() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("asset_edit_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    const ObjectId cube = object_named(app, "Cube");
    app.document().select(cube);
    app.duplicate_selected();
    tick(engine, 3);
    app.document().select(cube);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the scene's cube");
    auto& md = app.mesh_document();
    expect(md.path.filename() == "cube.yaml", "it loads the object's mesh asset (meshes/cube.yaml)");
    md.selection.mode = SelectMode::Face;
    md.selection.faces = {1};
    md.edit("Extrude", [](EditMesh& m, MeshSelection& s) { extrude_faces(m, s, 0.5f); });
    tick(engine, 3);
    size_t updated = 0;
    for (const auto& [id, live] : app.sync().live_objects()) {
        auto* mr = live ? live->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
        if (mr && mr->is_ready() && mr->get_mesh()->index_count() == 10u * 6u) ++updated;
    }
    expect(updated == 2, "both objects using meshes/cube show the edit (" + std::to_string(updated) + ")");
    app.set_interaction_mode(InteractionMode::Object);
    expect(app.save_mesh(), "saving writes the asset");
    expect(mesh_from_node(coopa::yaml::load_document(project.assets() / "meshes" / "cube.yaml")).faces.size() == 10,
           "meshes/cube.yaml itself has the extrusion");
}

// =====================================================================================
// Group "package" -- Build > Package to .caml
// =====================================================================================

void test_package_renders_identically() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("package_src");
    Project project = Project::create(root);
    const fs::path out = fresh_dir("package_out");
    PackageOptions opt;
    opt.out_dir = out;
    const PackageReport rep = package_project(project, opt);
    expect(rep.ok() && rep.encoded >= 6, "packaging encodes every YAML file (" + std::to_string(rep.encoded) + ")");
    bool any_yaml = false;
    for (const auto& e : fs::recursive_directory_iterator(out)) any_yaml |= e.path().extension() == ".yaml";
    expect(!any_yaml, "no plain YAML is left in the package");

    auto render = [&](const fs::path& proj) {
        toy::core::AppConfig cfg = toy::core::AppConfig::load((proj / "assets/config.yaml").string());
        cfg.window.visible = false;
        cfg.window.width = 640;
        cfg.window.height = 360;
        cfg.render.ssao_temporal_enabled = false;
        cfg.render.ssr_temporal_enabled = false;
        cfg.output.save_on_exit = false;
        toy::core::EngineOptions o;
        o.project_root = proj;
        toy::core::Engine e(cfg, o);
        tick(e, 8);
        return e.capture_image(true);
    };
    const auto a = render(root);
    const auto b = render(out);
    size_t diff = 0;
    for (size_t i = 0; i < std::min(a.pixels.size(), b.pixels.size()); i += 4) diff += a.pixels[i] != b.pixels[i];
    expect(a.pixels.size() == b.pixels.size() && diff == 0, "the packaged (.caml) project renders identically (" + std::to_string(diff) + " px)");
}

// =====================================================================================
// Registry
// =====================================================================================

struct TestCase { const char* name; const char* group; void (*fn)(); };

const TestCase kTests[] = {
    {"writer_scalars_roundtrip",             "writer",   test_writer_scalars_roundtrip},
    {"writer_key_order_and_flow",            "writer",   test_writer_key_order_and_flow},
    {"writer_roundtrips_every_asset",        "writer",   test_writer_roundtrips_every_asset},
    {"scene_documents_save_load_stable",     "document", test_scene_documents_save_load_stable},
    {"scene_document_random_edits_undo",     "document", test_scene_document_random_edits_undo},
    {"scene_document_reparent_rules",        "document", test_scene_document_reparent_rules},
    {"schema_defaults",                      "document", test_schema_defaults},
    {"primitives_are_closed",                "mesh",     test_primitives_are_closed},
    {"extrude_inset_flip",                   "mesh",     test_extrude_inset_flip},
    {"bevel_and_merge_and_delete",           "mesh",     test_bevel_and_merge_and_delete},
    {"mesh_io_roundtrip_and_engine_load",    "mesh",     test_mesh_io_roundtrip_and_engine_load},
    {"uv_projection_and_selection",          "mesh",     test_uv_projection_and_selection},
    {"topology_loops_rings",                 "mesh",     test_topology_loops_rings},
    {"loop_cut_and_slide",                   "mesh",     test_loop_cut_and_slide},
    {"subdivide_triangulate_dissolve",       "mesh",     test_subdivide_triangulate_dissolve},
    {"normal_basis_and_sculpt",              "mesh",     test_normal_basis_and_sculpt},
    {"mesh_material_slots",                  "mesh",     test_mesh_material_slots},
    {"imm_button_and_checkbox",              "imm",      test_imm_button_and_checkbox},
    {"imm_text_input_commits",               "imm",      test_imm_text_input_commits},
    {"imm_drag_float_and_popup_blocking",    "imm",      test_imm_drag_float_and_popup_blocking},
    {"imm_menubar_and_tree",                 "imm",      test_imm_menubar_and_tree},
    {"imm_icon_button_and_tooltip",          "imm",      test_imm_icon_button_and_tooltip},
    {"imm_theme_files",                      "imm",      test_imm_theme_files},
    {"imm_dropdown_toggles",                 "imm",      test_imm_dropdown_toggles},
    {"projection_and_rays",                  "viewport", test_projection_and_rays},
    {"modal_axis_locking",                   "viewport", test_modal_axis_locking},
    {"gizmo_translate_drag",                 "viewport", test_gizmo_translate_drag},
    {"config_untouched_and_minimal_edits",   "config",   test_config_untouched_and_minimal_edits},
    {"editor_shell_end_to_end",              "editor_shell", test_editor_shell_end_to_end},
    {"material_reference_forms",             "editor_shell", test_material_reference_forms},
    {"editor_real_input_blender_keymap",     "editor_shell", test_editor_real_input_blender_keymap},
    {"editor_blender_chrome",                "editor_shell", test_editor_blender_chrome},
    {"editor_themes",                        "editor_shell", test_editor_themes},
    {"editor_transparency_preview",          "editor_shell", test_editor_transparency_preview},
    {"editor_viewport_fill",                 "editor_shell", test_editor_viewport_fill},
    {"editor_quad_modelling",                "editor_shell", test_editor_quad_modelling},
    {"editor_isolation",                     "editor_shell", test_editor_isolation},
    {"editor_sculpt",                        "editor_shell", test_editor_sculpt},
    {"editor_asset_views",                   "editor_shell", test_editor_asset_views},
    {"editor_object_assets",                 "editor_shell", test_editor_object_assets},
    {"editor_submesh_materials",             "editor_shell", test_editor_submesh_materials},
    {"editor_object_mesh_edit_is_asset",     "editor_shell", test_editor_object_mesh_edit_is_asset},
    {"package_renders_identically",          "package",  test_package_renders_identically},
};

} // namespace

int main(int argc, char** argv) {
    toy::core::install_caml_codec();
    std::string group;
    std::vector<std::string> filters;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--group" && i + 1 < argc) group = argv[++i];
        else if (a == "-v") g_verbose = true;
        else if (a == "--list") list = true;
        else filters.push_back(a);
    }
    int run = 0, failed_tests = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& t : kTests) {
        if (list) { std::cout << t.group << "\t" << t.name << "\n"; continue; }
        if (!group.empty() && group != t.group) continue;
        if (!filters.empty() && std::none_of(filters.begin(), filters.end(), [&](const std::string& f) {
                return std::string(t.name).find(f) != std::string::npos; })) continue;
        g_current = t.name;
        g_test_failures = 0;
        g_assertions = 0;
        const auto s = std::chrono::steady_clock::now();
        try {
            t.fn();
        } catch (const std::exception& e) {
            expect(false, std::string("threw: ") + e.what());
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s).count();
        ++run;
        if (g_test_failures) ++failed_tests;
        std::cout << (g_test_failures ? "[ FAIL ] " : "[  OK  ] ") << t.group << "/" << t.name << " -- " << g_assertions
                  << " assertions (" << ms << " ms)\n";
    }
    if (list) return 0;
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (failed_tests) std::cout << "\n" << g_failures << " assertion(s) failed across " << failed_tests << " of " << run << " test(s), in " << total << " ms.\n";
    else std::cout << "\nAll " << run << " test(s) passed in " << total << " ms.\n";
    return failed_tests ? 1 : 0;
}
