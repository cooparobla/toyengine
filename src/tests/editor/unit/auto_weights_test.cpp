/**
 * @file auto_weights_test.cpp
 * @brief Binding a mesh to a rig in the editor (editor/mesh/auto_weights.h) and the vertex-group
 * tools that follow it: groups are named after the bones, the file's own skinning palette is
 * dropped so the generated weights are the ones the engine reads, the result survives the YAML
 * round trip, and Limit Total / Clean / Smooth / Mirror / Invert / Levels each do their job.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/auto_weights.h"
#include "editor/mesh/paint.h"
#include "editor/mesh/primitives.h"
#include "editor/mesh/sculpt.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("auto_weights");

namespace toy::editor::testing {

using coopa::test::expect_near;
using coopa::test::require;

namespace {

/** @brief One straight bone from `head` to `tail`. */
BoneSegments bone(const std::string& name, const glm::vec3& head, const glm::vec3& tail) {
    return BoneSegments{name, {{head, tail}}};
}

/** @brief A two-bone chain along Z spanning a cylinder centred on the origin. */
std::vector<BoneSegments> two_bone_chain(float height) {
    return {bone("Upper", {0, 0, -height * 0.5f}, {0, 0, 0}), bone("Lower", {0, 0, 0}, {0, 0, height * 0.5f})};
}

/** @brief Vertex `v`'s total weight across every group. */
float weight_sum(const EditMesh& m, uint32_t v) {
    float sum = 0.0f;
    if (v < m.weights.size()) for (const auto& w : m.weights[v]) sum += w.weight;
    return sum;
}

/** @brief Index of vertex group `name`, or -1. */
int group_of(const EditMesh& m, const std::string& name) {
    const auto it = std::find(m.groups.begin(), m.groups.end(), name);
    return (it == m.groups.end()) ? -1 : static_cast<int>(it - m.groups.begin());
}

} // namespace

/**
 * @brief An automatic bind names a group after every bone, weights every vertex to the bone it
 *        sits on, drops the file's explicit palette (which would otherwise outrank the groups at
 *        load) and survives the round trip to the mesh document and back.
 */
COOPA_TEST(automatic_bind_writes_bone_named_groups) {
    EditMesh m = make_cylinder(0.3f, 2.0f, 16, true);
    // An exporter's palette, which the engine would prefer over any vertex groups.
    m.joints.assign(m.positions.size(), glm::ivec4(7, -1, -1, -1));
    m.joint_weights.assign(m.positions.size(), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    m.passthrough["inverse_bind_matrices"] = Node::sequence();

    const AutoWeightReport report = apply_auto_weights(m, two_bone_chain(2.0f));
    expect(report.groups == 2 && group_of(m, "Upper") == 0 && group_of(m, "Lower") == 1,
           "bind: one vertex group per bone, named after it");
    expect(report.weighted_vertices == m.positions.size(), "bind: every vertex comes out weighted");
    expect(report.unsolved.empty() && !report.fell_back_to_envelope, "bind: the heat solve settles on a cylinder");
    expect(m.joints.empty() && m.joint_weights.empty() && !m.passthrough.contains("inverse_bind_matrices"),
           "bind: the file's own skinning palette is dropped, so the groups are what the engine reads");

    bool weighted = true;
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        weighted &= std::fabs(weight_sum(m, v) - 1.0f) < 1e-4f && m.weights[v].size() <= 4;
    }
    expect(weighted, "bind: every vertex is normalised and within the engine's four influences");

    // A vertex at each end follows the bone over it.
    uint32_t top = 0, bottom = 0;
    for (uint32_t v = 1; v < m.positions.size(); ++v) {
        if (m.positions[v].z > m.positions[top].z) top = v;
        if (m.positions[v].z < m.positions[bottom].z) bottom = v;
    }
    expect(m.weights[bottom].front().group == 0 && m.weights[top].front().group == 1,
           "bind: each end of the cylinder follows the bone it sits on");

    // The round trip re-welds in corner order, so vertices and groups come back renumbered:
    // match them up by position and by name.
    const EditMesh back = mesh_from_node(mesh_to_node(m));
    bool round_trips = back.positions.size() == m.positions.size() &&
                       group_of(back, "Upper") >= 0 && group_of(back, "Lower") >= 0;
    for (uint32_t bv = 0; bv < back.positions.size() && round_trips; ++bv) {
        uint32_t mv = 0;
        for (uint32_t v = 1; v < m.positions.size(); ++v) {
            if (glm::length(m.positions[v] - back.positions[bv]) < glm::length(m.positions[mv] - back.positions[bv])) mv = v;
        }
        for (const std::string& name : {std::string("Upper"), std::string("Lower")}) {
            round_trips &= std::fabs(back.weight(bv, static_cast<uint32_t>(group_of(back, name))) -
                                     m.weight(mv, static_cast<uint32_t>(group_of(m, name)))) < 1e-4f;
        }
    }
    expect(round_trips, "bind: the groups and weights survive the mesh document round trip");
    expect(back.joints.empty(), "bind: and the dropped palette stays dropped on disk");

    // Re-binding replaces rather than piles up.
    const EditMesh once = m;
    apply_auto_weights(m, two_bone_chain(2.0f));
    expect(m.groups == once.groups && m.weights == once.weights, "bind: running it again is idempotent");
}

