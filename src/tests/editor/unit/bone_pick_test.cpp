/**
 * @file bone_pick_test.cpp
 * @brief Octahedral bone projection and hit-testing: the ring survives projection, a click on
 *        the body hits, a miss stays a miss, and the closest of several bones wins.
 */

#include <coopa/testing/test.h>

#include <glm/glm.hpp>

#include "editor/rig/bone_pick.h"
#include "editor/support/view_helpers.h"

COOPA_TEST_SUITE("bone_pick");

using namespace coopa::test;
using namespace toy::editor::rig_pick;
using toy::editor::testing::test_view_proj;

/** @brief A bone with real length projects: head/tail land apart, and all four ring corners resolve. */
COOPA_TEST(project_bone_resolves_head_tail_and_ring) {
    const auto vp = test_view_proj();
    glm::vec2 head, tail, ring[4];
    const bool ok = project_bone(vp, glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), head, tail, ring);
    expect(ok, "a straight-up, in-view bone projects");
    expect(glm::distance(head, tail) > 1.0f, "head and tail land at different screen points");
}

/** @brief A zero-length bone never projects -- nothing to click on a bone with no body. */
COOPA_TEST(project_bone_rejects_zero_length) {
    const auto vp = test_view_proj();
    glm::vec2 head, tail, ring[4];
    expect(!project_bone(vp, glm::vec3(0, 0, 1), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), head, tail, ring),
           "head == tail: no bone to project");
}

/** @brief In-triangle: a point past the far corner of a degenerate (zero-area) triangle never
 *        matches -- the guard against claiming the whole screen when a bone is seen end-on. */
COOPA_TEST(in_triangle_rejects_degenerate_triangle) {
    expect(!in_triangle(glm::vec2(0, 0), glm::vec2(-5, 0), glm::vec2(5, 0), glm::vec2(0, 0)),
           "a zero-area (collinear) triangle hits nothing");
    expect(in_triangle(glm::vec2(0, 0), glm::vec2(-5, -5), glm::vec2(5, -5), glm::vec2(0, 5)),
           "a real triangle's centroid-ish point is inside it");
}

/** @brief Clicking directly on a bone's body is a 0-distance hit; clicking far away is not. */
COOPA_TEST(bone_pick_distance_hits_the_body_and_misses_far_away) {
    const auto vp = test_view_proj();
    BoneSegment bone;
    bone.head = glm::vec3(0, 0, 0);
    bone.tail = glm::vec3(0, 0, 1);
    const auto head_px = vp.project(bone.head);
    expect(head_px.has_value(), "the bone's head is on screen");
    const float at_head = bone_pick_distance(vp, bone, *head_px);
    expect(at_head < 5.0f, "clicking right on the bone's head/root is a close (near-zero) hit");
    const float far_away = bone_pick_distance(vp, bone, *head_px + glm::vec2(5000.0f, 5000.0f));
    expect(far_away > 1000.0f, "clicking far off-screen is not a hit");
}

/** @brief Among several bones, pick_closest_bone returns the nearest one within range, and -1
 *        when nothing is close enough. */
COOPA_TEST(pick_closest_bone_finds_the_nearest_within_range) {
    const auto vp = test_view_proj();
    std::vector<BoneSegment> bones;
    BoneSegment a;
    a.head = glm::vec3(0, 0, 0);
    a.tail = glm::vec3(0, 0, 1);
    bones.push_back(a);
    BoneSegment b;
    b.head = glm::vec3(3, 0, 0);
    b.tail = glm::vec3(3, 0, 1);
    bones.push_back(b);

    const auto near_a = vp.project(a.head);
    expect(near_a.has_value(), "bone a's head is on screen");
    expect(pick_closest_bone(vp, bones, *near_a) == 0, "a click near bone a's head picks bone a, not b");

    const auto nowhere = *near_a + glm::vec2(50000.0f, 50000.0f);
    expect(pick_closest_bone(vp, bones, nowhere) == -1, "a click nowhere near either bone picks none");
}

/** @brief pick_closest_endpoint tells a bone's HEAD from its TAIL, not just which bone, and
 *         returns nullopt when nothing is within range -- Rig Edit mode's joint-drag picking. */
COOPA_TEST(pick_closest_endpoint_distinguishes_head_from_tail) {
    const auto vp = test_view_proj();
    std::vector<BoneSegment> bones;
    BoneSegment a;
    a.head = glm::vec3(0, 0, 0);
    a.tail = glm::vec3(0, 0, 1);
    bones.push_back(a);

    const auto head_px = vp.project(a.head);
    const auto tail_px = vp.project(a.tail);
    coopa::test::require(head_px.has_value() && tail_px.has_value(), "both ends project");

    const auto hit_head = pick_closest_endpoint(vp, bones, *head_px);
    coopa::test::require(hit_head.has_value(), "a click on the head hits something");
    expect(hit_head->first == 0 && hit_head->second == BoneEndpoint::Head, "...and it is bone 0's HEAD");

    const auto hit_tail = pick_closest_endpoint(vp, bones, *tail_px);
    coopa::test::require(hit_tail.has_value(), "a click on the tail hits something");
    expect(hit_tail->first == 0 && hit_tail->second == BoneEndpoint::Tail, "...and it is bone 0's TAIL, not the head");

    const auto nowhere = *head_px + glm::vec2(50000.0f, 50000.0f);
    expect(!pick_closest_endpoint(vp, bones, nowhere).has_value(), "a click nowhere near either endpoint hits nothing");
}
