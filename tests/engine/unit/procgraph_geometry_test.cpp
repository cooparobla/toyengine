/**
 * @file procgraph_geometry_test.cpp
 * @brief GeometrySet's copy-on-write sharing, and MeshGeometry's own operations.
 *
 * The bake pipeline is covered by procgraph_bake_test; scene bindings by
 * procgraph_scene_input_test.
 */

#include <coopa/testing/test.h>

#include <string>

#include "engine/support/checks.h"

#include <toyengine/procgraph/geometry_set.h>
#include <toyengine/procgraph/proc_types.h>

COOPA_TEST_SUITE("procgraph_geometry");

using namespace toy::procgraph;
using coopa::test::expect;
using coopa::test::expect_near;

namespace {

/// @brief A single triangle in the XY plane, the smallest non-empty mesh.
MeshGeometry one_triangle(float z = 0.0f) {
    MeshGeometry m;
    for (const glm::vec3& p : {glm::vec3(0.0f, 0.0f, z), glm::vec3(1.0f, 0.0f, z),
                               glm::vec3(0.0f, 1.0f, z)}) {
        Vertex v;
        v.position = p;
        v.normal   = glm::vec3(0.0f, 0.0f, 1.0f);
        v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2};
    return m;
}

} // namespace

COOPA_TEST(a_geometry_set_fits_a_value_inline_so_a_socket_copy_is_a_refcount_bump) {
    expect(sizeof(GeometrySet) <= coopa::nodegraph::Value::k_inline_bytes,
           "procgraph: GeometrySet is " + std::to_string(sizeof(GeometrySet)) +
           " bytes, within a Value's " +
           std::to_string(coopa::nodegraph::Value::k_inline_bytes) +
           "-byte inline budget. If it ever grows past that, EVERY socket copy starts allocating");
}

COOPA_TEST(copying_a_geometry_set_shares_the_payload_and_writing_clones_it) {
    GeometrySet a = GeometrySet::from_mesh(one_triangle());
    expect(!a.shared(), "procgraph: a fresh set is not shared");

    GeometrySet b = a;
    expect(a.shared() && b.shared(), "procgraph: a copy shares the payload");
    expect(a.mesh() == b.mesh(),
           "procgraph: both point at the SAME mesh -- fanning geometry out to four nodes must not "
           "copy the vertices four times");

    b.mutable_data().mesh->vertices[0].position.x = 99.0f;
    expect(!b.shared(), "procgraph: writing through one holder cloned it");
    expect_near(a.mesh()->vertices[0].position.x, 0.0f, 1e-6f,
                "procgraph: the other holder is untouched by the write -- copy-on-write, so a node "
                "that modifies its input cannot corrupt a sibling node's view of it");
    expect_near(b.mesh()->vertices[0].position.x, 99.0f, 1e-6f, "procgraph: the writer saw its edit");
}

COOPA_TEST(an_empty_geometry_set_is_safe_to_query) {
    GeometrySet empty;
    expect(empty.empty(), "procgraph: a default set is empty");
    expect(empty.mesh() == nullptr && empty.points() == nullptr,
           "procgraph: its accessors return null rather than dereferencing nothing");
    expect(empty.instances().empty(), "procgraph: and its instance list is an empty vector");
    expect(empty.describe() == "empty", "procgraph: it describes itself without crashing");

    // Merging into and from an empty set must both be no-ops rather than faults.
    GeometrySet target;
    target.merge(empty);
    expect(target.empty(), "procgraph: merging an empty set leaves it empty");
}

COOPA_TEST(appending_meshes_offsets_indices_and_merges_slots_by_name) {
    MeshGeometry a = one_triangle(0.0f);
    a.slot_names = {"stone"};
    a.face_slots = {0};

    MeshGeometry b = one_triangle(1.0f);
    b.slot_names = {"wood"};
    b.face_slots = {0};

    a.append(b);

    expect(a.vertices.size() == 6, "procgraph: both triangles' vertices are present");
    expect(a.triangle_count() == 2, "procgraph: and both triangles");
    expect(a.indices[3] == 3 && a.indices[4] == 4 && a.indices[5] == 5,
           "procgraph: the appended triangle's indices were offset into the combined vertex array");

    expect(a.slot_names.size() == 2, "procgraph: the two distinct materials became two slots");
    expect(a.slot_of(0) == 0 && a.slot_of(1) == 1,
           "procgraph: slots are merged BY NAME, not by index. Two meshes built independently both "
           "number their slots from 0, so concatenating the index spaces would silently repaint "
           "one of them with the other's material");
    expect(a.slot_names[0] == "stone" && a.slot_names[1] == "wood",
           "procgraph: and both names survived in order");

    // Appending the same name again must reuse the slot rather than duplicate it.
    MeshGeometry c = one_triangle(2.0f);
    c.slot_names = {"stone"};
    c.face_slots = {0};
    a.append(c);
    expect(a.slot_names.size() == 2,
           "procgraph: appending a mesh whose material is already present reuses that slot");
    expect(a.slot_of(2) == 0, "procgraph: and its triangle points at the existing slot");
}

