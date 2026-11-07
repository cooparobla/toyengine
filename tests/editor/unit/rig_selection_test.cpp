/**
 * @file rig_selection_test.cpp
 * @brief Rig Edit mode's bone selection and the endpoint transform it drives: Blender's
 *        click/shift/box rules, the active bone, and what `G` actually moves.
 */

#include <coopa/testing/test.h>

#include <glm/gtc/matrix_transform.hpp>

#include "editor/rig/rig_selection.h"

COOPA_TEST_SUITE("rig_selection");

namespace toy::editor::testing {

using namespace coopa::test;
using namespace toy::editor;
using namespace toy::editor::rig_sel;
using rig_pick::BoneEndpoint;
using rig_pick::BonePickResult;

namespace {

/** @brief A straight two-bone chain up +Z: 0 (root) -> 1 (connected child). */
RigModel chain2() {
    RigModel m;
    m.add_bone("root", -1, {0, 0, 0}, {0, 0, 1});
    m.extrude_bone(0, "child", {0, 0, 2});
    return m;
}

BonePickResult body(int bone) { BonePickResult r; r.bone = bone; return r; }
BonePickResult endpoint(int bone, BoneEndpoint e) { BonePickResult r; r.bone = bone; r.endpoint = e; return r; }

} // namespace

/** @brief Clicking a bone's BODY selects both its endpoints and makes it active; clicking one
 *         endpoint handle selects only that endpoint. */
COOPA_TEST(click_selects_body_as_both_endpoints_and_an_endpoint_alone) {
    RigModel m = chain2();
    RigSelection sel;
    validate(sel, m);

    click_select(sel, body(1), false);
    expect(is_fully_selected(sel, 1), "a body click selects BOTH endpoints of that bone");
    expect(sel.active == 1, "...and makes it the active bone");
    expect(!is_selected(sel, 0), "...leaving the other bone alone");

    click_select(sel, endpoint(0, BoneEndpoint::Tail), false);
    expect(!sel.bones[0].head && sel.bones[0].tail, "an endpoint click selects ONLY that endpoint");
    expect(!is_selected(sel, 1), "...and a plain click clears what was selected before");
    expect(sel.active == 0, "...making its bone active");

    click_select(sel, BonePickResult{}, false);
    expect(empty(sel), "a plain click on nothing clears the selection");
}

/** @brief Shift-click extends and toggles, and only promotes a bone to active when it ADDS. */
COOPA_TEST(shift_click_toggles_and_only_activates_when_adding) {
    RigModel m = chain2();
    RigSelection sel;
    validate(sel, m);

    click_select(sel, body(0), false);
    click_select(sel, body(1), true);
    expect(is_fully_selected(sel, 0) && is_fully_selected(sel, 1), "shift-click EXTENDS to a second bone");
    expect(sel.active == 1, "...and the newly added bone becomes active");

    // Shift-clicking the active, fully-selected bone drops it (Object Mode's own rule).
    click_select(sel, body(1), true);
    expect(!is_selected(sel, 1), "shift-clicking the active fully-selected bone deselects it");
    expect(is_fully_selected(sel, 0), "...leaving the rest of the selection intact");
    expect(sel.active == 1, "...and it STAYS active (Blender keeps an active-but-unselected bone)");

    click_select(sel, endpoint(0, BoneEndpoint::Head), true);
    expect(!sel.bones[0].head && sel.bones[0].tail, "shift-click on a selected endpoint toggles just it off");

    const RigSelection before = sel;
    click_select(sel, BonePickResult{}, true);
    expect(sel.bones == before.bones, "shift-click on empty space changes nothing (no accidental wipe)");
}

/** @brief Select all / none / invert work per ENDPOINT, not per bone. */
COOPA_TEST(select_all_none_and_invert) {
    RigModel m = chain2();
    RigSelection sel;
    select_all(sel, m);
    expect(is_fully_selected(sel, 0) && is_fully_selected(sel, 1), "A selects every endpoint of every bone");

    select_none(sel);
    expect(empty(sel), "Alt+A clears everything");

    click_select(sel, endpoint(0, BoneEndpoint::Head), false);
    invert_selection(sel);
    expect(!sel.bones[0].head && sel.bones[0].tail, "invert flips the half-selected bone's own endpoints");
    expect(is_fully_selected(sel, 1), "...and selects the untouched bone entirely");
}

/** @brief Box select takes ENDPOINTS inside the box (not bodies crossing it), with the usual
 *         additive/subtract three-way. */
COOPA_TEST(box_select_takes_endpoints_inside_the_box) {
    RigSelection sel;
    sel.bones.resize(2);
    std::vector<BonePoints2D> pts(2);
    pts[0] = {{10.0f, 10.0f}, {10.0f, 90.0f}, true, true};   // head inside a small top box, tail far below
    pts[1] = {{500.0f, 500.0f}, {500.0f, 560.0f}, true, true};

    box_select(sel, pts, {0.0f, 0.0f}, {50.0f, 50.0f}, false, false);
    expect(sel.bones[0].head && !sel.bones[0].tail, "only the endpoint INSIDE the box is selected");
    expect(!sel.bones[1].any(), "a bone wholly outside is untouched");
    expect(sel.active == 0, "...and the bone it touched became active");

    box_select(sel, pts, {480.0f, 480.0f}, {520.0f, 520.0f}, true, false);
    expect(sel.bones[0].head, "additive keeps the earlier selection");
    expect(sel.bones[1].head && !sel.bones[1].tail, "...and adds the newly boxed endpoint");

    box_select(sel, pts, {0.0f, 0.0f}, {50.0f, 50.0f}, false, true);
    expect(!sel.bones[0].head, "subtract removes what it covers");
    expect(sel.bones[1].head, "...and leaves the rest alone");

    // A bone whose body crosses the box but whose endpoints are both outside stays unselected.
    sel = RigSelection{};
    sel.bones.resize(2);
    box_select(sel, pts, {5.0f, 40.0f}, {15.0f, 60.0f}, false, false);
    expect(!sel.bones[0].any(), "a body merely crossing the box does NOT select (that is Pose mode's rule)");
}

/** @brief validate() resizes to the model and drops a dangling active; remap() follows a reorder. */
COOPA_TEST(validate_and_remap_keep_the_selection_in_range) {
    RigModel m = chain2();
    RigSelection sel;
    select_all(sel, m);
    sel.active = 1;

    m.delete_bone(1);
    validate(sel, m);
    expect(sel.bones.size() == 1, "validate() resized the selection down with the model");
    expect(sel.active == -1, "...and dropped the active bone that no longer exists");

    RigSelection s2;
    s2.bones.resize(3);
    s2.bones[2].tail = true;
    s2.active = 2;
    remap(s2, {2, 0, 1});   // old 2 -> new 1
    expect(s2.bones[1].tail, "remap moved the selected endpoint to the bone's new index");
    expect(s2.active == 1, "...and followed the active bone");
}

/** @brief G on a TAIL alone lengthens the bone; G on both endpoints slides it rigidly; and a
 *         connected child's head and its parent's tail move together, because they are one joint. */
COOPA_TEST(transform_endpoints_moves_only_what_is_selected) {
    const RigModel base = chain2();
    RigSelection sel;
    validate(sel, base);

    // Tail only: the bone gets longer, its head stays put.
    sel.bones[0].tail = true;
    RigModel out = base;
    const glm::mat4 up = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 1));
    transform_endpoints(out, base, sel, up, glm::mat3(1.0f));
    expect_near(out.bones[0].head.z, 0.0f, 1e-5f, "a tail-only move leaves the head alone");
    expect_near(out.bones[0].tail.z, 2.0f, 1e-5f, "...and moves the tail by the delta");
    expect_near(out.bones[1].head.z, 2.0f, 1e-5f,
                "...carrying the CONNECTED child's head with it (one joint, as rig_model.h documents)");

    // Both endpoints: the bone slides rigidly, keeping its length.
    sel = RigSelection{};
    validate(sel, base);
    sel.bones[0].head = sel.bones[0].tail = true;
    out = base;
    transform_endpoints(out, base, sel, up, glm::mat3(1.0f));
    expect_near(out.bones[0].head.z, 1.0f, 1e-5f, "a whole-bone move takes the head");
    expect_near(out.bones[0].tail.z, 2.0f, 1e-5f, "...and the tail");
    expect_near(glm::length(out.bones[0].tail - out.bones[0].head), 1.0f, 1e-5f, "...keeping its length");
}

