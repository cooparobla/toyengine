/**
 * @file joints_test.cpp
 * @brief Joints on headless PhysicsWorld: hinge limits, free hinge pendulum, a zero-range hinge as a
 *        fixed joint, ball joint anchoring/removal, and cone-twist swing/twist limits. Joint
 *        components through the scene are in physics_system_test.cpp; joint islands in sleep_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/dynamics/inertia.h>

COOPA_TEST_SUITE("joints");

using namespace coopa::physx;

/**
 * A door: a static frame (world.add_body() with no shape -- Shape::enabled's default, "no
 * collision, just a fixed anchor") hinged to a dynamic panel via a vertical (Z) axis, with a
 * 90-degree swing limit. Given a fast initial spin (not gravity -- a vertical-axis door has no
 * gravity torque about its own hinge, matching real doors), it must swing and come to rest AT
 * the limit, not past it, while the point and axis constraints hold throughout the fast swing.
 */
COOPA_TEST(hinge_door_swings_and_stops_at_its_limit) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f));

    dynamics::Body frame_body;
    frame_body.type = dynamics::BodyType::Static;
    frame_body.position = glm::vec3(0.0f);
    dynamics::BodyId frame_id = world.add_body(frame_body);

    dynamics::Body door_body;
    door_body.position = glm::vec3(1.0f, 0.0f, 0.0f); // door's own center, 1 unit from the hinge
    door_body.angular_velocity = glm::vec3(0.0f, 0.0f, 5.0f); // fast push, well past the limit if unconstrained
    dynamics::BodyId door_id = world.add_body(door_body);

    // local_anchor_b = (-1,0,0): offset from the door's own center back to the hinge point, so
    // world_anchor_b starts exactly at world_anchor_a (0,0,0), matching the frame's anchor.
    dynamics::JointId joint = world.add_hinge_joint(
        frame_id, door_id, glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), /*use_limits=*/true, /*min_angle=*/0.0f, /*max_angle=*/glm::radians(90.0f));
    ASSERT_TRUE(joint.is_valid());

    for (int i = 0; i < 300; ++i) world.step_fixed(util::k_default_fixed_dt); // 5s -- generous settle budget

    const dynamics::Body* frame = world.get_body(frame_id);
    const dynamics::Body* door = world.get_body(door_id);
    const dynamics::HingeJoint* j = world.get_joint(joint);
    ASSERT_TRUE(j != nullptr);

    // Point constraint held: the door's hinge edge is still at the frame's anchor.
    glm::vec3 world_anchor_a = frame->position + frame->orientation * j->local_anchor_a;
    glm::vec3 world_anchor_b = door->position + door->orientation * j->local_anchor_b;
    ASSERT_NEAR(glm::length(world_anchor_b - world_anchor_a), 0.0f, 0.02f);

    // Axis constraint held: the door didn't tip out of its horizontal swing plane.
    glm::vec3 world_axis_b = glm::normalize(door->orientation * j->local_axis_b);
    ASSERT_NEAR(world_axis_b.z, 1.0f, 0.02f);

    // Stopped AT the limit, not past it, and settled (not still spinning against the stop).
    float angle = dynamics::hinge_current_angle(frame->orientation, door->orientation, j->local_axis_a, j->rest_relative_rotation);
    ASSERT_NEAR(angle, glm::radians(90.0f), glm::radians(3.0f));
    ASSERT_TRUE(glm::length(door->angular_velocity) < 0.05f);
}

/**
 * A free (no limits) hinge -- a pendulum hanging from a static ceiling anchor, swinging under
 * gravity about a horizontal (Y) axis. Confirms the point stays anchored and the axis stays
 * aligned throughout real, sustained swinging motion (not just at rest), and that it actually
 * DOES swing (gravity produces real torque through the joint, not a rigid lock).
 */
