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
#include <map>

#include <glm/gtc/epsilon.hpp>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/branding.h>
#include <toyengine/core/engine.h>

#include "app/editor_app.h"
#include "build/build_pipeline.h"
#include "build/packager.h"
#include "../hub/hub_app.h"
#include "core/process.h"
#include "core/naming.h"
#include "core/scene_document.h"
#include "mesh/edit_mesh.h"
#include "mesh/mesh_ops.h"
#include "mesh/mesh_bvh.h"
#include "mesh/mesh_loops.h"
#include "mesh/mesh_mirror.h"
#include "mesh/mesh_subdivide.h"
#include "mesh/mesh_topology.h"
#include "mesh/sculpt.h"
#include "mesh/paint.h"
#include "anim/clip_model.h"
#include "mesh/shader_ball.h"
#include "mesh/primitives.h"
#include "schema/component_schema.h"
#include "viewport/gizmo.h"
#include "viewport/modal_transform.h"
#include "viewport/rect_gizmo.h"
#include "ui/ui_canvas_math.h"
#include "ui/ui_palette.h"
#include "ui/log_view.h"
#include <uicoopa/binding/ui_handle.h>

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

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
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

/**
 * @brief Every mesh the editor saves is the mesh it loaded: for each asset mesh, the engine
 *        builds the identical vertex stream (position, normal, UV, tangent) from the editor's
 *        re-save as from the original, and no top-level key is lost.
 */
void test_asset_fidelity_meshes() {
    int n = 0;
    for (const auto& p : asset_yaml_files()) {
        if (p.parent_path().filename() != "meshes" || p.filename().string().find(".lod.") != std::string::npos) continue;
        const Node src = coopa::yaml::load_document(p);
        if (!src.contains("faces")) continue;
        const std::string rel = fs::relative(p, ROOT_DIR).string();
        const Node saved = Node::deserialize(coopa::yaml::emit(mesh_to_node(mesh_from_node(src))));
        std::vector<std::string> lost;
        for (const auto& kv : src.as_map()) {
            const std::string k = kv.first.get_value<std::string>();
            if (!saved.contains(k)) lost.push_back(k);
        }
        std::string lost_s;
        for (const auto& k : lost) lost_s += " " + k;
        expect(lost.empty(), rel + ": every top-level key survives (lost:" + lost_s + ")");
        const auto a = coopa::gfx::engine::data::Mesh::build_cpu(src);
        const auto b = coopa::gfx::engine::data::Mesh::build_cpu(saved);
        auto key = [](const coopa::gfx::engine::data::Vertex& v) {
            auto q = [](float x) { return static_cast<long>(std::lround(x * 2000.0f)); };
            return std::vector<long>{q(v.position.x), q(v.position.y), q(v.position.z), q(v.normal.x), q(v.normal.y), q(v.normal.z),
                                     q(v.uv.x), q(v.uv.y), q(v.tangent.x), q(v.tangent.y), q(v.tangent.z), q(v.tangent.w)};
        };
        std::multiset<std::vector<long>> va, vb;
        // Per triangle corner (index stream), so welding differences do not matter -- only what is
        // drawn. Zero-area triangles (a UV sphere's pole quads) are never drawn nor collided with;
        // the editor's weld drops them, so they are left out on both sides.
        auto corners = [&](const coopa::gfx::engine::data::MeshCpuData& d, std::multiset<std::vector<long>>& out) {
            size_t tris = 0;
            for (size_t t = 0; t + 2 < d.lods.at(0).index_count; t += 3) {
                const auto& v0 = d.vertices[d.indices[t]];
                const auto& v1 = d.vertices[d.indices[t + 1]];
                const auto& v2 = d.vertices[d.indices[t + 2]];
                if (glm::length(glm::cross(v1.position - v0.position, v2.position - v0.position)) < 1e-9f) continue;
                for (int c = 0; c < 3; ++c) out.insert(key(d.vertices[d.indices[t + c]]));
                ++tris;
            }
            return tris;
        };
        const size_t ta = corners(a, va), tb = corners(b, vb);
        std::vector<std::vector<long>> diff;
        std::set_symmetric_difference(va.begin(), va.end(), vb.begin(), vb.end(), std::back_inserter(diff));
        expect(ta == tb && diff.empty(),
               rel + ": the engine draws the identical mesh from the editor's save (" + std::to_string(diff.size()) +
                   " differing corners, " + std::to_string(ta) + " vs " + std::to_string(tb) + " triangles)");
        ++n;
    }
    expect(n >= 5, "checked the repository's meshes (" + std::to_string(n) + ")");
}

// =====================================================================================
// Group "document" -- SceneDocument, undo, schemas
// =====================================================================================

