/**
 * @file rig_edit_ops_test.cpp
 * @brief The armature operators: name side-flipping, mirrored roll (including the bone-along-Z
 *        case the obvious `-roll` gets wrong), subdivide, fill, merge, roll recalculation,
 *        align, symmetrize and the live X-mirror map.
 */

#include <coopa/testing/test.h>

#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_loader.h>
#include <coopa/yaml/document.h>

#include "editor/rig/rig_edit_ops.h"

COOPA_TEST_SUITE("rig_edit_ops");

namespace toy::editor::testing {

using namespace coopa::test;
using namespace toy::editor;
using namespace toy::editor::rig_ops;
namespace rs = toy::editor::rig_sel;

namespace {

/** @brief Selects a bone whole and makes it active. */
void pick(rs::RigSelection& sel, const RigModel& m, int i) {
    rs::validate(sel, m);
    sel.bones[static_cast<std::size_t>(i)].head = true;
    sel.bones[static_cast<std::size_t>(i)].tail = true;
    sel.active = i;
}

/** @brief Bone i's rest Z in rig space -- what a roll aims. */
glm::vec3 rest_z(const RigModel& m, int i) { return glm::vec3(m.world_rest_matrix(i)[2]); }

} // namespace

/** @brief The `_l`/`_r` side flip, in every spelling the rigs use. */
COOPA_TEST(mirrored_bone_name_flips_known_sides_only) {
    expect(mirrored_bone_name("upper_arm_l").value_or("") == "upper_arm_r", "_l -> _r");
    expect(mirrored_bone_name("upper_arm_r").value_or("") == "upper_arm_l", "_r -> _l");
    expect(mirrored_bone_name("Hand.L").value_or("") == "Hand.R", ".L -> .R keeps its case");
    expect(mirrored_bone_name("leg_left").value_or("") == "leg_right", "_left -> _right (longest match wins)");
    expect(!mirrored_bone_name("spine").has_value(), "a centre bone has no partner");
    expect(!mirrored_bone_name("l").has_value(), "a name that is ONLY the suffix is not a side");
}

/** @brief mirrored_roll is NOT simply -roll: a bone running along world Z hits
 *         bone_rest_matrix's seed-axis switch, and deriving the answer from the basis is what
 *         keeps both branches right. */
COOPA_TEST(mirrored_roll_is_correct_for_every_bone_direction) {
    const std::vector<glm::vec3> dirs = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {1, 1, 0}, {0.3f, -0.2f, 0.9f}, {-0.5f, 0.1f, 0.8f},
    };
    for (const glm::vec3& d : dirs) {
        for (float roll : {0.0f, 25.0f, -70.0f, 150.0f}) {
            const glm::vec3 head(0.2f, 0.1f, 0.3f);
            const glm::vec3 tail = head + glm::normalize(d);
            RigModel a;
            a.add_bone("a", -1, head, tail);
            a.set_roll(0, roll);

            RigModel b;
            b.add_bone("b", -1, glm::vec3(-head.x, head.y, head.z), glm::vec3(-tail.x, tail.y, tail.z));
            b.set_roll(0, mirrored_roll(head, tail, roll, 0));

            // The mirrored bone's rest Z must be the source's rest Z reflected across X.
            const glm::vec3 za = rest_z(a, 0);
            const glm::vec3 want(-za.x, za.y, za.z);
            expect(glm::dot(rest_z(b, 0), want) > 0.999f,
                   "mirrored roll aims the partner's Z at the reflected Z (dir " + std::to_string(d.x) + "," +
                       std::to_string(d.y) + "," + std::to_string(d.z) + " roll " + std::to_string(roll) + ")");
        }
    }
}

/** @brief Subdivide turns one bone into a connected chain whose ends match the original, and
 *         hands the original's children to the LAST segment. */