/** @brief "With Empty Groups" makes the groups and nothing else -- a mesh to paint by hand. */
COOPA_TEST(empty_groups_bind_makes_groups_without_weights) {
    EditMesh m = make_cylinder(0.3f, 2.0f, 12, false);
    const AutoWeightReport report = add_empty_bone_groups(m, two_bone_chain(2.0f));
    expect(report.groups == 2 && m.groups.size() == 2, "empty bind: the bones' groups exist");
    bool unweighted = true;
    for (uint32_t v = 0; v < m.positions.size(); ++v) unweighted &= weight_sum(m, v) == 0.0f;
    expect(unweighted, "empty bind: with no weights in them");
}

/**
 * @brief A bind leaves a mesh's own non-bone groups alone, and says so -- they still count
 *        toward the renderer's per-vertex weight sum.
 */
COOPA_TEST(a_bind_reports_groups_no_bone_claims) {
    EditMesh m = make_cylinder(0.3f, 2.0f, 12, false);
    const uint32_t mask = m.group_index("DissolveMask");
    m.sync_vertex_data();
    m.set_weight(0, mask, 0.5f);
    const AutoWeightReport report = apply_auto_weights(m, two_bone_chain(2.0f));
    expect(report.foreign_groups.size() == 1 && report.foreign_groups.front() == "DissolveMask",
           "bind: a group no bone claims is reported");
    expect(m.weight(0, mask) == 0.5f, "bind: ...and its weights are left untouched");
}

/** @brief Limit Total keeps the strongest few influences and hands the dropped weight to them. */
COOPA_TEST(limit_total_keeps_the_strongest_influences) {
    EditMesh m = make_grid(1, 1, 2.0f);
    m.sync_vertex_data();
    for (int g = 0; g < 6; ++g) m.set_weight(0, m.group_index("B" + std::to_string(g)), 0.1f * static_cast<float>(g + 1));
    const float before = weight_sum(m, 0);
    limit_total_weights(m, 4);
    expect(m.weights[0].size() == 4, "limit total: four influences left");
    expect_near(weight_sum(m, 0), before, 1e-4f, "limit total: the vertex keeps the same total pull");
    bool strongest = true;
    for (const auto& w : m.weights[0]) strongest &= w.group >= 2;
    expect(strongest, "limit total: the two faintest are the ones dropped");
}

/** @brief Clean drops the faintest weights, from one group or from all of them. */
COOPA_TEST(clean_drops_the_faintest_weights) {
    EditMesh m = make_grid(1, 1, 2.0f);
    m.sync_vertex_data();
    const uint32_t a = m.group_index("A"), b = m.group_index("B");
    m.set_weight(0, a, 0.01f);
    m.set_weight(0, b, 0.5f);
    EditMesh one = m;
    clean_weights(one, 0.05f, a);
    expect(one.weight(0, a) == 0.0f && one.weight(0, b) == 0.5f, "clean: only the named group is cleaned");
    clean_weights(m, 0.05f);
    expect(m.weight(0, a) == 0.0f && m.weight(0, b) == 0.5f, "clean: across all groups, above the threshold survives");
}