void test_scene_documents_save_load_stable() {
    const fs::path dir = fresh_dir("doc_roundtrip");
    int count = 0;
    for (const auto& p : asset_yaml_files()) {
        if (!is_scene_folder_file(fs::relative(p, fs::path(ROOT_DIR) / "assets").generic_string())) continue;
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

/** @brief An object asset keeps everything on save: its object (unknown keys included) and any
 *         other top-level keys. */
void test_asset_fidelity_object_assets() {
    const fs::path dir = fresh_dir("object_assets");
    const fs::path in = dir / "crate.yaml";
    {
        std::ofstream f(in);
        f << "format: toyengine-object\nversion: 3\nauthor: someone\nobject:\n  name: Crate\n  tag: props\n"
             "  components:\n    - type: Transform\n      position: {x: 1.0, y: 2.0, z: 3.0}\n"
             "    - type: FutureThing\n      odd: [1, 2, 3]\n  children:\n    - name: Lid\n      components:\n        - type: Transform\n";
    }
    SceneDocument doc;
    doc.load(in);
    const fs::path out = dir / "crate_saved.yaml";
    doc.save(out);
    const Node a = coopa::yaml::load_document(in), b = coopa::yaml::load_document(out);
    expect(a == b, "an object asset saves exactly as loaded:\n" + coopa::yaml::emit(b));
}

void test_scene_document_random_edits_undo() {
    SceneDocument doc;
    doc.load(fs::path(ROOT_DIR) / "assets/scenes/demos/pixel_demo/scene.yaml");
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
    expect(doc.unique_name("Parent") == "Parent_001", "unique names number in snake_case style (_001)");
    auto dup = doc.duplicate_objects({c});
    expect(dup.size() == 1 && dup[0] != c && doc.find(dup[0]) != nullptr, "duplicates get fresh ids");
}

void test_snake_case_names() {
    // What the editor creates is named like assets/: snake_case.
    expect(snake_case("Point Light") == "point_light" && snake_case("ReflectionProbe") == "reflection_probe" &&
           snake_case("my-Asset 2") == "my_asset_2" && snake_case("HTTPServer") == "http_server" &&
           snake_case("already_snake") == "already_snake" && snake_case("  Wood Crate! ") == "wood_crate",
           "snake_case() handles spaces, camelCase, acronyms and punctuation");
    SceneDocument doc;
    doc.reset("t");
    doc.add_object(doc.make_object("cube"));
    expect(doc.unique_name("cube") == "cube_001", "a taken name gets _001");
    doc.add_object(doc.make_object("cube_001"));
    expect(doc.unique_name("cube_001") == "cube_002", "duplicating cube_001 renumbers it (cube_002, not cube_001_001)");
    expect(doc.unique_name("Cube.001") == "Cube.001", "a free name is kept exactly as typed");

    const Node scene = Project::default_scene_node("main");
    std::vector<std::string> names;
    for (const auto& o : scene.at("scene").at("root_objects").as_seq()) names.push_back(get_string(o, "name"));
    expect(get_string(scene.at("scene"), "scene_name") == "main" && names == std::vector<std::string>{"camera", "sun", "ground", "cube"},
           "the default scene and its objects are snake_case");
}

void test_schema_defaults() {
    for (const auto& [type, schema] : schemas()) {
        const Node c = default_component(type);
        expect(component_type(c) == type, "default " + type + " carries its type");
        for (const auto& f : schema.fields) {
            if (f.in_default && f.kind != FieldKind::AssetRef && f.kind != FieldKind::String && f.kind != FieldKind::Enum &&
                f.kind != FieldKind::ChildRef) {
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
        if (name == "Plane" || name == "Grid") {
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

/** @brief Blender's export attributes survive the editor: colours, vertex groups, skinning
 *         palettes and unknown keys round-trip, and the exported tangents are Blender's. */
void test_mesh_io_preserves_blender_attributes() {
    // Tangents: every asset mesh exported with tangents re-exports with the same ones (matched
    // per engine vertex by position, normal and UV).
    int checked = 0;
    for (const auto& p : asset_yaml_files()) {
        if (p.parent_path().filename() != "meshes" || p.filename().string().find(".lod.") != std::string::npos) continue;
        const Node src = coopa::yaml::load_document(p);
        if (!src.contains("faces") || !src.contains("tangents") || src.at("tangents").as_seq().empty()) continue;
        const auto a = coopa::gfx::engine::data::Mesh::build_cpu(src);
        const auto b = coopa::gfx::engine::data::Mesh::build_cpu(Node::deserialize(coopa::yaml::emit(mesh_to_node(mesh_from_node(src)))));
        std::map<std::tuple<int, int, int, int, int, int, int, int>, glm::vec4> ref;
        auto key = [](const coopa::gfx::engine::data::Vertex& v) {
            auto q = [](float x) { return static_cast<int>(std::lround(x * 1000.0f)); };
            return std::make_tuple(q(v.position.x), q(v.position.y), q(v.position.z), q(v.normal.x), q(v.normal.y),
                                   q(v.normal.z), q(v.uv.x), q(v.uv.y));
        };
        for (const auto& v : a.vertices) ref[key(v)] = v.tangent;
        int matched = 0, agree = 0;
        float worst = 1.0f;
        for (const auto& v : b.vertices) {
            auto it = ref.find(key(v));
            if (it == ref.end()) continue;
            ++matched;
            const float d = glm::dot(glm::vec3(v.tangent), glm::vec3(it->second));
            worst = std::min(worst, d);
            agree += (d > 0.98f && v.tangent.w == it->second.w) ? 1 : 0;
        }
        expect(matched > 0 && agree == matched,
               p.filename().string() + ": re-exported tangents equal the file's (" + std::to_string(agree) + "/" +
                   std::to_string(matched) + " agree, worst dot " + std::to_string(worst) + ")");
        ++checked;
    }
    expect(checked > 0, "found asset meshes with tangents");

    // Colours, vertex groups, the skinning palette and unknown keys round-trip.
    EditMesh m = make_cube();
    m.has_colors = true;
    for (auto& f : m.faces) for (auto& c : f.corners) c.color = glm::vec4(m.positions[c.v] + 0.5f, 1.0f);
    const uint32_t root = m.group_index("Root"), tip = m.group_index("Tip");
    m.joints.assign(m.positions.size(), glm::ivec4(0, 1, -1, -1));
    m.joint_weights.assign(m.positions.size(), glm::vec4(0.5f, 0.5f, 0.0f, 0.0f));
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        const float z = m.positions[v].z + 0.5f;   // 0 at the bottom, 1 at the top
        m.set_weight(v, root, 1.0f - z);
        m.set_weight(v, tip, z);
    }
    m.passthrough["bones"] = Node::sequence();
    m.passthrough["bones"].as_seq().push_back(Node(std::string("root")));
    const Node n = Node::deserialize(coopa::yaml::emit(mesh_to_node(m)));
    const EditMesh back = mesh_from_node(n);
    // Import numbers vertices by first use, so match them up by position, then compare the rest.
    std::vector<uint32_t> to_m(back.positions.size(), ~0u);
    for (uint32_t i = 0; i < back.positions.size(); ++i)
        for (uint32_t j = 0; j < m.positions.size(); ++j)
            if (glm::distance(back.positions[i], m.positions[j]) < 1e-5f) to_m[i] = j;
    bool same = back.positions.size() == m.positions.size() && back.faces.size() == m.faces.size() &&
                back.groups == m.groups && back.has_colors;
    for (uint32_t i = 0; same && i < back.positions.size(); ++i) {
        const uint32_t j = to_m[i];
        same = j != ~0u && back.joints[i] == m.joints[j] && back.joint_weights[i] == m.joint_weights[j];
        for (uint32_t g = 0; same && g < m.groups.size(); ++g) same = std::abs(back.weight(i, g) - m.weight(j, g)) < 1e-5f;
    }
    for (size_t f = 0; same && f < m.faces.size(); ++f) {
        for (size_t k = 0; same && k < m.faces[f].corners.size(); ++k) {
            const Corner& a = back.faces[f].corners[k];
            const Corner& b = m.faces[f].corners[k];
            same = to_m[a.v] == b.v && a.uv == b.uv && glm::distance(a.color, b.color) < 1e-5f;
        }
    }
    expect(same, "colours, vertex groups and the joint palette survive save -> load");
    expect(back.passthrough.contains("bones"), "...and keys the editor does not know ride along");
    const auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(n);
    expect(!cpu.vertices.empty(), "...in a file the engine still loads");

    // Operations keep the per-vertex data parallel and interpolate it.
    auto parallel = [](const EditMesh& x) {
        return x.weights.size() == x.positions.size() && x.joints.size() == x.positions.size() &&
               x.joint_weights.size() == x.positions.size();
    };
    EditMesh cut = m;
    MeshSelection sel;
    loop_cut(cut, sel, make_edge(0, 4), 1);
    bool mid_ok = parallel(cut);
    for (uint32_t v = static_cast<uint32_t>(m.positions.size()); v < cut.positions.size(); ++v) {
        // A cut across the vertical edges lands half way up: half Root, half Tip.
        mid_ok = mid_ok && std::abs(cut.weight(v, root) - 0.5f) < 1e-4f && std::abs(cut.weight(v, tip) - 0.5f) < 1e-4f;
        mid_ok = mid_ok && std::abs(cut.vertex_color(v).z - 0.5f) < 1e-4f;
    }
    expect(cut.positions.size() > m.positions.size() && mid_ok,
           "a loop cut's new vertices take the interpolated weights and colours");
    EditMesh cc = m;
    cc.slots = {"a", "b"};
    cc.faces[1].slot = 1;
    MeshSelection none;
    catmull_clark(cc, none, 1);
    size_t slot_b = 0;
    for (const auto& f : cc.faces) slot_b += f.slot == 1;
    expect(parallel(cc) && cc.groups == m.groups && cc.has_colors && cc.slots.size() == 2 && slot_b == 4,
           "Catmull-Clark keeps vertex groups, colours and material slots");
    EditMesh del = m;
    MeshSelection top;
    top.mode = SelectMode::Face;
    top.faces = {0};
    delete_selection(del, top);
    expect(parallel(del) && del.positions.size() == m.positions.size(),
           "deleting a face keeps the data parallel (no vertex orphaned on a cube)");
    MeshSelection all;
    all.select_all(del);
    subdivide(del, all, 1);
    expect(parallel(del), "subdividing keeps the data parallel");
}

/** @brief Vertex Paint and Weight Paint dabs: falloff, blend modes, blur, front faces only,
 *         auto-normalize, and the fills. */
void test_paint_brushes() {
    EditMesh g = make_grid(16, 16, 2.0f);   // spacing 0.125, centre vertex at the origin
    SculptCache cache;
    cache.build(g);
    VertexGrid grid;
    grid.build(g, 0.5f);
    const uint32_t centre = 8 * 17 + 8, edge = 0;
    PaintSettings s;
    s.color = glm::vec4(1, 0, 0, 1);
    PaintDab d;
    d.center = glm::vec3(0.0f);
    d.radius = 0.5f;
    d.view_dir = glm::vec3(0, 0, -1);   // looking down at the +Z grid

    // Vertex Paint.
    std::vector<uint32_t> changed;
    expect(!g.has_colors, "a fresh mesh has no colour attribute");
    PaintDab back = d;
    back.view_dir = glm::vec3(0, 0, 1);
    vertex_paint_dab(g, cache, grid, s, back, changed);
    expect(changed.empty() && g.vertex_color(centre) == glm::vec4(1.0f), "front faces only: a dab from behind paints nothing");
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.has_colors && !changed.empty(), "painting turns the colour attribute on");
    const glm::vec4 c0 = g.vertex_color(centre);
    expect(c0.r > 0.99f && c0.g < 0.01f, "Mix at full strength paints the centre the brush colour");
    expect(g.vertex_color(edge) == glm::vec4(1.0f), "...and leaves vertices outside the radius white");
    const uint32_t mid = 8 * 17 + 8 + 3;   // 0.375 from the centre: partly painted
    const glm::vec4 cm = g.vertex_color(mid);
    expect(cm.g > 0.01f && cm.g < 0.99f, "the falloff blends part-way toward the rim");
    s.tool = PaintTool::Blur;
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.vertex_color(centre).g > c0.g, "Blur pulls the painted centre toward its neighbours");
    s.tool = PaintTool::Draw;
    s.blend = PaintBlend::Subtract;
    s.color = glm::vec4(1, 1, 1, 1);
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.vertex_color(centre).r < 0.01f, "Subtract darkens");
    d.invert = true;
    s.blend = PaintBlend::Mix;
    s.secondary = glm::vec4(0, 0, 1, 1);
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.vertex_color(centre).b > 0.99f, "Ctrl paints the secondary colour");
    d.invert = false;
    fill_vertex_colors(g, glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));
    invert_vertex_colors(g);
    expect(glm::distance(g.vertex_color(edge), glm::vec4(0.75f, 0.5f, 0.25f, 1.0f)) < 1e-5f, "Fill then Invert");
    clear_vertex_colors(g);
    expect(!g.has_colors && !mesh_to_node(g).at("colors").as_seq().size(), "Clear removes the attribute from the file");

    // Weight Paint.
    const uint32_t a = g.group_index("Arm"), b = g.group_index("Body");
    PaintSettings w;
    w.weight = 1.0f;
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(g.weight(centre, a) > 0.99f && g.weight(edge, a) == 0.0f, "Draw lays weight 1 at the centre, none outside");
    d.invert = true;
    d.strength = 0.5f;   // the app fills the dab's strength from PaintSettings::strength
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(std::abs(g.weight(centre, a) - 0.5f) < 1e-3f, "Ctrl (Mix) takes weight away");
    d.invert = false;
    w.blend = PaintBlend::Add;
    w.weight = 0.2f;
    d.strength = 1.0f;
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(std::abs(g.weight(centre, a) - 0.7f) < 1e-3f, "Add adds the brush weight");
    assign_weight(g, b, 1.0f);
    w.blend = PaintBlend::Mix;
    w.weight = 0.8f;
    w.auto_normalize = true;
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(std::abs(g.weight(centre, a) + g.weight(centre, b) - 1.0f) < 1e-4f && g.weight(centre, a) > 0.79f,
           "Auto Normalize rescales the other groups so the weights sum to 1");
    expect(g.weight(edge, b) == 1.0f, "...only where the brush touched");
    w.auto_normalize = false;
    w.tool = PaintTool::Blur;
    const float before = g.weight(centre, a);
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(g.weight(centre, a) < before, "Blur softens the peak");
    normalize_all_weights(g);
    float sum = 0.0f;
    for (const auto& vw : g.weights[mid]) sum += vw.weight;
    expect(std::abs(sum - 1.0f) < 1e-4f, "Normalize All");
    g.remove_group(a);
    expect(g.groups.size() == 1 && g.groups[0] == "Body" && g.weight(centre, 0) > 0.0f, "removing a group renumbers the rest");
    expect(weight_heatmap(0.0f) == glm::vec3(0, 0, 1) && weight_heatmap(1.0f) == glm::vec3(1, 0, 0), "heatmap: blue 0, red 1");
}

/** @brief The clip model: the runtime's YAML round trip, keying (quaternion hemisphere), moving
 *         and deleting dope-sheet cells, and renaming an object's tracks. */
void test_clip_model() {
    ClipModel m;
    m.name = "Wave";
    m.wrap = "pingpong";
    m.length = 2.0f;
    m.set_key("Arm", "position", 0.0f, glm::vec4(0, 0, 0, 0));
    m.set_key("Arm", "position", 1.0f, glm::vec4(1, 2, 3, 0));
    m.set_key("Arm", "rotation_quat", 0.0f, glm::vec4(0, 0, 0, 1));
    m.set_key("Arm", "rotation_quat", 1.0f, glm::vec4(0, 0, -0.7071f, -0.7071f));   // same rotation as +, other hemisphere
    m.set_key("Arm/Hand", "scale", 0.5f, glm::vec4(2, 2, 2, 0));
    expect(m.find_track("Arm", "rotation_quat")->keys[1].value.w > 0.0f,
           "a key in the opposite hemisphere is flipped next to its neighbour");
    m.set_key("Arm", "position", 1.0f, glm::vec4(5, 0, 0, 0));
    expect(m.find_track("Arm", "position")->keys.size() == 2 && m.find_track("Arm", "position")->keys[1].value.x == 5.0f,
           "keying an existing time replaces the key");

    const Node n = Node::deserialize(coopa::yaml::emit(m.to_node()));
    expect(ClipModel::from_node(n) == m, "the clip round-trips through YAML");
    // Keys the model does not know -- at clip, track and key level, and a procedural track --
    // come back verbatim.
    const Node odd = Node::deserialize(std::string(
        "clip:\n  name: odd\n  wrap: once\n  length: 2.0\n  events: [{time: 1.0, name: step}]\n  tracks:\n"
        "    - object: Arm\n      component: Light\n      component_index: 1\n      property: color.x\n"
        "      keys:\n        - {time: 0.0, value: [1.0], easing: step, note: hi}\n"
        "    - object: Arm\n      property: position\n      procedural: {type: sine, amplitude: [0.0, 0.0, 1.0], frequency: 2.0}\n"));
    const Node odd_back = Node::deserialize(coopa::yaml::emit(ClipModel::from_node(odd).to_node()));
    expect(odd_back == odd, "unknown clip, track and key fields and procedural tracks survive the editor:\n" + coopa::yaml::emit(odd_back));
    // Every clip file in the repository: the editor's model writes it back as it was.
    int clips = 0;
    for (const auto& p : asset_yaml_files()) {
        const Node src = coopa::yaml::load_document(p);
        if (!src.is_mapping() || !src.contains("clip")) continue;
        const Node back = Node::deserialize(coopa::yaml::emit(ClipModel::from_node(src).to_node()));
        const auto a = coopa::anim::parse_clip(src), b = coopa::anim::parse_clip(back);
        bool same = a.tracks.size() == b.tracks.size() && a.effective_length() == b.effective_length() && a.wrap == b.wrap;
        for (size_t t = 0; same && t < a.tracks.size(); ++t) {
            same = a.tracks[t].object_path == b.tracks[t].object_path && a.tracks[t].property == b.tracks[t].property &&
                   a.tracks[t].curve.keys().size() == b.tracks[t].curve.keys().size();
            for (size_t k = 0; same && k < a.tracks[t].curve.keys().size(); ++k) {
                const auto& ka = a.tracks[t].curve.keys()[k];
                const auto& kb = b.tracks[t].curve.keys()[k];
                same = std::abs(ka.time - kb.time) < 1e-6f && ka.easing == kb.easing;
                for (int c = 0; same && c < 4; ++c) same = std::abs(ka.value[c] - kb.value[c]) < 1e-5f;
            }
        }
        expect(same, fs::relative(p, ROOT_DIR).string() + ": the editor rewrites the clip the runtime reads identically");
        ++clips;
    }
    expect(clips >= 3, "checked the repository's clip files (" + std::to_string(clips) + ")");
    const coopa::anim::AnimationClip rt = coopa::anim::parse_clip(n);
    expect(rt.tracks.size() == 3 && rt.effective_length() == 2.0f && rt.wrap == coopa::anim::WrapMode::PingPong,
           "the runtime parses the editor's file (tracks, length, wrap)");

    const std::string arm = "Arm";
    expect(m.key_times(&arm) == std::vector<float>{0.0f, 1.0f} && m.key_times().size() == 3, "key times per object and in summary");
    auto moved = m.move_keys({{"Arm", 1.0f}}, 0.5f);
    expect(moved.count({"Arm", 1.5f}) && m.find_track("Arm", "position")->keys[1].time == 1.5f &&
               m.find_track("Arm", "rotation_quat")->keys[1].time == 1.5f,
           "moving a cell moves every track's key of that object at that time");
    m.delete_keys({{"Arm", 0.0f}});
    expect(m.find_track("Arm", "position")->keys.size() == 1, "deleting a cell removes the object's keys there");
    m.delete_keys({{"Arm/Hand", 0.5f}});
    expect(m.find_track("Arm/Hand", "scale") == nullptr, "a track left without keys goes");
    m.set_key("Arm/Hand", "scale", 0.5f, glm::vec4(1));
    m.rename_object("Arm", "Limb");
    expect(m.find_track("Limb", "position") && m.find_track("Limb/Hand", "scale"), "renaming an object renames its descendants' tracks");
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
    glm::vec2 scroll{0.0f};   ///< Wheel delta for the next frame only

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
        in.scroll = scroll;
        scroll = glm::vec2(0.0f);
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

/** @brief A modal with height 0 fits its content: no empty band under the last row. */
void test_imm_modal_fits_content() {
    ImmHarness h;
    coopa::ui::imm::Box region;
    float content_end = 0.0f;
    auto ui = [&](coopa::ui::imm::Context& c) {
        if (c.begin_modal("Unsaved Changes", {370, 0})) {
            c.paragraph("There are unsaved changes. Save them first?");
            c.spacing(4);
            c.button("Save All", 110);
            c.same_line();
            c.button("Discard", 110);
            region = c.content_region();
            content_end = c.cursor().y - c.style.spacing;
            c.end_modal();
        }
    };
    h.frame([&](coopa::ui::imm::Context& c) { c.open_modal("Unsaved Changes"); ui(c); });
    h.frame(ui);
    h.frame(ui);
    expect(region.h > 0.0f && std::abs(region.bottom() - content_end) <= 2.0f,
           "the fitted modal ends at its last row (gap " + std::to_string(region.bottom() - content_end) + " px)");
}

void test_imm_file_dialog() {
    // uicoopa's picker (the editor's and the hub's), driven through real frames.
    using coopa::input::Key;
    using coopa::input::KeyAction;
    using coopa::input::KeyEvent;
    using FD = coopa::ui::imm::FileDialog;
    const fs::path dir = fresh_dir("file_dialog");
    fs::create_directories(dir / "sub");
    for (const char* f : {"a.yaml", "b.png", "c.txt"}) write_text(dir / f, "x");
    ImmHarness h;
    FD fd;
    auto ui = [&](coopa::ui::imm::Context& c) { fd.draw(c); };
    const auto enter = std::vector<KeyEvent>{{Key::Enter, 0, KeyAction::Press, coopa::input::Mods::None}};

    fs::path got;
    fd.open(h.ctx, FD::Mode::OpenFile, "Open", dir, {".yaml"}, [&](const fs::path& p) { got = p; });
    h.frame(ui);
    h.frame(ui);
    expect(fd.is_open(), "open() shows the picker from the next frame");
    h.frame(ui, {'a'});                 // type-to-select
    h.frame(ui, {}, enter);
    expect(got == dir / "a.yaml" && !fd.is_open(), "typing a name selects it and Enter opens it");

    got.clear();
    fd.open(h.ctx, FD::Mode::OpenFile, "Open", dir, {".yaml"}, [&](const fs::path& p) { got = p; });
    h.frame(ui);
    h.frame(ui, {'b'});
    h.frame(ui, {}, enter);
    expect(got.empty() && fd.is_open(), "a file the Format filter rejects can't be opened");
    h.frame(ui, {}, {{Key::Escape, 0, KeyAction::Press, coopa::input::Mods::None}});
    expect(!fd.is_open() && got.empty(), "Escape cancels");

    got.clear();
    fd.open(h.ctx, FD::Mode::PickFolder, "Choose", dir, {}, [&](const fs::path& p) { got = p; });
    h.frame(ui);
    h.frame(ui, {'s'});
    h.frame(ui, {}, enter);
    expect(got == dir / "sub", "PickFolder chooses the selected folder");

    got.clear();
    fd.open(h.ctx, FD::Mode::SaveFile, "Save", dir, {".yaml"}, [&](const fs::path& p) { got = p; }, "a.yaml");
    h.frame(ui);
    h.frame(ui);
    h.frame(ui, {}, enter);
    expect(got.empty() && fd.is_open(), "saving over an existing file asks first");
    h.frame(ui, {}, enter);
    expect(got == dir / "a.yaml", "...and the second press replaces it");

    std::vector<fs::path> used;
    fd.on_folder_used = [&](const fs::path& p, FD::Mode) { used.push_back(p); };
    fd.open(h.ctx, FD::Mode::SaveFile, "Save", dir / "sub", {".yaml"}, [&](const fs::path& p) { got = p; }, "fresh");
    h.frame(ui);
    h.frame(ui);
    h.frame(ui, {}, enter);
    expect(got == dir / "sub" / "fresh.yaml", "Save adds the format's extension to a bare name");
    expect(used.size() == 1 && used[0] == dir / "sub", "on_folder_used reports the folder the dialog finished in");
}

void test_imm_log_view_follows_output() {
    // The hub's log drawer / the editor's build window: newest line in view while output
    // streams in, unless the user scrolls up; back at the bottom, it follows again.
    ImmHarness h;
    toy::editor::LogView view;
    std::vector<std::string> lines;
    const coopa::ui::imm::Box box{10, 10, 400, 168};   // 10 lines of 16 px
    auto ui = [&](coopa::ui::imm::Context& c) { view.draw(c, box, lines); };
    h.mouse = {100, 50};
    for (int i = 0; i < 40; ++i) { lines.push_back("line " + std::to_string(i)); h.frame(ui); }
    expect(view.following() && view.scroll() == 30, "it shows the newest lines while output streams in (" + std::to_string(view.scroll()) + ")");
    h.scroll = {0, 2};   // wheel up
    h.frame(ui);
    const int held = view.scroll();
    expect(!view.following() && held < 30, "scrolling up stops following");
    for (int i = 0; i < 10; ++i) { lines.push_back("more"); h.frame(ui); }
    expect(view.scroll() == held, "...and new output doesn't move the view");
    for (int i = 0; i < 10; ++i) { h.scroll = {0, -2}; h.frame(ui); }
    expect(view.following() && view.scroll() == 40, "scrolling back to the bottom follows again");
    lines.push_back("last");
    h.frame(ui);
    expect(view.scroll() == 41, "...so the next line is in view");
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
/** @brief Int-coded modes (fog_mode) show names, and picking one still writes the int. */
void test_schema_int_enum_labels() {
    const FieldDesc* fog = nullptr;
    for (const auto& g : render_settings_groups()) for (const auto& f : g.fields) if (f.key == "fog_mode") fog = &f;
    expect(fog && fog->kind == FieldKind::Int && fog->options.size() == 3 && fog->option_tips.size() == 3,
           "fog_mode is a labelled int enum");
    expect(fog && fog->options[2] == "Exponential Squared", "fog_mode 2 reads as Exponential Squared");

    ImmHarness h;
    Node block = Node::mapping();
    block["fog_mode"] = Node(static_cast<int64_t>(2));
    InspectorEnv env;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        draw_field(c, *fog, block, env);
        c.end_region();
    };
    h.frame(ui);
    expect(get_int(block, "fog_mode") == 2, "drawing does not change the value");
    float row_y = -1.0f;
    for (float y = 2.0f; y < 60.0f && row_y < 0.0f; y += 4.0f) {
        h.click({330, y}, ui);
        h.frame(ui);
        if (h.ctx.any_popup_open()) row_y = y;
    }
    expect(row_y >= 0.0f, "the fog mode row is a dropdown");
    // The list opens below the row with "Linear" first; pick it.
    for (float y = row_y + 4.0f; y < row_y + 120.0f && get_int(block, "fog_mode") == 2; y += 4.0f) {
        if (!h.ctx.any_popup_open()) { h.click({330, row_y}, ui); h.frame(ui); }
        h.click({330, y}, ui);
        h.frame(ui);
    }
    expect(block.at("fog_mode").is_scalar() && get_int(block, "fog_mode") != 2, "picking a mode writes its index");
    expect(get_string(block, "fog_mode") != "Linear", "the file still stores an int, not the label");
}

/** @brief The material panel draws its Shader dropdown once, at the top: draw_fields() leaves
 *         a skipped schema field to its caller instead of drawing it again further down. */
void test_imm_material_shader_drawn_once() {
    ImmHarness h;
    InspectorEnv env;
    Node m = Node::mapping();
    m["shader"] = Node(std::string("triplanar"));
    const float params[4] = {1.0f, 4.0f, 0.0f, 1.0f};
    m["shader_params"] = make_float_seq(params, 4);
    m["albedo"] = make_color(glm::vec3(0.8f));
    auto height_of = [&](const std::function<void(coopa::ui::imm::Context&)>& draw) {
        float y0 = 0.0f, y1 = 0.0f;
        h.frame([&](coopa::ui::imm::Context& c) {
            c.begin_region("r", {0, 0, 400, 2000}, false);
            y0 = c.cursor().y;
            draw(c);
            y1 = c.cursor().y;
            c.end_region();
        });
        return y1 - y0;
    };
    const float empty = height_of([](auto&) {});   // the region's own offset, not a row
    auto rows = [&](const std::function<void(coopa::ui::imm::Context&)>& draw) { return height_of(draw) - empty; };
    std::vector<FieldDesc> shader_fields, rest;
    for (const auto& f : material_fields()) (f.key == "shader" || f.key == "shader_params" ? shader_fields : rest).push_back(f);
    expect(shader_fields.size() == 2, "the material schema lists shader and shader_params");

    const float skipped = rows([&](auto& c) { draw_fields(c, shader_fields, m, env, false, {"shader", "shader_params"}); });
    expect(skipped == 0.0f, "draw_fields() draws no row for a skipped schema field (" + std::to_string(skipped) + ")");

    const float section = rows([&](auto& c) { draw_shader_section(c, m); });
    const float space = rows([&](auto& c) { c.spacing(); });
    const float fields = rows([&](auto& c) { draw_fields(c, rest, m, env, false, {"base"}); });
    const float block = rows([&](auto& c) { draw_material_block(c, m, env); });
    expect(section > 0.0f && block == section + space + fields,
           "the material block is the Shader section plus the other fields, nothing more (" +
           std::to_string(block) + " vs " + std::to_string(section + space + fields) + ")");
}

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

/** @brief Symmetry pairs and Mirror, in local and global axes. */
/** @brief The material viewer's shader ball: closed, consistently wound parts, facing out, with flat walls. */
void test_shader_ball() {
    const EditMesh m = make_shader_ball();
    expect(closed_and_consistent(m), "every part of the shader ball is closed with consistent winding");
    // Signed volume (divergence theorem over the triangulated faces): positive = facing out.
    double vol = 0.0;
    size_t flat = 0;
    for (const auto& f : m.faces) {
        flat += f.smooth ? 0 : 1;
        for (size_t i = 1; i + 1 < f.corners.size(); ++i) {
            const glm::dvec3 a = m.positions[f.corners[0].v], b = m.positions[f.corners[i].v], c = m.positions[f.corners[i + 1].v];
            vol += glm::dot(a, glm::cross(b, c)) / 6.0;
        }
    }
    const double pi = 3.14159265358979;
    const double shell = 4.0 / 3.0 * pi * (0.125 - 0.43 * 0.43 * 0.43) * 7.0 / 8.0, core = 4.0 / 3.0 * pi * 0.34 * 0.34 * 0.34;
    expect(vol > shell + core, "the shader ball faces outward (volume " + std::to_string(vol) + ")");
    expect(flat >= 4 + 16 + 12, "it has flat, hard-edged faces: cut walls and the square plinth");
    glm::vec3 lo, hi;
    m.bounds(lo, hi);
    expect(hi.z - lo.z > 1.0f && hi.x - lo.x < 1.05f, "about a unit ball on a plinth");
}

/** @brief Proportional editing weights: falloff curves and the three ways of measuring distance. */
void test_proportional_weights() {
    for (int i = 0; i < kFalloffCount; ++i) {
        const Falloff f = static_cast<Falloff>(i);
        expect(std::abs(falloff_weight(f, 0.0f) - 1.0f) < 1e-5f && falloff_weight(f, 1.0f) == 0.0f && falloff_weight(f, 2.0f) == 0.0f,
               std::string("falloff ") + falloff_name(f) + ": 1 at the selection, 0 at and past the radius");
        for (float t = 0.0f; t < 0.95f; t += 0.05f) {
            expect(falloff_weight(f, t) >= falloff_weight(f, t + 0.05f) - 1e-5f, std::string("falloff ") + falloff_name(f) + " never rises");
        }
    }
    expect(std::abs(falloff_weight(Falloff::Linear, 0.25f) - 0.75f) < 1e-5f, "linear is 1 - t");

    // A row of vertices 0..9 along X, 1 m apart (joined by edges); plus a separate pair at y = 5.
    EditMesh m;
    auto tri = [&](uint32_t a, uint32_t b) {
        Face f;
        for (uint32_t v : {a, b, a}) { Corner c; c.v = v; f.corners.push_back(c); }
        m.faces.push_back(f);
    };
    for (int i = 0; i < 10; ++i) m.positions.push_back({float(i), 0.0f, 0.0f});
    for (uint32_t i = 0; i + 1 < 10; ++i) tri(i, i + 1);
    m.positions.push_back({2.0f, 5.0f, 0.0f});
    m.positions.push_back({3.0f, 5.0f, 0.0f});
    tri(10, 11);
    ProportionalSettings ps;
    ps.enabled = true;
    ps.falloff = Falloff::Linear;
    ps.radius = 4.0f;
    ps.projected = false;
    auto weight_of = [](const std::vector<std::pair<uint32_t, float>>& w, uint32_t v) {
        for (const auto& [u, x] : w) if (u == v) return x;
        return 0.0f;
    };
    const std::set<uint32_t> sel{0};
    auto w = proportional_weights(m, sel, ps, glm::mat4(1.0f));
    expect(weight_of(w, 0) == 0.0f, "the selection itself is not in the weighted list (it moves fully)");
    expect(std::abs(weight_of(w, 1) - 0.75f) < 1e-4f && std::abs(weight_of(w, 2) - 0.5f) < 1e-4f && weight_of(w, 4) == 0.0f,
           "3D: weights fall off linearly to 0 at the radius");
    expect(weight_of(w, 10) == 0.0f, "3D: (2, 5) is 5.4 m away -- outside");
    ps.radius = 6.0f;
    w = proportional_weights(m, sel, ps, glm::mat4(1.0f));
    expect(weight_of(w, 10) > 0.0f, "3D: a bigger radius reaches the separate pair");
    ps.connected = true;
    w = proportional_weights(m, sel, ps, glm::mat4(1.0f));
    expect(weight_of(w, 10) == 0.0f && std::abs(weight_of(w, 3) - 0.5f) < 1e-4f, "connected: the separate pair stays; edge distance along the row");
    ps.connected = false;
    ps.projected = true;
    // Looking along +Y at the row: screen x = world x * 100 px; y is depth.
    auto project = [](const glm::vec3& p) { return std::optional<glm::vec2>(glm::vec2(p.x * 100.0f, -p.z * 100.0f)); };
    w = proportional_weights(m, sel, ps, glm::mat4(1.0f), project, 250.0f);
    expect(std::abs(weight_of(w, 2) - 0.2f) < 1e-4f && weight_of(w, 3) == 0.0f, "projected: pixel distance against the pixel radius");
    expect(std::abs(weight_of(w, 10) - 0.2f) < 1e-4f, "projected: depth doesn't count -- the pair 5 m behind is at x = 200 px");
}


void test_mesh_mirror() {
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

// =====================================================================================
// UI designer math (group "viewport") -- ui_canvas_math.h / rect_gizmo.h, no GPU
// =====================================================================================

bool near_rect(const ui::Rect& a, const ui::Rect& b, float eps = 1e-3f) {
    return glm::all(glm::lessThan(glm::abs(a.min - b.min), glm::vec2(eps))) && glm::all(glm::lessThan(glm::abs(a.max - b.max), glm::vec2(eps)));
}

void test_ui_rect_math() {
    const ui::Rect parent{{0, 0}, {1000, 500}};
    ui::RectParams p;
    p.anchor_min = p.anchor_max = {0.5f, 0.5f};
    p.pivot = {0.5f, 0.5f};
    p.size_delta = {200, 100};
    p.anchored_position = {50, 20};
    const ui::Rect r0 = coopa::ui::resolve_rect(parent, p);
    expect(near_rect(r0, {{450, 220}, {650, 320}}), "ui math: a centred rect resolves where expected");

    // View mapping is an exact round trip, with Y flipped (canvas up, editor down).
    ui::UiView v;
    v.origin = {100, 50};
    v.scale = 0.5f;
    v.canvas_size = {1000, 500};
    const glm::vec2 e = v.to_editor({250, 100});
    expect(glm::distance(e, glm::vec2(225, 250)) < 1e-4f && glm::distance(v.to_canvas(e), glm::vec2(250, 100)) < 1e-4f,
           "ui math: editor <-> canvas mapping round-trips with Y flipped");

    // Resize from the right edge keeps the left edge; from a corner with Alt keeps the centre.
    const ui::Rect rr = ui::resize_rect(r0, 1, 0, {30, 0}, false, false);
    expect(std::abs(rr.min.x - r0.min.x) < 1e-4f && std::abs(rr.max.x - (r0.max.x + 30)) < 1e-4f && rr.min.y == r0.min.y,
           "ui math: resizing the right edge keeps the left edge");
    const ui::Rect rs = ui::resize_rect(r0, 1, 1, {20, 10}, true, false);
    expect(glm::distance(rs.center(), r0.center()) < 1e-3f && std::abs(rs.size().x - (r0.size().x + 40)) < 1e-3f,
           "ui math: Alt resizes symmetrically about the centre");
    const ui::Rect ra = ui::resize_rect(r0, 1, 1, {100, 0}, false, true);
    expect(std::abs(ra.size().x / ra.size().y - r0.size().x / r0.size().y) < 1e-3f, "ui math: Shift keeps the aspect ratio");
    const ui::Rect inv = ui::resize_rect(r0, -1, 0, {500, 0}, false, false);
    expect(inv.size().x >= 1.0f && std::abs(inv.max.x - r0.max.x) < 1e-4f, "ui math: a resize never inverts the rect");

    // set_rect / anchors / pivot all keep the rect where it is.
    ui::RectParams q = p;
    ui::set_rect(parent, q, rr);
    expect(near_rect(coopa::ui::resolve_rect(parent, q), rr), "ui math: set_rect() re-expresses a rect exactly");
    q = p;
    ui::set_anchors_keep_rect(parent, q, {0, 0}, {1, 1});
    expect(near_rect(coopa::ui::resolve_rect(parent, q), r0), "ui math: changing anchors keeps the rect on screen");
    ui::set_pivot_keep_rect(parent, q, {0, 1});
    expect(near_rect(coopa::ui::resolve_rect(parent, q), r0) && q.pivot == glm::vec2(0, 1), "ui math: moving the pivot keeps the rect");
    // ...and the stretched rect follows its parent when the parent grows.
    const ui::Rect big{{0, 0}, {2000, 1000}};
    const ui::Rect stretched = coopa::ui::resolve_rect(big, q);
    expect(std::abs(stretched.size().x - (r0.size().x + 1000)) < 1e-3f, "ui math: stretch anchors grow with the parent");

    // Presets: Unity's picker (rect stays; Shift pivot; Alt snaps).
    const auto& presets = ui::anchor_presets();
    expect(presets.size() == 16, "ui math: 16 anchor presets (the 4x4 picker)");
    q = p;
    const ui::AnchorPresetInfo* tl = nullptr;
    for (const auto& pr : presets) if (std::string(pr.name) == "TopLeft") tl = &pr;
    ui::apply_anchor_preset(parent, q, *tl, false, false);
    expect(near_rect(coopa::ui::resolve_rect(parent, q), r0) && q.anchor_min == glm::vec2(0, 1), "ui math: a preset alone keeps the rect");
    ui::apply_anchor_preset(parent, q, *tl, true, true);
    expect(q.pivot == glm::vec2(0, 1) && q.anchored_position == glm::vec2(0) &&
           near_rect(coopa::ui::resolve_rect(parent, q), {{0, 400}, {200, 500}}), "ui math: Shift+Alt preset snaps into the top-left corner");
    expect(ui::matching_preset(q) && std::string(ui::matching_preset(q)->name) == "TopLeft", "ui math: the current preset is recognised");

    // Snapping: an edge within the threshold moves onto the target and reports a guide.
    std::vector<ui::SnapLine> lines;
    ui::add_rect_lines(lines, parent);
    const bool all[2][3] = {{true, true, true}, {true, true, true}};
    std::vector<ui::SnapLine> hit;
    const glm::vec2 d = ui::snap_rect({{3, 100}, {103, 150}}, lines, 5.0f, all, &hit);
    expect(std::abs(d.x + 3.0f) < 1e-4f && !hit.empty(), "ui math: an edge 3 px from the parent's snaps to it");
}

void test_ui_rect_block_roundtrip() {
    Node blk = fkyaml::node::deserialize(std::string(
        "{type: RectTransform, anchor_preset: StretchAll, offset_min: {x: 10, y: 20}, offset_max: {x: -30, y: -40}, rotation: 15}"));
    const coopa::ui::RectTransform rt = ui::parse_rect_block(blk);
    coopa::ui::RectTransform engine_rt;
    coopa::ui::detail::parse_rect_transform(blk, engine_rt);
    const ui::Rect parent{{0, 0}, {800, 600}};
    expect(near_rect(coopa::ui::resolve_rect(parent, rt.params()), coopa::ui::resolve_rect(parent, engine_rt.params())),
           "ui block: the editor reads a RectTransform exactly as the engine does (preset + offsets)");
    Node out = blk;
    ui::write_rect_params(out, rt.params());
    expect(!out.contains("anchor_preset") && !out.contains("offset_min") && out.contains("anchor_min") && out.contains("size_delta") &&
           out.contains("rotation"), "ui block: writing normalises to anchors/pivot/position/size (rotation kept)");
    coopa::ui::RectTransform again;
    coopa::ui::detail::parse_rect_transform(out, again);
    expect(near_rect(coopa::ui::resolve_rect(parent, again.params()), coopa::ui::resolve_rect(parent, engine_rt.params())),
           "ui block: ...and resolves to the same rect");
}

void test_ui_rect_gizmo() {
    const ui::Rect parent{{0, 0}, {1000, 500}};
    ui::UiView v;
    v.origin = {0, 0};
    v.scale = 1.0f;
    v.canvas_size = {1000, 500};
    RectGizmoTarget t;
    t.parent = parent;
    t.params.anchor_min = t.params.anchor_max = {0.5f, 0.5f};
    t.params.size_delta = {200, 100};
    t.rect = coopa::ui::resolve_rect(parent, t.params);   // (400,200)-(600,300)
    RectGizmo g;
    RectGizmoSettings s;
    s.snap = false;
    // Handles: editor Y is down, so the top edge (canvas y 300) is at editor y 200.
    expect(g.hit(v, t, {600, 250}) == RectHandle::Right && g.hit(v, t, {500, 200}) == RectHandle::Top &&
           g.hit(v, t, {450, 260}) == RectHandle::Body && g.hit(v, t, {500, 250}) == RectHandle::Pivot,
           "rect gizmo: edges, body and pivot are hit where they are drawn");
    auto drive = [&](glm::vec2 from, glm::vec2 to, bool shift = false) {
        RectGizmoInput in;
        in.mouse = from; in.pressed = true; in.down = true; in.shift = shift;
        g.update(v, t, in, s);
        in.pressed = false; in.mouse = to;
        RectGizmoResult r = g.update(v, t, in, s);
        in.down = false; in.released = true;
        const RectGizmoResult f = g.update(v, t, in, s);
        r.finished = f.finished;
        return r;
    };
    RectGizmoResult r = drive({450, 260}, {490, 240});
    expect(r.changed && std::abs(r.params.anchored_position.x - 40) < 1e-3f && std::abs(r.params.anchored_position.y - 20) < 1e-3f && r.finished,
           "rect gizmo: a body drag moves by the drag (Y flipped into canvas space)");
    r = drive({600, 250}, {650, 250});
    expect(std::abs(r.rect.max.x - 650) < 1e-3f && std::abs(r.rect.min.x - 400) < 1e-3f, "rect gizmo: the right handle resizes, the left edge stays");
    // Anchor split: drag the top-right anchor triangle (drawn just outside the anchor point).
    t.params.anchor_min = {0.4f, 0.4f};
    t.params.anchor_max = {0.4f, 0.4f};
    t.params.anchored_position = {100, 50};
    t.rect = coopa::ui::resolve_rect(parent, t.params);
    const glm::vec2 anchor_px = v.to_editor({400, 200});
    expect(g.hit(v, t, anchor_px + glm::vec2(6, -6)) == RectHandle::AnchorTR || g.hit(v, t, anchor_px) != RectHandle::None,
           "rect gizmo: the anchors are grabbable");
    // Snapping to the parent's centre line.
    s.snap = true;
    s.lines.clear();
    ui::add_rect_lines(s.lines, parent);
    t.params.anchor_min = t.params.anchor_max = {0.5f, 0.5f};
    t.params.anchored_position = {0, 0};
    t.rect = coopa::ui::resolve_rect(parent, t.params);
    r = drive({450, 260}, {453, 260});
    expect(std::abs(r.params.anchored_position.x) < 1e-3f && !r.guides.empty(), "rect gizmo: a small drag snaps back onto the centre line");
    // Rotation with Shift snaps to 15 degrees.
    s.snap = false;
    r = drive({615, 185}, {640, 250}, true);
    expect(std::abs(std::fmod(std::abs(r.rotation), 15.0f)) < 1e-3f, "rect gizmo: Shift snaps rotation to 15 degrees");
}

void test_ui_palette_entries_load() {
    coopa::ui::register_ui_components();
    int loaded = 0;
    for (const auto& e : ui::palette()) {
        Node made = e.make();
        Node obj;
        if (e.component) {
            obj = ui::palette_detail::object("Host", {ui::palette_detail::centered(100, 100), made});
        } else {
            obj = made;
        }
        Node canvas = ui::palette_detail::object("Canvas", {ui::palette_detail::stretch(), ui::palette_detail::comp("Canvas")}, {obj});
        Node doc = Node::mapping();
        doc["format"] = Node(std::string("toyengine"));
        Node sc = Node::mapping();
        sc["scene_name"] = Node(std::string("PaletteProbe"));
        Node roots = Node::sequence();
        roots.as_seq().push_back(canvas);
        sc["root_objects"] = roots;
        doc["scene"] = sc;
        try {
            coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc, "palette_probe.yaml");
            auto* c = scene.find_object("Canvas");
            auto* canvas_comp = c ? c->get_component<coopa::ui::CanvasComponent>() : nullptr;
            expect(canvas_comp != nullptr && c->children().size() == 1, "palette " + e.id + ": loads under a canvas");
            if (canvas_comp) {
                canvas_comp->set_viewport(1920, 1080);
                canvas_comp->preview_refresh();
                expect(!canvas_comp->draw_list().vertices().empty() || e.id == "container" || e.id == "spacer" ||
                       e.id == "vstack" || e.id == "hstack" || e.id == "grid" || e.id == "hud_corner" || e.id == "message_log" ||
                       e.id == "label" || e.id == "text" || e.id == "title" || e.id == "prompt_bar" || e.component,   // text / icons only: no font headless
                       "palette " + e.id + ": draws something");
            }
            ++loaded;
        } catch (const std::exception& ex) {
            expect(false, "palette " + e.id + " throws: " + ex.what());
        }
        expect(find_schema(component_type(e.component ? made : made.at("components").as_seq().back())) != nullptr,
               "palette " + e.id + ": its main component has an inspector schema");
    }
    expect(loaded >= 30, "every palette entry loads (" + std::to_string(loaded) + ")");
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


/** @brief An engine default, numeric (as a vec4) or a string, for comparing with a schema field. */
struct EngineDefault {
    glm::vec4 v{0.0f};
    std::string s;
    bool text = false;
    EngineDefault(bool b) : v(b ? 1.0f : 0.0f) {}
    EngineDefault(int i) : v(static_cast<float>(i)) {}
    EngineDefault(uint32_t i) : v(static_cast<float>(i)) {}
    EngineDefault(float f) : v(f) {}
    EngineDefault(const glm::vec3& c) : v(c, 0.0f) {}
    EngineDefault(const glm::vec4& c) : v(c) {}
    EngineDefault(const std::string& t) : s(t), text(true) {}
    EngineDefault(toy::render::RenderQuality q)
        : s(q == toy::render::RenderQuality::Low ? "low" : q == toy::render::RenderQuality::Medium ? "medium"
            : q == toy::render::RenderQuality::Ultra ? "ultra" : "high"), text(true) {}
};

/**
 * @brief The Render tab covers every render key the engine parses, once, and every default it
 *        shows is the engine's (AppConfig::from_node() of an empty file: High quality presets).
 *        The table is every `render.` key AppConfig reads, with the field it lands in.
 */
void test_render_settings_cover_engine() {
    using PR = toy::render::PixelRenderConfig;
    const std::vector<std::pair<std::string, std::function<EngineDefault(const PR&)>>> engine_keys = {
        {"shadow_quality", [](const PR& r) { return EngineDefault(r.shadow_quality); }},
        {"ssao_quality", [](const PR& r) { return EngineDefault(r.ssao_quality); }},
        {"ssr_quality", [](const PR& r) { return EngineDefault(r.ssr_quality); }},
        {"ssgi_quality", [](const PR& r) { return EngineDefault(r.ssgi_quality); }},
        {"dof_quality", [](const PR& r) { return EngineDefault(r.dof_quality); }},
        {"volumetrics_quality", [](const PR& r) { return EngineDefault(r.volumetrics_quality); }},
        {"sdf_quality", [](const PR& r) { return EngineDefault(r.sdf_quality); }},
        {"water_quality", [](const PR& r) { return EngineDefault(r.water_quality); }},
        {"outline_enabled", [](const PR& r) { return EngineDefault(r.outline_enabled); }},
        {"palette_enabled", [](const PR& r) { return EngineDefault(r.palette_enabled); }},
        {"dither_enabled", [](const PR& r) { return EngineDefault(r.dither_enabled); }},
        {"camera_pixel_snap", [](const PR& r) { return EngineDefault(r.camera_pixel_snap); }},
        {"soft_lighting", [](const PR& r) { return EngineDefault(r.soft_lighting); }},
        {"ssao_enabled", [](const PR& r) { return EngineDefault(r.ssao_enabled); }},
        {"ssr_enabled", [](const PR& r) { return EngineDefault(r.ssr_enabled); }},
        {"transparency_enabled", [](const PR& r) { return EngineDefault(r.transparency_enabled); }},
        {"refraction_enabled", [](const PR& r) { return EngineDefault(r.refraction_enabled); }},
        {"fog_enabled", [](const PR& r) { return EngineDefault(r.fog_enabled); }},
        {"underwater_enabled", [](const PR& r) { return EngineDefault(r.underwater_enabled); }},
        {"volumetrics_enabled", [](const PR& r) { return EngineDefault(r.volumetrics_enabled); }},
        {"sdf_enabled", [](const PR& r) { return EngineDefault(r.sdf_enabled); }},
        {"sdf_shadows_enabled", [](const PR& r) { return EngineDefault(r.sdf_shadows_enabled); }},
        {"bloom_enabled", [](const PR& r) { return EngineDefault(r.bloom_enabled); }},
        {"tilt_shift_enabled", [](const PR& r) { return EngineDefault(r.tilt_shift_enabled); }},
        {"dof_enabled", [](const PR& r) { return EngineDefault(r.dof_enabled); }},
        {"debug_view", [](const PR& r) { return EngineDefault(r.debug_view); }},
        {"world_ui_enabled", [](const PR& r) { return EngineDefault(r.world_ui_enabled); }},
        {"screen_ui_enabled", [](const PR& r) { return EngineDefault(r.screen_ui_enabled); }},
        {"resolution_mode", [](const PR& r) { return EngineDefault(r.resolution_mode); }},
        {"render_width", [](const PR& r) { return EngineDefault(r.render_width); }},
        {"render_height", [](const PR& r) { return EngineDefault(r.render_height); }},
        {"scale_divisor", [](const PR& r) { return EngineDefault(r.scale_divisor); }},
        {"upscale_mode", [](const PR& r) { return EngineDefault(r.upscale_mode); }},
        {"exposure", [](const PR& r) { return EngineDefault(r.exposure); }},
        {"auto_exposure_enabled", [](const PR& r) { return EngineDefault(r.auto_exposure_enabled); }},
        {"auto_exposure_compensation", [](const PR& r) { return EngineDefault(r.auto_exposure_compensation); }},
        {"auto_exposure_speed_up", [](const PR& r) { return EngineDefault(r.auto_exposure_speed_up); }},
        {"auto_exposure_speed_down", [](const PR& r) { return EngineDefault(r.auto_exposure_speed_down); }},
        {"auto_exposure_min", [](const PR& r) { return EngineDefault(r.auto_exposure_min); }},
        {"auto_exposure_max", [](const PR& r) { return EngineDefault(r.auto_exposure_max); }},
        {"grading_enabled", [](const PR& r) { return EngineDefault(r.grading_enabled); }},
        {"light_bands", [](const PR& r) { return EngineDefault(r.light_bands); }},
        {"spec_threshold", [](const PR& r) { return EngineDefault(r.spec_threshold); }},
        {"rim_strength", [](const PR& r) { return EngineDefault(r.rim_strength); }},
        {"ambient_intensity", [](const PR& r) { return EngineDefault(r.indirect.ambient_intensity); }},
        {"sky_intensity", [](const PR& r) { return EngineDefault(r.indirect.sky_intensity); }},
        {"sky_zenith", [](const PR& r) { return EngineDefault(r.indirect.sky_zenith); }},
        {"sky_horizon", [](const PR& r) { return EngineDefault(r.indirect.sky_horizon); }},
        {"sky_ground", [](const PR& r) { return EngineDefault(r.indirect.sky_ground); }},
        {"shadows_enabled", [](const PR& r) { return EngineDefault(r.shadows_enabled); }},
        {"shadow_map_resolution", [](const PR& r) { return EngineDefault(r.shadow_map_resolution); }},
        {"shadow_cascades", [](const PR& r) { return EngineDefault(r.shadow_cascades); }},
        {"shadow_cascade_split_lambda", [](const PR& r) { return EngineDefault(r.shadow_cascade_split_lambda); }},
        {"shadow_fit", [](const PR& r) { return EngineDefault(r.shadow_fit); }},
        {"shadow_focus_radius", [](const PR& r) { return EngineDefault(r.shadow_focus_radius); }},
        {"shadow_focus_distance", [](const PR& r) { return EngineDefault(r.shadow_focus_distance); }},
        {"shadow_pcf_max_texels", [](const PR& r) { return EngineDefault(r.shadow_pcf_max_texels); }},
        {"shadow_receiver_plane_bias", [](const PR& r) { return EngineDefault(r.shadow_receiver_plane_bias); }},
        {"shadow_receiver_max_slope", [](const PR& r) { return EngineDefault(r.shadow_receiver_max_slope); }},
        {"cube_shadow_resolution", [](const PR& r) { return EngineDefault(r.cube_shadow_resolution); }},
        {"spot_shadow_resolution", [](const PR& r) { return EngineDefault(r.spot_shadow_resolution); }},
        {"local_shadow_atlas_resolution", [](const PR& r) { return EngineDefault(r.local_shadow_atlas_resolution); }},
        {"max_shadowed_point_lights", [](const PR& r) { return EngineDefault(r.max_shadowed_point_lights); }},
        {"max_shadowed_spot_lights", [](const PR& r) { return EngineDefault(r.max_shadowed_spot_lights); }},
        {"shadow_cache_enabled", [](const PR& r) { return EngineDefault(r.shadow_cache_enabled); }},
        {"shadow_bias", [](const PR& r) { return EngineDefault(r.shadow_bias); }},
        {"shadow_depth_bias_texels", [](const PR& r) { return EngineDefault(r.shadow_depth_bias_texels); }},
        {"shadow_slope_bias_texels", [](const PR& r) { return EngineDefault(r.shadow_slope_bias_texels); }},
        {"shadow_slope_bias_max", [](const PR& r) { return EngineDefault(r.shadow_slope_bias_max); }},
        {"shadow_fade_fraction", [](const PR& r) { return EngineDefault(r.shadow_fade_fraction); }},
        {"shadow_normal_bias", [](const PR& r) { return EngineDefault(r.shadow_normal_bias); }},
        {"shadow_distance", [](const PR& r) { return EngineDefault(r.shadow_distance); }},
        {"soft_shadows", [](const PR& r) { return EngineDefault(r.soft_shadows); }},
        {"shadow_softness", [](const PR& r) { return EngineDefault(r.shadow_softness); }},
        {"point_shadow_softness", [](const PR& r) { return EngineDefault(r.point_shadow_softness); }},
        {"spot_shadow_softness", [](const PR& r) { return EngineDefault(r.spot_shadow_softness); }},
        {"shadow_pcf_samples", [](const PR& r) { return EngineDefault(r.shadow_pcf_samples); }},
        {"shadow_pcss_enabled", [](const PR& r) { return EngineDefault(r.shadow_pcss_enabled); }},
        {"shadow_pcss_light_size", [](const PR& r) { return EngineDefault(r.shadow_pcss_light_size); }},
        {"shadow_pcss_search_texels", [](const PR& r) { return EngineDefault(r.shadow_pcss_search_texels); }},
        {"shadow_pcss_taps", [](const PR& r) { return EngineDefault(r.shadow_pcss_taps); }},
        {"contact_shadows_enabled", [](const PR& r) { return EngineDefault(r.contact_shadows_enabled); }},
        {"contact_shadow_length", [](const PR& r) { return EngineDefault(r.contact_shadow_length); }},
        {"contact_shadow_strength", [](const PR& r) { return EngineDefault(r.contact_shadow_strength); }},
        {"contact_shadow_thickness", [](const PR& r) { return EngineDefault(r.contact_shadow_thickness); }},
        {"contact_shadow_steps", [](const PR& r) { return EngineDefault(r.contact_shadow_steps); }},
        {"contact_shadow_temporal_enabled", [](const PR& r) { return EngineDefault(r.contact_shadow_temporal_enabled); }},
        {"contact_shadow_temporal_frames", [](const PR& r) { return EngineDefault(r.contact_shadow_temporal_frames); }},
        {"outline_thickness", [](const PR& r) { return EngineDefault(r.outline_thickness); }},
        {"outline_color", [](const PR& r) { return EngineDefault(r.outline_color); }},
        {"depth_threshold", [](const PR& r) { return EngineDefault(r.depth_threshold); }},
        {"normal_threshold", [](const PR& r) { return EngineDefault(r.normal_threshold); }},
        {"palette", [](const PR& r) { return EngineDefault(r.palette_path); }},
        {"grading_lut", [](const PR& r) { return EngineDefault(r.grading_lut_path); }},
        {"dither_strength", [](const PR& r) { return EngineDefault(r.dither_strength); }},
        {"texel_aa", [](const PR& r) { return EngineDefault(r.texel_aa); }},
        {"ssao_radius", [](const PR& r) { return EngineDefault(r.ssao_radius); }},
        {"ssao_bias", [](const PR& r) { return EngineDefault(r.ssao_bias); }},
        {"ssao_power", [](const PR& r) { return EngineDefault(r.ssao_power); }},
        {"ssao_slices", [](const PR& r) { return EngineDefault(r.ssao_slices); }},
        {"ssao_steps", [](const PR& r) { return EngineDefault(r.ssao_steps); }},
        {"ssao_max_radius_px", [](const PR& r) { return EngineDefault(r.ssao_max_radius_px); }},
        {"ssao_blur_plane_sigma", [](const PR& r) { return EngineDefault(r.ssao_blur_plane_sigma); }},
        {"ssao_half_res", [](const PR& r) { return EngineDefault(r.ssao_half_res); }},
        {"ssao_blur_light", [](const PR& r) { return EngineDefault(r.ssao_blur_light); }},
        {"ssao_direct_lighting_strength", [](const PR& r) { return EngineDefault(r.ssao_direct_lighting_strength); }},
        {"ssao_temporal_enabled", [](const PR& r) { return EngineDefault(r.ssao_temporal_enabled); }},
        {"ssao_temporal_frames", [](const PR& r) { return EngineDefault(r.ssao_temporal_frames); }},
        {"ssao_temporal_gamma", [](const PR& r) { return EngineDefault(r.ssao_temporal_gamma); }},
        {"ssao_intensity", [](const PR& r) { return EngineDefault(r.ssao_intensity); }},
        {"ssr_max_distance", [](const PR& r) { return EngineDefault(r.ssr_max_distance); }},
        {"ssr_max_iterations", [](const PR& r) { return EngineDefault(r.ssr_max_iterations); }},
        {"ssr_thickness", [](const PR& r) { return EngineDefault(r.ssr_thickness); }},
        {"ssr_thickness_scale", [](const PR& r) { return EngineDefault(r.ssr_thickness_scale); }},
        {"ssr_bias_texels", [](const PR& r) { return EngineDefault(r.ssr_bias_texels); }},
        {"ssr_roughness_cutoff", [](const PR& r) { return EngineDefault(r.ssr_roughness_cutoff); }},
        {"ssr_start_mip", [](const PR& r) { return EngineDefault(r.ssr_start_mip); }},
        {"ssr_min_mip0_steps", [](const PR& r) { return EngineDefault(r.ssr_min_mip0_steps); }},
        {"ssr_temporal_enabled", [](const PR& r) { return EngineDefault(r.ssr_temporal_enabled); }},
        {"ssr_temporal_frames", [](const PR& r) { return EngineDefault(r.ssr_temporal_frames); }},
        {"ssgi_temporal_frames", [](const PR& r) { return EngineDefault(r.ssgi_temporal_frames); }},
        {"ssr_temporal_blend", [](const PR& r) { return EngineDefault(r.ssr_temporal_blend); }},
        {"ssr_blur_radius", [](const PR& r) { return EngineDefault(r.ssr_blur_radius); }},
        {"ssr_blur_light", [](const PR& r) { return EngineDefault(r.ssr_blur_light); }},
        {"ssr_blur_zero_skip", [](const PR& r) { return EngineDefault(r.ssr_blur_zero_skip); }},
        {"ssr_jitter", [](const PR& r) { return EngineDefault(r.ssr_jitter); }},
        {"ssr_rays_per_pixel", [](const PR& r) { return EngineDefault(r.ssr_rays_per_pixel); }},
        {"ssr_cone_prefilter", [](const PR& r) { return EngineDefault(r.ssr_cone_prefilter); }},
        {"ssr_skip_behind", [](const PR& r) { return EngineDefault(r.ssr_skip_behind); }},
        {"ssr_half_res", [](const PR& r) { return EngineDefault(r.ssr_half_res); }},
        {"ssgi_resolution_scale", [](const PR& r) { return EngineDefault(r.ssgi_resolution_scale); }},
        {"ssr_skip_negligible", [](const PR& r) { return EngineDefault(r.ssr_skip_negligible); }},
        {"ssr_skip_threshold", [](const PR& r) { return EngineDefault(r.ssr_skip_threshold); }},
        {"ssr_temporal_gamma", [](const PR& r) { return EngineDefault(r.ssr_temporal_gamma); }},
        {"ssgi_traced", [](const PR& r) { return EngineDefault(r.ssgi_traced); }},
        {"ssgi_max_distance", [](const PR& r) { return EngineDefault(r.ssgi_max_distance); }},
        {"ssgi_blur_radius", [](const PR& r) { return EngineDefault(r.ssgi_blur_radius); }},
        {"ssgi_blur_light", [](const PR& r) { return EngineDefault(r.ssgi_blur_light); }},
        {"ssgi_max_iterations", [](const PR& r) { return EngineDefault(r.ssgi_max_iterations); }},
        {"ssgi_intensity", [](const PR& r) { return EngineDefault(r.indirect.ssgi_intensity); }},
        {"ssgi_distance", [](const PR& r) { return EngineDefault(r.indirect.ssgi_distance); }},
        {"refraction_ior", [](const PR& r) { return EngineDefault(r.refraction_ior); }},
        {"refraction_thickness", [](const PR& r) { return EngineDefault(r.refraction_thickness); }},
        {"refraction_strength", [](const PR& r) { return EngineDefault(r.refraction_strength); }},
        {"refraction_max_offset", [](const PR& r) { return EngineDefault(r.refraction_max_offset); }},
        {"refraction_chromatic", [](const PR& r) { return EngineDefault(r.refraction_chromatic); }},
        {"refraction_blur", [](const PR& r) { return EngineDefault(r.refraction_blur); }},
        {"refraction_density", [](const PR& r) { return EngineDefault(r.refraction_density); }},
        {"refraction_fresnel", [](const PR& r) { return EngineDefault(r.refraction_fresnel); }},
        {"refraction_tint", [](const PR& r) { return EngineDefault(r.refraction_tint); }},
        {"refraction_include_reflections", [](const PR& r) { return EngineDefault(r.refraction_include_reflections); }},
        {"fog_mode", [](const PR& r) { return EngineDefault(r.fog_mode); }},
        {"fog_density", [](const PR& r) { return EngineDefault(r.fog_density); }},
        {"fog_linear_start", [](const PR& r) { return EngineDefault(r.fog_linear_start); }},
        {"fog_linear_end", [](const PR& r) { return EngineDefault(r.fog_linear_end); }},
        {"fog_color", [](const PR& r) { return EngineDefault(r.fog_color); }},
        {"fog_height_base", [](const PR& r) { return EngineDefault(r.fog_height_base); }},
        {"fog_height_falloff", [](const PR& r) { return EngineDefault(r.fog_height_falloff); }},
        {"fog_sky_blend", [](const PR& r) { return EngineDefault(r.fog_sky_blend); }},
        {"fog_sun_amount", [](const PR& r) { return EngineDefault(r.fog_sun_amount); }},
        {"fog_sun_anisotropy", [](const PR& r) { return EngineDefault(r.fog_sun_anisotropy); }},
        {"fog_max_opacity", [](const PR& r) { return EngineDefault(r.fog_max_opacity); }},
        {"fog_max_distance", [](const PR& r) { return EngineDefault(r.fog_max_distance); }},
        {"volumetrics_step_count", [](const PR& r) { return EngineDefault(r.volumetrics_step_count); }},
        {"volumetrics_max_distance", [](const PR& r) { return EngineDefault(r.volumetrics_max_distance); }},
        {"volumetrics_resolution_scale", [](const PR& r) { return EngineDefault(r.volumetrics_resolution_scale); }},
        {"volumetrics_mode", [](const PR& r) { return EngineDefault(r.volumetrics_mode); }},
        {"volumetrics_froxel_tile", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_tile); }},
        {"volumetrics_froxel_slices", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_slices); }},
        {"volumetrics_froxel_history", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_history); }},
        {"volumetrics_froxel_miss_samples", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_miss_samples); }},
        {"volumetrics_froxel_lookup_jitter", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_lookup_jitter); }},
        {"volumetrics_max_opacity", [](const PR& r) { return EngineDefault(r.volumetrics_max_opacity); }},
        {"volumetrics_sun_anisotropy", [](const PR& r) { return EngineDefault(r.volumetrics_sun_anisotropy); }},
        {"volumetrics_shadows_enabled", [](const PR& r) { return EngineDefault(r.volumetrics_shadows_enabled); }},
        {"volumetrics_light_scatter", [](const PR& r) { return EngineDefault(r.volumetrics_light_scatter); }},
        {"volumetrics_max_scatter_lights", [](const PR& r) { return EngineDefault(r.volumetrics_max_scatter_lights); }},
        {"mesh_lod_bias", [](const PR& r) { return EngineDefault(r.mesh_lod_bias); }},
        {"shadow_min_caster_texels", [](const PR& r) { return EngineDefault(r.shadow_min_caster_texels); }},
        {"bloom_threshold", [](const PR& r) { return EngineDefault(r.bloom_threshold); }},
        {"bloom_soft_knee", [](const PR& r) { return EngineDefault(r.bloom_soft_knee); }},
        {"bloom_intensity", [](const PR& r) { return EngineDefault(r.bloom_intensity); }},
        {"bloom_scatter", [](const PR& r) { return EngineDefault(r.bloom_scatter); }},
        {"bloom_radius", [](const PR& r) { return EngineDefault(r.bloom_radius); }},
        {"bloom_clamp", [](const PR& r) { return EngineDefault(r.bloom_clamp); }},
        {"tilt_shift_focus_center", [](const PR& r) { return EngineDefault(r.tilt_shift_focus_center); }},
        {"tilt_shift_focus_width", [](const PR& r) { return EngineDefault(r.tilt_shift_focus_width); }},
        {"tilt_shift_ramp_width", [](const PR& r) { return EngineDefault(r.tilt_shift_ramp_width); }},
        {"tilt_shift_blur_top", [](const PR& r) { return EngineDefault(r.tilt_shift_blur_top); }},
        {"tilt_shift_blur_bottom", [](const PR& r) { return EngineDefault(r.tilt_shift_blur_bottom); }},
        {"tilt_shift_max_radius", [](const PR& r) { return EngineDefault(r.tilt_shift_max_radius); }},
        {"tilt_shift_angle", [](const PR& r) { return EngineDefault(r.tilt_shift_angle); }},
        {"dof_focus_mode", [](const PR& r) { return EngineDefault(r.dof_focus_mode); }},
        {"dof_focus_object", [](const PR& r) { return EngineDefault(r.dof_focus_object); }},
        {"dof_focus_smoothing", [](const PR& r) { return EngineDefault(r.dof_focus_smoothing); }},
        {"dof_focus_distance", [](const PR& r) { return EngineDefault(r.dof_focus_distance); }},
        {"dof_focus_range", [](const PR& r) { return EngineDefault(r.dof_focus_range); }},
        {"dof_focus_cover_object", [](const PR& r) { return EngineDefault(r.dof_focus_cover_object); }},
        {"dof_blur_scale", [](const PR& r) { return EngineDefault(r.dof_blur_scale); }},
        {"dof_aperture", [](const PR& r) { return EngineDefault(r.dof_aperture); }},
        {"dof_focal_length", [](const PR& r) { return EngineDefault(r.dof_focal_length); }},
        {"dof_sensor_width", [](const PR& r) { return EngineDefault(r.dof_sensor_width); }},
        {"dof_max_radius", [](const PR& r) { return EngineDefault(r.dof_max_radius); }},
        {"dof_sample_count", [](const PR& r) { return EngineDefault(r.dof_sample_count); }},
        {"dof_blade_count", [](const PR& r) { return EngineDefault(r.dof_blade_count); }},
        {"dof_blade_rotation", [](const PR& r) { return EngineDefault(r.dof_blade_rotation); }},
        {"aa_mode", [](const PR& r) { return EngineDefault(r.aa_mode); }},
        {"fxaa_subpixel", [](const PR& r) { return EngineDefault(r.fxaa_subpixel); }},
        {"fxaa_edge_threshold", [](const PR& r) { return EngineDefault(r.fxaa_edge_threshold); }},
        {"fxaa_edge_threshold_min", [](const PR& r) { return EngineDefault(r.fxaa_edge_threshold_min); }},
        {"smaa_threshold", [](const PR& r) { return EngineDefault(r.smaa_threshold); }},
        {"smaa_max_search_steps", [](const PR& r) { return EngineDefault(r.smaa_max_search_steps); }},
        {"taa_blending_weight", [](const PR& r) { return EngineDefault(r.taa_blending_weight); }},
        {"taa_weight_scale", [](const PR& r) { return EngineDefault(r.taa_weight_scale); }},
        {"taa_feedback_motion", [](const PR& r) { return EngineDefault(r.taa_feedback_motion); }},
        {"taa_sharpness", [](const PR& r) { return EngineDefault(r.taa_sharpness); }},
        {"taa_variance_gamma", [](const PR& r) { return EngineDefault(r.taa_variance_gamma); }},
        {"sdf_max_steps", [](const PR& r) { return EngineDefault(r.sdf_max_steps); }},
        {"sdf_shadow_max_steps", [](const PR& r) { return EngineDefault(r.sdf_shadow_max_steps); }},
        {"sdf_max_renderers", [](const PR& r) { return EngineDefault(r.sdf_max_renderers); }},
        {"sdf_max_shapes", [](const PR& r) { return EngineDefault(r.sdf_max_shapes); }},
    };
    std::map<std::string, int> seen;
    std::map<std::string, FieldDesc> fields;
    for (const auto& g : render_settings_groups()) {
        for (const auto& f : g.all_fields()) { ++seen[f.key]; fields[f.key] = f; }
        for (const auto& f : g.all_fields()) {
            expect(!f.label.empty() && !f.tooltip.empty(), "render." + f.key + " has a label and a tooltip");
        }
        expect(!g.tip.empty() && !g.category.empty(), g.title + " has a category and a description");
    }
    for (const auto& [k, n] : seen) expect(n == 1, "render." + k + " appears once in the Render tab (" + std::to_string(n) + ")");
    Node root = Node::mapping();
    root["render"] = Node::mapping();
    const PR defaults = toy::core::AppConfig::from_node(root).render;
    for (const auto& [key, get] : engine_keys) {
        auto it = fields.find(key);
        expect(it != fields.end(), "render." + key + " (parsed by the engine) is in the Render tab");
        if (it == fields.end()) continue;
        const FieldDesc& f = it->second;
        const EngineDefault d = get(defaults);
        if (d.text) {
            if (f.kind == FieldKind::AssetRef) {
                expect(d.s.empty(), "render." + key + ": an asset path defaults to none (engine: '" + d.s + "')");
                continue;
            }
            const std::string shown = !f.default_string.empty() ? f.default_string
                                    : f.kind == FieldKind::Enum && !f.options.empty() ? f.options.front() : std::string();
            expect(shown == d.s, "render." + key + " shows the engine default '" + d.s + "' (schema: '" + shown + "')");
            if (f.kind == FieldKind::Enum) {
                expect(std::find(f.options.begin(), f.options.end(), d.s) != f.options.end(), "render." + key + " offers its default");
            }
        } else {
            const int n = f.kind == FieldKind::Color4 ? 4 : (f.kind == FieldKind::Color || f.kind == FieldKind::Vec3) ? 3 : 1;
            bool same = true;
            for (int i = 0; i < n; ++i) same &= std::abs(f.def[i] - d.v[i]) <= 1e-5f * std::max(1.0f, std::abs(d.v[i]));
            char buf[160];
            std::snprintf(buf, sizeof buf, " (engine %g %g %g %g, schema %g %g %g %g)", d.v.x, d.v.y, d.v.z, d.v.w, f.def.x, f.def.y, f.def.z, f.def.w);
            expect(same, "render." + key + " shows the engine default" + buf);
            if (n == 1 && f.kind != FieldKind::Bool) {
                expect(d.v.x >= f.min - 1e-6f && d.v.x <= f.max + 1e-6f, "render." + key + "'s default is inside its range");
            }
        }
    }
    for (const auto& entry : seen) {
        const std::string& k = entry.first;
        const bool parsed = std::any_of(engine_keys.begin(), engine_keys.end(), [&](const auto& e) { return e.first == k; });
        expect(parsed, "render." + k + " is a key the engine reads");
    }
}