COOPA_TEST(subdivide_splits_a_bone_into_a_connected_chain) {
    RigModel m;
    m.add_bone("upper", -1, {0, 0, 0}, {0, 0, 2});
    m.extrude_bone(0, "lower", {0, 0, 3});
    rs::RigSelection sel;
    pick(sel, m, 0);

    const int created = subdivide_bone(m, sel, 0, 1);
    expect(created == 1, "one cut makes one new bone");
    expect(m.bones.size() == 3, "...so the rig has three bones");

    const int a = m.find_bone("upper");
    coopa::test::require(a >= 0, "the first segment keeps the original name");
    expect_near(m.bones[static_cast<std::size_t>(a)].head.z, 0.0f, 1e-5f, "the chain still starts where the bone did");
    expect_near(m.bones[static_cast<std::size_t>(a)].tail.z, 1.0f, 1e-5f, "...with the first segment half as long");

    const int child = m.find_bone("lower");
    coopa::test::require(child >= 0, "the original's child survived");
    expect_near(m.bones[static_cast<std::size_t>(child)].head.z, 2.0f, 1e-5f,
                "...still attached at the END of the subdivided chain, not the middle");
}

/** @brief Fill spans two selected endpoints, connecting to a tail when exactly one was a tail. */
COOPA_TEST(fill_between_joints_spans_two_endpoints) {
    RigModel m;
    m.add_bone("a", -1, {0, 0, 0}, {0, 0, 1});
    m.add_bone("b", -1, {1, 0, 2}, {1, 0, 3});
    rs::RigSelection sel;
    rs::validate(sel, m);
    sel.bones[0].tail = true;   // a's tail
    sel.bones[1].head = true;   // b's head
    sel.active = 0;

    const int made = fill_between_joints(m, sel, "bridge");
    coopa::test::require(made >= 0, "a bone was created");
    const RigModelBone& nb = m.bones[static_cast<std::size_t>(made)];
    expect_near(glm::distance(nb.head, glm::vec3(0, 0, 1)), 0.0f, 1e-5f, "it starts at the TAIL endpoint");
    expect_near(glm::distance(nb.tail, glm::vec3(1, 0, 2)), 0.0f, 1e-5f, "...and ends at the other one");
    expect(nb.connected && nb.parent == m.find_bone("a"), "...connected to the bone whose tail it grew from");

    rs::RigSelection one;
    rs::validate(one, m);
    one.bones[0].tail = true;
    expect(fill_between_joints(m, one, "x") < 0, "F needs TWO endpoints, not one");
}

/** @brief Merge collapses a connected selected run into its first bone. */
COOPA_TEST(merge_collapses_a_connected_chain) {
    RigModel m;
    m.add_bone("a", -1, {0, 0, 0}, {0, 0, 1});
    m.extrude_bone(0, "b", {0, 0, 2});
    m.extrude_bone(1, "c", {0, 0, 3});
    rs::RigSelection sel;
    rs::select_all(sel, m);
    sel.active = 0;

    const int removed = merge_selected_chains(m, sel);
    expect(removed == 2, "a three-bone connected run collapses to one (two removed)");
    coopa::test::require(m.bones.size() == 1, "...leaving a single bone");
    expect(m.bones[0].name == "a", "...which keeps the FIRST bone's name");
    expect_near(m.bones[0].tail.z, 3.0f, 1e-5f, "...and reaches the last bone's tail");
}

/** @brief Recalculate Roll aims every selected bone's Z, and Align only changes roll. */
COOPA_TEST(recalculate_roll_and_align_only_touch_roll) {
    RigModel m;
    m.add_bone("a", -1, {0, 0, 0}, {1, 0, 0});
    m.add_bone("b", -1, {0, 1, 0}, {1, 1, 0});
    m.set_roll(0, 33.0f);
    m.set_roll(1, -70.0f);
    rs::RigSelection sel;
    rs::select_all(sel, m);
    sel.active = 0;

    RollTarget up;   // default: aim Z at global +Z
    recalculate_roll(m, sel, up);
    for (int i = 0; i < 2; ++i) {
        expect(glm::dot(glm::normalize(rest_z(m, i)), glm::vec3(0, 0, 1)) > 0.999f,
               "recalculated roll points the bone's Z at global +Z");
    }

    const glm::vec3 head_before = m.bones[1].head, tail_before = m.bones[1].tail;
    m.set_roll(0, 20.0f);
    align_to_active(m, sel);
    expect(glm::distance(m.bones[1].head, head_before) < 1e-6f, "Align leaves the head where it was");
    expect(glm::distance(m.bones[1].tail, tail_before) < 1e-6f, "...and the tail: it only rolls");
    expect(glm::dot(glm::normalize(rest_z(m, 1)), glm::normalize(rest_z(m, 0))) > 0.999f,
           "...matching the active bone's Z");
}