/** @brief Rotating a fully-selected bone re-derives its roll, so its twist follows the rotation
 *         instead of staying pointed the old way. */
COOPA_TEST(rotating_a_whole_bone_carries_its_roll) {
    RigModel base;
    base.add_bone("arm", -1, {0, 0, 0}, {1, 0, 0});   // along +X, so its roll plane is well-defined
    RigSelection sel;
    select_all(sel, base);

    const float deg = 50.0f;
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(deg), glm::vec3(1, 0, 0));
    RigModel out = base;
    transform_endpoints(out, base, sel, rot, glm::mat3(rot));

    // The bone lies ON the rotation axis, so its points do not move -- only its roll should.
    expect_near(glm::distance(out.bones[0].head, base.bones[0].head), 0.0f, 1e-5f, "head is on the axis: unmoved");
    expect_near(glm::distance(out.bones[0].tail, base.bones[0].tail), 0.0f, 1e-5f, "tail is on the axis: unmoved");
    const glm::mat4 rest = out.world_rest_matrix(0);
    const glm::vec3 z_expect = glm::mat3(rot) * glm::vec3(base.world_rest_matrix(0)[2]);
    expect(glm::dot(glm::vec3(rest[2]), z_expect) > 0.999f,
           "the re-derived roll points the bone's rest Z where the rotation put it");
}

} // namespace toy::editor::testing
