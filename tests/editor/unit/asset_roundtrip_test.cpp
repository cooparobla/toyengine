/**
 * @file asset_roundtrip_test.cpp
 * @brief The editor never loses data in the repository's own assets: every YAML file re-emits
 * byte-stable, every mesh re-saves to the identical drawn mesh, every scene saves exactly as it was
 * loaded, and an object asset keeps keys the editor does not know.
 * 
 * These sweep the whole assets/ tree, so this is the slowest device-free suite (a few seconds);
 * synthetic round trips of single features live with their systems (mesh_io, clip_model, ...).
 */

#include <coopa/testing/test.h>

#include <fstream>
#include <set>

#include <coopa/yaml/writer.h>
#include <gfxcoopa/engine/data/mesh.h>

#include "editor/app/project.h"
#include "editor/core/scene_document.h"
#include "editor/mesh/edit_mesh.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("asset_roundtrip");

namespace toy::editor::testing {

COOPA_TEST(every_asset_file_re_emits_byte_stable) {
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
COOPA_TEST(every_mesh_asset_resaves_to_the_identical_drawn_mesh) {
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

COOPA_TEST(every_scene_saves_exactly_as_loaded) {
    const fs::path dir = coopa::test::scratch_dir("doc_roundtrip");
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
COOPA_TEST(an_object_asset_keeps_unknown_keys_on_save) {
    const fs::path dir = coopa::test::scratch_dir("object_assets");
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

} // namespace toy::editor::testing
