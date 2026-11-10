/**
 * @file ik_test.cpp
 * @brief Inverse kinematics: solve_two_bone() against hand-checkable geometry (reach, bone
 *        lengths preserved, pole-side bend, soft limit when out of reach), TwoBoneIK through the
 *        IkSystem under a rotated parent (stable frame to frame, weight 0 restores the input
 *        pose), and LookAtIK clamping, roll-free turns and smoothing.
 */
#include <coopa/testing/test.h>

#include "support/animation.h"

#include <coopa/animation/ik.h>
#include <coopa/animation/ik_system.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

COOPA_TEST_SUITE("ik");

using namespace coopa::anim;
using namespace coopa::scene;
using libcoopa_test::add_bone;
using libcoopa_test::step;

COOPA_TEST(two_bone_solver_reaches_preserves_lengths_and_bends_toward_pole) {
    using namespace coopa::anim::ik;
    const glm::vec3 a(0.0f), b(0.0f, 0.0f, -1.0f), c(0.0f, 0.0f, -2.0f);
    const glm::vec3 target(0.5f, 0.3f, -1.2f), pole(0.0f, 3.0f, -1.0f);
    const TwoBoneResult r = solve_two_bone(a, b, c, target, pole, true, 0.0f);
    EXPECT_TRUE(r.reached);
    EXPECT_LT(glm::length(r.end - target), 1e-4f);
    EXPECT_NEAR(glm::length(r.mid - a), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(r.end - r.mid), 1.0f, 1e-4f);
    // Rotations reproduce the solved joints.
    const glm::vec3 mid = a + r.upper_delta * (b - a);
    const glm::vec3 end = mid + r.lower_delta * (r.upper_delta * (c - b));
    EXPECT_LT(glm::length(mid - r.mid), 1e-4f);
    EXPECT_LT(glm::length(end - target), 1e-4f);
    // Mid joint lies in the (a, target, pole) plane, on the pole's side of the reach line.
    const glm::vec3 n = glm::normalize(glm::cross(target - a, pole - a));
    EXPECT_LT(std::abs(glm::dot(r.mid - a, n)), 1e-4f);
    const glm::vec3 dir = glm::normalize(target - a);
    const glm::vec3 mid_off  = (r.mid - a) - dir * glm::dot(r.mid - a, dir);
    const glm::vec3 pole_off = (pole - a) - dir * glm::dot(pole - a, dir);
    EXPECT_GT(glm::dot(mid_off, pole_off), 0.0f);

    // Out of reach: fully extended toward it, no NaNs; the soft limit stops short of straight.
    const TwoBoneResult far = solve_two_bone(a, b, c, glm::vec3(0.0f, 5.0f, 0.0f), pole, true, 0.05f);
    EXPECT_FALSE(far.reached);
    EXPECT_TRUE(std::isfinite(far.mid.x) && std::isfinite(far.end.y));
    EXPECT_TRUE(far.end.y > 1.85f && far.end.y < 2.0f);
}

COOPA_TEST(two_bone_component_holds_target_under_rotated_parent_and_weight_zero_restores) {
    Scene scene("TwoBoneIK");
    auto root_obj = std::make_unique<SceneObject>("Rig");
    root_obj->add_component<TransformComponent>();
    auto* ikc = root_obj->add_component<TwoBoneIK>();
    SceneObject* rig = scene.add_root_object(std::move(root_obj));
    rig->get_transform()->transform().set_rotation(glm::vec3(0.0f, 0.0f, 30.0f)); // exercises to_local_delta
    rig->get_transform()->transform().set_position(glm::vec3(1.0f, 2.0f, 3.0f));
    SceneObject* upper = add_bone(rig, "upper", {0.0f, 0.0f, 0.0f});
    SceneObject* lower = add_bone(upper, "lower", {0.0f, 0.0f, -1.0f});
    SceneObject* end   = add_bone(lower, "end", {0.0f, 0.0f, -1.0f});
    auto tgt = std::make_unique<SceneObject>("target");
    tgt->add_component<TransformComponent>()->transform().set_position(glm::vec3(1.6f, 2.4f, 1.7f));
    scene.add_root_object(std::move(tgt));
    auto pole_obj = std::make_unique<SceneObject>("pole");
    pole_obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(1.0f, 6.0f, 2.0f));
    scene.add_root_object(std::move(pole_obj));
    ikc->upper  = "upper";
    ikc->lower  = "upper/lower";
    ikc->end    = "upper/lower/end";
    ikc->target = "target";
    ikc->pole   = "pole";
    ikc->soft_limit = 0.0f;
    install_ik_system(scene);
    scene.start();

    // An un-animated chain over several frames: stable, no drift frame to frame.
    const glm::vec3 target_p(1.6f, 2.4f, 1.7f);
    for (int frame = 0; frame < 4; ++frame) {
        step(scene, 1.0f / 60.0f);
        const glm::vec3 e = glm::vec3(end->get_transform()->get_world_matrix()[3]);
        EXPECT_LT(glm::length(e - target_p), 1e-3f);
    }
    const glm::vec3 m = glm::vec3(lower->get_transform()->get_world_matrix()[3]);
    EXPECT_GT(m.y, 2.0f); // elbow bent toward the +Y pole

    // Weight 0 restores the un-animated input pose exactly.
    ikc->weight = 0.0f;
    step(scene, 1.0f / 60.0f);
    EXPECT_VEC_NEAR(glm::vec3(end->get_transform()->get_world_matrix()[3]), glm::vec3(1.0f, 2.0f, 1.0f), 1e-4f);
}