/** @brief Symmetrize creates the missing partner, mirrored, parented to the parent's mirror --
 *         and the result still loads through the runtime's own rig parser. */
COOPA_TEST(symmetrize_builds_the_other_side) {
    RigModel m;
    m.name = "sym";
    m.add_bone("spine", -1, {0, 0, 0}, {0, 0, 1});          // centre bone: no side
    m.add_bone("upper_arm_l", 0, {0.2f, 0, 1}, {0.6f, 0, 1});
    m.set_roll(1, 25.0f);
    rs::RigSelection sel;
    pick(sel, m, 1);

    std::vector<std::string> skipped;
    const int touched = symmetrize(m, sel, SymmetrizeDirection::PositiveToNegative, 0, &skipped);
    expect(touched == 1, "the one sided bone was mirrored");
    const int r = m.find_bone("upper_arm_r");
    coopa::test::require(r >= 0, "its _r partner now exists");
    expect_near(m.bones[static_cast<std::size_t>(r)].head.x, -0.2f, 1e-5f, "...mirrored across X");
    expect_near(m.bones[static_cast<std::size_t>(r)].tail.x, -0.6f, 1e-5f, "...both endpoints");
    expect(m.bones[static_cast<std::size_t>(r)].parent == m.find_bone("spine"),
           "...hanging off the same CENTRE parent (a centre bone has no mirror to retarget to)");
    const glm::vec3 zl = rest_z(m, m.find_bone("upper_arm_l"));
    expect(glm::dot(rest_z(m, r), glm::vec3(-zl.x, zl.y, zl.z)) > 0.999f, "...with a correctly mirrored roll");

    // A selected bone with no side is reported rather than silently given one.
    rs::RigSelection centre;
    pick(centre, m, m.find_bone("spine"));
    skipped.clear();
    symmetrize(m, centre, SymmetrizeDirection::PositiveToNegative, 0, &skipped);
    expect(skipped.size() == 1 && skipped[0] == "spine", "a sideless selected bone is reported as skipped");

    // The whole thing is still a rig the runtime can load.
    const coopa::rig::RigAsset rt = coopa::rig::parse_rig(m.to_node());
    expect(rt.bones.size() == m.bones.size(), "the runtime's own parser reads the symmetrized rig");
}

/** @brief The live X-mirror map pairs selected bones with their unselected partners, and
 *         applying it mirrors exactly the endpoints that moved. */
COOPA_TEST(x_mirror_map_follows_the_moved_endpoints) {
    RigModel base;
    base.add_bone("arm_l", -1, {0.2f, 0, 0}, {0.6f, 0, 0});
    base.add_bone("arm_r", -1, {-0.2f, 0, 0}, {-0.6f, 0, 0});
    rs::RigSelection sel;
    rs::validate(sel, base);
    sel.bones[0].tail = true;   // only arm_l's TAIL is selected
    sel.active = 0;

    const RigMirrorMap map = build_rig_mirror_map(base, sel, 0);
    coopa::test::require(map.pairs.size() == 1, "the selected bone pairs with its unselected partner");
    expect(map.pairs[0].src == 0 && map.pairs[0].dst == 1, "...src -> dst");

    RigModel m = base;
    m.set_tail(0, {0.6f, 0.0f, 0.5f});   // the drag
    apply_rig_mirror_map(m, base, sel, map, 0);
    expect_near(m.bones[1].tail.x, -0.6f, 1e-5f, "the partner's tail mirrors on X");
    expect_near(m.bones[1].tail.z, 0.5f, 1e-5f, "...and follows on Z");
    expect_near(m.bones[1].head.x, -0.2f, 1e-5f, "...while its HEAD, whose source was unselected, stays put");

    // A partner that is itself selected is left out: it moves on its own.
    rs::RigSelection both;
    rs::select_all(both, base);
    expect(build_rig_mirror_map(base, both, 0).empty(), "both sides selected: nothing to mirror");
}

} // namespace toy::editor::testing
