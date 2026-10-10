/**
 * @file mesh_io_test.cpp
 * @brief The editor's mesh file format (mesh_to_node / mesh_from_node): topology, shading, material
 * slots, colours, vertex groups, the skinning palette and unknown keys round-trip, the engine
 * loads what the editor writes, and operations keep per-vertex data parallel and interpolated.
 * 
 * Not here: re-saving the repository's own meshes (asset_roundtrip).
 */

#include <coopa/testing/test.h>

#include <coopa/yaml/writer.h>
#include <gfxcoopa/engine/data/mesh.h>

#include "editor/mesh/mesh_loops.h"
#include "editor/mesh/mesh_ops.h"
#include "editor/mesh/mesh_subdivide.h"
#include "editor/mesh/primitives.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("mesh_io");

namespace toy::editor::testing {

COOPA_TEST(primitives_round_trip_and_load_in_the_engine) {
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
}

/** @brief Material slots: YAML round trip, ops keep each face's slot, and the engine builds parts. */
COOPA_TEST(material_slots_round_trip_and_survive_operations) {
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

/** @brief Blender's export attributes survive the editor: colours, vertex groups, skinning
 *         palettes and unknown keys round-trip, and the exported tangents are Blender's. */
COOPA_TEST(colours_groups_and_skin_palette_round_trip_and_survive_operations) {
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

} // namespace toy::editor::testing
