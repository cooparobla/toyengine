/**
 * @file mount_test.cpp
 * @brief Riding (toyengine/scene/mount.h, rider.h) on the generated horse prefab and the
 *        mannequin_ik player: getting on only in reach, sitting on the saddle with the feet in the
 *        stirrups, input going to the horse (and only while it is ridden), the horse moving along
 *        its facing and turning at its rate through the gaits, and getting off on a clear side.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/glm.hpp>

#include "engine/support/checks.h"
#include "engine/support/horse_rig.h"

COOPA_TEST_SUITE("mount");

using namespace toy::test;
using toy::scene::Rider;

namespace {

std::string str(const glm::vec3& v) {
    return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
}

/** @brief The horse's facing (unit, world XY) from its controller's yaw. */
glm::vec2 facing(const toy::scene::CharacterController* cc) {
    const float y = glm::radians(cc->yaw_deg());
    return glm::vec2(-std::sin(y), std::cos(y));
}

} // namespace

/**
 * @brief Interact out of reach does nothing; next to the left mount point it climbs on: the
 *        controller and FootIK are handed over, the root sits on the saddle (seat_offset in its
 *        frame), and the feet reach the stirrups and the hands the reins.
 */
COOPA_TEST(mounts_only_in_reach_and_sits_in_the_saddle) {
    HorseRig rig(glm::vec3(0.0f, 0.0f, 0.0f), 0.0f, true, glm::vec3(-4.0f, 0.0f, 0.0f));
    expect(rig.mount && rig.rider && rig.player_cc && rig.horse_cc && rig.hooves, "rig: horse prefab and rider loaded");
    if (!rig.mount || !rig.rider) return;
    rig.start();
    rig.step(30);
    expect(rig.mount->can_mount(), "reach: the standing horse can be mounted");

    rig.rider->interact = true;
    rig.step(5);
    expect(rig.rider->state() == Rider::State::OnFoot, "reach: 2.95 m from the mount point, interact does nothing");
    expect(!rig.mount->is_ridden(), "reach: the horse is still free");

    rig.mount_up();
    expect(rig.rider->state() == Rider::State::Riding, "mount: riding after the climb");
    expect(rig.mount->rider() == rig.rider, "mount: the horse knows its rider");
    expect(rig.player_cc->is_suspended(), "mount: the rider's controller is handed over");
    auto* foot_ik = rig.player->get_component<toy::scene::FootIK>();
    expect(foot_ik && foot_ik->is_suspended(), "mount: the rider's FootIK is handed over");

    rig.step(10);
    auto* saddle = rig.mount->socket(rig.mount->saddle);
    const glm::mat4 sm = saddle->get_transform()->get_world_matrix();
    const glm::quat sr = glm::normalize(glm::quat_cast(glm::mat3(sm)));
    const glm::vec3 seat = glm::vec3(sm[3]) + sr * rig.rider->seat_offset;
    const glm::vec3 root = HorseRig::world(rig.player);
    expect(glm::length(root - seat) < 1e-3f, "seat: the rider's root is on the saddle (" + str(root) + " vs " + str(seat) + ")");

    for (const char* side : {"l", "r"}) {
        const std::string s(side);
        const glm::vec3 foot = HorseRig::world(rig.bone(rig.player, "foot_" + s));
        const glm::vec3 stirrup = rig.socket(s == "l" ? rig.mount->stirrup_l : rig.mount->stirrup_r);
        expect(glm::length(foot - stirrup) < 0.03f, "limbs: foot_" + s + " in its stirrup (off by " +
                                                        std::to_string(glm::length(foot - stirrup)) + " m)");
        const glm::vec3 hand = HorseRig::world(rig.bone(rig.player, "hand_" + s));
        const glm::vec3 rein = rig.socket(s == "l" ? rig.mount->rein_l : rig.mount->rein_r);
        expect(glm::length(hand - rein) < 0.03f, "limbs: hand_" + s + " on its rein (off by " +
                                                     std::to_string(glm::length(hand - rein)) + " m)");
        // The knees bend out round the barrel, not into it.
        const glm::vec3 knee = HorseRig::world(rig.bone(rig.player, "shin_" + s));
        const float out = (knee.x - HorseRig::world(rig.horse).x) * (s == "l" ? -1.0f : 1.0f);
        expect(out > 0.25f, "limbs: the " + s + " knee is outside the barrel (" + std::to_string(out) + " m out)");
    }
}

/**
 * @brief Input goes where it should: an unridden horse ignores the move input, and a mounted
 *        rider's own (suspended) controller never moves it off the saddle; the player lookup
 *        names the rider, never the horse (which comes first in the scene).
 */