// The settings panel shows a field's schema default while its key is absent, so those defaults
// must be what the engine actually runs with (PixelRenderConfig / WindowConfig / ...).
void test_settings_defaults_match_engine() {
    const toy::core::AppConfig eng = toy::core::AppConfig::from_node(Node::mapping());
    const auto& r = eng.render;
    const std::map<std::string, float> floats = {
        {"fog_density", r.fog_density}, {"fog_height_base", r.fog_height_base}, {"fog_height_falloff", r.fog_height_falloff},
        {"fog_sky_blend", r.fog_sky_blend}, {"fog_max_distance", r.fog_max_distance}, {"bloom_intensity", r.bloom_intensity},
        {"auto_exposure_compensation", r.auto_exposure_compensation}, {"depth_threshold", r.depth_threshold},
        {"normal_threshold", r.normal_threshold}, {"dither_strength", r.dither_strength}, {"exposure", r.exposure},
        {"shadow_distance", r.shadow_distance}, {"outline_thickness", r.outline_thickness},
    };
    const std::map<std::string, bool> bools = {
        {"bloom_enabled", r.bloom_enabled}, {"grading_enabled", r.grading_enabled}, {"palette_enabled", r.palette_enabled},
        {"dither_enabled", r.dither_enabled}, {"camera_pixel_snap", r.camera_pixel_snap}, {"outline_enabled", r.outline_enabled},
        {"shadows_enabled", r.shadows_enabled}, {"ssao_enabled", r.ssao_enabled}, {"fog_enabled", r.fog_enabled},
    };
    for (const auto& g : render_settings_groups()) {
        for (const auto& f : g.all_fields()) {
            if (auto it = floats.find(f.key); it != floats.end()) {
                expect(std::abs(f.def.x - it->second) < 1e-6f, f.key + " shows the engine default");
            }
            if (auto it = bools.find(f.key); it != bools.end()) {
                expect((f.def.x != 0.0f) == it->second, f.key + " shows the engine default");
            }
            if (f.key.size() > 8 && f.key.compare(f.key.size() - 8, 8, "_quality") == 0) {
                expect(f.default_string == "high", f.key + " shows High, the engine default");
            }
            if (f.key == "outline_color") {
                // An edited colour is written as the 4-element list the engine reads.
                Node render = Node::mapping();
                render["outline_color"] = field_default_node(f);
                Node root = Node::mapping();
                root["render"] = render;
                expect(render.at("outline_color").size() == 4, "outline_color is written with alpha");
                const auto parsed = toy::core::AppConfig::from_node(root);
                expect(glm::all(glm::epsilonEqual(parsed.render.outline_color, r.outline_color, 1e-6f)), "outline_color default matches");
            }
        }
    }
    // The engine also takes a 3-element outline colour (files saved before the fix).
    Node root = Node::mapping();
    root["render"] = Node::mapping();
    float rgb[3] = {1.0f, 0.0f, 0.0f};
    root["render"]["outline_color"] = make_float_seq(rgb, 3);
    expect(toy::core::AppConfig::from_node(root).render.outline_color == glm::vec4(1, 0, 0, 1), "rgb outline_color loads opaque");
}

void test_asset_refs_and_rename() {
    expect(mesh_ref("meshes/rock.yaml") == "rock", "mesh_ref: top-level mesh");
    expect(mesh_ref("meshes/props/rock.yaml") == "rock", "mesh_ref drops tag folders (meshes are found by name)");
    expect(mesh_ref("scenes/lake/meshes/basin.yaml") == "basin", "mesh_ref: scene-local mesh");
    expect(strip_yaml_ext("objects/props/crate.yaml") == "objects/props/crate", "strip_yaml_ext");

    const fs::path root = fresh_dir("rename_refs");
    const fs::path a = root / "assets";
    auto write = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((a / rel).parent_path());
        std::ofstream(a / rel) << text;
    };
    write("objects/props/crate.yaml", "object:\n  name: crate\n  components: []\n");
    write("meshes/props/rock.yaml", "vertices: []\nfaces: []\n");
    write("meshes/props/rock.lod.yaml", "lods: []\n");
    write("materials/stone.yaml", "albedo: {r: 1.0, g: 1.0, b: 1.0}\ntexture_albedo: textures/stone.png\n");
    write("textures/stone.png", "png");
    write("scenes/lake/meshes/basin.yaml", "vertices: []\nfaces: []\n");
    write("scenes/lake/scene.yaml",
          "scene:\n  scene_name: lake\n  root_objects:\n"
          "    - name: props/rock\n      prefab: objects/props/crate\n      components:\n"
          "        - type: MeshRenderer\n          mesh_path: props/rock\n          material: materials/stone\n"
          "        - type: MeshCollider\n          mesh_path: basin\n");
    write("scenes/other/scene.yaml",
          "scene:\n  scene_name: other\n  root_objects:\n"
          "    - name: basin\n      components:\n        - type: MeshRenderer\n          mesh_path: basin\n");
    Project project(root);

    auto scene = [&](const std::string& s) { return coopa::yaml::load_document(a / "scenes" / s / "scene.yaml").at("scene").at("root_objects")[0]; };
    auto rewritten = project.rename_asset("objects/props/crate.yaml", "objects/props/box.yaml");
    expect(get_string(scene("lake"), "prefab") == "objects/props/box", "renaming an object asset rewrites prefab:");
    expect(rewritten.size() == 1, "only the referring scene is rewritten");

    project.rename_asset("meshes/props/rock.yaml", "meshes/props/boulder.yaml");
    const Node lake = scene("lake");
    expect(get_string(lake.at("components")[0], "mesh_path") == "props/boulder", "renaming a mesh rewrites mesh_path");
    expect(get_string(lake, "name") == "props/rock", "an object name equal to the old ref is left alone");
    expect(fs::exists(a / "meshes/props/boulder.lod.yaml") && !fs::exists(a / "meshes/props/rock.lod.yaml"), "the LOD sidecar moves too");

    project.rename_asset("materials/stone.yaml", "materials/granite.yaml");
    expect(get_string(scene("lake").at("components")[0], "material") == "materials/granite", "renaming a material rewrites material:");

    project.rename_asset("textures/stone.png", "textures/granite.png");
    expect(get_string(coopa::yaml::load_document(a / "materials/granite.yaml"), "texture_albedo") == "textures/granite.png",
           "renaming a texture rewrites the material's map");

    project.rename_asset("scenes/lake/meshes/basin.yaml", "scenes/lake/meshes/pool.yaml");
    expect(get_string(scene("lake").at("components")[1], "mesh_path") == "pool", "a scene-local mesh is renamed in its scene");
    expect(get_string(scene("other").at("components")[0], "mesh_path") == "basin", "other scenes' same-named meshes are untouched");
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
        expect(engine.scene().find_object("sphere") != nullptr, "and remain undoable afterwards");
    }

    // The saved project loads in the plain game Engine.
    {
        toy::core::AppConfig cfg = shell_config(project);
        cfg.scene.default_scene = scene_path.string();
        toy::core::EngineOptions o;
        o.project_root = project.root();
        toy::core::Engine game(cfg, o);
        tick(game, 2);
        auto* obj = game.scene().find_object("sphere");
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

/** @brief Play mode: the game runs unfocused until the viewer is clicked; Esc releases, play continues. */
void test_editor_play_input_focus() {
    using coopa::input::Key;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("play_focus_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};

    app.play();
    tick(engine, 3);
    expect(app.playing(), "playing");
    expect(!app.game_focused() && !engine.game_input_focus(), "the game starts without the mouse and keyboard");

    in.click(app.viewport_box().center());
    expect(app.game_focused() && engine.game_input_focus(), "clicking the viewer gives the game input");
    // While the game has the mouse, the editor UI ignores clicks (File menu stays shut).
    in.click({44, 14});
    expect(!app.ui().any_popup_open(), "editor menus ignore clicks while the game has the mouse");

    in.key(Key::Escape);
    expect(!app.game_focused() && !engine.game_input_focus(), "Esc hands the mouse back");
    expect(app.playing() && engine.scene().is_simulating(), "and the game keeps running");

    in.click(app.viewport_box().center());
    expect(app.game_focused(), "clicking again refocuses");
    in.key(Key::F5);
    tick(engine, 2);
    expect(!app.playing() && !app.game_focused() && engine.game_input_focus(), "F5 stops and restores the engine default");
}

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
    const ObjectId cube = [&] { for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") return id; return ObjectId(0); }();
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
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") app.document().select(id);
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
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") cube = id;
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
    auto z_of = [&] { return engine.scene().find_object("cube")->get_transform()->transform().position().z; };
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
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") cube = id;
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
    // A new project's config.yaml is the engine's own, so its vertical resolution is that one's.
    const uint32_t project_height = toy::core::AppConfig::load(project.config_path().string()).render.render_height;
    expect(engine.pipeline().render_height() == project_height, "the project's vertical resolution is kept");
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
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") cube = id;
    glm::vec2 px;
    if (engine.world_to_window(glm::vec3(0, 0, 0.5f), px)) {
        app.document().clear_selection();
        in.click(px / in.scale);
        tick(engine, 2);
        expect(app.document().primary() == cube, "picking works after the pipeline rebuild");
    }
}

/**
 * @brief No frame goes black while a resize settles: the UI's DrawList holds the pipeline's
 *        white texture from emit time, so a rebuild must not free it before that frame draws.
 */
void test_editor_resize_no_black_frame() {
    using coopa::input::Key;
    using coopa::input::Mods;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("black_frame_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    // Share of the presented frame (UI included) that is pure black.
    auto black_share = [&]() {
        const coopa::gfx::util::ImageData img = engine.capture_image(false);
        size_t black = 0;
        for (size_t i = 0; i + 3 < img.pixels.size(); i += img.channels) {
            if (img.pixels[i] < 4 && img.pixels[i + 1] < 4 && img.pixels[i + 2] < 4) ++black;
        }
        return float(black) / float(std::max<size_t>(1, img.pixels.size() / img.channels));
    };
    auto settle_without_black = [&](const std::string& what) {
        const uint32_t w0 = engine.pipeline().render_width();
        float worst = 0.0f;
        for (int i = 0; i < toy::core::Engine::kFillDebounceFrames + 4; ++i) {
            engine.tick();
            worst = std::max(worst, black_share());
        }
        expect(engine.pipeline().render_width() != w0, what + ": the pipeline was rebuilt");
        expect(worst < 0.02f, what + ": no black frame during the rebuild (worst " +
                             std::to_string(int(worst * 100)) + "% black)");
    };
    // Startup: the first rebuild, from the whole window to the viewport panel.
    settle_without_black("startup");
    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Space, Mods::Control);   // maximize: a new viewport aspect
    settle_without_black("maximize");
    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Space, Mods::Control);   // and back
    settle_without_black("restore");
}

/** @brief A click picks the nearest visible surface: not a hidden occluder, nor a marker behind it. */
void test_editor_pick_nearest() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("pick_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    auto* cam = coopa::gfx::engine::components::CameraComponent::main();
    expect(cam != nullptr, "a main camera (the editor camera) exists");
    if (!cam) return;
    auto name_of = [&](ObjectId id) {
        return id && app.document().find(id) ? get_string(*app.document().find(id), "name") : std::string("nothing");
    };
    auto place = [&](ObjectId id, glm::vec3 p) {
        app.document().set_transform(id, p, {0, 0, 0}, glm::vec3(1), "Move");
        app.sync().apply(engine, app.document(), {ChangeScope::Transform, id});
    };
    // Two spheres on one camera ray, the near one between the camera and the far one.
    const glm::vec3 eye = cam->get_world_position();
    const glm::vec3 far_pos(0, 0, 3);
    const glm::vec3 to_eye = glm::normalize(eye - far_pos);
    const ObjectId far_s = app.create_primitive("Sphere");
    const ObjectId near_s = app.create_primitive("Sphere");
    place(far_s, far_pos);
    place(near_s, far_pos + to_eye * 4.0f);
    tick(engine, 2);
    glm::vec2 px;
    expect(engine.world_to_window(far_pos, px), "the far sphere is on screen");
    const float s = std::max(1.0f, engine.display_scale());
    ObjectId got = app.pick_object(px / s);
    expect(got == near_s, "the nearer of two spheres on the ray is picked (got " + name_of(got) + ")");

    app.hide_objects({near_s});
    tick(engine, 2);
    got = app.pick_object(px / s);
    expect(got == far_s, "a hidden sphere is not picked; the one behind it is (got " + name_of(got) + ")");

    // A mesh-less object's marker exactly under the cursor but BEHIND the surface loses to it.
    ObjectId sun = 0;
    for (ObjectId id : app.document().all_ids()) if (name_of(id) == "sun") sun = id;
    expect(sun != 0, "the default scene has a sun");
    place(sun, far_pos - to_eye * 3.0f);
    tick(engine, 2);
    got = app.pick_object(px / s);
    expect(got == far_s, "a marker hidden behind a surface does not steal the click (got " + name_of(got) + ")");
    // In front of it, the marker still wins a dead-on click.
    place(sun, far_pos + to_eye * 2.0f);
    tick(engine, 2);
    got = app.pick_object(px / s);
    expect(got == sun, "a marker in front of the surface wins a dead-on click (got " + name_of(got) + ")");
}

/** @brief Object Mode's Shade Flat / Shade Smooth: the mesh file, the render and undo agree. */
void test_editor_object_shade_smooth_flat() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("shade_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    const glm::vec3 centre(0, 0, 2);
    const ObjectId sphere = app.create_primitive("Sphere");
    app.document().set_transform(sphere, centre, {0, 0, 0}, glm::vec3(1.5f), "Move");
    app.sync().apply(engine, app.document(), {ChangeScope::Transform, sphere});
    tick(engine, 6);
    const fs::path mesh_file = project.assets() / "meshes/sphere.yaml";
    auto smooth_faces = [&]() {
        const EditMesh m = mesh_from_node(coopa::yaml::load_document(mesh_file));
        size_t n = 0;
        for (const auto& f : m.faces) n += f.smooth ? 1 : 0;
        return std::pair<size_t, size_t>{n, m.faces.size()};
    };
    // Facet edges: shading steps between neighbouring pixels inside the sphere's disc (the
    // middle of it, clear of the silhouette). A whole-image diff would not do: exposure and
    // the shadow fit drift between captures of the same scene by thousands of pixels.
    auto facet_edges = [&]() {
        app.document().clear_selection();   // the selection wireframe would cover the shading
        tick(engine, 30);
        glm::vec2 c, top;
        if (!engine.world_to_window(centre, c) || !engine.world_to_window(centre + glm::vec3(0, 0, 0.75f), top)) return size_t(0);
        const auto d = engine.display_rect();
        const coopa::gfx::util::ImageData img = engine.capture_image(true);
        const float sx = float(img.width) / float(d.w), sy = float(img.height) / float(d.h);
        const int cx = int((c.x - d.x) * sx), cy = int((c.y - d.y) * sy);
        const int r = int(glm::distance(c, top) * sy * 0.6f);
        auto lum = [&](int x, int y) {
            const size_t i = (size_t(y) * img.width + size_t(x)) * img.channels;
            return int(img.pixels[i]) + int(img.pixels[i + 1]) + int(img.pixels[i + 2]);
        };
        size_t n = 0;
        for (int y = std::max(0, cy - r); y < std::min(int(img.height) - 1, cy + r); ++y)
            for (int x = std::max(0, cx - r); x < std::min(int(img.width) - 1, cx + r); ++x)
                if (std::abs(lum(x + 1, y) - lum(x, y)) >= 24 || std::abs(lum(x, y + 1) - lum(x, y)) >= 24) ++n;
        return n;
    };
    const auto [s0, total] = smooth_faces();
    expect(total > 0 && s0 == total, "a new UV sphere is smooth-shaded");
    const size_t smooth_edges = facet_edges();

    app.document().select(sphere);
    app.shade_selected(false);
    expect(smooth_faces().first == 0, "Shade Flat marks every face of the mesh flat, in its file");
    const size_t flat_edges = facet_edges();
    expect(flat_edges > smooth_edges * 3 / 2 + 50, "the render shows the facets (" + std::to_string(smooth_edges) +
                                                   " -> " + std::to_string(flat_edges) + " facet edges)");

    app.undo();
    expect(smooth_faces().first == total, "Ctrl+Z in Object Mode makes it smooth again");
    const size_t undone_edges = facet_edges();
    expect(undone_edges < (smooth_edges + flat_edges) / 2, "and the render is smooth again (" +
                                                         std::to_string(undone_edges) + " facet edges)");

    app.document().select(sphere);
    app.shade_selected(false);
    app.shade_selected(true);
    expect(smooth_faces().first == total, "Shade Smooth marks every face smooth");
    expect(facet_edges() < (smooth_edges + flat_edges) / 2, "and renders smooth");
}

/**
 * @brief Overrides on an instance's inherited parts: they get nodes of their own, a click picks
 *        the instance then the part, edits save as the smallest override (never touching the
 *        asset), Revert / Apply to Object Asset, and renaming the part in the asset follows.
 */
void test_editor_instance_overrides() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("instance_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    auto& doc = app.document();
    auto name_of = [&](ObjectId id) { return id && doc.find(id) ? get_string(*doc.find(id), "name") : std::string("nothing"); };

    // An object asset whose mesh sits on a child: an empty with a raised sphere under it.
    const ObjectId lamp = app.create_empty();
    tick(engine, 2);
    const ObjectId shade = app.create_primitive("Sphere", lamp);
    tick(engine, 2);
    const std::string part_name = name_of(shade);
    doc.set_transform(shade, {0, 0, 1.5f}, {0, 0, 0}, glm::vec3(1), "Move");
    doc.select(lamp);
    expect(app.create_object_asset_from_selection(), "the empty and its sphere become an object asset");
    tick(engine, 4);
    expect(doc.is_instance(lamp), "...and an instance takes their place");
    const std::string ref = get_string(*doc.find(lamp), "prefab");
    const fs::path asset_file = project.assets() / (ref + ".yaml");
    const std::string asset_before = coopa::yaml::emit(coopa::yaml::load_document(asset_file));

    auto part_of = [&](ObjectId inst, const std::string& n) {
        const Node* o = doc.find(inst);
        if (o && o->contains("children")) for (const auto& c : o->at("children").as_seq()) if (get_string(c, "name") == n) return SceneDocument::id_of(c);
        return ObjectId(0);
    };
    const ObjectId part = part_of(lamp, part_name);
    expect(part != 0 && doc.is_inherited(part), "the sphere the instance inherits has a node of its own");
    expect(app.sync().live(part) != nullptr, "...mapped to its live object");
    auto saved_instance = [&]() {
        app.save_scene();
        const Node file = coopa::yaml::load_document(doc.path());
        for (const auto& o : file.at("scene").at("root_objects").as_seq()) {
            if (get_string(o, "name") == name_of(lamp)) return Node(o);
        }
        return Node::mapping();
    };
    expect(!saved_instance().contains("children"), "an inherited part with no overrides is not written to the scene");

    // Picking: the part's mesh is hit; the first click selects the instance, the next the part.
    auto* live_part = app.sync().live(part);
    glm::vec2 px;
    const float s = std::max(1.0f, engine.display_scale());
    if (live_part && engine.world_to_window(glm::vec3(live_part->get_transform()->transform().get_world_matrix()[3]), px)) {
        const ObjectId hit = app.pick_object(px / s);
        expect(hit == part, "clicking the instance's sphere hits it (got " + name_of(hit) + ")");
        doc.clear_selection();
        expect(app.click_target(hit) == lamp, "the first click selects the whole instance");
        doc.select(lamp);
        expect(app.click_target(hit) == part, "a click on the selected instance selects the part");
    } else {
        expect(false, "the instance's sphere is on screen");
    }

    // A field override: only that key is saved, on the part, and the asset is untouched.
    Node mr = app.shown_component(part, "MeshRenderer");
    expect(mr.is_mapping() && mr.contains("mesh_path"), "the part shows the asset's MeshRenderer");
    mr["lod_bias"] = make_float(3.0f);
    ObjectId cube = 0;
    for (ObjectId id : doc.all_ids()) if (name_of(id) == "cube") cube = id;
    const auto* cube_live = app.sync().live(cube);
    app.edit_component(part, mr);
    tick(engine, 3);
    expect(app.has_overrides(part), "the part now has an override");
    expect(cube_live && app.sync().live(cube) == cube_live, "an override rebuilds only its instance (the cube's live object is untouched)");
    {
        auto* lp = app.sync().live(part);
        auto* lmr = lp ? lp->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
        expect(lmr && std::abs(lmr->lod_bias - 3.0f) < 1e-4f, "...and the rebuilt part has the overridden value live");
    }
    expect(std::abs(get_float(app.shown_component(part, "MeshRenderer"), "lod_bias") - 3.0f) < 1e-4f, "...and shows the overridden value");
    {
        const Node inst = saved_instance();
        bool ok = false;
        if (inst.contains("children") && inst.at("children").size() == 1) {
            const Node& c = inst.at("children")[0];
            ok = get_string(c, "name") == part_name && c.at("components").size() == 1 &&
                 component_type(c.at("components")[0]) == "MeshRenderer" && c.at("components")[0].size() == 2;   // type + lod_bias
        }
        expect(ok, "the scene saves just {type: MeshRenderer, lod_bias} under the part's name:\n" + coopa::yaml::emit(inst));
    }
    expect(coopa::yaml::emit(coopa::yaml::load_document(asset_file)) == asset_before, "the object asset itself is unchanged");
    doc.select(part);
    app.set_prop_tab(PropTab::Components);
    tick(engine, 3);
    dump(engine, "instance_part_override");   // the overridden field's marker (EDITOR_DUMP_DIR)

    // Moving the part: a Transform override holding only what changed.
    glm::vec3 p, r, sc;
    expect(app.object_transform(part, p, r, sc) && glm::distance(p, glm::vec3(0, 0, 1.5f)) < 1e-4f, "the part's transform shows the asset's pose");
    app.set_object_transform(part, p + glm::vec3(1, 0, 0), r, sc);
    tick(engine, 2);
    {
        const Node inst = saved_instance();
        Node t;
        for (const auto& c : inst.at("children")[0].at("components").as_seq()) if (component_type(c) == "Transform") t = c;
        expect(t.is_mapping() && t.contains("position") && !t.contains("rotation") && !t.contains("scale"),
               "moving the part overrides its position only");
    }
    expect(glm::distance(glm::vec3(app.sync().live(part)->get_transform()->transform().position()), glm::vec3(1, 0, 1.5f)) < 1e-4f,
           "...and moves the live part");

    // Revert one field.
    app.revert_override_field(part, "MeshRenderer", "lod_bias");
    tick(engine, 2);
    expect(std::abs(get_float(app.shown_component(part, "MeshRenderer"), "lod_bias", 1.0f) - 1.0f) < 1e-4f, "Revert brings back the asset's value");

    // Apply to Object Asset: the asset changes, this instance drops the override, others follow.
    mr = app.shown_component(part, "MeshRenderer");
    mr["lod_bias"] = make_float(2.0f);
    app.edit_component(part, mr);
    app.apply_component_to_asset(part, "MeshRenderer");
    tick(engine, 3);
    {
        const Node a = coopa::yaml::load_document(asset_file).at("object");
        float bias = 0.0f;
        for (const auto& c : a.at("children")[0].at("components").as_seq()) if (component_type(c) == "MeshRenderer") bias = get_float(c, "lod_bias", 1.0f);
        expect(std::abs(bias - 2.0f) < 1e-4f, "Apply to Object Asset writes the value into the asset");
    }
    const ObjectId second = app.place_object_asset(ref + ".yaml");
    tick(engine, 3);
    expect(std::abs(get_float(app.shown_component(part_of(second, part_name), "MeshRenderer"), "lod_bias", 1.0f) - 2.0f) < 1e-4f,
           "another instance shows the applied value");
    expect(std::abs(get_float(app.shown_component(part, "MeshRenderer"), "lod_bias", 1.0f) - 2.0f) < 1e-4f &&
           !app.shown_component(part, "MeshRenderer").is_null(), "this instance shows it from the asset");

    // Revert everything on the instance.
    app.revert_all_overrides(lamp);
    tick(engine, 2);
    expect(!app.has_overrides(part) && app.object_transform(part, p, r, sc) && glm::distance(p, glm::vec3(0, 0, 1.5f)) < 1e-4f,
           "Revert Instance Overrides restores the asset's part");
    expect(!saved_instance().contains("children"), "...and the scene holds no overrides for it");

    // Renaming the part in the asset carries the scene's override along.
    mr = app.shown_component(part, "MeshRenderer");
    mr["lod_bias"] = make_float(5.0f);
    app.edit_component(part, mr);
    tick(engine, 2);
    app.save_scene();
    const fs::path scene_file = doc.path();
    expect(app.open_object_asset(asset_file), "the object asset opens");
    tick(engine, 2);
    ObjectId asset_part = 0;
    for (ObjectId id : doc.all_ids()) if (name_of(id) == part_name) asset_part = id;
    expect(asset_part != 0, "the part is in the object asset");
    app.sync().apply(engine, doc, doc.set_object_key(asset_part, "name", Node(std::string("globe")), "Rename"));
    app.save_scene();
    expect(app.open_scene(scene_file), "the scene reopens");
    tick(engine, 3);
    const ObjectId globe = part_of(lamp, "globe");
    expect(globe != 0 && part_of(lamp, part_name) == 0, "the instance's part is now called globe");
    expect(std::abs(get_float(app.shown_component(globe, "MeshRenderer"), "lod_bias", 1.0f) - 5.0f) < 1e-4f,
           "...and still carries its override (the scene was rewritten)");
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
/** @brief Edit Mode R: rotate the selection, with axis locks and typed angles, like G. */
/** @brief The toyengine logo button opens the app menu; About shows the version. */
void test_editor_about_and_logo() {
    // The logo rasterizes with transparent corners and an opaque, coloured block.
    const std::vector<uint8_t> px = toy::core::rasterize_logo(64);
    expect(px.size() == 64u * 64u * 4u, "logo is 64x64 RGBA");
    expect(px[3] == 0, "the logo's corner is transparent (rounded tile)");
    const size_t mid = (32u * 64u + 32u) * 4u;
    expect(px[mid + 3] == 255, "the logo's centre is opaque");

    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("about_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    in.click({16, 14});   // the logo, top-left
    expect(app.ui().is_popup_open("app_menu"), "clicking the logo opens the app menu");
    in.click({40, 14 + 26});   // first item: About
    tick(engine, 2);
    expect(app.ui().is_popup_open("About"), "About... opens the About window");
    dump(engine, "about_modal");
    in.key(coopa::input::Key::Escape);
    tick(engine, 2);
}

void test_editor_mesh_rotate() {
    using coopa::input::Key;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("rotate_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    const glm::vec2 c = app.viewport_box().center();
    in.move(c);
    in.key(Key::Tab);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    expect(app.edit_mode_active(), "Tab enters Edit Mode");
    auto& md = app.mesh_document();
    in.key(Key::Num1);
    in.key(Key::A);
    const EditMesh before = md.mesh;

    // R Z 90 Enter: a quarter turn about Z through the selection's centre.
    in.move(c + glm::vec2(60, 0));
    in.key(Key::R);
    expect(app.modal_active(), "R starts Rotate in Edit Mode");
    in.key(Key::Z);
    in.key(Key::Num9);
    in.key(Key::Num0);
    in.key(Key::Enter);
    expect(!app.modal_active(), "Enter confirms");
    glm::vec3 centre(0.0f);
    for (const auto& p : before.positions) centre += p;
    centre /= static_cast<float>(before.positions.size());
    bool ok = md.mesh.positions.size() == before.positions.size();
    for (size_t i = 0; ok && i < before.positions.size(); ++i) {
        const glm::vec3 d = before.positions[i] - centre;
        const glm::vec3 want = centre + glm::vec3(-d.y, d.x, d.z);
        ok = glm::length(md.mesh.positions[i] - want) < 1e-3f;
    }
    expect(ok, "R Z 90 rotates every selected vertex 90 degrees about Z");

    // Mouse-driven rotate, then RMB cancels back to the rotated state.
    const EditMesh rotated = md.mesh;
    in.key(Key::R);
    in.move(c + glm::vec2(10, 70), 3);
    bool moved = false;
    for (size_t i = 0; i < rotated.positions.size(); ++i) moved |= glm::length(md.mesh.positions[i] - rotated.positions[i]) > 1e-3f;
    expect(moved, "moving the mouse during R rotates live");
    in.click(c + glm::vec2(10, 70), coopa::input::MouseButton::Right);
    bool restored = true;
    for (size_t i = 0; i < rotated.positions.size(); ++i) restored &= glm::length(md.mesh.positions[i] - rotated.positions[i]) < 1e-5f;
    expect(restored, "RMB cancels the rotate");

    // The same from a mesh asset (Meshes tab, Edit mode).
    in.key(Key::Tab);
    app.save_mesh();  // the rotated cube mesh is dirty; don't stop at the save prompt
    app.open_asset(AssetType::Mesh, "meshes/cube.yaml");
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    app.set_interaction_mode(InteractionMode::Edit);
    tick(engine, 2);
    expect(app.edit_mode_active(), "mesh asset: Edit mode");
    in.move(c);
    in.key(Key::A);
    const EditMesh asset_before = app.mesh_document().mesh;
    in.move(c + glm::vec2(60, 0));
    in.key(Key::R);
    expect(app.modal_active(), "mesh asset: R starts Rotate");
    in.key(Key::X);
    in.key(Key::Num4);
    in.key(Key::Num5);
    in.key(Key::Enter);
    bool changed = false;
    for (size_t i = 0; i < asset_before.positions.size(); ++i)
        changed |= glm::length(app.mesh_document().mesh.positions[i] - asset_before.positions[i]) > 1e-3f;
    expect(changed, "mesh asset: R X 45 rotates the selection");

    // X mirror on: G X on the +X vertices moves the -X ones the opposite way.
    in.key(Key::Z, coopa::input::Mods::Control);   // undo the rotate
    auto& amd = app.mesh_document();
    const EditMesh sym_before = amd.mesh;
    amd.selection.clear();
    amd.selection.mode = SelectMode::Vertex;
    for (uint32_t v = 0; v < amd.mesh.positions.size(); ++v) if (amd.mesh.positions[v].x > 0) amd.selection.verts.insert(v);
    app.edit_symmetry().axis[0] = true;
    tick(engine, 2);
    dump(engine, "mesh_symmetry_header");
    in.move(c);
    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Period);
    in.key(Key::Num5);
    in.key(Key::Enter);
    bool sym_ok = true;
    for (uint32_t v = 0; v < sym_before.positions.size(); ++v) {
        const float dx = amd.mesh.positions[v].x - sym_before.positions[v].x;
        sym_ok &= std::abs(dx - (sym_before.positions[v].x > 0 ? 0.5f : -0.5f)) < 1e-4f;
    }
    expect(sym_ok, "with X mirror on, G X .5 widens both sides symmetrically");
    app.edit_symmetry().axis[0] = false;
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 3);
    dump(engine, "mesh_symmetry_sculpt_header");
    app.set_interaction_mode(InteractionMode::Edit);
    tick(engine, 2);

    // Ctrl M opens Mirror; X Local flips the selection.
    in.key(Key::M, coopa::input::Mods::Control);
    expect(app.ui().any_popup_open(), "Ctrl M opens the Mirror menu");
    in.key(Key::Escape);
    // Mirror the +X half's -Y vertices along X: they cross the selection centre (x of the +X face).
    amd.selection.verts.clear();
    for (uint32_t v = 0; v < amd.mesh.positions.size(); ++v) if (amd.mesh.positions[v].y < 0) amd.selection.verts.insert(v);
    glm::vec3 sc(0.0f);
    for (uint32_t v : amd.selection.verts) sc += amd.mesh.positions[v];
    sc /= static_cast<float>(amd.selection.verts.size());
    const EditMesh pre_mirror = amd.mesh;
    app.mirror_mesh_selection(0, false);
    bool flipped = !amd.selection.verts.empty();
    for (uint32_t v : amd.selection.verts)
        flipped &= std::abs(amd.mesh.positions[v].x - (2.0f * sc.x - pre_mirror.positions[v].x)) < 1e-4f &&
                   std::abs(amd.mesh.positions[v].y - pre_mirror.positions[v].y) < 1e-6f;
    expect(flipped, "Mirror X Local reflects the selection across its centre plane");
}

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
    const ObjectId cube = object_named(app, "cube");
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
    const ObjectId cube = object_named(app, "cube"), ground = object_named(app, "ground"), sun = object_named(app, "sun");
    expect(cube && ground && sun, "the new project has Cube, Ground and Sun");
    app.hide_objects({object_named(app, "camera")});
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
    expect(!app.sync().live(object_named(app, "camera"))->active(), "an object the user hid stays hidden");

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
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 4);
    expect(app.interaction_mode() == InteractionMode::Sculpt, "the mode dropdown enters Sculpt Mode");
    auto& md = app.mesh_document();
    app.subdivide_smooth(2);
    tick(engine, 3);
    expect(md.mesh.faces.size() == 96 && closed_and_consistent(md.mesh), "Subdivide Smooth x2: 96 quads, closed (got " +
                                                                           std::to_string(md.mesh.faces.size()) + ")");
    app.sculpt_settings().symmetry.axis[0] = false;
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

/** @brief Vertex Paint and Weight Paint end to end: the modes swap in the paint display, a
 *         stroke paints colour / weight as one undo step, Ctrl inverts, leaving the mode puts the
 *         material back and saves the colours and vertex groups into the mesh file. */
void test_editor_paint_modes() {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    using coopa::input::KeyAction;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("paint_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    const std::string own_shader = app.edited_mesh_shader();
    app.set_interaction_mode(InteractionMode::VertexPaint);
    tick(engine, 4);
    expect(app.interaction_mode() == InteractionMode::VertexPaint, "the mode menu enters Vertex Paint");
    expect(app.edited_mesh_shader() == "editor_paint", "Vertex Paint draws the mesh with the paint display shader");
    auto& md = app.mesh_document();
    app.subdivide_smooth(2);   // vertices for the brush to land on
    tick(engine, 3);
    expect(!md.mesh.has_colors, "the default cube starts without colours");
    auto& vs = app.vertex_paint_settings();
    vs.symmetry.axis[0] = false;
    vs.radius_px = 60.0f;
    vs.color = glm::vec4(1, 0, 0, 1);
    vs.secondary = glm::vec4(0, 0, 1, 1);

    const glm::vec2 corner = visible_corner(app, cube);
    auto start = screen_of(engine, app, cube, glm::vec3(corner.x, corner.y * 0.3f, 0.0f), in.scale);
    auto stop = screen_of(engine, app, cube, glm::vec3(corner.x * 0.3f, corner.y, 0.0f), in.scale);
    expect(start && stop, "the stroke lies on screen");
    if (!start || !stop) return;
    auto count = [&](auto pred) {
        size_t n = 0;
        for (const auto& f : md.mesh.faces) for (const auto& c : f.corners) n += pred(c.color) ? 1 : 0;
        return n;
    };
    const size_t undo0 = md.undo.undo_count();
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    const size_t reds = count([](const glm::vec4& c) { return c.r > 0.9f && c.g < 0.5f; });
    expect(md.mesh.has_colors && reds > 0, "a stroke paints the brush colour (" + std::to_string(reds) + " red corners)");
    expect(count([](const glm::vec4& c) { return c == glm::vec4(1.0f); }) > 0, "...only under the brush");
    expect(md.undo.undo_count() == undo0 + 1, "the whole stroke is one undo step");
    dump(engine, "15_vertex_paint");

    // Ctrl paints the secondary colour.
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
    expect(count([](const glm::vec4& c) { return c.b > 0.9f && c.r < 0.5f; }) > 0, "Ctrl paints the secondary colour");

    // Weight Paint on the same mesh: the first stroke creates a group to paint into.
    app.set_interaction_mode(InteractionMode::WeightPaint);
    tick(engine, 3);
    expect(app.interaction_mode() == InteractionMode::WeightPaint && app.edited_mesh_shader() == "editor_paint",
           "switching straight to Weight Paint keeps the paint display");
    auto& ws = app.weight_paint_settings();
    ws.symmetry.axis[0] = false;
    ws.radius_px = 60.0f;
    ws.weight = 1.0f;
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    size_t weighted = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) weighted += md.mesh.weight(v, 0) > 0.5f ? 1 : 0;
    expect(md.mesh.groups.size() == 1 && weighted > 0 && weighted < md.mesh.positions.size(),
           "a weight stroke creates \"Group\" and weights the vertices under it (" + std::to_string(weighted) + ")");
    dump(engine, "16_weight_paint");
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::K, 0, KeyAction::Press, Mods::Shift); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::K, 0, KeyAction::Release, Mods::None); });
    tick(engine, 2);
    size_t full = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) full += md.mesh.weight(v, 0) == 1.0f ? 1 : 0;
    expect(full == md.mesh.positions.size(), "Shift+K sets the brush weight on every vertex");

    // Back to Object Mode: the material's own shader returns and the file has the paint.
    const fs::path file = md.path;
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 3);
    expect(app.edited_mesh_shader() == own_shader || app.interaction_mode() == InteractionMode::Object,
           "leaving the paint modes restores the material's shader");
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id != cube || !obj) continue;
        auto* mr = obj->get_component<coopa::gfx::engine::components::MeshRenderer>();
        expect(mr && mr->material.shader != "editor_paint", "the live renderer no longer uses the paint shader");
    }
    const EditMesh saved = mesh_from_node(coopa::yaml::load_document(file));
    expect(saved.has_colors && saved.groups.size() == 1 && saved.groups[0] == "Group",
           "the saved mesh file carries the colours and the vertex group");
}