COOPA_TEST(look_at_clamps_to_max_angle_without_roll_and_smooths_toward_target) {
    Scene scene("LookAtIK");
    auto root_obj = std::make_unique<SceneObject>("Rig");
    root_obj->add_component<TransformComponent>();
    SceneObject* rig  = scene.add_root_object(std::move(root_obj));
    SceneObject* head = add_bone(rig, "head", {0.0f, 0.0f, 1.6f});
    auto* look = head->add_component<LookAtIK>();
    look->max_angle = 60.0f;
    look->set_target_position(glm::vec3(-3.0f, -3.0f, 1.6f)); // 135 degrees off +Y
    install_ik_system(scene);
    scene.start();

    auto forward = [&]() {
        const glm::quat r = coopa::anim::ik::rotation_of(head->get_transform()->get_world_matrix());
        return glm::normalize(r * glm::vec3(0.0f, 1.0f, 0.0f));
    };
    for (int i = 0; i < 5; ++i) {
        step(scene, 1.0f / 60.0f);
        const glm::vec3 f = forward();
        const float ang = glm::degrees(std::acos(std::clamp(f.y, -1.0f, 1.0f)));
        EXPECT_NEAR(ang, 60.0f, 0.1f); // clamped, and not accumulating
        EXPECT_LT(f.x, 0.0f);          // toward the target's side
        EXPECT_LT(std::abs(f.z), 1e-3f); // in the horizontal plane
    }
    // Up axis kept: no roll introduced by a level turn.
    const glm::quat r = coopa::anim::ik::rotation_of(head->get_transform()->get_world_matrix());
    EXPECT_GT((r * glm::vec3(0.0f, 0.0f, 1.0f)).z, 0.999f);

    // In range: looks straight at it.
    look->set_target_position(glm::vec3(1.0f, 2.0f, 2.6f));
    step(scene, 1.0f / 60.0f);
    EXPECT_LT(glm::length(forward() - glm::normalize(glm::vec3(1.0f, 2.0f, 1.0f))), 1e-3f);

    // Smoothing approaches the target gradually.
    look->smoothing = 0.2f;
    look->set_target_position(glm::vec3(-1.0f, 2.0f, 1.6f));
    step(scene, 1.0f / 60.0f);
    const glm::vec3 want2 = glm::normalize(glm::vec3(-1.0f, 2.0f, 0.0f));
    EXPECT_GT(glm::length(forward() - want2), 0.05f);
    step(scene, 1.0f / 60.0f, 120);
    EXPECT_LT(glm::length(forward() - want2), 1e-2f);
}

namespace {

/** @brief A driver that logs its pre- and post-solves. */
struct LoggingDriver : IkDriver {
    std::string tag;
    std::vector<std::string>* log = nullptr;
    std::string type_name() const override { return "LoggingDriver"; }
    void ik_pre_solve(float) override { log->push_back("pre:" + tag); }
    void ik_post_solve(float) override { log->push_back("post:" + tag); }
};

} // namespace

/**
 * @brief Drivers run by `priority` (lower first), scene pre-order among equals -- so a driver
 *        that reads a rig another driver poses (a rider on a horse whose FootIK pitches it) can
 *        go after it whatever the scene order -- for the post-solves too.
 */
COOPA_TEST(drivers_run_by_priority_then_scene_order) {
    Scene scene("ik_driver_order");
    std::vector<std::string> log;
    auto add = [&](const std::string& name, int priority) {
        auto obj = std::make_unique<SceneObject>(name);
        obj->add_component<TransformComponent>();
        auto* d = obj->add_component<LoggingDriver>();
        d->tag = name;
        d->log = &log;
        d->priority = priority;
        scene.add_root_object(std::move(obj));
    };
    add("rider", 100);   // first in the scene, but reads the others' pose
    add("horse", 0);
    add("dog", 0);
    scene.start();
    install_ik_system(scene);
    step(scene, 1.0f / 60.0f);
    const std::vector<std::string> want = {"pre:horse", "pre:dog", "pre:rider", "post:horse", "post:dog", "post:rider"};
    EXPECT_TRUE(log == want);
}