/** @brief Smooth Weights pulls a hard step toward its neighbours without inventing weight. */
COOPA_TEST(smooth_softens_a_hard_weight_step) {
    EditMesh m = make_grid(8, 8, 2.0f);
    SculptCache cache;
    cache.build(m);
    const uint32_t g = m.group_index("Bone");
    m.sync_vertex_data();
    // The left half fully weighted, the right half not at all: a seam down the middle.
    for (uint32_t v = 0; v < m.positions.size(); ++v) m.set_weight(v, g, m.positions[v].x < 0.0f ? 1.0f : 0.0f);
    uint32_t inside = 0, outside = 0;
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        if (m.positions[v].x < -0.24f && m.positions[v].x > -0.26f) inside = v;
        if (m.positions[v].x > 0.24f && m.positions[v].x < 0.26f) outside = v;
    }
    smooth_weights(m, cache, g, 0.5f, 4);
    expect(m.weight(inside, g) < 1.0f, "smooth: the seam's weighted side gives ground");
    expect(m.weight(outside, g) > 0.0f, "smooth: ...and the bare side picks some up");
    bool bounded = true;
    for (uint32_t v = 0; v < m.positions.size(); ++v) bounded &= m.weight(v, g) <= 1.0f + 1e-6f;
    expect(bounded, "smooth: nothing overshoots");
}

/** @brief Mirror Vertex Groups swaps weights across an axis, and swaps side-named groups with them. */
COOPA_TEST(mirror_swaps_sides_and_side_named_groups) {
    expect(flipped_group_name("Arm_L") == "Arm_R" && flipped_group_name("arm.l") == "arm.r",
           "flip name: a trailing side marker flips, keeping its separator and case");
    expect(flipped_group_name("left_leg") == "right_leg" && flipped_group_name("LEFT_leg") == "RIGHT_leg",
           "flip name: a leading side word flips too");
    expect(flipped_group_name("Spine").empty(), "flip name: a group with no side has no opposite number");

    EditMesh m = make_grid(2, 2, 2.0f);
    const uint32_t left = m.group_index("Arm_L");
    m.sync_vertex_data();
    uint32_t plus = 0, minus = 0;
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        if (m.positions[v].x > 0.9f && std::fabs(m.positions[v].y) < 1e-5f) plus = v;
        if (m.positions[v].x < -0.9f && std::fabs(m.positions[v].y) < 1e-5f) minus = v;
    }
    m.set_weight(plus, left, 0.75f);

    mirror_weights(m, 0);
    const int right = group_of(m, "Arm_R");
    require(right >= 0, "mirror: the opposite-side group is created");
    expect_near(m.weight(minus, static_cast<uint32_t>(right)), 0.75f, 1e-6f,
                "mirror: the weight lands on the opposite vertex, in the opposite group");
    expect(m.weight(plus, left) == 0.0f, "mirror: and leaves the side it came from, as a mirror does");
}

/** @brief Invert and Levels do their arithmetic on one group. */
COOPA_TEST(invert_and_levels_adjust_one_group) {
    EditMesh m = make_grid(1, 1, 2.0f);
    m.sync_vertex_data();
    const uint32_t g = m.group_index("Bone");
    m.set_weight(0, g, 0.25f);
    invert_group_weights(m, g);
    expect_near(m.weight(0, g), 0.75f, 1e-6f, "invert: a weight becomes its complement");
    expect_near(m.weight(1, g), 1.0f, 1e-6f, "invert: a vertex that was not in the group joins it fully");

    levels_group_weights(m, g, 2.0f, -0.5f);
    expect_near(m.weight(0, g), 1.0f, 1e-6f, "levels: gain and offset apply, clamped to 1");
    m.set_weight(0, g, 0.2f);
    levels_group_weights(m, g, 1.0f, -0.5f);
    expect(m.weight(0, g) == 0.0f, "levels: a weight pushed below zero leaves the group");
}

} // namespace toy::editor::testing