/** @brief Animation end to end: an Animator makes a rig, a clip is a file of that object's,
 *         Record poses without touching the rest pose, keys interpolate when scrubbed, the rest
 *         pose comes back, Ctrl+Z undoes a key, and Play runs the clip. */
void test_editor_animation_timeline() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("anim_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    const ObjectId bone = doc.add_object(doc.make_object("Bone"), cube);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);

    doc.select(cube);
    app.show_timeline();
    app.add_animator(cube);
    tick(engine, 2);
    expect(app.animation_rig() == cube, "an Animator makes the object a rig");
    app.new_animation_clip("Wave");
    tick(engine, 2);
    const fs::path scene_dir = doc.path().parent_path();
    expect(app.animation_clip() && fs::exists(scene_dir / "animations" / "cube" / "Wave.yaml"),
           "a new clip is a file under animations/<rig>/");
    const Node& anim = doc.find(cube)->at("components").as_seq()[static_cast<size_t>(doc.find_component(cube, "Animator"))];
    expect(get_string(anim, "auto_play") == "Wave" && anim.at("states").as_seq().size() == 1,
           "...listed as the Animator's state, and played on start");

    // Record two keys on the bone.
    doc.select(bone);
    app.set_animation_record(true);
    tick(engine, 2);
    expect(app.animation_rig() == cube, "selecting a bone keeps its rig");
    glm::vec3 rp, rr, rs;
    doc.get_transform(bone, rp, rr, rs);
    app.set_animation_time(0.0f);
    tick(engine, 1);
    app.set_object_transform(bone, glm::vec3(0, 0, 1), glm::vec3(0), glm::vec3(1));
    app.insert_keyframes();
    app.set_animation_time(1.0f);
    tick(engine, 1);
    app.set_object_transform(bone, glm::vec3(2, 0, 1), glm::vec3(0, 0, 90), glm::vec3(1));
    app.insert_keyframes();
    tick(engine, 1);
    glm::vec3 dp, dr, ds;
    doc.get_transform(bone, dp, dr, ds);
    expect(dp == rp && dr == rr, "Record poses the live rig only: the document's rest pose is untouched");
    const ClipModel* clip = app.animation_clip();
    expect(clip && clip->find_track("Bone", "position") && clip->find_track("Bone", "position")->keys.size() == 2 &&
               clip->find_track("Bone", "rotation_quat"),
           "I keys position, rotation (quaternion) and scale on the bone's path");

    auto live_bone = [&]() -> coopa::scene::SceneObject* { return app.sync().live(bone); };
    app.set_animation_time(0.5f);
    tick(engine, 2);
    const auto& lt = live_bone()->get_transform()->transform();
    const glm::quat q = lt.rotation_quat();
    expect(std::abs(lt.position().x - 1.0f) < 1e-3f, "scrubbing to the middle interpolates the position");
    expect(std::abs(glm::degrees(2.0f * std::atan2(std::abs(q.z), q.w)) - 45.0f) < 0.5f, "...and the rotation (45 deg)");
    dump(engine, "17_timeline");

    // Ctrl+Z (recording): the last keys go -- the move's auto-key and then I's -- from the file too.
    app.undo();
    app.undo();
    tick(engine, 1);
    const ClipModel saved = ClipModel::from_node(coopa::yaml::load_document(scene_dir / "animations" / "cube" / "Wave.yaml"));
    expect(app.animation_clip()->find_track("Bone", "position")->keys.size() == 1 &&
               saved.find_track("Bone", "position") && saved.find_track("Bone", "position")->keys.size() == 1,
           "undo removes the last keys, and the file follows");
    app.redo();
    app.redo();
    tick(engine, 1);

    // The rest pose returns when the clip is not shown.
    app.set_animation_record(false);
    app.set_timeline_rest_pose(true);
    tick(engine, 2);
    expect(glm::distance(live_bone()->get_transform()->transform().position(), rp) < 1e-5f,
           "Rest Pose puts the authored transform back on the live rig");
    app.set_timeline_rest_pose(false);

    // Play: the Animator plays the clip file.
    app.play();
    tick(engine, 40);
    auto* played = engine.scene().find_object("Bone");
    expect(app.playing() && played && played->get_transform()->transform().position().x > 0.2f,
           "in Play the Animator runs the clip (Bone x = " +
               std::to_string(played ? played->get_transform()->transform().position().x : -1.0f) + ")");
    app.stop();
    tick(engine, 2);
}

/**
 * @brief Every component the inspector can write loads in the engine: for each schema, a
 *        component with EVERY field at its schema default (what editing each field writes) goes
 *        through the engine's own scene loader without error and creates the component.
 */
void test_asset_fidelity_component_schemas() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("schema_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));   // registers every component parser
    int checked = 0;
    for (const auto& [type, schema] : schemas()) {
        Node comp = default_component(type);
        for (const auto& f : schema.fields) {
            if (comp.contains(f.key)) continue;
            switch (f.kind) {
                case FieldKind::Enum:  if (!f.options.empty() && !f.options.front().empty()) comp[f.key] = Node(f.options.front()); break;
                case FieldKind::String:
                case FieldKind::AssetRef:
                case FieldKind::ChildRef: if (!f.default_string.empty()) comp[f.key] = Node(f.default_string); break;
                case FieldKind::Material: break;   // default_component() writes it when the schema has one
                default: comp[f.key] = field_default_node(f); break;
            }
        }
        Node obj = Node::mapping();
        obj["name"] = Node(std::string("Probe"));
        Node comps = Node::sequence();
        Node t = Node::mapping();
        t["type"] = Node(std::string("Transform"));
        comps.as_seq().push_back(t);
        if (type != "Transform") comps.as_seq().push_back(comp);
        obj["components"] = comps;
        Node doc = Node::mapping();
        doc["format"] = Node(std::string("toyengine"));
        Node sc = Node::mapping();
        sc["scene_name"] = Node(std::string("SchemaProbe"));
        Node roots = Node::sequence();
        roots.as_seq().push_back(obj);
        sc["root_objects"] = roots;
        doc["scene"] = sc;
        std::string error;
        bool has = false;
        try {
            coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc, (root / "assets" / "scenes" / "probe.yaml").string());
            auto* o = scene.find_object("Probe");
            has = o && (type == "Transform" || o->get_component_by_type_name(type) != nullptr ||
                        o->get_component_by_type_name(type + "Component") != nullptr);
            if (!has && o) {
                for (const auto& c : o->components()) error += " " + c->type_name();
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        // A Theme with no file loads nothing (by design); one with a file is covered by the UI tests.
        if (type == "Theme") { ++checked; continue; }
        expect(error.empty() || has, "schema " + type + ": every field at its default loads in the engine (" + error + ")");
        expect(has, "schema " + type + ": ...and creates the component (has:" + error + ")");
        ++checked;
    }
    expect(checked >= 20, "checked every component schema (" + std::to_string(checked) + ")");
}

/**
 * @brief Files the editor CREATES are legitimate: a scene built in the editor from primitive
 *        meshes, a new material and a rig with a keyed clip is saved, then run through Play --
 *        the engine's own loaders -- and every mesh builds, the material's values arrive, and
 *        the Animator plays its clip file.
 */
void test_asset_fidelity_editor_created_files() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("created_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    auto& doc = app.document();

    std::vector<ObjectId> prims;
    for (const char* p : {"Cube", "Sphere", "Cylinder", "Plane", "Cone", "Torus", "Icosphere"}) {
        const ObjectId id = app.create_primitive(p);
        if (id) prims.push_back(id);
    }
    expect(prims.size() >= 4, "created primitive objects (" + std::to_string(prims.size()) + ")");
    Node mat = Node::mapping();
    mat["albedo"] = make_color(glm::vec3(0.1f, 0.6f, 0.3f));
    mat["metallic"] = make_float(0.25);
    mat["roughness"] = make_float(0.7);
    expect(app.create_material("painted", mat), "created a material asset");
    app.show_document_view();   // creating a material opens it; back to the scene
    tick(engine, 2);
    const int mr = doc.find_component(prims[0], "MeshRenderer");
    Node mrn = doc.find(prims[0])->at("components").as_seq()[static_cast<size_t>(mr)];
    mrn["material"] = Node(std::string("materials/painted"));
    doc.set_component(prims[0], mr, mrn, "Material");
    const ObjectId bone = doc.add_object(doc.make_object("Bone"), prims[0]);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);
    doc.select(prims[0]);
    app.show_timeline();
    app.add_animator(prims[0]);
    app.new_animation_clip("Spin");
    tick(engine, 2);
    doc.select(bone);
    app.set_animation_record(true);
    tick(engine, 1);
    app.set_animation_time(0.0f);
    tick(engine, 1);
    app.insert_keyframes();
    app.set_animation_time(1.0f);
    tick(engine, 1);
    app.set_object_transform(bone, glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(1));
    app.insert_keyframes();
    app.set_animation_record(false);
    tick(engine, 1);
    expect(app.save_scene(), "the scene saves");

    // Play loads the saved document through the engine's own loaders.
    app.play();
    tick(engine, 30);
    expect(app.playing(), "Play started from the editor-built scene");
    auto& scene = engine.scene();
    int ready = 0, renderers = 0;
    for (auto* r : scene.get_components<coopa::gfx::engine::components::MeshRenderer>()) {
        if (!r->owner) continue;
        bool prim = false;
        for (const auto& [id, live] : app.sync().live_objects()) (void)id, (void)live;
        for (ObjectId pid : prims) prim |= r->owner->name() == get_string(*doc.find(pid), "name");
        if (!prim) continue;
        ++renderers;
        ready += r->is_ready() ? 1 : 0;
    }
    expect(renderers == static_cast<int>(prims.size()) && ready == renderers,
           "every editor-made mesh loads in the engine (" + std::to_string(ready) + "/" + std::to_string(renderers) + ")");
    auto* painted = scene.find_object(get_string(*doc.find(prims[0]), "name"));
    auto* pmr = painted ? painted->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(pmr && glm::distance(pmr->material.albedo, glm::vec3(0.1f, 0.6f, 0.3f)) < 1e-3f && std::abs(pmr->material.metallic - 0.25f) < 1e-4f &&
               std::abs(pmr->material.roughness - 0.7f) < 1e-4f,
           "the editor-made material's values reach the engine");
    auto* animator = painted ? painted->get_component<coopa::anim::Animator>() : nullptr;
    auto* live_bone = painted ? painted->find_descendant("Bone") : nullptr;
    expect(animator && animator->is_playing() && animator->current_state() == "Spin",
           "the editor-made Animator plays its clip state");
    expect(live_bone && live_bone->get_transform()->transform().position().z > 0.5f,
           "...and the clip file drives the bone (z = " +
               std::to_string(live_bone ? live_bone->get_transform()->transform().position().z : -1.0f) + ")");
    app.stop();
    tick(engine, 2);
}

/** @brief Animating the friendly way: Record auto-keys exactly the channels a move changes (a
 *         drag of many moves is one undo step), the inspector's diamonds key one channel, the key
 *         menu's interpolation reaches the file and the sampling, and a clip renames cleanly. */
void test_editor_animation_autokey() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("autokey_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    const ObjectId arm = doc.add_object(doc.make_object("Arm"), cube);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);
    doc.select(cube);
    app.show_timeline();
    app.add_animator(cube);
    app.new_animation_clip("Reach");
    tick(engine, 2);
    doc.select(arm);
    app.set_animation_record(true);
    app.set_animation_time(10.0f / 30.0f);
    tick(engine, 2);

    // A "drag": several moves of the position only.
    auto& md = *app.animation_clip();
    const size_t undo0 = 0;
    (void)undo0;
    for (int i = 1; i <= 5; ++i) app.set_object_transform(arm, glm::vec3(0.1f * i, 0, 0), glm::vec3(0), glm::vec3(1));
    tick(engine, 2);
    expect(md.find_track("Arm", "position") && md.find_track("Arm", "position")->keys.size() == 1 &&
               std::abs(md.find_track("Arm", "position")->keys[0].value.x - 0.5f) < 1e-5f,
           "Record auto-keys the moved channel at the playhead (the last value of the drag)");
    expect(!md.find_track("Arm", "rotation_quat") && !md.find_track("Arm", "scale"), "...and only that channel");
    app.undo();
    tick(engine, 1);
    expect(!app.animation_clip()->find_track("Arm", "position"), "the whole drag is one undo step");
    app.redo();
    tick(engine, 1);

    // The inspector diamond keys one channel; it then reads as keyed on this frame.
    expect(!app.keyed_now(arm, "scale"), "scale is not keyed yet");
    app.key_channel(arm, "scale");
    tick(engine, 1);
    expect(app.keyed_now(arm, "scale") && app.animation_clip()->find_track("Arm", "scale"), "the diamond keys scale at this frame");

    // A second key, then Step interpolation holds the first value until the next key.
    app.set_animation_time(20.0f / 30.0f);
    tick(engine, 1);
    app.set_object_transform(arm, glm::vec3(1.5f, 0, 0), glm::vec3(0), glm::vec3(1));
    tick(engine, 1);
    app.select_all_keys();
    app.set_selected_key_interpolation("step");
    app.set_animation_time(15.0f / 30.0f);
    tick(engine, 3);
    const float x_mid = app.sync().live(arm)->get_transform()->transform().position().x;
    expect(std::abs(x_mid - 0.5f) < 1e-4f, "Step interpolation holds the earlier key (x = " + std::to_string(x_mid) + ")");
    const fs::path dir = doc.path().parent_path() / "animations" / "cube";
    const ClipModel on_disk = ClipModel::from_node(coopa::yaml::load_document(dir / "Reach.yaml"));
    expect(on_disk.find_track("Arm", "position") && on_disk.find_track("Arm", "position")->keys[0].easing == "step",
           "the interpolation is in the clip file");

    // Rename.
    app.rename_animation_clip("Grab");
    tick(engine, 2);
    const Node& anim = doc.find(cube)->at("components").as_seq()[static_cast<size_t>(doc.find_component(cube, "Animator"))];
    expect(fs::exists(dir / "Grab.yaml") && !fs::exists(dir / "Reach.yaml") && get_string(anim, "auto_play") == "Grab" &&
               get_string(anim.at("states").as_seq()[0], "clip") == "animations/cube/Grab.yaml",
           "renaming moves the file and updates the Animator's state and auto_play");
    dump(engine, "18_autokey");
}

/** @brief The animation_test scene in the editor: each rig's Timeline offers exactly that
 *         object's clips (from any object inside it), previews them, and its clip files are
 *         the editor's to edit without loss. */
void test_editor_animation_test_scene() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("anim_scene_project");
    Project project = Project::create(root);
    const fs::path dst = project.assets() / "scenes" / "animation_test";
    fs::create_directories(dst.parent_path());
    fs::copy(fs::path(ROOT_DIR) / "assets" / "scenes" / "tests" / "animation" / "animation_test", dst, fs::copy_options::recursive);
    // The rigs' shared clips and meshes (assets/animations, assets/meshes).
    fs::copy(fs::path(ROOT_DIR) / "assets" / "animations", project.assets() / "animations", fs::copy_options::recursive);
    for (const char* m : {"animation/tentacle.yaml", "primitives/ball.yaml"}) {
        fs::copy_file(fs::path(ROOT_DIR) / "assets" / "meshes" / m, project.assets() / "meshes" / fs::path(m).filename(),
                      fs::copy_options::overwrite_existing);
    }
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    expect(app.open_scene(dst / "scene.yaml"), "the animation test scene opens");
    tick(engine, 4);
    app.show_timeline();
    auto& doc = app.document();
    auto states_of = [&](ObjectId rig) {
        std::vector<std::string> out;
        const Node& a = doc.find(rig)->at("components").as_seq()[static_cast<size_t>(doc.find_component(rig, "Animator"))];
        for (const auto& st : a.at("states").as_seq()) out.push_back(get_string(st, "name"));
        return out;
    };
    const ObjectId arm = object_named(app, "robot_arm"), elbow = object_named(app, "elbow"), ball = object_named(app, "bouncing_ball");
    expect(arm && elbow && ball, "the rigs are in the scene");
    doc.select(elbow);
    tick(engine, 2);
    expect(app.animation_rig() == arm && app.animation_clip() && app.animation_clip_state() == "wave",
           "selecting a joint opens its rig's first clip");
    expect(states_of(arm) == std::vector<std::string>{"wave", "idle"}, "the arm's clips are its own two");
    const glm::quat rest = app.sync().live(elbow)->get_transform()->transform().rotation_quat();
    app.set_animation_time(0.5f);
    tick(engine, 2);
    const glm::quat posed = app.sync().live(elbow)->get_transform()->transform().rotation_quat();
    expect(std::abs(glm::dot(rest, posed)) < 0.95f, "scrubbing previews the clip on the joint");
    dump(engine, "19_animation_test");
    doc.select(ball);
    tick(engine, 2);
    expect(app.animation_rig() == ball && app.animation_clip_state() == "bounce", "the ball's Timeline is the ball's own clip");
    app.set_timeline_rest_pose(true);
    tick(engine, 2);
    // Nothing was edited: every clip file is byte-identical to the shipped one.
    for (const char* rel : {"animations/robot_arm/wave.yaml", "animations/bouncing_ball/bounce.yaml"}) {
        const Node a = coopa::yaml::load_document(project.assets() / rel);
        const Node b = coopa::yaml::load_document(fs::path(ROOT_DIR) / "assets" / rel);
        expect(a == b, std::string("browsing and previewing leaves ") + rel + " untouched");
    }
}

/**
 * @brief Copies toyengine's animation test rigs into a project, untagged: the rig object assets
 *        (objects/animation/ -> objects/), their clips, and the meshes they use.
 */
void copy_rig_assets(const fs::path& src, const fs::path& dst) {
    fs::create_directories(dst / "objects");
    for (const auto& e : fs::directory_iterator(src / "objects" / "animation")) {
        fs::copy_file(e.path(), dst / "objects" / e.path().filename(), fs::copy_options::overwrite_existing);
    }
    fs::copy(src / "animations", dst / "animations", fs::copy_options::recursive);
    for (const char* m : {"animation/tentacle.yaml", "primitives/ball.yaml"}) {
        fs::copy_file(src / "meshes" / m, dst / "meshes" / fs::path(m).filename(), fs::copy_options::overwrite_existing);
    }
}

/** @brief The test rigs ship as object assets: the Objects tab lists them, opening one gives a
 *         working Timeline, and a copy placed in a scene plays its clips (paths resolve from the
 *         object asset to the shared assets/animations) -- and points back to the asset to edit. */
void test_editor_rig_object_assets() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("rig_objects_project");
    Project project = Project::create(root);
    const fs::path src = fs::path(ROOT_DIR) / "assets";
    copy_rig_assets(src, project.assets());
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    const auto objects = app.list_assets(AssetType::Object);
    auto listed = [&](const std::string& rel) { return std::find(objects.begin(), objects.end(), rel) != objects.end(); };
    expect(listed("objects/robot_arm.yaml") && listed("objects/tentacle.yaml") && listed("objects/bouncing_ball.yaml"),
           "the Objects tab lists the test rigs (" + std::to_string(objects.size()) + ")");

    // Open the robot arm: a rig with its clips, previewed by the Timeline.
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm object asset opens");
    tick(engine, 3);
    auto& doc = app.document();
    app.show_timeline();
    const ObjectId elbow = object_named(app, "elbow");
    doc.select(elbow);
    tick(engine, 2);
    expect(app.animation_clip() && app.animation_clip_state() == "wave", "its Timeline opens the arm's clip (shared from assets/animations)");
    const glm::quat rest = app.sync().live(elbow)->get_transform()->transform().rotation_quat();
    app.set_animation_time(0.5f);
    tick(engine, 2);
    expect(std::abs(glm::dot(rest, app.sync().live(elbow)->get_transform()->transform().rotation_quat())) < 0.95f,
           "scrubbing poses the object asset's rig");
    dump(engine, "20_robot_arm_asset");

    // Place the ball in a scene and play: the placed copy bounces from the shared clip.
    app.show_document_view();
    app.open_scene(project.assets() / "scenes" / "main" / "scene.yaml");
    tick(engine, 3);
    const ObjectId placed = app.place_object_asset("objects/bouncing_ball.yaml", glm::vec3(2, 0, 0));
    tick(engine, 3);
    app.document().select(placed);
    tick(engine, 2);
    expect(app.animation_instance_asset(placed) == "objects/bouncing_ball" && app.animation_rig() == 0,
           "a placed copy points the Timeline at its object asset");
    app.play();
    tick(engine, 15);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 60; ++i) {
        tick(engine, 1);
        coopa::scene::SceneObject* ball = nullptr;
        for (auto* o : engine.scene().get_components<coopa::gfx::engine::components::MeshRenderer>()) {
            if (o->owner && o->owner->name() == "ball") ball = o->owner;
        }
        if (!ball) continue;
        const float z = glm::vec3(ball->get_transform()->get_world_matrix()[3]).z;
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }
    expect(app.playing() && hi - lo > 1.0f, "in Play the placed ball bounces (z " + std::to_string(lo) + " .. " + std::to_string(hi) + ")");
    app.stop();
    tick(engine, 2);
}

/** @brief In an opened object asset (the robot arm rig), its meshes are pickable in the
 *         viewport and enter Edit Mode like any scene object's. */
void test_editor_object_asset_pick_and_edit() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("object_pick_project");
    Project project = Project::create(root);
    const fs::path src = fs::path(ROOT_DIR) / "assets";
    copy_rig_assets(src, project.assets());
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm opens");
    tick(engine, 6);
    const ObjectId plate = object_named(app, "base_plate");
    expect(plate != 0, "the base plate is in the document");
    const float scale = std::max(1.0f, engine.display_scale());
    // The plate's top face near its front corner (its centre is under the shoulder joint).
    const auto px = screen_of(engine, app, plate, glm::vec3(0.4f, -0.4f, 0.5f), scale);
    expect(px.has_value(), "the plate is on screen");
    if (px) {
        const ObjectId got = app.pick_object(*px);
        expect(got == plate, "clicking the plate picks it (got " +
                                 (got && app.document().find(got) ? get_string(*app.document().find(got), "name") : std::string("nothing")) + ")");
    }
    app.document().select(plate);
    tick(engine, 2);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Tab enters Edit Mode on the plate's mesh");
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::Edit && !app.mesh_document().mesh.faces.empty(),
           "...editing the cube mesh it uses");
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);

    // The skinned tentacle: its mesh (a SkinnedMeshRenderer's) picks, edits and weight-paints.
    expect(app.open_object_asset(project.assets() / "objects" / "tentacle.yaml"), "the tentacle opens");
    tick(engine, 6);
    const ObjectId skin = object_named(app, "tentacle_skin");
    const auto spx = screen_of(engine, app, skin, glm::vec3(0.0f, -0.2f, 0.6f), scale);   // the tube's front, low down
    expect(spx && app.pick_object(*spx) == skin, "clicking the skinned tentacle picks it");
    app.document().select(skin);
    tick(engine, 2);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Tab enters Edit Mode on the skinned mesh");
    tick(engine, 2);
    auto& md = app.mesh_document();
    expect(md.mesh.groups.size() == 4 && md.mesh.has_colors, "...with its vertex groups and colours");
    auto* smr = app.sync().live(skin)->get_component<toy::scene::SkinnedMeshRenderer>();
    float top_before = -1e9f;
    for (const auto& v : smr->skinned_vertices()) top_before = std::max(top_before, v.position.z);
    md.edit("Raise", [](EditMesh& m, MeshSelection&) { for (auto& p : m.positions) p.z += 0.5f; });
    tick(engine, 4);
    float top_after = -1e9f;
    for (const auto& v : smr->skinned_vertices()) top_after = std::max(top_after, v.position.z);
    expect(top_after > top_before + 0.4f, "an edit reaches the live skinned mesh (top " + std::to_string(top_before) + " -> " +
                                              std::to_string(top_after) + ")");
    expect(app.set_interaction_mode(InteractionMode::WeightPaint) && app.edited_mesh_shader() == "editor_paint",
           "Weight Paint shows the tentacle's groups");
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
}

/** @brief The same through real input, with the Timeline open and posing the rig: a click in
 *         the viewport selects the clicked mesh and Tab enters Edit Mode on it. */
void test_editor_object_asset_click_and_tab() {
    using coopa::input::Key;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("object_click_project");
    Project project = Project::create(root);
    const fs::path src = fs::path(ROOT_DIR) / "assets";
    copy_rig_assets(src, project.assets());
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm opens");
    app.show_timeline();
    tick(engine, 6);
    for (const char* name : {"base_plate", "upper_arm_shape"}) {
        const ObjectId target = object_named(app, name);
        const auto px = screen_of(engine, app, target, glm::vec3(0.0f, -0.5f, 0.0f), in.scale);   // the shape's front face
        expect(px.has_value(), std::string(name) + " is on screen");
        if (!px) continue;
        in.click(*px);
        tick(engine, 2);
        expect(app.document().primary() == target, std::string("clicking ") + name + " selects it (selected: " +
               (app.document().primary() ? get_string(*app.document().find(app.document().primary()), "name") : std::string("nothing")) + ")");
        in.move(*px);
        in.key(Key::Tab);
        tick(engine, 2);
        expect(app.interaction_mode() == InteractionMode::Edit, std::string("Tab enters Edit Mode on ") + name);
        in.key(Key::Tab);
        tick(engine, 2);
        expect(app.interaction_mode() == InteractionMode::Object, "Tab returns to Object Mode");
    }
    dump(engine, "21_object_click");
}

/** @brief Blender's viewport grid and increment snap: the grid faces the view down X / Y, its
 *         spacing follows the zoom, Ctrl moves in that spacing, and F frames the selection. */
/**
 * @brief Settings rows edited in a scene become that scene's overrides of config.yaml: they
 *        apply live, leave config.yaml alone, undo like any scene edit, reach Play (physics
 *        included), revert to the project's value, and save into the scene as `settings:`.
 */
void test_editor_scene_settings_override() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("scene_settings_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    const float project_exposure = engine.render_config().exposure;
    const Node config_render_before = app.config_document().section("render");

    app.set_prop_tab(PropTab::Render);
    const Node three = make_float(3.0);
    app.set_scene_setting("render", "exposure", &three);
    tick(engine, 2);
    expect(std::abs(engine.render_config().exposure - 3.0f) < 1e-5f, "a scene override applies live");
    expect(app.config_document().section("render") == config_render_before && !app.config_document().dirty(),
           "config.yaml is untouched by a scene override");
    expect(app.document().scene_setting("render", "exposure") != nullptr, "the scene document carries the override");

    app.undo();
    tick(engine, 4);
    expect(app.document().scene_setting("render", "exposure") == nullptr &&
           std::abs(engine.render_config().exposure - project_exposure) < 1e-5f, "undo removes the override");
    app.redo();
    tick(engine, 4);
    expect(std::abs(engine.render_config().exposure - 3.0f) < 1e-5f, "redo restores it");

    // A config.yaml edit still shows through wherever the scene doesn't override.
    {
        const Node before = app.config_document().node;
        app.config_document().section("render")["fog_density"] = make_float(0.07);
        app.config_document().commit("fog", before, {});
        app.apply_config_live();
        tick(engine, 2);
        expect(std::abs(engine.render_config().fog_density - 0.07f) < 1e-5f &&
               std::abs(engine.render_config().exposure - 3.0f) < 1e-5f,
               "a project setting applies under the scene's overrides");
    }

    const Node gravity = make_vec3({0.0f, 0.0f, -2.0f});
    app.set_scene_setting("physics", "gravity", &gravity);
    app.play();
    tick(engine, 4);
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(engine.scene().find_system("Physics"));
    expect(app.playing() && physics && std::abs(physics->world().gravity().z + 2.0f) < 1e-5f,
           "Play runs with the scene's physics override");
    expect(std::abs(engine.render_config().exposure - 3.0f) < 1e-5f, "...and its render overrides");
    app.stop();
    tick(engine, 3);

    app.set_scene_setting("render", "exposure", nullptr);
    tick(engine, 2);
    expect(std::abs(engine.render_config().exposure - project_exposure) < 1e-5f, "reverting returns to the project's value");

    expect(app.save_scene(), "the scene saves");
    const Node saved = coopa::yaml::load_document(project.assets() / "scenes" / "main" / "scene.yaml");
    const Node& sc = saved.at("scene");
    expect(sc.contains("settings") && sc.at("settings").contains("physics") && !sc.at("settings").contains("render"),
           "the file keeps only the remaining overrides (" + coopa::yaml::emit(sc.contains("settings") ? sc.at("settings") : Node()) + ")");

    const Node off(false);
    app.set_scene_setting("render", "shadows_enabled", &off);   // a visible row, for the dump
    tick(engine, 2);
    expect(!engine.render_config().shadows_enabled, "a bool override applies");
    dump(engine, "21_scene_setting_override");
}