COOPA_TEST(to_cpu_data_sorts_triangles_into_contiguous_parts_per_slot) {
    MeshGeometry m;
    // Three triangles, interleaved slots: 0, 1, 0. The parts must come out contiguous anyway.
    for (int i = 0; i < 3; ++i) {
        MeshGeometry t = one_triangle(static_cast<float>(i));
        m.append(t);
    }
    m.slot_names = {"a", "b"};
    m.face_slots = {0, 1, 0};

    const MeshCpuData cpu = m.to_cpu_data();

    expect(cpu.vertices.size() == 9, "procgraph: every vertex came through");
    expect(cpu.indices.size() == 9, "procgraph: and every index");
    expect(cpu.slot_names.size() == 2, "procgraph: both slot names survived");
    expect(cpu.lods.size() == 1, "procgraph: one LOD level, the full mesh");
    expect(cpu.lods[0].parts.size() == 2, "procgraph: one part per material slot");

    const auto& parts = cpu.lods[0].parts;
    expect(parts[0].first_index == 0 && parts[0].index_count == 6,
           "procgraph: slot a's two interleaved triangles were gathered into ONE contiguous range "
           "-- which is what MeshLod::parts means and what lets the renderer issue one draw per "
           "material");
    expect(parts[1].first_index == 6 && parts[1].index_count == 3,
           "procgraph: slot b's single triangle follows it, also contiguous");
    expect(parts[0].index_count + parts[1].index_count == cpu.indices.size(),
           "procgraph: the parts cover every index with no gap and no overlap");
}

COOPA_TEST(to_cpu_data_computes_bounds_from_the_vertices) {
    MeshGeometry m = one_triangle();
    m.vertices[2].position = glm::vec3(0.0f, 4.0f, -2.0f);

    const MeshCpuData cpu = m.to_cpu_data();
    expect_near(cpu.bounds_min.z, -2.0f, 1e-6f, "procgraph: bounds_min saw the lowest z");
    expect_near(cpu.bounds_max.y, 4.0f, 1e-6f, "procgraph: bounds_max saw the highest y");
    expect_near(cpu.bounds_min.x, 0.0f, 1e-6f, "procgraph: and the x range is right");
}

COOPA_TEST(recomputing_normals_area_weights_and_leaves_a_usable_tangent) {
    MeshGeometry m = one_triangle();
    for (auto& v : m.vertices) v.normal = glm::vec3(0.0f);

    m.recompute_normals();

    for (size_t i = 0; i < m.vertices.size(); ++i) {
        expect_near(glm::length(m.vertices[i].normal), 1.0f, 1e-5f,
                    "procgraph: vertex " + std::to_string(i) + "'s normal is unit length");
        expect_near(m.vertices[i].normal.z, 1.0f, 1e-5f,
                    "procgraph: a triangle in the XY plane gets a +Z normal");
        expect_near(glm::dot(glm::vec3(m.vertices[i].tangent), m.vertices[i].normal), 0.0f, 1e-5f,
                    "procgraph: the tangent is perpendicular to the normal -- a zero or parallel "
                    "tangent breaks normal mapping on generated geometry");
        expect_near(glm::length(glm::vec3(m.vertices[i].tangent)), 1.0f, 1e-5f,
                    "procgraph: and it is unit length");
    }
}

COOPA_TEST(a_degenerate_triangle_does_not_produce_a_nan_normal) {
    MeshGeometry m;
    for (int i = 0; i < 3; ++i) {
        Vertex v;
        v.position = glm::vec3(1.0f, 2.0f, 3.0f);   // All three corners identical.
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2};
    m.recompute_normals();
    for (const auto& v : m.vertices) {
        expect(std::isfinite(v.normal.x) && std::isfinite(v.normal.y) && std::isfinite(v.normal.z),
               "procgraph: a zero-area triangle falls back to a fixed normal rather than "
               "normalizing a zero vector -- one NaN vertex would poison the whole mesh's bounds");
        expect_near(glm::length(v.normal), 1.0f, 1e-5f, "procgraph: and it is still unit length");
    }
}

COOPA_TEST(merging_sets_combines_instance_groups_sharing_a_prototype) {
    InstanceGroup rocks;
    rocks.prototype = "objects/rock";
    rocks.transforms = {glm::mat4(1.0f), glm::mat4(1.0f)};

    InstanceGroup more_rocks;
    more_rocks.prototype = "objects/rock";
    more_rocks.transforms = {glm::mat4(1.0f)};

    InstanceGroup trees;
    trees.prototype = "objects/tree";
    trees.transforms = {glm::mat4(1.0f)};

    GeometrySet a = GeometrySet::from_instances(rocks);
    a.merge(GeometrySet::from_instances(more_rocks));
    a.merge(GeometrySet::from_instances(trees));

    expect(a.instances().size() == 2,
           "procgraph: groups are combined by prototype, so a hundred Instance-on-Points nodes "
           "over one asset publish one batch rather than a hundred");
    size_t rock_count = 0;
    for (const auto& g : a.instances()) {
        if (g.prototype == "objects/rock") rock_count = g.size();
    }
    expect(rock_count == 3, "procgraph: all three rock transforms ended up in the one group");
}

COOPA_TEST(merging_point_clouds_densifies_the_optional_frames) {
    PointCloud plain;
    plain.positions = {glm::vec3(0.0f), glm::vec3(1.0f)};   // No rotations, no scales.

    PointCloud framed;
    framed.positions = {glm::vec3(2.0f)};
    framed.scales    = {glm::vec3(5.0f)};

    GeometrySet a = GeometrySet::from_points(plain);
    a.merge(GeometrySet::from_points(framed));

    const PointCloud* merged = a.points();
    expect(merged != nullptr && merged->size() == 3, "procgraph: all three points are present");
    expect(merged->scales.size() == 3,
           "procgraph: the scale array was densified when one side had it. Leaving it short would "
           "make scale_of() read the later points as unit scale, silently losing the 5x");
    expect_near(merged->scale_of(0).x, 1.0f, 1e-6f, "procgraph: the unscaled points read as 1");
    expect_near(merged->scale_of(2).x, 5.0f, 1e-6f, "procgraph: and the scaled one kept its value");
}