COOPA_TEST(hinge_free_pendulum_swings_without_drift) {
    PhysicsWorld world; // default gravity -Z

    dynamics::Body ceiling_body;
    ceiling_body.type = dynamics::BodyType::Static;
    ceiling_body.position = glm::vec3(0.0f);
    dynamics::BodyId ceiling_id = world.add_body(ceiling_body);

    dynamics::Body pendulum_body;
    pendulum_body.position = glm::vec3(1.0f, 0.0f, 0.0f); // hangs out horizontally at first
    dynamics::BodyId pendulum_id = world.add_body(pendulum_body);

    dynamics::JointId joint = world.add_hinge_joint(ceiling_id, pendulum_id, glm::vec3(0.0f),
                                                      glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                                                      glm::vec3(0.0f, 1.0f, 0.0f), /*use_limits=*/false);
    ASSERT_TRUE(joint.is_valid());

    float start_z = world.get_body(pendulum_id)->position.z;
    float max_drift = 0.0f;
    float min_axis_alignment = 1.0f;
    for (int i = 0; i < 120; ++i) { // 2s -- enough to swing well away from the horizontal start
        world.step_fixed(util::k_default_fixed_dt);
        const dynamics::Body* ceiling = world.get_body(ceiling_id);
        const dynamics::Body* pendulum = world.get_body(pendulum_id);
        const dynamics::HingeJoint* j = world.get_joint(joint);
        glm::vec3 world_anchor_a = ceiling->position + ceiling->orientation * j->local_anchor_a;
        glm::vec3 world_anchor_b = pendulum->position + pendulum->orientation * j->local_anchor_b;
        max_drift = std::max(max_drift, glm::length(world_anchor_b - world_anchor_a));

        glm::vec3 world_axis_a = glm::normalize(ceiling->orientation * j->local_axis_a);
        glm::vec3 world_axis_b = glm::normalize(pendulum->orientation * j->local_axis_b);
        min_axis_alignment = std::min(min_axis_alignment, glm::dot(world_axis_a, world_axis_b));
    }

    ASSERT_TRUE(max_drift < 0.05f); // point constraint never drifted meaningfully, even mid-swing
    ASSERT_TRUE(min_axis_alignment > 0.98f); // axes stayed close to parallel throughout (cos ~11 degrees)

    // Actually swung: gravity pulled it down and away from its purely-horizontal start.
    float end_z = world.get_body(pendulum_id)->position.z;
    ASSERT_TRUE(end_z < start_z - 0.3f);
}

/**
 * `min_angle == max_angle == 0` locks the hinge's one remaining DOF entirely -- a fully rigid
 * attachment with no separate "fixed joint" implementation (see joint.h's file doc). A hard
 * spin plus gravity torque (both trying to rotate the panel) must produce ~zero net rotation.
 */
COOPA_TEST(hinge_zero_range_limit_acts_rigid) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f, 0.0f, -9.81f));

    dynamics::Body frame_body;
    frame_body.type = dynamics::BodyType::Static;
    frame_body.position = glm::vec3(0.0f);
    dynamics::BodyId frame_id = world.add_body(frame_body);

    dynamics::Body panel_body;
    panel_body.position = glm::vec3(1.0f, 0.0f, 0.0f);
    panel_body.angular_velocity = glm::vec3(0.0f, 0.0f, 5.0f); // hard spin about the (rigidly locked) hinge axis
    dynamics::BodyId panel_id = world.add_body(panel_body);

    // Hinge axis Z (spin is about the locked axis); gravity (-Z) has no torque about a
    // vertical axis here either, so this specifically isolates whether the ANGULAR SPIN gets
    // absorbed -- gravity is still on to confirm the point constraint holds a hanging weight too.
    dynamics::JointId joint = world.add_hinge_joint(
        frame_id, panel_id, glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), /*use_limits=*/true, /*min_angle=*/0.0f, /*max_angle=*/0.0f);
    ASSERT_TRUE(joint.is_valid());

    for (int i = 0; i < 120; ++i) world.step_fixed(util::k_default_fixed_dt);

    const dynamics::Body* frame = world.get_body(frame_id);
    const dynamics::Body* panel = world.get_body(panel_id);
    const dynamics::HingeJoint* j = world.get_joint(joint);
    float angle = dynamics::hinge_current_angle(frame->orientation, panel->orientation, j->local_axis_a, j->rest_relative_rotation);
    ASSERT_NEAR(angle, 0.0f, glm::radians(3.0f));

    // Point constraint still holds the panel up against gravity.
    glm::vec3 world_anchor_a = frame->position + frame->orientation * j->local_anchor_a;
    glm::vec3 world_anchor_b = panel->position + panel->orientation * j->local_anchor_b;
    ASSERT_NEAR(glm::length(world_anchor_b - world_anchor_a), 0.0f, 0.05f);
}

/**
 * A ball joint holds its anchor through sustained 3D motion: a capsule-shaped bob hanging from a
 * static ceiling point, kicked sideways AND forward so it swings on a cone (a hinge could not),
 * must keep its end at the ceiling anchor every step -- and actually move.
 */