COOPA_TEST(input_reaches_the_horse_only_while_ridden) {
    HorseRig rig(glm::vec3(0.0f), 0.0f, true, glm::vec3(-4.0f, 0.0f, 0.0f));
    if (!rig.mount || !rig.rider) return;
    rig.start();
    rig.step(30);
    expect(toy::scene::find_player_controller(rig.scene) == rig.player_cc, "lookup: the player, not the horse");
    const glm::vec3 start = HorseRig::world(rig.horse);
    for (int i = 0; i < 120; ++i) {
        // The engine zeroes an unridden mount's input; push it anyway: the Mount ignores it too.
        rig.mount->move_input = glm::vec2(0.0f, 1.0f);
        rig.mount->sprint = true;
        rig.player_cc->move_input = glm::vec2(1.0f, 0.0f);
        rig.step(1);
    }
    expect(glm::length(HorseRig::world(rig.horse) - start) < 0.02f, "unridden: the horse stays put");

    rig.mount->move_input = glm::vec2(0.0f);   // the horse itself stands still now it is ridden
    rig.mount->sprint = false;
    rig.mount_up();
    const glm::vec3 seated = HorseRig::world(rig.player);
    for (int i = 0; i < 60; ++i) {
        rig.player_cc->move_input = glm::vec2(1.0f, 0.0f);   // as if the engine still pushed it
        rig.player_cc->jump = true;
        rig.step(1);
    }
    expect(glm::length(HorseRig::world(rig.player) - seated) < 0.05f, "mounted: the rider's own controller doesn't move it");
    expect(toy::scene::find_player_controller(rig.scene) == rig.player_cc, "lookup: still the player while mounted");
}

/**
 * @brief Ridden, the horse moves along its own facing only (never sideways), trots on input and
 *        gallops with sprint at the gait speeds, playing the matching gait; a hard turn is taken
 *        at no more than turn_rate, the rider staying in the saddle throughout.
 */
COOPA_TEST(ridden_horse_moves_along_its_facing_and_turns_at_its_rate) {
    HorseRig rig(glm::vec3(0.0f), 0.0f, true, glm::vec3(-4.0f, 0.0f, 0.0f));
    if (!rig.mount || !rig.rider) return;
    rig.start();
    rig.mount_up();
    if (rig.rider->state() != Rider::State::Riding) { expect(false, "ride: mounted"); return; }

    for (int i = 0; i < 120; ++i) { rig.input(glm::vec2(0.0f, 1.0f)); rig.step(1); }
    glm::vec3 v = rig.horse_cc->velocity();
    expect_near(glm::length(glm::vec2(v.x, v.y)), rig.mount->trot_speed, 0.15f, "trot: input alone trots");
    expect(rig.mount->gait() == rig.mount->trot_state, "trot: playing " + rig.mount->gait());

    for (int i = 0; i < 180; ++i) { rig.input(glm::vec2(0.0f, 1.0f), true); rig.step(1); }
    v = rig.horse_cc->velocity();
    expect_near(glm::length(glm::vec2(v.x, v.y)), rig.mount->gallop_speed, 0.25f, "gallop: sprint gallops");
    expect(rig.mount->gait() == rig.mount->gallop_state, "gallop: playing " + rig.mount->gait());

    // Hard right (the camera basis is world here): the yaw turns at no more than the rate, the
    // velocity stays along the facing, and the rider stays seated.
    const float dt = 1.0f / 60.0f;
    float worst_rate = 0.0f, worst_side = 0.0f, worst_seat = 0.0f;
    float prev = rig.horse_cc->yaw_deg();
    for (int i = 0; i < 180; ++i) {
        rig.input(glm::vec2(1.0f, 0.0f), true);
        rig.step(1);
        float d = rig.horse_cc->yaw_deg() - prev;
        d = std::fmod(d + 540.0f, 360.0f) - 180.0f;
        prev = rig.horse_cc->yaw_deg();
        worst_rate = std::max(worst_rate, std::abs(d) / dt);
        const glm::vec2 hv(rig.horse_cc->velocity());
        const glm::vec2 f = facing(rig.horse_cc);
        worst_side = std::max(worst_side, std::abs(hv.x * f.y - hv.y * f.x));
        auto* saddle = rig.mount->socket(rig.mount->saddle);
        const glm::mat4 sm = saddle->get_transform()->get_world_matrix();
        const glm::vec3 seat = glm::vec3(sm[3]) + glm::normalize(glm::quat_cast(glm::mat3(sm))) * rig.rider->seat_offset;
        worst_seat = std::max(worst_seat, glm::length(HorseRig::world(rig.player) - seat));
    }
    expect(worst_rate <= rig.mount->turn_rate + 1.0f, "turn: never faster than turn_rate (" + std::to_string(worst_rate) + " deg/s)");
    expect(worst_side < 0.25f, "turn: no sideways slide (" + std::to_string(worst_side) + " m/s across the facing)");
    expect(worst_seat < 1e-3f, "turn: the rider stays in the saddle (" + std::to_string(worst_seat) + " m)");
    expect_near(rig.horse_cc->yaw_deg(), -90.0f, 2.0f, "turn: ends facing the input (+X)");

    // Let go: it slows to a stop and stands.
    for (int i = 0; i < 240; ++i) { rig.input(glm::vec2(0.0f)); rig.step(1); }
    v = rig.horse_cc->velocity();
    expect(glm::length(glm::vec2(v.x, v.y)) < 0.05f, "stop: no input stops the horse");
    expect(rig.mount->gait() == rig.mount->idle_state, "stop: back to " + rig.mount->gait());
}

