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
        if (name == "Plane" || name == "Tile Side") {
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
        app.set_tab(Tab::Layout);
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
        expect(app.tab() == Tab::Modeling && app.mesh_document().mesh.faces.size() == 6, "a new cube mesh opens in the Modeling workspace");
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
        app.set_tab(Tab::Layout);
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
        toy::core::Engine engine(cfg, shell_options(project));
        EditorApp app(engine, project, {}, std::move(restart_state));
        tick(engine, 3);
        expect(engine.pipeline().render_width() == 320, "a restart applies startup-only render settings");
        expect(app.document().dirty() && app.document().find_component(sphere_id_for_restart(app), "MeshRenderer") >= 0,
               "unsaved scene edits survive the restart");
        app.set_tab(Tab::Layout);
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
    app.set_tab(Tab::Shading);
    tick(engine, 4);
    dump(engine, "07_shading_workspace");
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
    {"imm_button_and_checkbox",              "imm",      test_imm_button_and_checkbox},
    {"imm_text_input_commits",               "imm",      test_imm_text_input_commits},
    {"imm_drag_float_and_popup_blocking",    "imm",      test_imm_drag_float_and_popup_blocking},
    {"imm_menubar_and_tree",                 "imm",      test_imm_menubar_and_tree},
    {"imm_icon_button_and_tooltip",          "imm",      test_imm_icon_button_and_tooltip},
    {"projection_and_rays",                  "viewport", test_projection_and_rays},
    {"gizmo_translate_drag",                 "viewport", test_gizmo_translate_drag},
    {"config_untouched_and_minimal_edits",   "config",   test_config_untouched_and_minimal_edits},
    {"editor_shell_end_to_end",              "editor_shell", test_editor_shell_end_to_end},
    {"material_reference_forms",             "editor_shell", test_material_reference_forms},
    {"editor_real_input_blender_keymap",     "editor_shell", test_editor_real_input_blender_keymap},
    {"editor_blender_chrome",                "editor_shell", test_editor_blender_chrome},
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