COOPA_TEST(ball_joint_holds_anchor_and_releases_on_remove) {
    PhysicsWorld world; // default gravity -Z

    dynamics::Body ceiling_body;
    ceiling_body.type = dynamics::BodyType::Static;
    dynamics::BodyId ceiling_id = world.add_body(ceiling_body);

    dynamics::Body bob_body;
    bob_body.position = glm::vec3(0.0f, 0.0f, -0.5f); // centre 0.5 below the anchor
    bob_body.mass = 2.0f;
    bob_body.inv_mass = 0.5f;
    collision::Shape bob_shape = collision::Shape::make_capsule(0.08f, 0.42f);
    bob_body.inv_inertia_local = dynamics::inertia_for_shape(bob_shape, 2.0f);
    bob_body.linear_velocity = glm::vec3(2.0f, 1.5f, 0.0f);
    dynamics::BodyId bob_id = world.add_body(bob_body, bob_shape);

    dynamics::JointId joint = world.add_ball_joint(ceiling_id, bob_id, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 0.5f));
    ASSERT_TRUE(joint.is_valid());
    ASSERT_TRUE(world.get_joint(joint)->type == dynamics::JointType::Ball);

    float max_drift = 0.0f;
    float max_x = 0.0f, max_y = 0.0f;
    for (int i = 0; i < 180; ++i) {
        world.step_fixed(util::k_default_fixed_dt);
        const dynamics::Body* bob = world.get_body(bob_id);
        const dynamics::Joint* j = world.get_joint(joint);
        glm::vec3 anchor_b = bob->position + bob->orientation * j->local_anchor_b;
        max_drift = std::max(max_drift, glm::length(anchor_b));
        max_x = std::max(max_x, std::abs(bob->position.x));
        max_y = std::max(max_y, std::abs(bob->position.y));
    }
    ASSERT_TRUE(max_drift < 0.02f);
    // Swung out in BOTH horizontal directions (a cone, not a plane) and stayed on the 0.5 m sphere.
    ASSERT_TRUE(max_x > 0.15f && max_y > 0.1f);
    ASSERT_NEAR(glm::length(world.get_body(bob_id)->position), 0.5f, 0.03f);

    // remove_joint() stops the constraint at once: the bob falls away.
    world.remove_joint(joint);
    for (int i = 0; i < 30; ++i) world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(glm::length(world.get_body(bob_id)->position) > 0.8f);
}

/**
 * A cone-twist joint keeps the anchor, keeps the swing inside its cone and the twist inside its
 * range: a limb hanging along -Z from a static shoulder is spun hard about a horizontal axis
 * (swing) and about its own long axis (twist), with gravity off so nothing but the limits stops
 * it. Both limits must hold within a few degrees, and both must actually be reached.
 */
COOPA_TEST(cone_twist_respects_swing_and_twist_limits) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f));

    dynamics::Body shoulder_body;
    shoulder_body.type = dynamics::BodyType::Static;
    dynamics::BodyId shoulder_id = world.add_body(shoulder_body);

    dynamics::Body arm_body;
    arm_body.position = glm::vec3(0.0f, 0.0f, -0.3f);
    collision::Shape arm_shape = collision::Shape::make_capsule(0.06f, 0.24f);
    arm_body.mass = 2.0f;
    arm_body.inv_mass = 0.5f;
    arm_body.inv_inertia_local = dynamics::inertia_for_shape(arm_shape, 2.0f);
    arm_body.angular_velocity = glm::vec3(6.0f, 0.0f, 9.0f); // swing about X + twist about Z
    dynamics::BodyId arm_id = world.add_body(arm_body, arm_shape);

    const float swing_limit = glm::radians(40.0f);
    const float twist_min = glm::radians(-20.0f), twist_max = glm::radians(25.0f);
    dynamics::JointId joint = world.add_cone_twist_joint(shoulder_id, arm_id, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 0.3f),
                                                          glm::vec3(0.0f, 0.0f, -1.0f), swing_limit, twist_min, twist_max);
    ASSERT_TRUE(joint.is_valid());

    float max_swing = 0.0f, max_twist = -10.0f, min_twist = 10.0f, max_drift = 0.0f;
    for (int i = 0; i < 240; ++i) {
        world.step_fixed(util::k_default_fixed_dt);
        const dynamics::Body* a = world.get_body(shoulder_id);
        const dynamics::Body* b = world.get_body(arm_id);
        const dynamics::Joint* j = world.get_joint(joint);
        glm::vec3 ta, tb, swing_axis;
        dynamics::cone_twist_axes(*a, *b, *j, ta, tb);
        max_swing = std::max(max_swing, dynamics::cone_swing(ta, tb, swing_axis));
        float twist = dynamics::cone_twist_angle(*a, *b, *j);
        max_twist = std::max(max_twist, twist);
        min_twist = std::min(min_twist, twist);
        glm::vec3 anchor_b = b->position + b->orientation * j->local_anchor_b;
        max_drift = std::max(max_drift, glm::length(anchor_b));
    }
    const float tol = glm::radians(4.0f);
    ASSERT_TRUE(max_swing <= swing_limit + tol);
    ASSERT_TRUE(max_swing >= swing_limit - tol);   // the cone was actually reached
    ASSERT_TRUE(min_twist >= twist_min - tol);
    ASSERT_TRUE(min_twist <= twist_min + tol);     // ...and the twist range's lower end (+Z spin
                                                   // is a NEGATIVE twist about the -Z axis)
    ASSERT_TRUE(max_twist <= twist_max + tol);
    ASSERT_TRUE(max_drift < 0.02f);
}