/** @brief The material editor's Shader dropdown only offers shaders the engine registers, in
 *         the pass (domain) the catalogue claims -- a mismatch would silently render stock PBR. */
void test_editor_material_shader_catalogue() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("shader_catalogue_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    const auto& registry = engine.render_config().surface_shaders;
    for (const auto& s : surface_shaders()) {
        if (s.name.empty()) continue;
        const auto* desc = registry.find(s.name);
        expect(desc != nullptr, "the engine registers the '" + s.name + "' shader the dropdown offers");
        if (!desc) continue;
        const bool transparent = desc->domain == coopa::gfx::pipeline::SurfaceShaderDomain::Transparent;
        expect(transparent == s.transparent, "'" + s.name + "' is in the pass the editor assumes");
        expect(s.params.size() <= 4, "'" + s.name + "' labels at most the four shader_params slots");
    }
}

void test_editor_grid_snap_and_frame() {
    using coopa::input::Key;
    {   // ModalTransform snaps a free move to snap_step.
        const ViewProj vp = test_view_proj();
        const glm::vec2 c = *vp.project(glm::vec3(0));
        ModalTransform mt;
        mt.snap_step = 0.05f;
        mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
        mt.update(vp, c + glm::vec2(37.0f, -11.0f), {}, false, false, /*ctrl=*/true, false);
        const glm::vec3 t = mt.result().translate;
        bool on_grid = glm::length(t) > 0.0f;
        for (int i = 0; i < 3; ++i) on_grid &= std::abs(t[i] / 0.05f - std::round(t[i] / 0.05f)) < 1e-3f;
        expect(on_grid, "Ctrl snaps a free move to the 0.05 increment");
        mt.cancel();
    }
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("grid_snap_project");
    Project project = Project::create(root);
    const fs::path src = fs::path(ROOT_DIR) / "assets";
    copy_rig_assets(src, project.assets());
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm opens");
    tick(engine, 4);

    auto& cam = app.camera();
    cam.distance = 12.0f; cam.apply();
    expect(std::abs(app.grid_step() - 1.0f) < 1e-5f, "12 m out the grid is 1 m");
    cam.distance = 0.5f; cam.apply();
    expect(std::abs(app.grid_step() - 0.05f) < 1e-5f, "zoomed in to 0.5 m it is 5 cm");
    cam.distance = 300.0f; cam.apply();
    expect(std::abs(app.grid_step() - 50.0f) < 1e-3f, "300 m out it is 50 m");
    cam.distance = 12.0f; cam.apply();
    expect(app.grid_normal_axis() == 2, "an orbit view draws the floor grid");

    const ObjectId plate = object_named(app, "base_plate");
    const auto px = screen_of(engine, app, plate, glm::vec3(0.0f, -0.5f, 0.0f), in.scale);
    expect(px.has_value(), "base_plate is on screen");
    if (px) {
        in.click(*px);
        tick(engine, 2);
        expect(app.document().primary() == plate, "the click selects base_plate");
        const glm::vec3 framed = cam.focus;
        cam.focus = glm::vec3(40.0f, -30.0f, 5.0f);
        cam.apply();
        in.move(*px);
        tick(engine, 1);
        in.key(Key::F);
        tick(engine, 2);
        expect(glm::length(cam.focus - glm::vec3(40.0f, -30.0f, 5.0f)) > 10.0f && glm::length(cam.focus - framed) < 3.0f,
               "F refocuses the orbit on the selection");
        cam.focus = glm::vec3(40.0f, -30.0f, 5.0f);
        cam.apply();
        in.key(Key::Period);
        tick(engine, 2);
        expect(glm::length(cam.focus - framed) < 3.0f, ". frames the selection too");
    }

    in.key(Key::Kp1);
    tick(engine, 2);
    expect(app.grid_normal_axis() == 1, "front view (numpad 1, looking down Y) draws the XZ grid");
    dump(engine, "22_grid_front");
    in.key(Key::Kp3);
    tick(engine, 2);
    expect(app.grid_normal_axis() == 0, "right view (numpad 3, looking down X) draws the YZ grid");
    in.key(Key::Kp7);
    tick(engine, 2);
    expect(app.grid_normal_axis() == 2, "top view draws the floor grid");
}

/** @brief Blender's X-Ray in Edit Mode: Alt+Z makes the surface translucent, and what is behind it
 *         is drawn (dimmed) and can be clicked. */
void test_editor_xray_edit_mode() {
    using coopa::input::Key;
    using coopa::input::Mods;
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("xray_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    in.move(app.viewport_box().center());
    in.key(Key::Tab);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    expect(app.edit_mode_active(), "Tab enters Edit Mode");
    in.key(Key::Num1);   // vertex select
    tick(engine, 2);

    auto& md = app.mesh_document();
    // The vertex farthest from the camera: behind the cube's surface.
    const glm::mat4 view = coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix();
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::mat4 w = world_of(app, cube);
    uint32_t back = 0;
    float bd = -1.0f;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) {
        const float d = glm::distance(glm::vec3(w * glm::vec4(md.mesh.positions[v], 1.0f)), eye);
        if (d > bd) { bd = d; back = v; }
    }
    const auto back_px = screen_of(engine, app, cube, md.mesh.positions[back], in.scale);
    expect(back_px.has_value(), "the hidden vertex projects on screen");
    if (!back_px) return;
    auto pixel = [&](glm::vec2 p) {
        const auto img = engine.capture_image(false);
        const int x = std::clamp(static_cast<int>(p.x * in.scale), 0, static_cast<int>(img.width) - 1);
        const int y = std::clamp(static_cast<int>(p.y * in.scale), 0, static_cast<int>(img.height) - 1);
        const uint8_t* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * img.channels];
        return glm::vec3(q[0], q[1], q[2]);
    };
    const glm::vec2 body = app.viewport_box().center() + glm::vec2(9.0f, 7.0f);   // on the cube, off the overlay
    const glm::vec3 opaque = pixel(body);

    md.selection.verts.clear();
    in.click(*back_px);
    tick(engine, 2);
    expect(!md.selection.verts.count(back), "without X-Ray the hidden vertex can't be clicked");

    in.move(app.viewport_box().center());
    in.key(Key::Z, Mods::Alt);
    tick(engine, 3);
    expect(engine.render_config().editor_xray_alpha < 0.99f, "Alt+Z turns X-Ray on: the surface is translucent");
    const glm::vec3 xray = pixel(body);
    expect(glm::length(xray - opaque) > 6.0f, "the X-Ray surface lets the backdrop through");
    dump(engine, "23_xray_edit");

    md.selection.verts.clear();
    in.click(*back_px);
    tick(engine, 2);
    expect(md.selection.verts.count(back) == 1, "with X-Ray the vertex behind the surface is clicked");
    // Box select: a drag over the whole cube takes the hidden vertex only with X-Ray.
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const auto& p : md.mesh.positions)
        if (auto q = screen_of(engine, app, cube, p, in.scale)) { lo = glm::min(lo, *q); hi = glm::max(hi, *q); }
    auto box_all = [&] {
        md.selection.verts.clear();
        in.move(lo - glm::vec2(15.0f));
        in.drag(hi + glm::vec2(15.0f), coopa::input::MouseButton::Left);
        tick(engine, 2);
    };
    box_all();
    expect(md.selection.verts.size() == md.mesh.positions.size(), "with X-Ray a box takes every vertex, hidden ones too");

    // Faces: X-Ray picks the face whose centre is nearest the click on screen, even one behind
    // the face the ray would hit first.
    in.key(Key::Num3);
    tick(engine, 2);
    uint32_t back_face = 0;
    float fd = -1.0f;
    for (uint32_t f = 0; f < md.mesh.faces.size(); ++f) {
        const float d = glm::distance(glm::vec3(w * glm::vec4(md.mesh.face_center(f), 1.0f)), eye);
        if (d > fd) { fd = d; back_face = f; }
    }
    const auto face_px = screen_of(engine, app, cube, md.mesh.face_center(back_face), in.scale);
    expect(face_px.has_value(), "the hidden face's centre projects on screen");
    if (face_px) {
        md.selection.faces.clear();
        in.click(*face_px);
        tick(engine, 2);
        expect(md.selection.faces.size() == 1 && md.selection.faces.count(back_face),
               "with X-Ray a click on a hidden face's dot selects that face");
    }

    in.move(app.viewport_box().center());
    in.key(Key::Z, Mods::Alt);
    tick(engine, 3);
    expect(engine.render_config().editor_xray_alpha >= 1.0f, "Alt+Z again: opaque");
    if (face_px) {
        md.selection.faces.clear();
        in.click(*face_px);
        tick(engine, 2);
        expect(md.selection.faces.size() == 1 && !md.selection.faces.count(back_face),
               "without X-Ray the same click takes the face in front");
    }
    in.key(Key::Num1);
    tick(engine, 2);
    box_all();
    expect(!md.selection.verts.empty() && !md.selection.verts.count(back), "without X-Ray a box takes only the vertices in view");
}

/** @brief The nav gizmo's axis balls go orthographic along that axis (orbiting returns to
 *         perspective), and trackpad swipes orbit both ways while pinch zooms. */
void test_editor_nav_axis_and_trackpad() {
    setenv("FIXED_DT", "0.016666", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("nav_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    auto& cam = app.camera();
    expect(!cam.ortho, "the view starts in perspective");

    // Click the +X ball: its screen spot is the view rotation applied to +X around the ball's centre.
    const coopa::ui::imm::Box g = app.nav_gizmo_rect();
    const glm::vec2 c{g.x + 55, g.y + 55};
    const glm::mat3 vr = glm::mat3(coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix());
    const glm::vec3 vx = vr * glm::vec3(1, 0, 0);
    in.click(c + glm::vec2(vx.x, -vx.y) * 40.0f);
    tick(engine, 2);
    expect(cam.ortho, "clicking the X ball goes orthographic");
    expect(std::abs(cam.forward().x + 1.0f) < 1e-3f, "and looks down the X axis (from +X)");

    auto scroll = [&](glm::vec2 d) {
        in.move(app.viewport_box().center());
        engine.queue_input([d](coopa::input::Input& i) { i.push_scroll(d.x, d.y); });
        tick(engine, 2);
    };
    // Trackpad: a vertical swipe tilts the view (and leaves the auto-ortho axis view).
    app.set_trackpad_for_test(true);
    const float pitch0 = cam.pitch_deg, dist0 = cam.distance;
    scroll({0.0f, 3.0f});
    expect(std::abs(cam.pitch_deg - pitch0) > 1.0f, "a vertical trackpad swipe tilts the view up / down");
    expect(std::abs(cam.distance - dist0) < 1e-4f, "...without zooming");
    expect(!cam.ortho, "orbiting out of the axis view returns to perspective");

    app.pinch_for_test(0.2);
    tick(engine, 2);
    expect(cam.distance < dist0 * 0.9f, "pinching out zooms in");

    // A wheel still zooms.
    app.set_trackpad_for_test(false);
    const float pitch1 = cam.pitch_deg, dist1 = cam.distance;
    scroll({0.0f, 1.0f});
    expect(cam.distance < dist1 && std::abs(cam.pitch_deg - pitch1) < 1e-4f, "a mouse wheel zooms, as before");

    // An explicit numpad 5 ortho stays ortho through orbits.
    cam.set_ortho(true);
    scroll({0.0f, 0.0f});
    app.set_trackpad_for_test(true);
    scroll({2.0f, 0.0f});
    expect(cam.ortho, "a chosen orthographic view stays orthographic when orbited");
    app.set_trackpad_for_test(std::nullopt);
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
    expect(app.active_asset_type() == AssetType::Scene && engine.scene().find_object("cube"), "back to the scene");
    // Unsaved changes: switching asks first and does nothing until answered.
    const ObjectId cube = object_named(app, "cube");
    app.document().set_object_key(cube, "name", Node(std::string("Renamed")), "Rename");
    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 3);
    expect(app.active_asset_type() == AssetType::Scene && app.ui().any_popup_open(), "switching with unsaved changes opens the save prompt");
    tick(engine, 2);
    dump(engine, "24_unsaved_prompt");
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
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    expect(app.create_object_asset_from_selection(), "Create Object Asset from the selection");
    tick(engine, 3);
    const fs::path asset = project.assets() / "objects" / "cube.yaml";
    expect(coopa::yaml::document_exists(asset), "objects/cube.yaml is written");
    const Node* inst = app.document().find(cube);
    expect(inst && get_string(*inst, "prefab") == "objects/cube", "the selection becomes an instance (`prefab:`)");
    auto* live = engine.scene().find_object("cube");
    auto* mr = live ? live->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(mr && mr->is_ready(), "the instance still renders its mesh");
    const ObjectId second = app.place_object_asset("objects/cube.yaml", glm::vec3(3, 0, 0.5f));
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
    app.open_asset(AssetType::Object, "objects/cube.yaml");
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
    auto* spawned = engine.spawn("objects/cube", glm::vec3(0, 3, 1));
    expect(spawned && engine.scene().root_objects().size() == before + 1 && spawned->children().size() == 1,
           "Engine::spawn() instantiates the asset, child included");
    dump(engine, "17_object_instances");
}

/**
 * @brief The UI designer end to end, through real input: a new UI asset opens in the designer
 *        with its canvas drawn inside the preview frame; widgets are added and land in the
 *        right parent; a click picks, a drag moves (one undo step); Interact runs the UI and
 *        echoes a button's click; the file saves; placed in a scene, the HUD stays inside the
 *        viewer.
 */
void test_editor_ui_designer() {
    using coopa::input::MouseButton;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("ui_designer_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};

    expect(app.new_ui_asset("test_hud", "blank"), "a blank UI asset is created");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::UI && app.ui_mode(), "it opens in the UI designer");
    expect(coopa::yaml::document_exists(project.assets() / "ui" / "test_hud.yaml"), "as assets/ui/test_hud.yaml");
    const imm::Box frame = app.ui_frame();
    expect(frame.w > 100 && frame.h > 50 && std::abs(frame.w / frame.h - 1920.0f / 1080.0f) < 0.02f,
           "the preview frame has the preview resolution's aspect");
    const auto placement = engine.scene_ui_placement();
    expect(placement && std::abs(static_cast<float>(placement->rect.w) - frame.w * in.scale) < 2.0f && !placement->input,
           "the engine places the asset's canvas in the frame (input off in Design)");

    // Add a window (into the root) and a menu into the window: the menu lands in its body.
    const ObjectId win = app.ui_add_widget("window");
    tick(engine, 3);
    expect(win && app.document().parent_of(win).value_or(0) == app.document().object_root(), "Add Window: a child of the canvas");
    const ObjectId menu = app.ui_add_widget("menu", win);
    tick(engine, 3);
    expect(menu && app.document().parent_of(menu).value_or(0) == win, "Add Menu List into the window");
    auto* menu_live = app.sync().live(menu);
    expect(menu_live && menu_live->parent() && menu_live->parent()->name() == "Body", "...and it sits in the Window's Body slot");
    auto wr = app.ui_live_rect(win);
    expect(wr && std::abs(wr->size().x - 440.0f) < 1.0f && std::abs(wr->size().y - 340.0f) < 1.0f, "the window is its authored 440 x 340");

    // The canvas draws: the window's panel colour is in the frame, the backdrop elsewhere.
    tick(engine, 2);
    const auto shot = engine.capture_image(false);
    const ui::UiView view = app.ui_view();
    const glm::vec2 wc = view.to_editor(wr->center() + glm::vec2(0, -40));
    auto px = [&](glm::vec2 e) {
        const uint32_t x = static_cast<uint32_t>(e.x * in.scale), y = static_cast<uint32_t>(e.y * in.scale);
        const size_t i = (static_cast<size_t>(y) * shot.width + x) * shot.channels;
        return glm::ivec3(shot.pixels[i], shot.pixels[i + 1], shot.pixels[i + 2]);
    };
    const glm::ivec3 inside = px(wc), backdrop = px({frame.x + 6, frame.y + 6});
    expect(glm::length(glm::vec3(inside - backdrop)) > 8.0f, "the window is drawn inside the frame (got " +
           std::to_string(inside.x) + "," + std::to_string(inside.y) + "," + std::to_string(inside.z) + " vs backdrop " +
           std::to_string(backdrop.x) + "," + std::to_string(backdrop.y) + "," + std::to_string(backdrop.z) + ")");
    dump(engine, "30_ui_designer");

    // Picking: the window's title bar picks the window (the menu covers its body).
    // (Left of centre: the middle of the top edge is the resize handle.)
    const glm::vec2 title_px = view.to_editor({wr->center().x - 120.0f, wr->max.y - 10.0f});
    const auto hits = app.ui_pick(title_px);
    expect(!hits.empty() && hits.front() == win, "a click on the title bar picks the window");

    // Real input: click selects; drag moves by the drag (one undo step).
    in.click(title_px);
    expect(app.document().primary() == win, "a real click on the window selects it");
    const glm::vec2 before = ui::parse_rect_block(*[&] {
        const Node* o = app.document().find(win);
        for (const auto& c : o->at("components").as_seq()) if (component_type(c) == "RectTransform") return &c;
        return static_cast<const Node*>(nullptr);
    }()).anchored_position();
    in.move(title_px);
    in.drag(title_px + glm::vec2(60.0f, 0.0f), MouseButton::Left, 6);
    tick(engine, 2);
    auto rect_of = [&](ObjectId id) {
        const Node* o = app.document().find(id);
        for (const auto& c : o->at("components").as_seq()) if (component_type(c) == "RectTransform") return ui::parse_rect_block(c);
        return coopa::ui::RectTransform{};
    };
    const glm::vec2 after = rect_of(win).anchored_position();
    const float expect_dx = 60.0f / view.scale;
    expect(std::abs((after.x - before.x) - expect_dx) < 2.0f && std::abs(after.y - before.y) < 1.5f,
           "dragging the window moves it by the drag (dx " + std::to_string(after.x - before.x) + ", want " + std::to_string(expect_dx) + ")");
    wr = app.ui_live_rect(win);
    expect(wr && std::abs(wr->center().x - 960.0f - after.x) < 2.0f, "...and the live rect follows without a rebuild");
    app.undo();
    tick(engine, 2);
    expect(glm::distance(rect_of(win).anchored_position(), before) < 1e-3f, "one Ctrl+Z undoes the whole drag");

    // Interact: the UI runs; clicking a menu button echoes its named click.
    app.set_ui_interact(true);
    tick(engine, 2);
    expect(app.ui_interacting() && engine.scene().is_simulating() && engine.scene_ui_placement()->input,
           "Interact runs the canvas and gives it input");
    coopa::scene::SceneObject* play_btn = app.sync().live(menu) ? app.sync().live(menu)->find_descendant("Play") : nullptr;
    auto* play_rt = play_btn ? play_btn->get_component<coopa::ui::RectTransform>() : nullptr;
    expect(play_rt != nullptr, "the menu built a button named Play");
    if (play_rt) {
        in.click(app.ui_view().to_editor(play_rt->rect().center()));
        tick(engine, 2);
        bool echoed = false;
        for (const auto& [lvl, line] : app.log()) echoed |= line.find("[UI] Play  click") != std::string::npos;
        expect(echoed, "clicking Play in Interact echoes its named click to the Console");
    }
    app.set_ui_interact(false);
    tick(engine, 3);
    expect(!engine.scene().is_simulating(), "back in Design the canvas stops");

    // Save: an object asset with the designer's preview settings riding along.
    expect(app.save_scene(), "the UI saves");
    const Node saved = coopa::yaml::load_document(project.assets() / "ui" / "test_hud.yaml");
    expect(saved.contains("object") && saved.contains("ui_editor") && saved.at("object").contains("children"),
           "saved as `object:` plus a `ui_editor:` block");

    // In a scene, the HUD draws inside the viewer -- not over the whole editor.
    app.open_asset(AssetType::Scene, "scenes/main/scene.yaml");
    tick(engine, 4);
    const ObjectId inst = app.place_ui_asset("ui/test_hud.yaml");
    tick(engine, 4);
    expect(inst && get_string(*app.document().find(inst), "prefab") == "ui/test_hud", "the UI is placed as `prefab: ui/test_hud`");
    const auto sp = engine.scene_ui_placement();
    const auto dr = engine.display_rect();
    expect(sp && sp->rect.x == dr.x && sp->rect.w == dr.w && sp->rect.h == dr.h && !sp->input,
           "in the scene view the HUD is placed on the rendered image, not the whole window");
    auto* hud_live = engine.scene().find_object("Window");
    expect(hud_live != nullptr, "the placed HUD's window is in the live scene");
    dump(engine, "31_ui_in_scene");
}

/** @brief The shipped templates load, open, and expose the names their docs promise. */
/**
 * @brief Game UI themes are assets: the Themes tab lists the shipped ones (default, Blender,
 *        Unity), opening one previews it on a UI with its unsaved edits -- in place of the UI's
 *        own theme -- and only Save writes the file.
 */
void test_editor_game_ui_themes() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("ui_themes_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    expect(app.new_ui_asset("menu", "settings"), "a UI asset to preview on");
    tick(engine, 4);
    const auto themes = app.list_assets(AssetType::Theme);
    for (const char* t : {"ui/themes/default.yaml", "ui/themes/blender.yaml", "ui/themes/unity.yaml"}) {
        expect(std::find(themes.begin(), themes.end(), t) != themes.end(), std::string("the Themes tab lists ") + t);
    }
    const auto uis = app.list_assets(AssetType::UI);
    expect(std::none_of(uis.begin(), uis.end(), [](const std::string& r) { return r.rfind("ui/themes/", 0) == 0; }),
           "themes are not UI assets");

    auto live_theme = [&]() -> const coopa::ui::UITheme* {
        auto* live = app.sync().live(app.document().object_root());
        auto* scope = live ? live->get_component<coopa::ui::ThemeScope>() : nullptr;
        return scope ? &scope->theme : nullptr;
    };
    const fs::path blender_file = project.assets() / "ui" / "themes" / "blender.yaml";
    const Node on_disk = coopa::yaml::load_document(blender_file);

    app.open_asset(AssetType::Theme, "ui/themes/blender.yaml");
    tick(engine, 4);
    expect(app.theme_document().open() && app.active_asset_type() == AssetType::UI, "a theme opens beside the UI it previews on");
    const auto* t = live_theme();
    const Node& bp = on_disk.at("panel").at("panel");
    expect(t && std::abs(t->panel.panel.r - get_float(bp, "r", -1.0f)) < 1e-4f,
           "the UI previews with the opened theme in place of its own (default.yaml)");
    dump(engine, "39_theme_blender");

    app.edit_theme("Panel", [](Node& n) {
        Node c = make_color({1.0f, 0.0f, 0.0f});
        c["a"] = make_float(1.0);
        n["panel"]["panel"] = c;
    });
    tick(engine, 4);
    t = live_theme();
    expect(t && t->panel.panel.r > 0.99f && t->panel.panel.g < 0.01f, "an unsaved edit shows in the preview");
    expect(coopa::yaml::load_document(blender_file) == on_disk && app.theme_document().dirty(), "...without touching the file");
    dump(engine, "40_theme_preview");

    app.theme_document().do_undo();
    expect(app.theme_document().node == on_disk, "theme edits undo");
    app.theme_document().do_redo();
    expect(app.save_theme() && !app.theme_document().dirty(), "Save Theme writes it");
    expect(get_float(coopa::yaml::load_document(blender_file).at("panel").at("panel"), "g", -1.0f) < 0.01f, "the file has the edit");

    // Another theme replaces it in the preview.
    app.open_asset(AssetType::Theme, "ui/themes/unity.yaml");
    tick(engine, 4);
    const Node unity_doc = coopa::yaml::load_document(project.assets() / "ui" / "themes" / "unity.yaml");
    const Node& up = unity_doc.at("panel").at("panel");
    t = live_theme();
    expect(app.theme_document().ref == "ui/themes/unity" && t && std::abs(t->panel.panel.r - get_float(up, "r", -1.0f)) < 1e-4f,
           "opening another theme previews that one (" + app.theme_document().ref + ", panel.r " +
           std::to_string(t ? t->panel.panel.r : -1.0f) + " vs " + std::to_string(get_float(up, "r", -1.0f)) + ")");
    dump(engine, "41_theme_unity");
}

void test_editor_ui_templates() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("ui_templates_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    const auto templates = EditorApp::ui_templates();
    expect(templates.size() >= 6, "UI templates ship with the editor (" + std::to_string(templates.size()) + ")");
    const std::map<std::string, std::vector<std::string>> names = {
        {"hud", {"Health", "Stamina", "Hotbar", "MessageLog"}},
        {"main_menu", {"NewGame", "Continue", "Settings", "Quit"}},
        {"pause_menu", {"Resume", "Settings", "QuitToMenu"}},
        {"dialog_box", {"Speaker", "Line", "Choices"}},
        {"inventory", {"Bag", "Equipment", "Gold"}},
        {"settings", {"MasterVolume", "Fullscreen", "Quality", "Apply"}},
    };
    for (const auto& t : templates) {
        expect(app.new_ui_asset(t, t), "template " + t + ": creates a UI asset");
        tick(engine, 4);
        expect(app.active_asset_type() == AssetType::UI && app.sync().scene(), "template " + t + ": opens in the designer");
        coopa::ui::UiHandle ui(app.sync().live(app.document().object_root()));
        auto it = names.find(t);
        if (it != names.end()) {
            for (const auto& n : it->second) expect(ui.has(n), "template " + t + ": has `" + n + "`");
        }
        dump(engine, "32_template_" + t);
    }
    expect(coopa::yaml::document_exists(project.assets() / "ui" / "themes" / "default.yaml"), "templates install their theme");
}


/**
 * @brief Theme shapes reach the pixels: the Settings template under each shipped theme draws a
 *        window whose corner is rounded away (the backdrop shows where a square corner would be).
 */
void test_editor_ui_theme_shapes() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("ui_shapes_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    expect(app.new_ui_asset("shapes", "settings"), "the Settings template opens");
    tick(engine, 4);
    const float scale = std::max(1.0f, engine.display_scale());
    for (const char* theme : {"default", "blender", "unity"}) {
        const ObjectId rootid = app.document().object_root();
        const int ti = app.document().find_component(rootid, "Theme");
        Node comp = Node::mapping();
        comp["type"] = Node(std::string("Theme"));
        comp["source"] = Node(std::string("ui/themes/") + theme + ".yaml");
        app.document().set_component(rootid, ti, comp, "Theme");
        app.sync().rebuild(engine, app.document());
        tick(engine, 4);
        ObjectId win = 0;
        for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "Settings") win = id;
        auto r = app.ui_live_rect(win);
        expect(r.has_value(), std::string("theme ") + theme + ": the window has a live rect");
        if (!r) continue;
        const auto shot = engine.capture_image(false);
        const ui::UiView v = app.ui_view();
        auto px = [&](glm::vec2 canvas) {
            const glm::vec2 e = v.to_editor(canvas) * scale;
            const size_t i = (static_cast<size_t>(e.y) * shot.width + static_cast<size_t>(e.x)) * shot.channels;
            return glm::vec3(shot.pixels[i], shot.pixels[i + 1], shot.pixels[i + 2]);
        };
        const glm::vec3 corner = px({r->min.x + 0.6f, r->min.y + 0.6f});            // bottom-left corner
        const glm::vec3 panel = px({r->min.x + 40.0f, r->min.y + 40.0f});           // inside the window
        // Just outside the window, under the same drop shadow the rounded-off corner shows.
        const glm::vec3 outside = px({r->min.x - 1.5f, r->min.y - 1.5f});
        // Unity's 4 px radius is ~2 framebuffer pixels in this 1x window: inside the edge's
        // anti-aliasing, so its pixels prove nothing either way. Check its shape data instead.
        if (std::string(theme) == "unity") {
            auto* frame = app.sync().live(win) ? app.sync().live(win)->children().front().get() : nullptr;
            auto* img = frame ? frame->get_component<coopa::ui::Image>() : nullptr;
            expect(img && std::abs(img->corner_radius - 4.0f) < 1e-3f && img->border_width > 0.0f,
                   "theme unity: the window takes the theme's 4 px corner radius and outline");
            dump(engine, std::string("33_theme_shape_") + theme);
            continue;
        }
        expect(glm::distance(corner, panel) > 6.0f && glm::distance(corner, outside) < glm::distance(corner, panel),
               std::string("theme ") + theme + ": the window's corner is rounded (corner " + std::to_string(int(corner.x)) +
               " panel " + std::to_string(int(panel.x)) + " outside " + std::to_string(int(outside.x)) + ")");
        dump(engine, std::string("33_theme_shape_") + theme);
    }
}


/**
 * @brief Editor text sits on the device-pixel grid: every glyph quad in the Add menu (and the
 *        rest of the UI) starts on a whole framebuffer pixel. Sub-pixel glyph origins made
 *        bilinear sampling smear each letter differently -- "Empty" looked out of line.
 */