/**
 * @brief Getting off: with the left side blocked the rider climbs down on the right, ends on the
 *        ground beside the horse with its controller back; with both sides blocked it stays on.
 */
COOPA_TEST(dismounts_onto_a_clear_side) {
    {
        HorseRig rig(glm::vec3(0.0f), 0.0f, true, glm::vec3(-4.0f, 0.0f, 0.0f));
        if (!rig.mount || !rig.rider) return;
        rig.start();
        rig.mount_up();
        // A wall where the left mount point is (added after the climb: physics refresh()).
        rig.step(1);
        const glm::vec3 left = rig.socket(rig.mount->mount_point_l);
        const glm::vec3 right = rig.socket(rig.mount->mount_point_r);
        rig.box("wall_l", glm::vec3(left.x - 0.2f, left.y, 1.0f), glm::vec3(0.6f, 2.0f, 2.0f));
        rig.physics->refresh();
        rig.step(2);
        rig.rider->interact = true;
        rig.step(60);
        expect(rig.rider->state() == Rider::State::OnFoot, "off: on foot after the climb down");
        expect(!rig.player_cc->is_suspended(), "off: the controller is back");
        expect(!rig.mount->is_ridden(), "off: the horse is free");
        const glm::vec3 feet = HorseRig::world(rig.player);
        expect(glm::length(glm::vec2(feet - right)) < 0.05f, "off: landed on the right (free) side " + str(feet) + " vs " + str(right));
        rig.step(30);
        expect(rig.player_cc->is_grounded(), "off: standing on the ground");
        expect_near(HorseRig::world(rig.player).z, rig.player_cc->skin, 0.03f, "off: at ground height");
    }
    {
        HorseRig rig(glm::vec3(0.0f), 0.0f, true, glm::vec3(-4.0f, 0.0f, 0.0f));
        if (!rig.mount || !rig.rider) return;
        rig.start();
        rig.mount_up();
        rig.step(1);
        for (const std::string* side : {&rig.mount->mount_point_l, &rig.mount->mount_point_r}) {
            const glm::vec3 p = rig.socket(*side);
            const float out = p.x < 0.0f ? -0.2f : 0.2f;
            rig.box("wall", glm::vec3(p.x + out, p.y, 1.0f), glm::vec3(0.6f, 2.0f, 2.0f));
        }
        rig.physics->refresh();
        rig.step(2);
        rig.rider->interact = true;
        rig.step(60);
        expect(rig.rider->state() == Rider::State::Riding, "blocked: both sides walled, the rider stays on");
        expect(rig.player_cc->is_suspended(), "blocked: still handed over");
    }
}

/**
 * @brief On a slope the horse's FootIK raises and pitches its body -- and the saddle with it --
 *        in its own IK pre-solve; the Rider (a later-priority IK driver) seats itself after that,
 *        so the rider sits exactly on the pitched saddle, even though it comes first in the scene.
 */
COOPA_TEST(rider_sits_on_the_saddle_of_a_horse_pitched_by_its_foot_ik) {
    const float slope = glm::radians(10.0f);
    HorseRig rig(glm::vec3(0.0f, 0.0f, 1.05f), 0.0f, true, glm::vec3(-4.0f, 0.0f, 1.2f));
    if (!rig.mount || !rig.rider || !rig.hooves) return;
    const glm::vec3 n(0.0f, -std::sin(slope), std::cos(slope));
    rig.box("slope", glm::vec3(0.0f, 0.0f, 1.0f) - 0.5f * n, glm::vec3(16.0f, 16.0f, 1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    rig.start();
    rig.mount_up();
    expect(rig.rider->state() == Rider::State::Riding, "slope: mounted");
    rig.step(30);
    expect(rig.hooves->body_pitch_deg() > 5.0f, "slope: the horse's body is pitched (" + std::to_string(rig.hooves->body_pitch_deg()) + " deg)");
    auto* saddle = rig.mount->socket(rig.mount->saddle);
    const glm::mat4 sm = saddle->get_transform()->get_world_matrix();
    const glm::vec3 seat = glm::vec3(sm[3]) + glm::normalize(glm::quat_cast(glm::mat3(sm))) * rig.rider->seat_offset;
    const float off = glm::length(HorseRig::world(rig.player) - seat);
    expect(off < 1e-3f, "slope: the rider sits on the pitched saddle (" + std::to_string(off) + " m off)");
}
