/**
 * @file mesh_build_test.cpp
 * @brief Mesh::build_cpu (gfxcoopa's mesh loader as the engine drives it): corner welding,
 *        generated LOD chains, and material slots regrouped into contiguous parts per LOD.
 *        The simplifier's quality is not measured -- only that each level is a real, smaller,
 *        well-formed subset.
 */

#include <coopa/testing/test.h>

#include <fstream>
#include <iostream>
#include <string>

#include <fkYAML/node.hpp>
#include <gfxcoopa/engine/data/mesh.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("mesh_build");

namespace {

/// A flat n x n grid of quads in mesh-YAML form, with SHARED vertex indices across faces
/// (the welded topology the loader should recover) but emitted per-corner by the parser.
std::string make_grid_mesh_yaml(int n) {
    std::string y = "vertices:\n";
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            y += "  - [" + std::to_string(i) + ".0, " + std::to_string(j) + ".0, 0.0]\n";
    y += "normals:\n";
    for (int k = 0; k < (n + 1) * (n + 1); ++k) y += "  - [0.0, 0.0, 1.0]\n";
    y += "uvs:\n";
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            y += "  - [" + std::to_string(i) + ".0, " + std::to_string(j) + ".0]\n";
    y += "faces:\n";
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int a = j * (n + 1) + i;
            y += "  - [" + std::to_string(a) + ", " + std::to_string(a + 1) + ", " +
                 std::to_string(a + n + 2) + ", " + std::to_string(a + n + 1) + "]\n";
        }
    return y;
}

} // namespace

COOPA_TEST(welds_corners_and_generates_a_lod_chain) {
    using coopa::gfx::engine::data::Mesh;
    const int n = 8;
    const fkyaml::node node = fkyaml::node::deserialize(make_grid_mesh_yaml(n));

    const auto plain = Mesh::build_cpu(node);
    expect(plain.indices.size() == static_cast<size_t>(n * n * 6), "welding keeps every triangle");
    expect(plain.vertices.size() == static_cast<size_t>((n + 1) * (n + 1)),
           "per-corner vertices weld back to one per grid point");
    expect(plain.lods.size() == 1, "no lods block -> a single LOD covering the whole mesh");
    expect(plain.lods[0].index_count == plain.indices.size(), "LOD 0 is the full mesh");
    expect_near(plain.bounds_max.x, static_cast<float>(n), 1e-6f, "bounds survive welding");

    const fkyaml::node cfg = fkyaml::node::deserialize(std::string(
        "lods:\n  - { ratio: 0.5, screen_size: 0.2 }\ncull_screen_size: 0.01\n"));
    const auto lodded = Mesh::build_cpu(node, &cfg);
    expect(lodded.lods.size() == 2, "one ratio level -> two LODs");
    expect(lodded.lods[1].first_index == lodded.lods[0].index_count,
           "LOD 1's indices follow LOD 0's in the shared index buffer");
    expect(lodded.lods[1].index_count < lodded.lods[0].index_count, "LOD 1 has fewer triangles");
    expect(lodded.lods[1].index_count % 3 == 0, "LOD 1 is whole triangles");
    expect_near(lodded.lods[1].screen_size, 0.2f, 1e-6f, "screen_size is carried through");
    expect_near(lodded.cull_screen_size, 0.01f, 1e-6f, "cull_screen_size is carried through");
    expect(lodded.vertices.size() == plain.vertices.size(), "a ratio level shares LOD 0's vertices");
}

/** @brief Material slots (submeshes): triangles grouped by slot into contiguous parts, per LOD. */
COOPA_TEST(material_slots_become_contiguous_parts_per_lod) {
    using coopa::gfx::engine::data::Mesh;
    const int n = 8;
    // Left half of the grid (i < 4) is slot 0 "body", right half slot 1 "glass", interleaved in
    // file order so the build has to regroup them.
    std::string y = make_grid_mesh_yaml(n);
    y += "material_slots: [body, glass]\nface_materials:\n";
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) y += std::string("  - ") + (i < n / 2 ? "0" : "1") + "\n";
    const fkyaml::node node = fkyaml::node::deserialize(y);
    const auto m = Mesh::build_cpu(node);
    expect(m.slot_names.size() == 2 && m.slot_names[1] == "glass", "slot names are kept");
    expect(m.lods[0].parts.size() == 2, "LOD 0 has one part per slot");
    const auto& p0 = m.lods[0].parts[0];
    const auto& p1 = m.lods[0].parts[1];
    expect(p0.first_index == 0 && p0.index_count == static_cast<uint32_t>(n * n / 2 * 6) &&
               p1.first_index == p0.index_count && p0.index_count + p1.index_count == m.lods[0].index_count,
           "the parts are contiguous and cover the whole level");
    bool left = true, right = true;
    for (uint32_t k = p0.first_index; k < p0.first_index + p0.index_count; ++k) left &= m.vertices[m.indices[k]].position.x <= n / 2 + 1e-4f;
    for (uint32_t k = p1.first_index; k < p1.first_index + p1.index_count; ++k) right &= m.vertices[m.indices[k]].position.x >= n / 2 - 1e-4f;
    expect(left && right, "each part holds exactly its slot's triangles");

    const fkyaml::node cfg = fkyaml::node::deserialize(std::string("lods:\n  - { ratio: 0.5, screen_size: 0.2 }\n"));
    const auto l = Mesh::build_cpu(node, &cfg);
    expect(l.lods.size() == 2 && l.lods[1].parts.size() == 2, "a simplified LOD keeps one part per slot");
    uint32_t sum = 0;
    for (const auto& p : l.lods[1].parts) sum += p.index_count;
    expect(sum == l.lods[1].index_count && l.lods[1].parts[0].first_index == l.lods[1].first_index,
           "LOD 1's parts cover its range");

    const auto plain = Mesh::build_cpu(fkyaml::node::deserialize(make_grid_mesh_yaml(n)));
    expect(plain.lods[0].parts.empty() && plain.slot_names.empty(), "a mesh without slots has a single implicit part");
}

COOPA_TEST(lod_generation_reduces_a_flat_shaded_mesh) {
    using coopa::gfx::engine::data::Mesh;
    // The shared sphere mesh is exported flat-shaded: every face has its own normals, so after
    // welding no two triangles share a vertex. LOD generation must still reduce it.
    std::ifstream in(std::string(ROOT_DIR) + "/assets/meshes/primitives/sphere.yaml");
    expect(static_cast<bool>(in), "meshes/sphere.yaml opens");
    if (!in) return;
    const fkyaml::node node = fkyaml::node::deserialize(in);
    const fkyaml::node cfg = fkyaml::node::deserialize(std::string(
        "lods:\n  - { ratio: 0.5, screen_size: 0.2 }\n  - { ratio: 0.2, screen_size: 0.05 }\n"));
    const auto m = Mesh::build_cpu(node, &cfg);
    expect(m.lods.size() == 3, "two ratio levels -> three LODs");
    if (m.lods.size() != 3) return;
    if (coopa::test::verbose()) std::cout << "    sphere LOD index counts: " << m.lods[0].index_count << " / "
              << m.lods[1].index_count << " / " << m.lods[2].index_count << "\n";
    expect(m.lods[1].index_count <= m.lods[0].index_count * 6 / 10, "LOD 1 is roughly half of LOD 0");
    expect(m.lods[2].index_count < m.lods[1].index_count, "LOD 2 is coarser than LOD 1");
}