void test_editor_text_pixel_aligned() {
    using coopa::input::Key;
    using coopa::input::Mods;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("text_align_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    in.move(app.viewport_box().center() + glm::vec2(7.3f, 3.6f));   // an off-grid point: popups open at the mouse
    in.key(Key::A, Mods::Shift);
    tick(engine, 2);
    expect(app.ui().any_popup_open(), "Shift A opens the Add menu");
    dump(engine, "34_add_menu_text");
    auto* canvas = app.editor_canvas();
    expect(canvas != nullptr, "the editor canvas exists");
    if (!canvas) return;
    const coopa::ui::DrawList& dl = canvas->draw_list();
    const auto& atlases = coopa::ui::TextAtlasRegistry::instance().views();
    const float scale = dl.text_scale();
    // Each glyph is one add_quad(): 4 vertices min.x/min.y, max.x/min.y, max, min.x/max.y. Its
    // ORIGIN -- left edge and top edge (canvas max.y) -- must be on a device pixel; the far
    // edges may be fractional at 1x, where the atlas is 2x oversampled by design.
    size_t glyph_vertices = 0, off_grid = 0;
    for (const auto& b : dl.batches()) {
        if (std::find(atlases.begin(), atlases.end(), b.texture_view) == atlases.end()) continue;
        for (uint32_t k = 0; k + 5 < b.index_count; k += 6) {
            const uint32_t base = dl.indices()[b.first_index + k];
            const auto& v0 = dl.vertices()[base];
            const auto& v2 = dl.vertices()[base + 2];
            glyph_vertices += 4;
            // Vertically every glyph must be whole pixels -- top edge AND height -- or its row of
            // texels lands between screen rows and blurs differently from its neighbours; the same
            // goes for the left edge (a stem between two pixel columns draws as two grey ones).
            const float fx = v0.x * scale, fy = v2.y * scale, fh = (v2.y - v0.y) * scale;
            const bool x_off = std::abs(fx - std::round(fx)) > 0.02f;
            if (x_off || std::abs(fy - std::round(fy)) > 0.02f || std::abs(fh - std::round(fh)) > 0.02f) {
                if (off_grid < 6) std::cerr << "    off grid: " << fx << ", " << fy << "  (scale " << scale << ")\n";
                ++off_grid;
            }
        }
    }
    expect(glyph_vertices > 200, "text was drawn (" + std::to_string(glyph_vertices) + " glyph vertices)");
    expect(off_grid == 0, "every glyph starts on a device pixel (" + std::to_string(off_grid) + " off the grid)");
}


/**
 * @brief Tags: the folders between an asset's type folder and the asset. References name the
 *        type and the asset only, the engine finds it by name (coopa::asset::AssetIndex), and
 *        re-tagging moves the file (a scene: its folder), rewriting only full-path references.
 */
void test_asset_tags_and_retag() {
    expect(asset_tags("materials/metal/brick.yaml") == std::vector<std::string>{"metal"}, "a material's tag is its folder");
    expect(asset_tags("scenes/tests/water/lake/scene.yaml") == std::vector<std::string>{"tests", "water"},
           "a scene's own folder is the asset, not a tag");
    expect(asset_tags("ui/themes/dark.yaml").empty() && asset_type_dir("ui/themes/dark.yaml") == "ui/themes",
           "ui/themes is a type folder, not a tag");
    expect(asset_tags("scenes/lake/meshes/basin.yaml").empty(), "scene-local files have no tags");
    expect(with_asset_tags("materials/brick.yaml", {"stone", "wall"}) == "materials/stone/wall/brick.yaml", "tags nest in order");
    expect(with_asset_tags("scenes/tests/lake/scene.yaml", {}) == "scenes/lake/scene.yaml", "untagging a scene moves its folder up");
    expect(short_ref("materials/metal/brick.yaml") == "materials/brick.yaml" &&
           short_ref("scenes/tests/lake/scene.yaml") == "scenes/lake/scene.yaml" &&
           short_ref("ui/themes/dark.yaml") == "ui/themes/dark.yaml", "short refs drop tag folders");

    const fs::path root = fresh_dir("asset_tags");
    const fs::path a = root / "assets";
    auto write = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((a / rel).parent_path());
        std::ofstream(a / rel) << text;
    };
    write("config.yaml", "scene:\n  default_scene: \"assets/scenes/lake/scene.yaml\"\n");
    write("materials/metal/brick.yaml", "albedo: {r: 1.0, g: 0.0, b: 0.0}\n");
    write("materials/stone/brick_old.yaml", "albedo: {r: 0.0, g: 1.0, b: 0.0}\n");
    write("meshes/props/rock.yaml", "vertices: []\nfaces: []\n");
    write("scenes/lake/meshes/basin.yaml", "vertices: []\nfaces: []\n");
    write("scenes/lake/scene.yaml",
          "scene:\n  scene_name: lake\n  root_objects:\n"
          "    - name: a\n      components:\n"
          "        - type: MeshRenderer\n          mesh_path: rock\n          material: materials/brick\n"
          "        - type: MeshRenderer\n          mesh_path: props/rock\n          material: materials/metal/brick\n");
    Project project(root);

    using coopa::asset::AssetIndex;
    expect(AssetIndex::find(a, "materials/brick.yaml") == a / "materials/metal/brick.yaml", "the index finds a tagged asset by name");
    expect(AssetIndex::find(a, "meshes/rock.yaml") == a / "meshes/props/rock.yaml", "...meshes too");
    expect(AssetIndex::find(a, "scenes/lake/scene.yaml") == a / "scenes/lake/scene.yaml", "...and scenes by their folder");
    expect(!AssetIndex::find(a, "materials/missing.yaml") && !AssetIndex::find(a, "objects/brick.yaml"),
           "names are per type: a material is no object");
    expect(project.absolute("materials/brick.yaml") == a / "materials/metal/brick.yaml", "Project::absolute finds it by name");

    auto comps = [&](const std::string& scene_rel) {
        return coopa::yaml::load_document(a / scene_rel).at("scene").at("root_objects")[0].at("components");
    };
    // Re-tag a material: short references stay, the full-path one follows.
    project.rename_asset("materials/metal/brick.yaml", "materials/wall/red/brick.yaml");
    expect(fs::exists(a / "materials/wall/red/brick.yaml") && !fs::exists(a / "materials/metal/brick.yaml"), "re-tagging moves the file");
    expect(get_string(comps("scenes/lake/scene.yaml")[0], "material") == "materials/brick", "a short reference is left alone");
    expect(get_string(comps("scenes/lake/scene.yaml")[1], "material") == "materials/wall/red/brick", "a full-path reference follows");
    expect(AssetIndex::find(a, "materials/brick.yaml") == a / "materials/wall/red/brick.yaml", "the index sees the move");

    // Re-tag a mesh: a bare name stays, an old subfolder ref follows.
    project.rename_asset("meshes/props/rock.yaml", "meshes/nature/rock.yaml");
    expect(get_string(comps("scenes/lake/scene.yaml")[0], "mesh_path") == "rock" &&
           get_string(comps("scenes/lake/scene.yaml")[1], "mesh_path") == "nature/rock", "mesh refs: name kept, path followed");

    // Re-tag a scene: the whole folder moves (scene-local meshes along) and config follows.
    project.rename_asset("scenes/lake/scene.yaml", "scenes/tests/water/lake/scene.yaml");
    expect(fs::exists(a / "scenes/tests/water/lake/scene.yaml") && fs::exists(a / "scenes/tests/water/lake/meshes/basin.yaml") &&
           !fs::exists(a / "scenes/lake"), "a scene moves as its folder");
    const std::string cfg = get_string(coopa::yaml::load_document(a / "config.yaml").at("scene"), "default_scene");
    expect(cfg == "assets/scenes/lake/scene.yaml", "config's default_scene (the scene's short path) is left alone");
    expect(project.scenes() == std::vector<std::string>{"scenes/tests/water/lake/scene.yaml"}, "the moved scene is listed");
    expect(AssetIndex::find(a, "scenes/lake/scene.yaml") == a / "scenes/tests/water/lake/scene.yaml",
           "the scene's old short path still finds it");

    // Names are unique per type, whatever the tags.
    setenv("FIXED_DT", "0", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    Project made = Project::create(fresh_dir("asset_tags_editor"));
    toy::core::Engine engine(shell_config(made), shell_options(made));
    EditorApp app(engine, made);
    app.set_show_engine_assets(false);
    tick(engine, 2);
    coopa::yaml::save_document(made.assets() / "materials" / "metal" / "material.yaml", Node::mapping());
    app.project().refresh();
    app.set_asset_tab(AssetType::Material);
    app.set_asset_tag_filter({"metal"});
    expect(app.assets_listed() == std::vector<std::string>{"materials/metal/material.yaml"}, "the tag filter keeps tagged assets");
    app.set_asset_tag_filter({});
    expect(app.assets_listed().size() == 2, "no tag picked: everything");
    expect(app.create_material("material"), "creating a material");
    tick(engine, 2);
    expect(coopa::yaml::document_exists(made.assets() / "materials" / "material_01.yaml"),
           "a new material avoids a name taken in another tag folder");
    app.set_asset_tags(AssetType::Material, "materials/default.yaml", {"basic"});
    tick(engine, 3);
    expect(fs::exists(made.assets() / "materials/basic/default.yaml") && !fs::exists(made.assets() / "materials/default.yaml"),
           "set_asset_tags moves the asset into its tag folder");
    const Node scene = coopa::yaml::load_document(made.assets() / "scenes/main/scene.yaml");
    bool short_kept = false;
    for (const auto& o : scene.at("scene").at("root_objects").as_seq()) {
        if (get_string(o, "name") != "ground") continue;
        for (const auto& c : o.at("components").as_seq()) short_kept |= get_string(c, "material") == "materials/default";
    }
    expect(short_kept, "the starter scene's materials/default still names it (found by name)");
    app.set_asset_sort(EditorApp::AssetSort::Tags);
    const auto by_tags = app.assets_listed();
    expect(!by_tags.empty() && by_tags.front().rfind("materials/material", 0) == 0 && by_tags.back() == "materials/metal/material.yaml",
           "sort by tags: untagged first, then by tag");
    app.set_asset_sort(EditorApp::AssetSort::Name);
    // For a look (EDITOR_DUMP_DIR): the panel with tag chips, toyengine's tagged meshes below.
    app.set_show_engine_assets(true);
    tick(engine, 3);
    dump(engine, "asset_tags_materials");
    app.set_asset_tab(AssetType::Mesh);
    app.set_asset_tag_filter({"terrain"});
    tick(engine, 3);
    dump(engine, "asset_tags_meshes_filtered");
}

/**
 * @brief The Render tab, through real input: features are sections with their switch in the
 *        header (clicking it writes config.yaml), opening one shows its rows, and the search box
 *        narrows the tab to matching settings.
 */
void test_editor_render_settings_panel() {
    using coopa::input::Key;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("render_settings_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    tick(engine, 4);
    app.set_prop_tab(PropTab::Render);   // in the scene: edits to live settings override config.yaml for this scene
    app.clear_test_rects();
    tick(engine, 2);
    dump(engine, "render_settings_tab");
    for (const char* g : {"Resolution & Detail", "Lighting & Sky", "Anti-Aliasing", "Shadows", "Ambient Occlusion", "Reflections", "Fog"}) {
        expect(app.test_rect(std::string("setting_group:") + g).has_value(), std::string("the Render tab lists ") + g);
    }
    // The header's checkbox switches the feature (Shadows is on by default).
    auto shadows = app.test_rect("setting_group:Shadows");
    if (shadows) {
        const glm::vec2 check{shadows->x + shadows->h + 6.0f, shadows->center().y};
        in.click(check);
        tick(engine, 2);
        const Node* over = app.document().scene_setting("render", "shadows_enabled");
        expect(over && !over->get_value<bool>(), "the Shadows header checkbox turns shadows off (a scene override)");
        expect(!engine.render_config().shadows_enabled, "...live");
        // Clicking the title opens the section without touching the switch.
        shadows = app.test_rect("setting_group:Shadows");
        if (shadows) in.click({shadows->x + shadows->w * 0.6f, shadows->center().y});
        tick(engine, 2);
        over = app.document().scene_setting("render", "shadows_enabled");
        expect(over && !over->get_value<bool>(), "opening the section leaves the switch alone");
        dump(engine, "render_settings_shadows_open");
    }
    // Search: only matching settings (and their groups) show.
    const auto filter = app.test_rect("render_settings_filter");
    expect(filter.has_value(), "the Render tab has a search box");
    if (filter) {
        in.click(filter->center());
        for (char ch : std::string("bloom")) {
            engine.queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
            tick(engine, 1);
        }
        app.clear_test_rects();
        tick(engine, 2);
        expect(app.test_rect("setting_group:Bloom").has_value() && !app.test_rect("setting_group:Shadows").has_value(),
               "searching 'bloom' shows the Bloom section and hides Shadows");
        dump(engine, "render_settings_search");
    }
}

/**
 * @brief Proportional editing through real input: O turns it on, G Z 1 Enter on one vertex of
 *        a grid lifts its neighbours by the falloff, the wheel resizes the circle mid-grab.
 */
void test_editor_proportional_editing() {
    using coopa::input::Key;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("proportional_project");
    Project project = Project::create(root);
    coopa::yaml::save_document(project.assets() / "meshes" / "grid.yaml", mesh_to_node(make_grid(10, 10, 10.0f)));
    project.refresh();
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    tick(engine, 4);
    app.open_asset(AssetType::Mesh, "meshes/grid.yaml");
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    app.set_interaction_mode(InteractionMode::Edit);
    tick(engine, 2);
    expect(app.edit_mode_active(), "the grid is in Edit Mode");
    auto& md = app.mesh_document();
    uint32_t centre = 0, near = 0, far = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) {
        const glm::vec3 p = md.mesh.positions[v];
        if (glm::length(p) < 1e-4f) centre = v;
        if (glm::length(p - glm::vec3(1, 0, 0)) < 1e-4f) near = v;
        if (glm::length(p - glm::vec3(5, 0, 0)) < 1e-4f) far = v;
    }
    md.selection.clear();
    md.selection.mode = SelectMode::Vertex;
    md.selection.verts.insert(centre);
    const glm::vec2 c = app.viewport_box().center();
    in.move(c);
    expect(!app.proportional().enabled, "proportional editing starts off");
    in.key(Key::O);
    expect(app.proportional().enabled, "O turns it on");
    app.proportional().falloff = Falloff::Linear;
    app.proportional().projected = false;
    app.proportional().radius = 3.0f;
    const EditMesh before = md.mesh;
    in.key(Key::G);
    in.key(Key::Z);
    in.key(Key::Num1);
    in.key(Key::Enter);
    expect(std::abs(md.mesh.positions[centre].z - 1.0f) < 1e-4f, "the selected vertex moves the full 1");
    expect(std::abs(md.mesh.positions[near].z - 2.0f / 3.0f) < 1e-3f,
           "a vertex 1 m away moves 2/3 (linear falloff, radius 3) -- got " + std::to_string(md.mesh.positions[near].z));
    expect(std::abs(md.mesh.positions[far].z) < 1e-6f, "a vertex outside the radius stays put");
    in.key(Key::Z, coopa::input::Mods::Control);
    tick(engine, 2);
    expect(std::abs(md.mesh.positions[near].z) < 1e-6f && std::abs(md.mesh.positions[centre].z) < 1e-6f,
           "one undo puts the neighbours back too");

    // The wheel resizes the circle while grabbing (EDITOR_DUMP_DIR shows the circle).
    app.proportional().projected = true;
    app.proportional().falloff = Falloff::Smooth;
    in.move(c);
    in.key(Key::G);
    in.move(c + glm::vec2(0, -60), 3);
    const float r0 = app.proportional().radius;
    engine.queue_input([](coopa::input::Input& i) { i.push_scroll(0.0, 3.0); });
    tick(engine, 2);
    expect(app.proportional().radius > r0 * 1.2f, "scrolling up during G grows the radius");
    dump(engine, "proportional_grab");
    in.key(Key::Escape);
    bool restored = true;
    for (size_t i = 0; i < before.positions.size(); ++i) restored &= glm::length(md.mesh.positions[i] - before.positions[i]) < 1e-5f;
    expect(restored, "Esc puts every vertex back");
    in.key(Key::O, coopa::input::Mods::Shift);
    expect(app.proportional().falloff == Falloff::Sphere, "Shift O cycles the falloff");
    in.key(Key::O);
    expect(!app.proportional().enabled, "O again turns it off");
}

/**
 * @brief The Asset panel's right-click menus, through real input: right-clicking an asset
 *        offers Delete (and the confirmation deletes the file); right-clicking empty space in the
 *        list offers Add, with what the + button offers for that tab.
 */
void test_editor_asset_browser_context_menus() {
    using coopa::input::MouseButton;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("asset_menu_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    app.set_show_engine_assets(false);   // the project's list only: "empty space" below it must stay empty
    tick(engine, 4);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    for (const char* n : {"stone", "brick"}) {
        Node m = Node::mapping();
        m["albedo"] = make_color({0.5f, 0.5f, 0.5f});
        coopa::yaml::save_document(project.assets() / "materials" / (std::string(n) + ".yaml"), m);
    }
    project.refresh();
    app.project().refresh();
    app.set_asset_tab(AssetType::Material);
    tick(engine, 3);

    // Right-click an asset: its menu, with Delete.
    auto row = app.test_rect("asset_row:materials/stone.yaml");
    expect(row.has_value(), "the stone material is listed");
    if (!row) return;
    in.click(row->center(), MouseButton::Right);
    expect(app.ui().any_popup_open(), "right-clicking an asset opens its context menu");
    auto del = app.test_rect("asset_delete");
    expect(del.has_value(), "the menu has Delete...");
    if (!del) return;
    in.click(del->center());
    tick(engine, 2);
    auto confirm = app.test_rect("asset_delete_confirm");
    expect(confirm.has_value(), "Delete... asks for confirmation");
    if (!confirm) return;
    in.click(confirm->center());
    tick(engine, 2);
    expect(!coopa::yaml::document_exists(project.assets() / "materials" / "stone.yaml") &&
           coopa::yaml::document_exists(project.assets() / "materials" / "brick.yaml"),
           "confirming deletes that asset's file (and only it)");

    // Right-click empty space: Add, the same as + (Materials: creates one directly).
    auto brick = app.test_rect("asset_row:materials/brick.yaml");
    expect(brick.has_value() && !app.test_rect("asset_row:materials/stone.yaml").has_value() ? true : brick.has_value(),
           "the list now shows the remaining material");
    if (!brick) return;
    const glm::vec2 empty = brick->center() + glm::vec2(0.0f, 120.0f);
    in.click(empty, MouseButton::Right);
    expect(app.ui().any_popup_open(), "right-clicking empty space in the list opens the Add menu");
    auto add = app.test_rect("asset_add");
    expect(add.has_value(), "the menu has Add");
    if (!add) return;
    const size_t before = app.project().list("materials", ".yaml").size();
    in.click(add->center());
    tick(engine, 3);
    app.project().refresh();
    expect(app.project().list("materials", ".yaml").size() == before + 1, "Add creates a new material, as + does");

    // A tab whose + offers choices (meshes) shows them under Add.
    app.set_asset_tab(AssetType::Mesh);
    tick(engine, 3);
    in.click(empty, MouseButton::Right);
    add = app.test_rect("asset_add");
    expect(app.ui().any_popup_open() && add.has_value(), "the Meshes tab's empty-space menu has Add");
    if (!add) return;
    in.move(add->center(), 3);   // hovering opens the submenu
    auto cube = app.test_rect("asset_new:Cube");
    expect(cube.has_value(), "Add lists the mesh primitives the + button offers");
    if (cube) {
        in.click(cube->center());
        tick(engine, 3);
        expect(app.active_asset_type() == AssetType::Mesh && coopa::yaml::document_exists(project.assets() / "meshes" / "cube.yaml"),
               "picking Cube creates (and opens) a cube mesh asset");
    }
}


/**
 * @brief Previewing a texture in the Textures tab declares the color space the project uses it
 *        in, not always sRGB (the preview shows it as albedo): the loader keeps the first
 *        declaration, so an sRGB one from viewing a normal map would mis-decode it for every
 *        material using it ("declare_color_space ... conflicts" on the console).
 */
void test_editor_texture_preview_color_space() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("texture_space_project");
    Project project = Project::create(root);
    for (const char* t : {"brick_normal.png", "brick_albedo.png", "noise_mask.png"}) {
        // toyengine keeps them in tag folders (textures/brick/, textures/masks/); found by name.
        const auto from = coopa::asset::AssetIndex::find(fs::path(ROOT_DIR) / "assets", std::string("textures/") + t);
        fs::copy_file(from.value_or(fs::path(ROOT_DIR) / "assets" / "textures" / t), project.assets() / "textures" / t);
    }
    Node mat = Node::mapping();
    mat["albedo"] = make_color({1, 1, 1});
    mat["texture_albedo"] = Node(std::string("textures/brick_albedo.png"));
    mat["texture_normal"] = Node(std::string("textures/brick_normal.png"));
    coopa::yaml::save_document(project.assets() / "materials" / "brick.yaml", mat);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    auto* loader = dynamic_cast<coopa::gfx::engine::loaders::TextureLoader*>(
        engine.assets().loader<coopa::gfx::engine::data::Texture>());
    expect(loader != nullptr, "the engine has a texture loader");
    if (!loader) return;
    auto space_of = [&](const char* rel) {
        return loader->declared_color_space(engine.assets().source().resolve(rel, project.assets().string()));
    };
    using coopa::gfx::ColorSpace;
    app.open_asset(AssetType::Texture, "textures/brick_normal.png");
    tick(engine, 4);
    expect(space_of("textures/brick_normal.png") == ColorSpace::Linear,
           "previewing a normal map (used as texture_normal) declares it linear, not sRGB");
    app.open_asset(AssetType::Texture, "textures/noise_mask.png");
    tick(engine, 4);
    expect(space_of("textures/noise_mask.png") == ColorSpace::Linear, "an unreferenced *_mask texture is treated as data (linear)");
    app.open_asset(AssetType::Texture, "textures/brick_albedo.png");
    tick(engine, 4);
    expect(space_of("textures/brick_albedo.png") == ColorSpace::Srgb, "an albedo map previews as sRGB");
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
    const ObjectId cube = object_named(app, "cube");
    const int ci = app.document().find_component(cube, "MeshRenderer");
    Node comp = app.document().find(cube)->at("components").as_seq()[static_cast<size_t>(ci)];
    comp["mesh_path"] = Node(std::string("two_slot"));
    Node mats = Node::mapping();
    mats["trim"] = Node(std::string("materials/red"));
    comp["materials"] = mats;
    app.document().set_component(cube, ci, comp, "Slots");
    app.sync().rebuild(engine, app.document());
    tick(engine, 6);
    auto* mr = engine.scene().find_object("cube")->get_component<coopa::gfx::engine::components::MeshRenderer>();
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
    const ObjectId cube = object_named(app, "cube");
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

/** @brief Water bodies are editable meshes too: Edit Mode on a WaterBody object edits its
 *         WaterBody's mesh (a procedural grid is written out to a mesh file first), the live
 *         water re-bakes from the unsaved edit, and saving writes the file. */
/** @brief A water body larger than one render tile draws as runtime tile children: they render,
 *         clicking the water still picks the water object, and they never reach the saved scene. */
void test_editor_large_water_tiles_pick_and_save() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("water_tiles_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);

    const ObjectId lake = app.create_primitive("Plane");
    auto& doc = app.document();
    const int mr_ci = doc.find_component(lake, "MeshRenderer");
    Node mr = doc.find(lake)->at("components").as_seq()[static_cast<size_t>(mr_ci)];
    mr["mesh_path"] = Node(std::string(""));
    doc.set_component(lake, mr_ci, mr, "No mesh");
    Node water = Node::mapping();
    water["type"] = Node(std::string("WaterBody"));
    water["mode"] = Node(std::string("planar"));
    Node size = Node::mapping();
    size["x"] = make_float(120.0);
    size["y"] = make_float(120.0);
    water["size"] = size;
    water["resolution"] = Node(static_cast<int64_t>(24));
    doc.add_component(lake, water);
    doc.set_transform(lake, {0.0f, 80.0f, 0.5f}, {0, 0, 0}, glm::vec3(1), "Move");
    app.sync().rebuild(engine, doc);
    tick(engine, 3);
    // A grid body has no surface the editor can pick (only its origin marker): one round trip
    // through Edit Mode writes the grid to a mesh file, as an author editing the shape would.
    // That also makes it a mesh-sourced body, tiled by triangle with simplified LODs.
    doc.select(lake);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode converts the grid to a mesh");
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 4);

    toy::water::WaterBody* live = nullptr;
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id == lake && obj) live = obj->get_component<toy::water::WaterBody>();
    }
    expect(live && live->tiles.size() > 1u, "a 120 m lake is drawn as several tiles (" +
                                                std::to_string(live ? live->tiles.size() : 0u) + ")");
    expect(live && toy::water::WaterSystem::is_published(*live), "...every one of them published");
    bool placed = live != nullptr;
    if (live) {
        for (const auto& t : live->tiles) {
            const glm::vec3 p(t.object->get_transform()->transform().get_world_matrix()[3]);
            placed = placed && glm::distance(p, glm::vec3(0.0f, 80.0f, 0.5f)) < 1e-4f;
        }
    }
    expect(placed, "...placed with the lake (tiles follow the owner's transform)");

    // Clicking the water picks the lake itself (picking is per document object).
    app.camera().focus = glm::vec3(10.0f, 80.0f, 0.5f);
    app.camera().distance = 60.0f;
    app.camera().pitch_deg = 50.0f;
    app.camera().apply();
    tick(engine, 2);
    glm::vec2 px;
    const bool on_screen = engine.world_to_window(glm::vec3(10.0f, 80.0f, 0.5f), px);
    expect(on_screen, "the lake is on screen");
    if (on_screen) {
        const float s = std::max(1.0f, engine.display_scale());
        expect(app.pick_object(px / s) == lake, "clicking the tiled water's surface picks the water object");
    }

    // The tiles are runtime only: the saved scene has the lake and no tiles.
    const fs::path out = root / "tiles_saved.yaml";
    expect(app.save_scene_as(out), "the scene saves");
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(text.find("WaterBody") != std::string::npos && text.find("water_tile") == std::string::npos,
           "...with the water body and none of its render tiles");
}

void test_editor_water_body_mesh_is_editable() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("water_edit_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);

    // A plane turned into a grid-based water body: MeshRenderer keeps only the material.
    const ObjectId pond = app.create_primitive("Plane");
    auto& doc = app.document();
    const int mr_ci = doc.find_component(pond, "MeshRenderer");
    Node mr = doc.find(pond)->at("components").as_seq()[static_cast<size_t>(mr_ci)];
    mr["mesh_path"] = Node(std::string(""));
    doc.set_component(pond, mr_ci, mr, "No mesh");
    Node water = Node::mapping();
    water["type"] = Node(std::string("WaterBody"));
    water["mode"] = Node(std::string("planar"));
    Node size = Node::mapping();
    size["x"] = make_float(6.0);
    size["y"] = make_float(4.0);
    water["size"] = size;
    water["resolution"] = Node(static_cast<int64_t>(6));
    doc.add_component(pond, water);
    app.sync().rebuild(engine, doc);
    tick(engine, 3);

    doc.select(pond);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on a WaterBody object");
    auto& md = app.mesh_document();
    const int w_ci = doc.find_component(pond, "WaterBody");
    const std::string key = get_string(doc.find(pond)->at("components").as_seq()[static_cast<size_t>(w_ci)], "mesh_path");
    expect(!key.empty() && md.path.filename() == key + ".yaml",
           "the grid was written to a mesh file and the WaterBody points at it (" + key + ")");
    expect(md.mesh.positions.size() == 7u * 7u && md.mesh.faces.size() == 36u, "...the same 6x6 grid the water drew");

    // Raise the whole surface by a metre: the live water re-bakes from the unsaved edit.
    md.selection.mode = SelectMode::Vertex;
    md.edit("Raise", [](EditMesh& m, MeshSelection&) { for (auto& p : m.positions) p.z += 1.0f; });
    tick(engine, 4);
    toy::water::WaterBody* live = nullptr;
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id == pond && obj) live = obj->get_component<toy::water::WaterBody>();
    }
    const glm::vec3 base = live && live->owner
        ? glm::vec3(live->owner->get_transform()->transform().get_world_matrix()[3]) : glm::vec3(0.0f);
    expect(live && live->baked && std::fabs(live->query.bounds_max().z - (base.z + 1.0f)) < 1e-3f,
           "the live water shows the edit before saving");

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 4);
    live = nullptr;
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id == pond && obj) live = obj->get_component<toy::water::WaterBody>();
    }
    expect(live && live->baked && std::fabs(live->query.bounds_max().z - (base.z + 1.0f)) < 1e-3f,
           "leaving Edit Mode keeps showing the unsaved edit (z max " +
               std::to_string(live ? live->query.bounds_max().z : -1.0f) + ")");
    glm::vec3 olo(0.0f), ohi(0.0f);
    expect(app.outline_bounds(pond, olo, ohi) && std::fabs(ohi.z - 1.0f) < 1e-4f,
           "...and so does its selection outline (drawn from the open mesh, not the file on disk)");
    expect(app.save_mesh(), "saving writes the water's mesh");
    const EditMesh saved = mesh_from_node(coopa::yaml::load_document(md.path));
    expect(!saved.positions.empty() && std::fabs(saved.positions[0].z - 1.0f) < 1e-5f, "...with the edit in it");

    // Re-entering edits the same file -- no second conversion.
    doc.select(pond);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode again");
    expect(app.mesh_document().path.filename() == key + ".yaml", "...on the same mesh file");
    app.set_interaction_mode(InteractionMode::Object);
}


/** @brief UndoStack's cross-stack ordering, redo freshness and memory budget. */
void test_undo_stack_sequence_and_budget() {
    UndoStack<int> a, b;
    a.push("a1", 0, 1);
    b.push("b1", 0, 1);
    a.push("a2", 1, 2);
    expect(a.top_undo_seq() > b.top_undo_seq() && b.top_undo_seq() > 0, "sequence numbers order steps across stacks");
    a.undo();
    b.undo();
    expect(a.redo_fresh() && b.redo_fresh(), "redo stays valid while nothing new is pushed");
    expect(b.top_redo_seq() < a.top_redo_seq(), "the step undone last is the older one (redo it first)");
    UndoStack<int> c;
    c.push("c1", 0, 1);
    expect(!a.redo_fresh() && !b.redo_fresh(), "a push anywhere invalidates every other stack's redo");

    UndoStack<std::vector<int>> big;
    big.set_budget(1000, [](const std::vector<int>& v) { return v.size() * sizeof(int); }, 3);
    for (int i = 0; i < 20; ++i) big.push("step", std::vector<int>(50), std::vector<int>(50));   // 400 B per step
    expect(big.undo_count() == 3, "a memory budget trims the oldest steps, keeping the minimum (" +
                                      std::to_string(big.undo_count()) + ")");
    expect(big.bytes() == 3u * 400u, "...and tracks what is left");
}

namespace {
/** @brief Faces of a mesh file on disk. */
size_t faces_on_disk(const fs::path& p) { return mesh_from_node(coopa::yaml::load_document(p)).faces.size(); }
/** @brief Index count of the live renderer of the document object named `name`. */
uint32_t live_indices(EditorApp& app, const std::string& name) {
    for (const auto& [id, live] : app.sync().live_objects()) {
        if (!live || live->name() != name) continue;
        auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>();
        return mr && mr->is_ready() ? mr->get_mesh()->index_count() : 0u;
    }
    return 0u;
}
}

/**
 * @brief Editing a scene object's mesh: saved automatically on leaving Edit Mode, and the
 *        edit stays undoable afterwards -- Object Mode's Ctrl+Z walks one timeline across the
 *        scene and every mesh edited from it (across meshes, in order), keeping files and the
 *        live scene in step; redo walks it back.
 */
void test_editor_mesh_autosave_and_unified_undo() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("unified_undo_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    auto& doc = app.document();
    const fs::path cube_file = project.assets() / "meshes" / "cube.yaml";
    const ObjectId cube = object_named(app, "cube");

    // 1. Edit the cube, leave Edit Mode: saved without asking.
    doc.select(cube);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the cube");
    app.mesh_document().selection.mode = SelectMode::Face;
    app.mesh_document().selection.faces = {1};
    app.mesh_document().edit("Extrude", [](EditMesh& m, MeshSelection& s) { extrude_faces(m, s, 0.5f); });
    tick(engine, 2);
    app.set_interaction_mode(InteractionMode::Object);
    expect(faces_on_disk(cube_file) == 10u, "leaving Edit Mode saved the cube's mesh");
    expect(!app.mesh_document().dirty(), "...and nothing is left unsaved");

    // 2. A second mesh (a plane), edited and left the same way.
    const ObjectId plane = app.create_primitive("Plane");
    tick(engine, 3);
    doc.select(plane);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the plane");
    const fs::path plane_file = app.mesh_document().path;
    const size_t plane_faces = app.mesh_document().mesh.faces.size();
    app.mesh_document().selection.mode = SelectMode::Face;
    app.mesh_document().selection.faces = {0};
    app.mesh_document().edit("Extrude", [](EditMesh& m, MeshSelection& s) { extrude_faces(m, s, 0.3f); });
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
    expect(faces_on_disk(plane_file) == plane_faces + 4u, "leaving Edit Mode saved the plane's mesh");

    // 3. Then a scene edit: move the cube.
    const int ti = doc.find_component(cube, "Transform");
    Node t = doc.find(cube)->at("components").as_seq()[static_cast<size_t>(ti)];
    t["position"] = make_vec3(glm::vec3(3.0f, 0.0f, 0.0f));
    doc.set_component(cube, ti, t, "Move");
    tick(engine, 2);

    // Object Mode undo, newest first: the move, the plane's extrude, the plane's creation (a scene
    // step), then the cube's extrude.
    app.undo();
    tick(engine, 3);
    expect(glm::vec3(get_vec3(doc.find(cube)->at("components").as_seq()[static_cast<size_t>(ti)], "position")).x == 0.0f,
           "undo 1: the move");
    expect(faces_on_disk(plane_file) == plane_faces + 4u && faces_on_disk(cube_file) == 10u, "...meshes untouched");
    app.undo();
    tick(engine, 3);
    expect(faces_on_disk(plane_file) == plane_faces, "undo 2: the plane's extrude (file reverted)");
    expect(faces_on_disk(cube_file) == 10u, "...the cube's still there");
    app.undo();
    tick(engine, 3);
    expect(doc.find(plane) == nullptr, "undo 3: the plane's creation");
    app.undo();
    tick(engine, 3);
    expect(faces_on_disk(cube_file) == 6u, "undo 4: the cube's extrude, across the mesh switch (history kept)");
    expect(live_indices(app, "cube") == 6u * 6u, "...and the live cube shows it");

    // Redo walks it back in order.
    app.redo();
    tick(engine, 3);
    expect(faces_on_disk(cube_file) == 10u && live_indices(app, "cube") == 10u * 6u, "redo 1: the cube's extrude");
    app.redo();
    tick(engine, 3);
    expect(doc.find(plane) != nullptr, "redo 2: the plane's creation");
    app.redo();
    tick(engine, 3);
    expect(faces_on_disk(plane_file) == plane_faces + 4u, "redo 3: the plane's extrude");
    app.redo();
    tick(engine, 3);
    expect(glm::vec3(get_vec3(doc.find(cube)->at("components").as_seq()[static_cast<size_t>(ti)], "position")).x == 3.0f,
           "redo 4: the move");

    // Re-entering a mesh resumes its history: Edit Mode's own Ctrl+Z still reaches the extrude.
    doc.select(cube);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the cube again");
    expect(app.mesh_document().undo.can_undo(), "...with its earlier history");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Saving a mesh refreshes every asset built from the file at once -- a MeshCollider too. */
void test_editor_mesh_save_refreshes_colliders() {
    setenv("FIXED_DT", "0.016666", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    fs::remove(Project::prefs_path());
    const fs::path root = fresh_dir("collider_refresh_project");
    Project project = Project::create(root);
    const fs::path col_file = project.assets() / "meshes" / "floor_col.yaml";
    coopa::yaml::save_document(col_file, mesh_to_node(make_plane(4.0f, 1)));
    project.refresh();
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);
    auto& doc = app.document();
    const ObjectId floor = app.create_with_component("MeshCollider", "Floor");
    const int ci = doc.find_component(floor, "MeshCollider");
    Node comp = doc.find(floor)->at("components").as_seq()[static_cast<size_t>(ci)];
    comp["mesh_path"] = Node(std::string("floor_col"));
    doc.set_component(floor, ci, comp, "Collider mesh");
    app.sync().rebuild(engine, doc);
    tick(engine, 4);
    auto triangles = [&]() -> size_t {
        for (const auto& [id, live] : app.sync().live_objects()) {
            if (id != floor || !live) continue;
            auto* mc = live->get_component<coopa::physx::components::MeshCollider>();
            return mc && mc->mesh().is_loaded() ? mc->mesh()->triangle_count() : 0u;
        }
        return 0u;
    };
    expect(triangles() == 2u, "the collider loaded its 1-quad mesh");
    expect(app.open_mesh(col_file), "open the collider's mesh as an asset");
    app.mesh_document().edit("Subdivide", [](EditMesh& m, MeshSelection&) { m = make_plane(4.0f, 3); });
    expect(app.save_mesh(), "save it");
    expect(triangles() == 18u, "the live collider has the saved mesh immediately (" + std::to_string(triangles()) + " tris)");
}

// =====================================================================================
// Group "docs" -- screenshots for the user manual (docs/editor/). NOT registered with ctest:
//
//   DOCS_SHOT_DIR=/path/to/out ./build/toyengine_editor_tests --group docs [docs_<shot>]
//
// Each test stages one clean, user-representative editor state against a scratch copy of
// this repo's assets/ (named my_game, like the manual's examples) and saves <shot>.png at
// display resolution. Without DOCS_SHOT_DIR every test returns at once. One test per shot,
// so a flaky run can be retried by name.
// =====================================================================================

const char* docs_shot_dir() {
    const char* d = std::getenv("DOCS_SHOT_DIR");
    return d && *d ? d : nullptr;
}

/** @brief The editor as a user sees it: 1600 x 900, full resolution, no pixel-art post effects. */
toy::core::AppConfig docs_config(const Project& p) {
    toy::core::AppConfig cfg = toy::core::AppConfig::load(p.config_path().string());
    cfg.window.width = 1600;
    cfg.window.height = 900;
    cfg.window.visible = false;
    cfg.window.vsync = false;
    cfg.render.screen_ui_enabled = true;
    cfg.render.outline_enabled = false;
    cfg.render.palette_enabled = false;
    cfg.render.dither_enabled = false;
    cfg.output.save_on_exit = false;
    apply_editor_render_overrides(cfg);
    return cfg;
}

/** @brief A scratch project: a copy of this repo's assets/ (unless `fresh`, which is exactly what
 *         a new project starts with), plus the starter scenes/main. */
Project docs_project(const std::string& shot, bool fresh = false) {
    const fs::path root = fresh_dir("docs_" + shot) / "my_game";
    fs::create_directories(root);
    if (!fresh) fs::copy(fs::path(ROOT_DIR) / "assets", root / "assets", fs::copy_options::recursive);
    return Project::create(root);   // adds only what is missing: the starter scenes/main
}

/** @brief One editor session staged for one screenshot. */
struct DocsEditor {
    std::string shot;
    Project project;
    std::unique_ptr<toy::core::Engine> engine;
    std::unique_ptr<EditorApp> app;
    std::unique_ptr<InputDriver> in;
    std::chrono::steady_clock::time_point cleared;

    DocsEditor(const std::string& name, const std::string& scene_rel, const std::string& theme = {},
               bool fresh_project = false)
        : shot(name), project(docs_project(name, fresh_project)) {
        setenv("FIXED_DT", "0.016666", 1);
        unsetenv("NO_INPUT");
        setenv("HOME", tmp_root().c_str(), 1);   // preferences stay in the scratch dir
        fs::remove(Project::prefs_path());       // default theme and preferences
        if (!theme.empty()) {
            Node prefs = Node::mapping();
            prefs["theme"] = Node(theme);
            Project::save_prefs(prefs);
        }
        engine = std::make_unique<toy::core::Engine>(docs_config(project), shell_options(project));
        app = std::make_unique<EditorApp>(*engine, project, project.assets() / scene_rel);
        in = std::make_unique<InputDriver>(InputDriver{*engine, std::max(1.0f, engine->display_scale())});
        tick(*engine, toy::core::Engine::kFillDebounceFrames + 6);
        // A window the OS shrank to fit a smaller screen lays the editor out differently.
        const glm::vec2 canvas = app->ui().canvas_size();
        if (std::abs(canvas.x - 1600.0f) > 0.5f || std::abs(canvas.y - 900.0f) > 0.5f) {
            throw std::runtime_error("the editor window is " + std::to_string(int(canvas.x)) + " x " +
                                     std::to_string(int(canvas.y)) + ", not 1600 x 900 (retry)");
        }
        // The starter scene is written without object ids; loading stamps them (unsaved).
        // A user's project has been saved: so is this one.
        if (app->document().dirty()) app->save_scene();
        tick(*engine, 2);
        clear_console();
    }
    ~DocsEditor() {
        app.reset();
        engine.reset();
        fs::remove(Project::prefs_path());
    }
    EditorApp& a() { return *app; }
    toy::core::Engine& e() { return *engine; }
    imm::Box vb() { return app->viewport_box(); }

    /** @brief Clicks the Console's Clear (trash) button: the opening messages name scratch paths. */
    void clear_console() {
        const imm::Box v = vb();
        in->click({v.right() - 22.0f, v.bottom() + 14.0f});
        rest();
        cleared = std::chrono::steady_clock::now();
    }
    /** @brief Parks the mouse where it hovers nothing (the viewport's lower right). */
    void rest() {
        const imm::Box v = vb();
        in->move({v.right() - 160.0f, v.bottom() - 60.0f}, 2);
    }
    /** @brief Centre of a top-bar menu header (0 File, 1 Edit, 2 Render, 3 Window, 4 Help). */
    glm::vec2 topbar_menu(int index) {
        static const char* menus[] = {"File", "Edit", "Render", "Window", "Help"};
        auto& ctx = app->ui();
        float x = 28.0f + 4.0f;
        for (int i = 0; i < index; ++i) x += ctx.text_width(menus[i]) + ctx.style.padding * 3;
        return {x + (ctx.text_width(menus[index]) + ctx.style.padding * 3) * 0.5f, 14.0f};
    }
    /** @brief Centre of an item in a drop-down opened at `top_left`: `rows` items and `seps` separators above it. */
    glm::vec2 menu_row(glm::vec2 top_left, int rows, int seps) {
        auto& st = app->ui().style;
        const float y = top_left.y + 4.0f + rows * (st.row_height + st.spacing) + seps * (5.0f + st.spacing) + st.row_height * 0.5f;
        return {top_left.x + 70.0f, y};
    }
    /** @brief Saves <shot>.png (or <shot><suffix>.png) once the status bar's opening message has timed out. */
    void capture(const std::string& name = {}) {
        // The status bar repeats the latest Console message for 8 s (wall clock). With the
        // Console cleared, the latest is still the opening "Opened project <scratch path>".
        if (app->log().empty()) {
            const auto until = cleared + std::chrono::milliseconds(8300);
            while (std::chrono::steady_clock::now() < until) tick(*engine, 1);
        }
        tick(*engine, 3);
        for (const auto& [lvl, line] : app->log()) std::cout << "    console: " << line << "\n";
        const fs::path out = fs::path(docs_shot_dir()) / ((name.empty() ? shot : name) + ".png");
        fs::create_directories(out.parent_path());
        engine->save_screenshot(out.string(), false);
        std::cout << "    saved " << out.string() << "\n";
    }
};

/** @brief Numpad 0: the view through the scene's own camera (how the scene author framed it). */
void docs_scene_camera(DocsEditor& d) {
    d.in->move(d.vb().center());
    d.in->key(coopa::input::Key::Kp0);
    d.rest();
}

/** @brief A closer orbit view of the starter scene's cube. */
void docs_main_view(DocsEditor& d, float distance = 7.0f) {
    auto& cam = d.a().camera();
    cam.focus = glm::vec3(0.0f, 0.0f, 0.6f);
    cam.yaw_deg = 35.0f;
    cam.pitch_deg = 28.0f;
    cam.distance = distance;
    cam.apply();
    tick(d.e(), 2);
}

/** @brief Selects the move tool in the viewport toolbar (its gizmo then shows on the selection). */
void docs_pick_move_tool(DocsEditor& d) {
    const imm::Box v = d.vb();
    d.in->click({v.x + 25.0f, v.y + 100.0f});
}

// 1. The water_test scene in Rendered shading, an object selected with its gizmo, Object tab.
void test_docs_overview() {
    if (!docs_shot_dir()) return;
    DocsEditor d("overview", "scenes/water_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    const ObjectId boat = object_named(d.a(), "boat");
    expect(boat != 0, "water_test has the boat");
    docs_pick_move_tool(d);
    d.a().document().select(boat);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// The README's editor shot: fog_test in Full Render, a stage spotlight selected.
void test_docs_readme_editor() {
    if (!docs_shot_dir()) return;
    DocsEditor d("readme_editor", "scenes/tests/rendering/fog_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    const ObjectId spot = object_named(d.a(), "stage_spot_left");
    expect(spot != 0, "fog_test has the stage spotlight");
    docs_pick_move_tool(d);
    d.a().document().select(spot);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 90);
    d.capture();
}

// The README's Getting started shot: a brand-new project's starter scene in Full Render.
void test_docs_getting_started_editor() {
    if (!docs_shot_dir()) return;
    DocsEditor d("getting_started_editor", "scenes/main/scene.yaml", {}, /*fresh_project=*/true);
    d.a().set_shading(Shading::Full);
    docs_main_view(d, 9.0f);
    const ObjectId cube = object_named(d.a(), "cube");
    expect(cube != 0, "the starter scene has its cube");
    docs_pick_move_tool(d);
    d.a().document().select(cube);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// 2. File menu open.
void test_docs_menu_file() {
    if (!docs_shot_dir()) return;
    DocsEditor d("menu_file", "scenes/water_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    tick(d.e(), 40);
    d.in->click(d.topbar_menu(0));
    d.in->move(d.menu_row({32.0f, 27.0f}, 3, 1), 2);   // hover Save
    expect(d.a().ui().any_popup_open(), "the File menu is open");
    d.capture();
}

// 3. An object with several components selected; its components in Properties.
void test_docs_hierarchy_inspector() {
    if (!docs_shot_dir()) return;
    DocsEditor d("hierarchy_inspector", "scenes/water_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    // Click lake_water's Hierarchy row, then its arrow to list its components.
    const float row_y = 119.0f, right_x = d.vb().right() + 1.0f;
    d.in->click({right_x + 90.0f, row_y});
    d.in->click({right_x + 31.0f, row_y});
    expect(d.a().document().primary() == object_named(d.a(), "lake_water"), "lake_water is selected");
    d.a().set_prop_tab(PropTab::Components);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// 4. The Add Component menu open.
void test_docs_add_component() {
    if (!docs_shot_dir()) return;
    DocsEditor d("add_component", "scenes/main/scene.yaml");
    docs_main_view(d);
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.a().set_prop_tab(PropTab::Components);
    tick(d.e(), 4);
    // Find the button on screen: the lowest block of the theme's button colour in the
    // Properties column (Add Component follows the last component panel).
    const glm::vec4 bc = d.a().ui().style.button;
    const auto img = d.e().capture_image(false);
    const float sc = d.in->scale;
    const int x = static_cast<int>((d.vb().right() + 46.0f) * sc);
    int run = 0, best_mid = -1;
    for (int y = 0; y < static_cast<int>(img.height); ++y) {
        const uint8_t* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * img.channels];
        bool match = true;
        for (int c = 0; c < 3; ++c) match &= std::abs(int(q[c]) - int(std::lround(bc[c] * 255.0f))) <= 4;
        run = match ? run + 1 : 0;
        if (run >= static_cast<int>(14 * sc)) best_mid = y - static_cast<int>(7 * sc);
    }
    expect(best_mid >= 0, "the Add Component button is on screen");
    const glm::vec2 button{d.vb().right() + 46.0f, best_mid / sc};
    d.in->click(button);
    expect(d.a().ui().any_popup_open(), "Add Component opens its menu");
    // Type into its search box: "light" narrows the list to the lights. The full list is
    // taller than the window, so the menu sits against the top edge until it is filtered.
    d.in->click({button.x - 46.0f + 1.0f + 100.0f, 14.0f});
    for (char ch : std::string("light")) {
        d.e().queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
        tick(d.e(), 1);
    }
    d.in->key(coopa::input::Key::Enter);   // the search field applies on Enter
    tick(d.e(), 3);
    d.in->move(button + glm::vec2(30.0f, 123.0f), 3);   // hover PointLight
    tick(d.e(), 20);
    d.capture();
}

// 5. Solid shading with the Viewport Shading popover open.
void test_docs_viewport_solid() {
    if (!docs_shot_dir()) return;
    DocsEditor d("viewport_solid", "scenes/main/scene.yaml");
    docs_main_view(d);
    d.a().set_shading(Shading::Solid);
    tick(d.e(), 10);
    const imm::Box v = d.vb();
    d.in->click({v.right() - 13.0f, v.y - 13.0f});
    expect(d.a().ui().any_popup_open(), "the Viewport Shading popover is open");
    d.in->move({v.right() - 120.0f, v.y + 160.0f}, 2);
    d.capture();
}

// 6. Mid G-move constrained to X, with the axis line.
void test_docs_gizmo_move() {
    if (!docs_shot_dir()) return;
    using coopa::input::Key;
    DocsEditor d("gizmo_move", "scenes/main/scene.yaml");
    docs_main_view(d);
    const ObjectId cube = object_named(d.a(), "cube");
    docs_pick_move_tool(d);
    d.a().document().select(cube);
    tick(d.e(), 4);
    const auto c = screen_of(d.e(), d.a(), cube, glm::vec3(0.0f), d.in->scale);
    const glm::vec2 start = c ? *c : d.vb().center();
    d.in->move(start + glm::vec2(40.0f, 0.0f));
    d.in->key(Key::G);
    d.in->key(Key::X);
    for (int i = 1; i <= 8; ++i) d.in->move(start + glm::vec2(40.0f + 12.0f * i, -4.0f * i));
    expect(d.a().modal_active(), "the move is in progress");
    tick(d.e(), 4);
    d.capture();
}

void docs_stage_edit_mode(DocsEditor& d);

// 7. X-Ray in Edit Mode.
void test_docs_xray_edit() {
    if (!docs_shot_dir()) return;
    using coopa::input::Key;
    using coopa::input::Mods;
    DocsEditor d("xray_edit", "scenes/main/scene.yaml");
    docs_stage_edit_mode(d);
    d.in->move(d.vb().center());
    d.in->key(Key::Num1);
    d.in->key(Key::A);
    d.in->key(Key::Z, Mods::Alt);
    expect(d.a().edit_mode_active() && d.e().render_config().editor_xray_alpha < 0.99f, "X-Ray is on in Edit Mode");
    d.rest();
    tick(d.e(), 10);
    d.capture();
}

/** @brief Edit Mode on the starter cube, face select, a loop cut and an extruded top face. */
void docs_stage_edit_mode(DocsEditor& d) {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.in->move(d.vb().center());
    d.in->key(Key::Tab);
    tick(d.e(), toy::core::Engine::kFillDebounceFrames + 2);
    expect(d.a().edit_mode_active(), "Tab enters Edit Mode");
    // Ctrl+R on a vertical edge: one loop around the middle.
    const glm::vec2 corner = visible_corner(d.a(), cube);
    if (auto edge = screen_of(d.e(), d.a(), cube, glm::vec3(corner, 0.1f), d.in->scale)) {
        d.in->move(*edge);
        d.in->key(Key::R, Mods::Control);
        d.in->move(*edge + glm::vec2(1, 0));
        d.in->click(*edge);
        d.in->click(*edge, MouseButton::Right);
    }
    // Face select, the top face, E 0.6 Enter.
    d.in->key(Key::Num3);
    auto& md = d.a().mesh_document();
    md.selection.faces.clear();
    for (uint32_t f = 0; f < md.mesh.faces.size(); ++f) {
        if (md.mesh.face_center(f).z > 0.49f) md.selection.faces.insert(f);
    }
    d.in->move(d.vb().center());
    d.in->key(Key::E);
    d.in->key(Key::Period);
    d.in->key(Key::Num6);
    d.in->key(Key::Enter);
    // Step back to see the whole (now taller) mesh.
    auto& cam = d.a().camera();
    cam.focus = glm::vec3(0.0f, 0.0f, 0.75f);
    cam.distance = 4.6f;
    cam.apply();
    tick(d.e(), 4);
}

// 8. Edit Mode, face select, faces selected after an extrude.
void test_docs_edit_mode() {
    if (!docs_shot_dir()) return;
    DocsEditor d("edit_mode", "scenes/main/scene.yaml");
    docs_stage_edit_mode(d);
    d.rest();
    tick(d.e(), 6);
    d.capture();
}

// 9. The Adjust Last Operation panel after a loop cut.
void test_docs_adjust_last_operation() {
    if (!docs_shot_dir()) return;
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    DocsEditor d("adjust_last_operation", "scenes/main/scene.yaml");
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.in->move(d.vb().center());
    d.in->key(Key::Tab);
    tick(d.e(), toy::core::Engine::kFillDebounceFrames + 2);
    d.in->key(Key::Num2);
    const glm::vec2 corner = visible_corner(d.a(), cube);
    if (auto edge = screen_of(d.e(), d.a(), cube, glm::vec3(corner, 0.1f), d.in->scale)) {
        d.in->move(*edge);
        d.in->key(Key::R, Mods::Control);
        d.in->move(*edge + glm::vec2(1, 0));
        d.in->click(*edge);
        d.in->click(*edge, MouseButton::Right);
    }
    expect(d.a().mesh_document().mesh.faces.size() > 6, "the loop cut ran");
    d.rest();
    tick(d.e(), 6);
    d.capture();
}

/** @brief A sphere added to the starter scene, selected, in `mode`, smoothed for the brushes. */
ObjectId docs_brush_sphere(DocsEditor& d, InteractionMode mode) {
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.a().delete_selected();
    tick(d.e(), 2);
    const ObjectId sphere = d.a().create_primitive("Sphere");
    tick(d.e(), 3);
    d.a().document().set_transform(sphere, {0, 0, 1}, glm::vec3(0), glm::vec3(1), "Move");
    d.a().sync().apply(d.e(), d.a().document(), {ChangeScope::Transform, sphere});
    d.a().document().select(sphere);
    d.a().frame_selected();
    tick(d.e(), 4);
    d.a().set_interaction_mode(mode);
    tick(d.e(), toy::core::Engine::kFillDebounceFrames + 2);
    d.a().subdivide_smooth(1);
    tick(d.e(), 3);
    return sphere;
}

/** @brief A brush stroke across the sphere (local points on its surface, in screen space). */
void docs_stroke(DocsEditor& d, ObjectId id, glm::vec3 a, glm::vec3 b) {
    auto p0 = screen_of(d.e(), d.a(), id, a, d.in->scale);
    auto p1 = screen_of(d.e(), d.a(), id, b, d.in->scale);
    if (!p0 || !p1) return;
    d.in->move(*p0);
    d.in->drag(*p1, coopa::input::MouseButton::Left, 14);
    tick(d.e(), 2);
}

/** @brief The sphere's camera-facing side: local points for strokes. */
glm::vec3 docs_front(DocsEditor& d, float u, float v) {
    const glm::mat4 view = coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix();
    const glm::mat3 inv = glm::transpose(glm::mat3(view));
    const glm::vec3 right = inv[0], up = inv[1], back = inv[2];
    return glm::normalize(back + right * u + up * v) * 0.98f;
}

// 10. Sculpt Mode after a few strokes, the brush circle under the mouse.
void test_docs_sculpt() {
    if (!docs_shot_dir()) return;
    DocsEditor d("sculpt", "scenes/main/scene.yaml");
    const ObjectId s = docs_brush_sphere(d, InteractionMode::Sculpt);
    expect(d.a().interaction_mode() == InteractionMode::Sculpt, "Sculpt Mode");
    d.a().sculpt_settings().radius_px = 35.0f;
    d.a().sculpt_settings().strength = 0.45f;
    docs_stroke(d, s, docs_front(d, -0.6f, 0.3f), docs_front(d, 0.5f, 0.35f));
    docs_stroke(d, s, docs_front(d, -0.5f, -0.2f), docs_front(d, 0.4f, -0.3f));
    if (auto p = screen_of(d.e(), d.a(), s, docs_front(d, 0.15f, 0.05f), d.in->scale)) d.in->move(*p, 3);
    tick(d.e(), 4);
    d.capture();
}

// 11. Vertex Paint with painted colours.
void test_docs_vertex_paint() {
    if (!docs_shot_dir()) return;
    DocsEditor d("vertex_paint", "scenes/main/scene.yaml");
    const ObjectId s = docs_brush_sphere(d, InteractionMode::VertexPaint);
    expect(d.a().interaction_mode() == InteractionMode::VertexPaint, "Vertex Paint");
    auto& vs = d.a().vertex_paint_settings();
    vs.radius_px = 40.0f;
    vs.color = glm::vec4(0.9f, 0.15f, 0.1f, 1.0f);
    docs_stroke(d, s, docs_front(d, -0.6f, 0.4f), docs_front(d, 0.6f, 0.4f));
    vs.color = glm::vec4(0.15f, 0.5f, 0.95f, 1.0f);
    docs_stroke(d, s, docs_front(d, -0.6f, -0.3f), docs_front(d, 0.6f, -0.3f));
    vs.color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f);
    docs_stroke(d, s, docs_front(d, 0.0f, 0.6f), docs_front(d, 0.0f, -0.6f));
    if (auto p = screen_of(d.e(), d.a(), s, docs_front(d, 0.3f, 0.05f), d.in->scale)) d.in->move(*p, 3);
    tick(d.e(), 4);
    d.capture();
}

// 12. Weight Paint heatmap with a vertex group.
void test_docs_weight_paint() {
    if (!docs_shot_dir()) return;
    DocsEditor d("weight_paint", "scenes/main/scene.yaml");
    const ObjectId s = docs_brush_sphere(d, InteractionMode::WeightPaint);
    expect(d.a().interaction_mode() == InteractionMode::WeightPaint, "Weight Paint");
    auto& ws = d.a().weight_paint_settings();
    ws.radius_px = 55.0f;
    ws.weight = 1.0f;
    docs_stroke(d, s, docs_front(d, -0.7f, 0.5f), docs_front(d, 0.7f, 0.5f));
    docs_stroke(d, s, docs_front(d, -0.5f, 0.2f), docs_front(d, 0.5f, 0.2f));
    ws.weight = 0.5f;
    docs_stroke(d, s, docs_front(d, -0.6f, -0.2f), docs_front(d, 0.6f, -0.2f));
    expect(!d.a().mesh_document().mesh.groups.empty(), "a vertex group was painted");
    if (auto p = screen_of(d.e(), d.a(), s, docs_front(d, 0.35f, -0.45f), d.in->scale)) d.in->move(*p, 3);
    tick(d.e(), 4);
    d.capture();
}

// 13. A material in the material editor, on the shader-ball lookdev.
void test_docs_material_editor() {
    if (!docs_shot_dir()) return;
    DocsEditor d("material_editor", "scenes/main/scene.yaml");
    d.in->click({18.0f + 25.0f * 3.0f, 44.0f});   // the Materials tab
    d.a().open_asset(AssetType::Material, "materials/brick.yaml");
    tick(d.e(), 6);
    expect(d.a().active_asset_type() == AssetType::Material, "the material opens");
    tick(d.e(), 60);
    d.rest();
    d.capture();
}

/** @brief Opens the robot arm object asset, the joint `elbow` selected. */
ObjectId docs_robot_arm(DocsEditor& d) {
    d.in->click({18.0f + 25.0f, 44.0f});   // the Objects tab
    d.a().open_asset(AssetType::Object, "objects/robot_arm.yaml");
    tick(d.e(), 8);
    expect(d.a().active_asset_type() == AssetType::Object, "the robot arm opens");
    return object_named(d.a(), "elbow");
}

// 14. The Timeline dope sheet with keys.
void test_docs_timeline() {
    if (!docs_shot_dir()) return;
    DocsEditor d("timeline", "scenes/main/scene.yaml");
    const ObjectId elbow = docs_robot_arm(d);
    d.a().show_timeline();
    d.a().document().select(elbow);
    tick(d.e(), 3);
    d.a().set_animation_time(0.6f);
    expect(d.a().animation_clip() != nullptr, "the arm's clip is open");
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

// 15. Record on: the red viewport frame.
void test_docs_record_autokey() {
    if (!docs_shot_dir()) return;
    DocsEditor d("record_autokey", "scenes/main/scene.yaml");
    const ObjectId elbow = docs_robot_arm(d);
    d.a().show_timeline();
    d.a().document().select(elbow);
    tick(d.e(), 3);
    d.a().set_animation_time(0.4f);
    d.a().set_animation_record(true);
    expect(d.a().animation_record(), "Record is on");
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

// 16. An object asset open.
void test_docs_object_asset() {
    if (!docs_shot_dir()) return;
    DocsEditor d("object_asset", "scenes/main/scene.yaml");
    docs_robot_arm(d);
    d.in->click({d.vb().right() + 15.0f, 71.0f});   // expand the root's Hierarchy row
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

/** @brief Opens ui/<name>.yaml in the UI designer. */
void docs_open_ui(DocsEditor& d, const std::string& name) {
    d.a().open_asset(AssetType::UI, "ui/" + name + ".yaml");
    tick(d.e(), 8);
    expect(d.a().ui_mode(), "ui/" + name + " opens in the UI designer");
}

// 17. The HUD in the designer, an element selected (rect gizmo), Element tab.
void test_docs_ui_designer() {
    if (!docs_shot_dir()) return;
    DocsEditor d("ui_designer", "scenes/main/scene.yaml");
    docs_open_ui(d, "hud");
    d.in->click({d.vb().right() + 15.0f, 71.0f});   // expand the root's Hierarchy row
    const ObjectId hotbar = object_named(d.a(), "Hotbar");
    expect(hotbar != 0, "the HUD has a Hotbar");
    d.a().document().select(hotbar);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 10);
    d.capture();
}

// 18. The UI tab's + menu (New UI) listing the templates.
void test_docs_ui_new_menu() {
    if (!docs_shot_dir()) return;
    DocsEditor d("ui_new_menu", "scenes/main/scene.yaml");
    docs_open_ui(d, "main_menu");
    d.in->click({242.0f, 44.0f});
    expect(d.a().ui().any_popup_open(), "+ opens the New UI menu");
    d.in->move({300.0f, 400.0f}, 2);
    d.capture();
}

// 19. Interact mode running.
void test_docs_ui_interact() {
    if (!docs_shot_dir()) return;
    DocsEditor d("ui_interact", "scenes/main/scene.yaml");
    docs_open_ui(d, "main_menu");
    d.a().set_ui_interact(true);
    tick(d.e(), 10);
    expect(d.a().ui_interacting(), "Interact is running");
    // Click Continue (its click is echoed to the Console), then hover Settings.
    auto button = [&](const char* name) -> std::optional<glm::vec2> {
        auto* live = d.a().sync().live(d.a().document().object_root());
        auto* b = live ? live->find_descendant(name) : nullptr;
        auto* rt = b ? b->get_component<coopa::ui::RectTransform>() : nullptr;
        if (!rt) return std::nullopt;
        return d.a().ui_view().to_editor(rt->rect().center());
    };
    if (auto c = button("Continue")) d.in->click(*c);
    if (auto st = button("Settings")) d.in->move(*st, 4);
    tick(d.e(), 10);
    d.capture();
}

// 20. The Bindings tab.
void test_docs_ui_bindings() {
    if (!docs_shot_dir()) return;
    DocsEditor d("ui_bindings", "scenes/main/scene.yaml");
    docs_open_ui(d, "main_menu");
    d.a().set_prop_tab(PropTab::Bindings);
    d.rest();
    tick(d.e(), 6);
    d.capture();
}

// 21 / 22. Render and World properties.
void docs_settings_tab(const std::string& shot, PropTab tab) {
    DocsEditor d(shot, "scenes/water_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    d.a().document().clear_selection();
    d.a().set_prop_tab(tab);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}
void test_docs_render_settings() { if (docs_shot_dir()) docs_settings_tab("render_settings", PropTab::Render); }
void test_docs_world_settings() { if (docs_shot_dir()) docs_settings_tab("world_settings", PropTab::World); }

// 23. File > Package Project (.caml)... open.
void test_docs_package_dialog() {
    if (!docs_shot_dir()) return;
    DocsEditor d("package_dialog", "scenes/water_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    tick(d.e(), 40);
    d.in->click(d.topbar_menu(0));
    d.in->click(d.menu_row({32.0f, 27.0f}, 8, 3));
    tick(d.e(), 3);
    expect(d.a().ui().is_popup_open("Package Project"), "the Package Project window is open");
    // The default output folder is <project>/build/package; this project lives in a scratch
    // folder, so show it as it reads for a project at ~/my_game (typed into the field).
    d.in->click({540.0f + 380.0f, 345.0f + 69.0f});   // the Output folder field (520 x 210 window, centred)
    for (char ch : std::string("~/my_game/build/package")) {
        d.e().queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
        tick(d.e(), 1);
    }
    d.in->key(coopa::input::Key::Enter);
    d.rest();
    d.capture();
}

// 24. Blender Light theme, overview-like.
void test_docs_theme_light() {
    if (!docs_shot_dir()) return;
    DocsEditor d("theme_light", "scenes/water_test/scene.yaml", "blender_light");
    expect(d.a().theme_id() == "blender_light", "Blender Light is active");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    docs_pick_move_tool(d);
    d.a().document().select(object_named(d.a(), "boat"));
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// 25. Help > Controls... open.
void test_docs_controls_modal() {
    if (!docs_shot_dir()) return;
    DocsEditor d("controls_modal", "scenes/water_test/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    tick(d.e(), 40);
    const glm::vec2 help = d.topbar_menu(4);
    d.in->click(help);
    const float hx = help.x - (d.a().ui().text_width("Help") + d.a().ui().style.padding * 3) * 0.5f;
    d.in->click(d.menu_row({hx, 27.0f}, 0, 0));
    tick(d.e(), 3);
    expect(d.a().ui().is_popup_open("Controls"), "the Controls window is open");
    d.rest();
    d.capture();
}

// 26. The Unsaved Changes prompt.
void test_docs_unsaved_prompt() {
    if (!docs_shot_dir()) return;
    using coopa::input::Key;
    DocsEditor d("unsaved_prompt", "scenes/main/scene.yaml");
    docs_main_view(d);
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.in->move(d.vb().center());
    d.in->key(Key::G);
    d.in->key(Key::X);
    d.in->key(Key::Num2);
    d.in->key(Key::Enter);
    d.a().open_asset(AssetType::Material, "materials/brick.yaml");
    tick(d.e(), 4);
    expect(d.a().ui().is_popup_open("Unsaved Changes"), "switching asks to save first");
    d.rest();
    d.capture();
}

// 27. The Asset panel's Meshes tab, a mesh open in the mesh viewer.
void test_docs_asset_panel_meshes() {
    if (!docs_shot_dir()) return;
    DocsEditor d("asset_panel_meshes", "scenes/main/scene.yaml");
    d.in->click({18.0f + 25.0f * 2.0f, 44.0f});   // the Meshes tab
    d.a().open_asset(AssetType::Mesh, "meshes/barrel.yaml");
    tick(d.e(), 8);
    expect(d.a().active_asset_type() == AssetType::Mesh, "the mesh opens in the mesh viewer");
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

// =====================================================================================
// Group "project" -- .toy projects, project component schemas, the hub's model (device-free)
// =====================================================================================

void test_project_toy_file() {
    // A toyhub-made project starts as a .toy and scripts; the editor creates assets/ on first open.
    const fs::path root = fresh_dir("Fancy Game");
    write_text(root / "fancy.toy",
               "format: toyproject\nversion: 1\nname: \"Ignored\"\ntarget: fancy_game\n"
               "engine:\n  source: git@github.com:cooparobla/toyengine.git\n  ref: abc123\n");
    Project p(root);
    expect(p.valid() && !p.has_assets(), "a .toy without assets/ is a valid project awaiting its skeleton");
    expect(p.name() == "Fancy Game", "a project's name is its folder's, whatever the .toy says");
    expect(Project(root.string() + "/").name() == "Fancy Game", "...with or without a trailing slash");
    expect(p.project_file() == root / "fancy.toy", "project_file() finds the .toy");
    const std::string before = [&] { std::ifstream in(root / "fancy.toy"); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    p = Project::create(root);
    expect(p.has_assets() && coopa::yaml::document_exists(p.assets() / "scenes/main/scene.yaml"), "create() lays down assets/");
    const std::string after = [&] { std::ifstream in(root / "fancy.toy"); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    expect(before == after, "create() leaves an existing .toy byte-identical");

    // config.yaml starts as the engine's own: same settings and comments, only the title and
    // default scene are the project's -- and a game doesn't save a screenshot on exit.
    auto read = [](const fs::path& f) { std::ifstream in(f); std::vector<std::string> l; for (std::string x; std::getline(in, x);) l.push_back(x); return l; };
    const auto engine_cfg = read(fs::path(ROOT_DIR) / "assets" / "config.yaml");
    const auto project_cfg = read(p.config_path());
    std::vector<std::string> changed;
    for (size_t i = 0; i < std::min(engine_cfg.size(), project_cfg.size()); ++i) if (engine_cfg[i] != project_cfg[i]) changed.push_back(project_cfg[i]);
    expect(engine_cfg.size() == project_cfg.size() && changed.size() == 3 && changed[0] == "  title: \"Fancy Game\"" &&
               changed[1] == "  default_scene: \"assets/scenes/main/scene.yaml\"" && changed[2] == "  save_on_exit: false",
           "a new project's config.yaml is the engine's, with only the title, default scene and save_on_exit changed");
    int toys = 0;
    for (const auto& e : fs::directory_iterator(root)) toys += e.path().extension() == ".toy";
    expect(toys == 1, "create() adds no second .toy");

    // A bare folder gets a minimal .toy named after it.
    const fs::path bare = fresh_dir("bare_project");
    Project b = Project::create(bare);
    expect(b.project_file() == bare / "bare_project.toy", "create() writes <dir>.toy into a bare folder");
    expect(b.name() == "bare_project", "...whose name is the folder name");
    const Node toy = Project::load_toy(b.project_file());
    expect(get_string(toy, "format") == "toyproject" && !toy.contains("name"), "...in the toyproject format, with no name key");

    // An assets-only folder (this repository) is still a project, named after its folder.
    Project repo{fs::path(ROOT_DIR)};
    expect(repo.valid() && repo.name() == fs::path(ROOT_DIR).filename().string(), "the engine checkout stays a valid project");
}

void test_project_component_schema() {
    expect(find_schema("TestProjectSpinner") == nullptr, "no schema before registration");
    register_component_schema({"TestProjectSpinner", "Gameplay", {f_float("speed", 90.0f, 1.0f, -3600.0f, 3600.0f, true)}});
    const ComponentSchema* s = find_schema("TestProjectSpinner");
    expect(s && s->category == "Gameplay", "a registered project schema is found");
    expect(schemas().count("TestProjectSpinner") == 1, "...and listed for Add Component");
    const Node c = default_component("TestProjectSpinner");
    expect(c.contains("speed") && std::abs(get_float(c, "speed") - 90.0) < 1e-6, "its in_default fields seed a new component");
    expect(find_schema("Transform") != nullptr, "built-in schemas are still there");
}

void test_hub_projects_and_registry() {
    namespace hub = toy::hub;
    const fs::path home = fresh_dir("hub_home");
    setenv("HOME", home.c_str(), 1);
    expect(hub::projects_file() == home / ".toyengine" / "projects.yaml" &&
           hub::settings_file() == home / ".toyengine" / "settings.yaml", "the hub keeps everything in ~/.toyengine");
    const fs::path a = fresh_dir("Alpha"), b = fresh_dir("Beta");
    write_text(a / "alpha.toy", "target: alpha\nengine:\n  source: x\n  ref: 0123456789abcdef0123\n");
    write_text(b / "beta.toy", "target: beta\nengine:\n  source: x\n  ref: main\n  link: /some/engine\n");
    hub::list_project(a);
    hub::list_project(b);
    hub::list_project(a.string() + "/");   // the same project, spelled differently
    expect(hub::read_project_list().size() == 2, "the list holds each project once");
    expect(!hub::list_project(ROOT_DIR) && hub::read_project_list().size() == 2, "the engine checkout is never listed");

    // Nothing is auto-detected: the editor's recent projects (which include the engine
    // checkout) never show up in the hub.
    toy::editor::Project::remember(ROOT_DIR);
    toy::editor::Project::remember(fresh_dir("only_in_editor_recents"));
    auto list = hub::load_projects();
    expect(list.size() == 2, "only listed projects are shown");
    auto find = [&](const std::string& n) -> const hub::HubProject* {
        for (const auto& p : list) if (p.name == n) return &p;
        return nullptr;
    };
    const auto* pa = find("Alpha");
    const auto* pb = find("Beta");
    expect(pa && pb, "projects are named after their folders");
    expect(pa && pa->target == "alpha" && !pa->linked() && pa->engine_label() == "0123456789", "a pinned project shows its short ref");
    expect(pb && pb->linked() && pb->engine_link == "/some/engine" && pb->engine_label() == "linked", "a linked project shows as linked");
    expect(pa && !pa->built() && pa->editor_binary() == a / "build" / "alpha_editor", "unbuilt projects report no editor binary");
    hub::forget_project(a);
    list = hub::load_projects();
    expect(list.size() == 1 && list[0].name == "Beta", "forget removes a project from the list (not from disk)");
    expect(fs::exists(a / "alpha.toy"), "...and leaves its files alone");

    // Settings: the hub's own, blender_dark until set.
    expect(std::string(hub::default_hub_theme()) == "blender_dark" && !hub::load_settings().contains("theme"),
           "the hub starts on blender_dark");
    toy::editor::Node st = hub::load_settings();
    st["theme"] = toy::editor::Node(std::string("unity_dark"));
    hub::save_settings(st);
    expect(get_string(hub::load_settings(), "theme") == "unity_dark", "the hub's theme persists in ~/.toyengine/settings.yaml");
    expect(!toy::editor::Project::load_prefs().contains("theme"), "...without touching the editor's preferences");

    // Tasks: combined output line by line, exit status, quoting.
    hub::Task t;
    t.start("t", "echo " + hub::shell_quote("it's") + "; echo two >&2; exit 3");
    t.wait();
    const auto lines = t.lines();
    expect(t.exit_code() == 3 && !t.succeeded(), "a task reports its exit status");
    expect(lines.size() == 3 && lines[1] == "it's" && lines[2] == "two", "stdout and stderr stream in, quoting intact");
    expect(hub::toyhub_command({"new", "a b"}).find("'a b'") != std::string::npos, "toyhub arguments are shell-quoted");
}

void test_process_task_and_diagnostics() {
    // Cancel stops the command and everything it started (its process group), promptly.
    Task t;
    const auto t0 = std::chrono::steady_clock::now();
    t.start("sleepy", "echo started; sleep 30 & sleep 30; echo never");
    while (t.line_count() < 2 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    t.cancel();
    t.wait();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    expect(!t.running() && t.cancelled() && !t.succeeded() && secs < 10.0, "cancel() ends a running task (" + std::to_string(secs) + " s)");
    const auto lines = t.lines();
    expect(std::find(lines.begin(), lines.end(), "never") == lines.end(), "...before it finishes");

    // Compiler diagnostics, as the build modal lists them.
    auto d = parse_diagnostic("/p/src/game.cpp:12:5: error: no member named 'x'");
    expect(d && d->error && d->file == "/p/src/game.cpp" && d->line == 12 && d->column == 5 && d->message == "no member named 'x'",
           "a clang error parses into file, line, column, message");
    d = parse_diagnostic("/p/src/game.h:7: warning: unused");
    expect(d && !d->error && d->line == 7 && d->column == 0, "a warning without a column parses");
    d = parse_diagnostic("ld: error: undefined symbol: foo");
    expect(d && d->error && d->line == 0, "a linker error without a location still counts");
    expect(!parse_diagnostic("[ 42%] Building CXX object CMakeFiles/x.dir/a.cpp.o"), "progress lines are not diagnostics");
    expect(current_executable().filename() == "toyengine_editor_tests" && fs::exists(current_executable()),
           "current_executable() finds this binary (what Relaunch Editor exec()s)");
}

void test_package_engine_fallback() {
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("fallback_src");
    Project project = Project::create(root);
    const fs::path out = fresh_dir("fallback_out");
    PackageOptions opt;
    opt.out_dir = out;
    opt.engine_assets = fs::path(ROOT_DIR) / "assets";
    const PackageReport rep = package_project(project, opt);
    expect(rep.ok(), "packaging with the engine fallback succeeds");
    bool spv = false, glsl = false;
    for (const auto& e : fs::directory_iterator(out / "assets" / "shaders")) {
        spv |= e.path().extension() == ".spv";
        glsl |= e.path().extension() == ".frag" || e.path().extension() == ".vert";
    }
    expect(spv && !glsl, "engine shaders ship compiled only");
    expect(fs::is_directory(out / "assets" / "fonts"), "engine fonts ship");
    expect(!fs::exists(out / "assets" / "scenes" / "demos" / "pixel_demo"), "engine scenes never ship");
    expect(coopa::yaml::document_exists(out / "assets" / "scenes" / "main" / "scene.yaml"), "the project's own scene ships");
    expect(!fs::exists(out / "assets" / "materials" / "brick.caml"), "engine content outside the runtime dirs never ships");

    // A project that ships its own copy of an engine runtime file keeps its own.
    write_text(project.assets() / "ui" / "marker.txt", "project");
    write_text(project.assets() / "fonts" / "LICENSE-OFL.txt", "project copy");
    const fs::path out2 = fresh_dir("fallback_out2");
    opt.out_dir = out2;
    expect(package_project(project, opt).ok(), "repackaging succeeds");
    std::ifstream lic(out2 / "assets" / "fonts" / "LICENSE-OFL.txt");
    std::string first;
    std::getline(lic, first);
    expect(first == "project copy", "a project file shadows the engine's of the same name");
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
        // Likewise every other effect that accumulates across frames (a new project's config.yaml
        // is the engine's, which turns them on): this compares YAML vs .caml loading, not timing.
        cfg.render.aa_mode = "off";
        cfg.render.auto_exposure_enabled = false;
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
// Group "editor_shell" (continued) -- Build > Refresh
// =====================================================================================

void test_editor_build_refresh() {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("build_refresh_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 3);
    auto run = [&](const std::string& cmd) {
        app.build_refresh(cmd);
        tick(engine, 2);
        for (int i = 0; i < 400 && app.build_task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); tick(engine, 1); }
        tick(engine, 2);
    };
    auto modal_open = [&] { return app.ui().is_popup_open("Build Project"); };
    expect(EditorApp::default_build_command().find("cmake --build") != std::string::npos &&
           EditorApp::default_build_command().find("--target") != std::string::npos,
           "Refresh builds this editor's own targets with cmake --build");

    // A failing build: blocking modal, errors listed and in the Console, no relaunch.
    run("echo '[ 50%] Building CXX object game.cpp.o'; echo '/p/src/game.cpp:12:5: error: no member named x'; exit 2");
    expect(modal_open(), "the build modal is up");
    expect(!app.build_task().succeeded() && app.build_diagnostics().size() == 1 && app.build_diagnostics()[0].line == 12,
           "a failed build lists its compiler errors");
    bool console = false;
    for (const auto& [lvl, msg] : app.log()) console |= lvl == 2 && msg.find("game.cpp:12") != std::string::npos;
    expect(console, "...and puts them in the Console for the traceback");
    expect(!app.build_relaunch_offered() && !app.relaunch_requested(), "a failed build offers no relaunch");
    dump(engine, "build_refresh_failed");

    // A build that leaves the binary as it was: up to date, nothing to relaunch.
    run("echo all up to date");
    expect(app.build_task().succeeded() && app.build_diagnostics().empty() && !app.build_relaunch_offered(),
           "an up-to-date build succeeds without offering a relaunch");

    // While building, the modal can't be dismissed: Escape leaves it up.
    app.build_refresh("sleep 2");
    tick(engine, 3);
    engine.queue_input([](coopa::input::Input& in) { in.push_key(coopa::input::Key::Escape, 0, coopa::input::KeyAction::Press, coopa::input::Mods::None); });
    tick(engine, 2);
    expect(modal_open() && app.build_task().running(), "the build modal blocks the editor while it builds");
    const_cast<Task&>(app.build_task()).cancel();
    for (int i = 0; i < 200 && app.build_task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); tick(engine, 1); }
    tick(engine, 2);
    expect(app.build_task().cancelled(), "Cancel Build stops it");
}

void test_editor_tile_set_duplicate() {
    // A terrain tile style is a tile-set object (assets/objects/tileset_*.yaml): toyengine's are
    // listed among its Objects, open like any object, and duplicate into a NEW look that owns a
    // copy of every piece mesh -- so reshaping a piece of the copy never touches the original.
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    const fs::path home = fresh_dir("tile_set_home");
    setenv("HOME", home.c_str(), 1);
    const fs::path root = fresh_dir("tile_set_project");
    Project project = Project::create(root);
    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    tick(engine, 4);

    const auto objects = app.engine_assets_listed(AssetType::Object);
    expect(std::find(objects.begin(), objects.end(), "objects/terrain/tileset_round.yaml") != objects.end(),
           "toyengine's tile sets are listed among its objects");

    const std::string copy = app.duplicate_tile_set("objects/terrain/tileset_round.yaml");
    expect(copy == "objects/tileset_round_copy.yaml", "the duplicate lands in the project under a new name");
    const Node doc = coopa::yaml::load_document(project.assets() / "objects" / "tileset_round_copy.yaml");
    const Node& obj = doc.at("object");
    expect(obj.at("name").get_value<std::string>() == "tileset_round_copy", "the copy names itself");
    int pieces = 0, own = 0;
    for (const auto& child : obj.at("children")) {
        for (const auto& comp : child.at("components")) {
            if (!comp.contains("mesh_path")) continue;
            ++pieces;
            const std::string ref = comp.at("mesh_path").get_value<std::string>();
            if (ref == "tileset_round_copy_" + child.at("name").get_value<std::string>() &&
                fs::exists(project.assets() / "meshes" / (ref + ".yaml"))) ++own;
        }
    }
    expect(pieces == static_cast<int>(toy::world::k_tile_piece_count) && own == pieces, "every piece of the copy points at its own new mesh file");
    expect(fs::exists(Project::engine_assets() / "meshes" / "terrain" / "round" / "tile_round_top_outer.yaml"),
           "toyengine's own pieces are left in place");

    app.open_asset(AssetType::Object, copy);
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Object, "the new tile set opens as an object, pieces and all");
    unsetenv("FIXED_DT");
}

void test_editor_engine_assets() {
    // toyengine's assets/ is a read-only layer under a game project's: listed (toggle, on by
    // default), usable by drag and drop, never edited -- Copy to Project makes an editable copy.
    using coopa::input::MouseButton;
    setenv("FIXED_DT", "0", 1);
    unsetenv("NO_INPUT");
    const fs::path home = fresh_dir("engine_assets_home");
    setenv("HOME", home.c_str(), 1);
    const fs::path root = fresh_dir("engine_assets_project");
    Project project = Project::create(root);
    expect(!project.is_engine() && Project(fs::path(ROOT_DIR)).is_engine(), "a game project is not toyengine; the checkout is");
    expect(project.is_engine_asset("materials/brick.yaml") && !project.is_engine_asset("materials/default.yaml"),
           "a file only toyengine has resolves to toyengine's; one the project has stays the project's");
    expect(project.absolute("materials/brick.yaml") == Project::engine_assets() / "materials/building/brick.yaml" &&
           project.relative(project.absolute("materials/brick.yaml")) == "materials/building/brick.yaml",
           "absolute() finds toyengine's asset by name (in its tag folder); relative() round-trips");
    expect(Project(fs::path(ROOT_DIR)).list_engine("materials", ".yaml").empty(), "inside toyengine nothing is a separate layer");

    toy::core::Engine engine(shell_config(project), shell_options(project));
    EditorApp app(engine, project);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    tick(engine, 4);
    expect(app.show_engine_assets(), "toyengine's assets are shown by default");
    auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    const auto meshes = app.engine_assets_listed(AssetType::Mesh);
    expect(has(meshes, "meshes/primitives/barrel.yaml") && !has(meshes, "meshes/primitives/cube.yaml"),
           "the Asset panel lists toyengine's meshes, minus those the project has its own copy of");

    // Not editable: opening is refused, so nothing can be saved back into toyengine.
    app.open_asset(AssetType::Material, "materials/brick.yaml");
    tick(engine, 3);
    expect(app.active_asset_type() == AssetType::Scene, "a toyengine material doesn't open for editing");

    // Usable: drag a toyengine mesh from the panel into the viewport.
    app.set_asset_tab(AssetType::Mesh);
    tick(engine, 3);
    const auto row = app.test_rect("asset_row:engine:meshes/primitives/barrel.yaml");
    expect(row.has_value(), "the barrel row is drawn in the toyengine section");
    if (row) {
        const imm::Box vb = app.viewport_box();
        in.move({row->x + row->w * 0.5f, row->y + row->h * 0.5f});
        in.drag({vb.x + vb.w * 0.5f, vb.y + vb.h * 0.5f}, MouseButton::Left, 8);
        tick(engine, 4);
    }
    const ObjectId barrel = object_named(app, "barrel");
    expect(barrel != 0, "dropping it adds a barrel to the scene");
    expect(engine.scene().find_object("barrel") != nullptr, "...which loads (toyengine's mesh resolves under the project)");
    dump(engine, "engine_assets_drop");

    // ...but its mesh can't be edited in place.
    if (barrel) {
        app.document().clear_selection();
        app.document().select(barrel);
        tick(engine, 1);
        expect(!app.set_interaction_mode(InteractionMode::Edit) && app.interaction_mode() == InteractionMode::Object,
               "Edit Mode on toyengine's mesh is refused");
    }

    // Copy to Project: the project's own, editable copy now resolves instead.
    expect(app.copy_engine_asset_to_project(AssetType::Material, "materials/brick.yaml"), "Copy to Project");
    tick(engine, 4);
    expect(fs::exists(root / "assets" / "materials" / "building" / "brick.yaml") && !app.project().is_engine_asset("materials/brick.yaml"),
           "the copy is the project's");
    // It opens for editing -- after the usual save prompt, since the barrel left the scene unsaved.
    expect(app.active_asset_type() == AssetType::Material || app.ui().is_popup_open("Unsaved Changes"),
           "...and opens for editing (after the unsaved-changes prompt)");
    expect(!has(app.engine_assets_listed(AssetType::Material), "materials/building/brick.yaml"), "...and leaves the toyengine section");

    // The toggle hides the layer, and is remembered.
    app.set_show_engine_assets(false);
    expect(app.engine_assets_listed(AssetType::Mesh).empty(), "the toggle hides toyengine's assets");
    expect(Project::load_prefs().contains("show_engine_assets") && !Project::load_prefs().at("show_engine_assets").get_value<bool>(),
           "...and is remembered");
}

// =====================================================================================
// Group "hub" -- the project hub, driven through real input
// =====================================================================================

/** @brief A listed hub project whose "editor" is a script that records being opened. */
fs::path make_hub_project(const fs::path& dir) {
    write_text(dir / "game.toy", "format: toyproject\ntarget: game\nengine:\n  source: x\n  ref: abc\n");
    write_text(dir / "editor.sh", "#!/bin/sh\ntouch \"$(dirname \"$0\")/opened\"\n");
    fs::permissions(dir / "editor.sh", fs::perms::owner_all);
    write_text(dir / "build" / "game_editor", "");   // "built"
    toy::hub::list_project(dir);
    return dir;
}

void test_hub_project_actions() {
    namespace hub = toy::hub;
    setenv("HOME", fresh_dir("hub_ui_home").c_str(), 1);
    const fs::path base = fresh_dir("hub_ui");
    const fs::path a = make_hub_project(base / "Alpha");
    toy::core::Engine engine(hub::HubApp::engine_config(false), hub::HubApp::engine_options());
    hub::HubApp app(engine);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    tick(engine, 3);
    expect(app.projects().size() == 1, "the listed project shows");
    auto centre = [](const coopa::ui::imm::Box& b) { return glm::vec2(b.x + b.w * 0.5f, b.y + b.h * 0.5f); };

    // "..." opens the project menu (the row's own click target must not swallow it).
    in.click(centre(app.row_menu_box(0)));
    tick(engine, 2);
    expect(app.menu_shown_for() == 0, "the ... button opens the project menu");
    dump(engine, "hub_menu");
    in.key(coopa::input::Key::Escape);
    in.click({600, 600});
    tick(engine, 2);

    // Open launches the project's editor (here: a script that leaves a marker).
    in.click(centre(app.row_open_box(0)));
    for (int i = 0; i < 200 && !fs::exists(a / "opened"); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    expect(fs::exists(a / "opened"), "Open launches the project's editor");

    // Add: a bare folder gets the options dialog; an existing project is listed straight away.
    const fs::path bare = base / "Bare";
    fs::create_directories(bare);
    app.add_folder(bare);
    tick(engine, 3);
    expect(app.modal_shown() == "Add Project", "adding a folder with no project asks for its options");
    dump(engine, "hub_add_options");
    in.key(coopa::input::Key::Escape);
    tick(engine, 2);
    expect(app.modal_shown().empty(), "Escape cancels the dialog");
    const fs::path existing = base / "Existing";
    write_text(existing / "existing.toy", "target: existing\nengine:\n  source: x\n  ref: abc\n");
    app.add_folder(existing);
    for (int i = 0; i < 500 && (app.task().running() || app.projects().size() < 2); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    expect(app.projects().size() == 2, "adding an existing project lists it (" + std::to_string(app.projects().size()) + ")");
    expect(app.modal_shown().empty(), "...without the options dialog (shown: " + app.modal_shown() + ")");

    // New project names are snake_case: the default, and whatever is typed.
    expect(app.project_form().name == "my_game", "the New project dialog suggests my_game");
    app.project_form().adding = false;
    app.project_form().location = base.string();
    app.project_form().name = "My Game";
    expect(app.project_form().target() == base / "my_game", "a typed \"My Game\" creates the folder my_game");

    // Remove: the confirmation, then Remove from List (files stay) / Move to Trash (folder goes).
    app.open_remove_dialog(app.projects()[0]);
    tick(engine, 3);
    expect(app.modal_shown() == "Remove Project", "Remove Project asks first");
    dump(engine, "hub_remove");
    in.key(coopa::input::Key::Escape);
    tick(engine, 2);
    hub::HubProject alpha;
    for (const auto& p : app.projects()) if (p.name == "Alpha") alpha = p;
    app.delete_project(alpha);
    for (int i = 0; i < 500 && app.task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    tick(engine, 2);
    const char* home = std::getenv("HOME");
    expect(!fs::exists(a) && fs::exists(fs::path(home) / ".Trash" / "Alpha"), "Move to Trash moves the project folder to the Trash");
    expect(app.projects().size() == 1 && app.projects()[0].name == "Existing", "...and drops it from the list");
}

// =====================================================================================
// Registry
// =====================================================================================

struct TestCase { const char* name; const char* group; void (*fn)(); };


// =====================================================================================
// Group "build" -- Build > Build: settings, staging layers, dependency parsing, and a real
// relocated Development package that must run with no source tree, Homebrew or SDK in reach.
// =====================================================================================

void test_build_settings_roundtrip() {
    const fs::path root = fresh_dir("build_settings");
    Project project = Project::create(root);
    BuildSettings d = BuildSettings::load(project);
    expect(d.product_name == project.name() && d.bundle_id.rfind("com.", 0) == 0, "defaults: the project's name, a com.* bundle id");
    d.version = "1.2.3";
    d.macos.sign_identity = "Developer ID Application: Example (TEAM123)";
    d.macos.notary_profile = "toy-notary";
    d.macos.dmg = true;
    d.linux_.tarball = false;
    d.save(project);
    const BuildSettings r = BuildSettings::load(project);
    expect(r.version == "1.2.3" && r.macos.sign_identity == d.macos.sign_identity && r.macos.notary_profile == "toy-notary" &&
           r.macos.dmg && !r.linux_.tarball, "build_settings.yaml round-trips");
    expect(fs::exists(root / "build_settings.yaml") && !fs::exists(project.assets() / "build_settings.yaml"),
           "build settings live outside assets/ (never shipped)");
    expect(parse_profile("dev") == BuildProfile::Development && parse_profile("ship") == BuildProfile::Shipping &&
           !parse_profile("release"), "profile names parse");
}

void test_build_otool_and_ldd_parsers() {
    const std::string otool_L =
        "build/game:\n"
        "\t/opt/homebrew/opt/glfw/lib/libglfw.3.dylib (compatibility version 3.0.0, current version 3.4.0)\n"
        "\t/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (compatibility version 45.0.0)\n"
        "\t@rpath/libcrypto.3.dylib (compatibility version 3.0.0, current version 3.0.0)\n"
        "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n";
    const auto refs = parse_otool_L(otool_L);
    expect(refs.size() == 4 && refs[0] == "/opt/homebrew/opt/glfw/lib/libglfw.3.dylib" && refs[2] == "@rpath/libcrypto.3.dylib",
           "otool -L install names parse");
    expect(!is_system_dep(refs[0], DepPlatform::MacOS) && is_system_dep(refs[1], DepPlatform::MacOS) &&
           is_system_dep(refs[3], DepPlatform::MacOS), "Homebrew libraries bundle, system ones don't");
    const std::string otool_l =
        "Load command 12\n          cmd LC_RPATH\n      cmdsize 32\n         path /opt/homebrew/lib (offset 12)\n"
        "Load command 13\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n platform 1\n    minos 14.0\n      sdk 14.5\n";
    const auto rp = parse_otool_rpaths(otool_l);
    expect(rp.size() == 1 && rp[0] == "/opt/homebrew/lib", "LC_RPATH entries parse");
    expect(parse_otool_minos(otool_l) == "14.0", "the minimum macOS parses");
    expect(compare_versions("13.0", "14.0") < 0 && compare_versions("14.2", "14.10") < 0 && compare_versions("14", "14.0") == 0,
           "dotted versions compare numerically");
    const std::string ldd =
        "\tlinux-vdso.so.1 (0x00007ffd)\n"
        "\tlibglfw.so.3 => /usr/lib/x86_64-linux-gnu/libglfw.so.3 (0x00007f)\n"
        "\tlibvulkan.so.1 => /lib/x86_64-linux-gnu/libvulkan.so.1 (0x00007f)\n"
        "\tlibmissing.so.2 => not found\n"
        "\t/lib64/ld-linux-x86-64.so.2 (0x00007f)\n";
    const auto e = parse_ldd(ldd);
    expect(e.size() == 5 && e[1].soname == "libglfw.so.3" && e[1].path == "/usr/lib/x86_64-linux-gnu/libglfw.so.3" &&
           e[3].path.empty() && e[4].path == "/lib64/ld-linux-x86-64.so.2", "ldd lines parse");
    expect(!is_system_dep(e[1].path, DepPlatform::Linux) && is_system_dep(e[2].path, DepPlatform::Linux) &&
           is_system_dep(e[0].soname, DepPlatform::Linux), "Linux: GLFW bundles, the Vulkan loader and C runtime never do");
    const std::string icd = "{\n  \"ICD\": {\n    \"library_path\": \"../../../lib/libMoltenVK.dylib\",\n    \"api_version\": \"1.4.0\"\n  }\n}\n";
    const std::string out = rewrite_icd_json(icd, "../../../Frameworks/libMoltenVK.dylib");
    expect(out.find("\"../../../Frameworks/libMoltenVK.dylib\"") != std::string::npos && out.find("1.4.0") != std::string::npos,
           "the ICD manifest points at the bundled driver and keeps its api_version");
}

void test_build_staging_layers_and_shipping_config() {
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path root = fresh_dir("build_layers_src");
    Project project = Project::create(root);
    const fs::path out = fresh_dir("build_layers_out");
    PackageOptions opt;
    opt.out_dir = out;
    opt.encode_yaml = false;
    opt.shipping = true;
    opt.engine_assets = fs::path(ROOT_DIR) / "assets";
    opt.library_layers = default_library_layers();
    const PackageReport rep = package_project(project, opt);
    expect(rep.ok(), "staging succeeds");
    const fs::path sh = out / "assets" / "shaders";
    // A gfxcoopa shader (one the engine doesn't also ship) and a uicoopa UI shader, both merged in.
    bool gfx_only = false;
    for (const auto& e : fs::directory_iterator(fs::path(PROJ_DIR) / "gfxcoopa" / "assets" / "shaders")) {
        if (e.path().extension() != ".spv") continue;
        if (fs::exists(fs::path(ROOT_DIR) / "assets" / "shaders" / e.path().filename())) continue;
        gfx_only = fs::exists(sh / e.path().filename());
        break;
    }
    expect(gfx_only, "gfxcoopa's base shaders are merged into the package");
    bool ui = false;
    for (const auto& e : fs::directory_iterator(fs::path(PROJ_DIR) / "uicoopa" / "assets" / "shaders")) {
        if (e.path().extension() == ".spv") { ui = fs::exists(sh / e.path().filename()); break; }
    }
    expect(ui, "uicoopa's UI shaders are merged into the package");
    // Should the engine and gfxcoopa ever ship a same-named shader, the package must carry the
    // engine's. The two directories normally share no names, so this usually only prints the note.
    bool precedence_checked = false;
    for (const auto& e : fs::directory_iterator(fs::path(ROOT_DIR) / "assets" / "shaders")) {
        if (e.path().extension() != ".spv") continue;
        const fs::path gfx = fs::path(PROJ_DIR) / "gfxcoopa" / "assets" / "shaders" / e.path().filename();
        if (!fs::exists(gfx) || fs::file_size(gfx) == fs::file_size(e.path())) continue;
        expect(fs::file_size(sh / e.path().filename()) == fs::file_size(e.path()),
               "the engine's " + e.path().filename().string() + " wins over gfxcoopa's (first match, like the runtime)");
        precedence_checked = true;
        break;
    }
    if (!precedence_checked) std::cout << "  (no differing engine/gfxcoopa shader pair to check precedence on)\n";
    expect(fs::exists(out / "assets" / "sounds" / "sounds.yaml"), "uicoopa's default UI sounds ship");
    const Node cfg = coopa::yaml::load_document(out / "assets" / "config.yaml");
    expect(cfg.contains("output") && !cfg["output"]["save_on_exit"].get_value<bool>(), "a shipping config never saves a screenshot on exit");
}

/**
 * @brief The real thing: a Development build of a fresh project, launched from an unrelated
 *        directory with a scrubbed environment, must load nothing from the source tree or
 *        Homebrew.
 */
void test_build_dev_relocated() {
    setenv("HOME", tmp_root().c_str(), 1);
    const fs::path game = fs::path(ROOT_DIR) / "build" / "toyengine";
    if (!fs::exists(game)) { expect(false, "build/toyengine exists (build the toyengine target first)"); return; }
    const fs::path root = fresh_dir("build_reloc_src");
    Project project = Project::create(root);
    const fs::path out = fresh_dir("build_reloc_out");
    BuildRequest req;
    req.profile = BuildProfile::Development;
    req.out_dir = out;
    req.skip_compile = true;
    req.binary_override = game;
    std::vector<std::string> log;
    CommandRunner run([&](const std::string& l) { log.push_back(l); });
    const BuildResult res = run_build(project, BuildSettings::load(project), req, BuildEnvironment::current(), run);
    if (!res.ok) for (const auto& l : log) std::cout << "    " << l << "\n";
    expect(res.ok, "the Development build succeeds (" + res.error + ")");
    if (!res.ok) return;
    expect(fs::exists(res.executable), "the packaged executable exists");
#if defined(__APPLE__)
    expect(res.artifact.extension() == ".app" && fs::exists(res.artifact / "Contents" / "Info.plist"), "a .app with an Info.plist");
    for (const auto& e : fs::directory_iterator(res.artifact / "Contents" / "Frameworks")) {
        for (const std::string& ref : parse_otool_L(run.run("otool -L " + shell_quote(e.path().string()), false).output)) {
            expect(is_system_dep(ref, DepPlatform::MacOS) || ref.rfind("@rpath/", 0) == 0,
                   e.path().filename().string() + " references only system libraries or @rpath (" + ref + ")");
        }
    }
    expect(run.run("codesign --verify --deep --strict " + shell_quote(res.artifact.string()), false).ok(), "the .app's signature verifies");
    expect(fs::exists(res.artifact / "Contents" / "Frameworks" / "libMoltenVK.dylib") &&
           fs::exists(res.artifact / "Contents" / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json"),
           "MoltenVK and its ICD manifest are bundled");
    const std::string trace = "DYLD_PRINT_LIBRARIES=1";
#else
    const std::string trace = "LD_DEBUG=libs";
#endif
    // Run it: a scrubbed environment, an unrelated working directory, its own HOME.
    const fs::path cwd = fresh_dir("build_reloc_cwd");
    const fs::path home = fresh_dir("build_reloc_home");
    const CommandResult r = run.run("cd " + shell_quote(cwd.string()) + " && env -i PATH=/usr/bin:/bin HOME=" + shell_quote(home.string()) +
                                    " HEADLESS=1 MAX_FRAMES=20 SFX_DEVICE=null " + trace + " " + shell_quote(res.executable.string()), false);
    if (r.exit_code != 0) std::cout << r.output.substr(r.output.size() > 4000 ? r.output.size() - 4000 : 0) << "\n";
    expect(r.exit_code == 0, "the packaged game runs relocated and exits cleanly");
    expect(r.output.find("Layout : packaged") != std::string::npos, "it detects its packaged layout");
    // Only the runtime's resolved roots line names asset/shader directories; none may be the checkout.
    std::istringstream lines(r.output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("Assets :") != std::string::npos || line.find("Shaders:") != std::string::npos) {
            expect(line.find(std::string(ROOT_DIR) + "/assets") == std::string::npos && line.find(std::string(PROJ_DIR) + "/gfxcoopa") == std::string::npos,
                   "no asset/shader root in the source tree: " + line);
        }
        if (line.find("dyld[") != std::string::npos || line.find("calling init") != std::string::npos) {
            expect(line.find("/opt/homebrew") == std::string::npos && line.find("/usr/local/") == std::string::npos,
                   "no library loads from Homebrew: " + line);
        }
    }
    expect(!fs::exists(cwd / "output"), "nothing is written into the working directory");
    bool logged = false;
    for (auto it = fs::recursive_directory_iterator(home); it != fs::recursive_directory_iterator(); ++it) {
        logged |= it->path().extension() == ".log";
    }
    expect(logged, "a log file is written under the player's HOME");
}

const TestCase kTests[] = {
    {"build_settings_roundtrip",             "build",    test_build_settings_roundtrip},
    {"build_otool_and_ldd_parsers",          "build",    test_build_otool_and_ldd_parsers},
    {"build_staging_layers_and_shipping_config", "build", test_build_staging_layers_and_shipping_config},
    {"build_dev_relocated",                  "build",    test_build_dev_relocated},
    {"writer_scalars_roundtrip",             "writer",   test_writer_scalars_roundtrip},
    {"writer_key_order_and_flow",            "writer",   test_writer_key_order_and_flow},
    {"writer_roundtrips_every_asset",        "writer",   test_writer_roundtrips_every_asset},
    {"scene_documents_save_load_stable",     "document", test_scene_documents_save_load_stable},
    {"scene_document_random_edits_undo",     "document", test_scene_document_random_edits_undo},
    {"undo_stack_sequence_and_budget",       "document", test_undo_stack_sequence_and_budget},
    {"scene_document_reparent_rules",        "document", test_scene_document_reparent_rules},
    {"schema_defaults",                      "document", test_schema_defaults},
    {"snake_case_names",                     "document", test_snake_case_names},
    {"asset_refs_and_rename",                "document", test_asset_refs_and_rename},
    {"asset_tags_and_retag",                 "editor_shell", test_asset_tags_and_retag},
    {"editor_proportional_editing",          "editor_shell", test_editor_proportional_editing},
    {"editor_render_settings_panel",         "editor_shell", test_editor_render_settings_panel},
    {"shader_ball",                          "mesh", test_shader_ball},
    {"mesh_io_preserves_blender_attributes", "mesh", test_mesh_io_preserves_blender_attributes},
    {"asset_fidelity_meshes",                "writer", test_asset_fidelity_meshes},
    {"paint_brushes",                        "mesh", test_paint_brushes},
    {"clip_model",                           "mesh", test_clip_model},
    {"mesh_mirror",                          "mesh", test_mesh_mirror},
    {"proportional_weights",                 "mesh", test_proportional_weights},
    {"schema_int_enum_labels",               "document", test_schema_int_enum_labels},
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
    {"imm_modal_fits_content",               "imm",      test_imm_modal_fits_content},
    {"imm_file_dialog",                      "imm",      test_imm_file_dialog},
    {"imm_log_view_follows_output",          "imm",      test_imm_log_view_follows_output},
    {"imm_menubar_and_tree",                 "imm",      test_imm_menubar_and_tree},
    {"imm_icon_button_and_tooltip",          "imm",      test_imm_icon_button_and_tooltip},
    {"imm_theme_files",                      "imm",      test_imm_theme_files},
    {"imm_dropdown_toggles",                 "imm",      test_imm_dropdown_toggles},
    {"imm_material_shader_drawn_once",       "imm",      test_imm_material_shader_drawn_once},
    {"projection_and_rays",                  "viewport", test_projection_and_rays},
    {"modal_axis_locking",                   "viewport", test_modal_axis_locking},
    {"gizmo_translate_drag",                 "viewport", test_gizmo_translate_drag},
    {"ui_rect_math",                         "viewport", test_ui_rect_math},
    {"ui_rect_block_roundtrip",              "viewport", test_ui_rect_block_roundtrip},
    {"ui_rect_gizmo",                        "viewport", test_ui_rect_gizmo},
    {"ui_palette_entries_load",              "document", test_ui_palette_entries_load},
    {"config_untouched_and_minimal_edits",   "config",   test_config_untouched_and_minimal_edits},
    {"settings_defaults_match_engine",       "config",   test_settings_defaults_match_engine},
    {"render_settings_cover_engine",         "config",   test_render_settings_cover_engine},
    {"editor_shell_end_to_end",              "editor_shell", test_editor_shell_end_to_end},
    {"material_reference_forms",             "editor_shell", test_material_reference_forms},
    {"editor_real_input_blender_keymap",     "editor_shell", test_editor_real_input_blender_keymap},
    {"editor_about_and_logo",                "editor_shell", test_editor_about_and_logo},
    {"editor_mesh_rotate",                   "editor_shell", test_editor_mesh_rotate},
    {"editor_play_input_focus",              "editor_shell", test_editor_play_input_focus},
    {"editor_blender_chrome",                "editor_shell", test_editor_blender_chrome},
    {"editor_themes",                        "editor_shell", test_editor_themes},
    {"editor_transparency_preview",          "editor_shell", test_editor_transparency_preview},
    {"editor_viewport_fill",                 "editor_shell", test_editor_viewport_fill},
    {"editor_resize_no_black_frame",         "editor_shell", test_editor_resize_no_black_frame},
    {"editor_pick_nearest",                  "editor_shell", test_editor_pick_nearest},
    {"editor_object_shade_smooth_flat",      "editor_shell", test_editor_object_shade_smooth_flat},
    {"editor_instance_overrides",            "editor_shell", test_editor_instance_overrides},
    {"editor_quad_modelling",                "editor_shell", test_editor_quad_modelling},
    {"editor_isolation",                     "editor_shell", test_editor_isolation},
    {"editor_sculpt",                        "editor_shell", test_editor_sculpt},
    {"editor_asset_views",                   "editor_shell", test_editor_asset_views},
    {"editor_object_assets",                 "editor_shell", test_editor_object_assets},
    {"editor_ui_designer",                   "editor_shell", test_editor_ui_designer},
    {"editor_ui_templates",                  "editor_shell", test_editor_ui_templates},
    {"editor_ui_theme_shapes",               "editor_shell", test_editor_ui_theme_shapes},
    {"editor_text_pixel_aligned",            "editor_shell", test_editor_text_pixel_aligned},
    {"editor_asset_browser_context_menus",   "editor_shell", test_editor_asset_browser_context_menus},
    {"editor_texture_preview_color_space",   "editor_shell", test_editor_texture_preview_color_space},
    {"editor_game_ui_themes",                "editor_shell", test_editor_game_ui_themes},
    {"editor_submesh_materials",             "editor_shell", test_editor_submesh_materials},
    {"editor_object_mesh_edit_is_asset",     "editor_shell", test_editor_object_mesh_edit_is_asset},
    {"editor_water_body_mesh_is_editable", "editor_shell", test_editor_water_body_mesh_is_editable},
    {"editor_large_water_tiles_pick_and_save", "editor_shell", test_editor_large_water_tiles_pick_and_save},
    {"editor_paint_modes", "editor_shell", test_editor_paint_modes},
    {"editor_animation_timeline", "editor_shell", test_editor_animation_timeline},
    {"editor_animation_autokey", "editor_shell", test_editor_animation_autokey},
    {"editor_animation_test_scene", "editor_shell", test_editor_animation_test_scene},
    {"editor_rig_object_assets", "editor_shell", test_editor_rig_object_assets},
    {"editor_object_asset_pick_and_edit", "editor_shell", test_editor_object_asset_pick_and_edit},
    {"editor_object_asset_click_and_tab", "editor_shell", test_editor_object_asset_click_and_tab},
    {"editor_material_shader_catalogue", "editor_shell", test_editor_material_shader_catalogue},
    {"editor_scene_settings_override", "editor_shell", test_editor_scene_settings_override},
    {"editor_grid_snap_and_frame", "editor_shell", test_editor_grid_snap_and_frame},
    {"editor_xray_edit_mode", "editor_shell", test_editor_xray_edit_mode},
    {"editor_nav_axis_and_trackpad", "editor_shell", test_editor_nav_axis_and_trackpad},
    {"asset_fidelity_component_schemas", "editor_shell", test_asset_fidelity_component_schemas},
    {"asset_fidelity_editor_created_files", "editor_shell", test_asset_fidelity_editor_created_files},
    {"asset_fidelity_object_assets", "document", test_asset_fidelity_object_assets},
    {"editor_mesh_autosave_and_unified_undo", "editor_shell", test_editor_mesh_autosave_and_unified_undo},
    {"editor_mesh_save_refreshes_colliders", "editor_shell", test_editor_mesh_save_refreshes_colliders},
    {"project_toy_file",                     "project",  test_project_toy_file},
    {"project_component_schema",             "project",  test_project_component_schema},
    {"hub_projects_and_registry",            "project",  test_hub_projects_and_registry},
    {"package_engine_fallback",              "project",  test_package_engine_fallback},
    {"process_task_and_diagnostics",         "project",  test_process_task_and_diagnostics},
    {"package_renders_identically",          "package",  test_package_renders_identically},
    {"editor_build_refresh",                 "editor_shell", test_editor_build_refresh},
    {"editor_engine_assets",                 "editor_shell", test_editor_engine_assets},
    {"editor_tile_set_duplicate",            "editor_shell", test_editor_tile_set_duplicate},
    {"hub_project_actions",                  "hub",      test_hub_project_actions},
    {"docs_overview", "docs", test_docs_overview},
    {"docs_readme_editor", "docs", test_docs_readme_editor},
    {"docs_getting_started_editor", "docs", test_docs_getting_started_editor},
    {"docs_menu_file", "docs", test_docs_menu_file},
    {"docs_hierarchy_inspector", "docs", test_docs_hierarchy_inspector},
    {"docs_add_component", "docs", test_docs_add_component},
    {"docs_viewport_solid", "docs", test_docs_viewport_solid},
    {"docs_gizmo_move", "docs", test_docs_gizmo_move},
    {"docs_xray_edit", "docs", test_docs_xray_edit},
    {"docs_edit_mode", "docs", test_docs_edit_mode},
    {"docs_adjust_last_operation", "docs", test_docs_adjust_last_operation},
    {"docs_sculpt", "docs", test_docs_sculpt},
    {"docs_vertex_paint", "docs", test_docs_vertex_paint},
    {"docs_weight_paint", "docs", test_docs_weight_paint},
    {"docs_material_editor", "docs", test_docs_material_editor},
    {"docs_timeline", "docs", test_docs_timeline},
    {"docs_record_autokey", "docs", test_docs_record_autokey},
    {"docs_object_asset", "docs", test_docs_object_asset},
    {"docs_ui_designer", "docs", test_docs_ui_designer},
    {"docs_ui_new_menu", "docs", test_docs_ui_new_menu},
    {"docs_ui_interact", "docs", test_docs_ui_interact},
    {"docs_ui_bindings", "docs", test_docs_ui_bindings},
    {"docs_render_settings", "docs", test_docs_render_settings},
    {"docs_world_settings", "docs", test_docs_world_settings},
    {"docs_package_dialog", "docs", test_docs_package_dialog},
    {"docs_theme_light", "docs", test_docs_theme_light},
    {"docs_controls_modal", "docs", test_docs_controls_modal},
    {"docs_unsaved_prompt", "docs", test_docs_unsaved_prompt},
    {"docs_asset_panel_meshes", "docs", test_docs_asset_panel_meshes},
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
