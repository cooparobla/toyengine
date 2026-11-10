/**
 * @file contact_solver_test.cpp
 * @brief The contact solver end to end on headless PhysicsWorld scenes: restitution, kinematic pushing,
 *        tumbling to rest, speculative CCD, stacking, friction on a slope, extreme mass ratios and
 *        capsule piles. Contact generation itself is narrowphase_test.cpp; sleeping is sleep_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("contact_solver");

using namespace coopa::physx;
using namespace physxtest;

/** @brief Restitution 1 on both sides bounces a dropped sphere back up most of the way; the
 *         default (restitution 0) material lands dead, with no meaningful upward velocity. */
COOPA_TEST(restitution_one_bounces_high_and_zero_never_bounces) {
    {
        dynamics::PhysicsMaterial bouncy;
        bouncy.restitution = 1.0f;

        PhysicsWorld world;
        add_static_ground(world, &bouncy);
        float start_z = 2.0f;
        dynamics::BodyId id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, start_z), 0.5f, &bouncy);
        float drop_height = start_z - 0.5f;

        bool has_bounced = false;
        float max_z_after_bounce = 0.0f;
        for (int i = 0; i < 300; ++i) {
            world.step_fixed(util::k_default_fixed_dt);
            const dynamics::Body* b = world.get_body(id);
            if (b->linear_velocity.z > 0.0f) {
                has_bounced = true;
                max_z_after_bounce = std::max(max_z_after_bounce, b->position.z);
            }
        }
        ASSERT_TRUE(has_bounced);
        // A near-elastic bounce should return well above half the drop height -- loose bound to
        // stay robust to discretization/solver damping while still proving restitution works.
        EXPECT_GT(max_z_after_bounce, 0.5f + 0.6f * drop_height);
    }
    {
        PhysicsWorld world;
        add_static_ground(world);
        dynamics::BodyId id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 2.0f), 0.5f);

        bool contacted = false;
        float max_upward_after_contact = 0.0f;
        for (int i = 0; i < 300; ++i) {
            world.step_fixed(util::k_default_fixed_dt);
            const dynamics::Body* b = world.get_body(id);
            if (b->position.z < 0.6f) contacted = true;
            if (contacted) max_upward_after_contact = std::max(max_upward_after_contact, b->linear_velocity.z);
        }
        ASSERT_TRUE(contacted);
        EXPECT_LT(max_upward_after_contact, 1.0f);
    }
}

COOPA_TEST(kinematic_platform_pushes_resting_body) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f)); // isolate the push effect from gravity

    dynamics::Body platform;
    platform.type = dynamics::BodyType::Kinematic;
    platform.position = glm::vec3(0.0f);
    platform.linear_velocity = glm::vec3(0.0f, 0.0f, 2.0f);
    world.add_body(platform, collision::Shape::make_box(glm::vec3(2.0f, 2.0f, 0.1f)));

    dynamics::BodyId sphere_id = add_dynamic_sphere(world, glm::vec3(0.0f, 0.0f, 0.6f), 0.5f);

    for (int i = 0; i < 10; ++i) world.step_fixed(util::k_default_fixed_dt);

    const dynamics::Body* s = world.get_body(sphere_id);
    ASSERT_TRUE(s->position.z > 0.55f);
    ASSERT_TRUE(s->linear_velocity.z > 0.5f);
}

/**
 * A box dropped at an oblique seed orientation must tumble on landing, then come fully to rest
 * flat on a face -- not freeze mid-topple, balanced on an edge/corner. A general regression
 * guard; offset_box_collider_binds_and_settles_flat_on_its_pivot covers the scene-level
 * stack case.
 */
COOPA_TEST(tumbled_box_settles_flat_not_balanced_on_an_edge) {
    PhysicsWorld world;
    add_static_ground(world);

    // Tilted ~25 degrees about a horizontal axis so it lands corner/edge-first and must tumble,
    // rather than settling straight down already aligned with a face.
    glm::quat tilt = glm::angleAxis(glm::radians(25.0f), glm::normalize(glm::vec3(1.0f, 0.3f, 0.0f)));
    dynamics::BodyId id = add_dynamic_box(world, glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.5f), tilt);

    for (int i = 0; i < 600; ++i) world.step_fixed(util::k_default_fixed_dt); // 10s -- generous settle budget

    const dynamics::Body* b = world.get_body(id);
    EXPECT_TRUE(is_settled(*b));
    // "Flat" means one of the box's local axes ends up within ~2 degrees of world +Z.
    EXPECT_GT(best_up_alignment(b->orientation), 0.999f);
}

/**
 * CCD (speculative contacts): a genuine tunneling reproduction. A small box (half-
 * extent 0.1) starts entirely clear of a thin static wall (half-extent 0.05, so 0.1 thick) and
 * moves at 50 m/s toward it -- one substep's displacement (50/60 ~= 0.833) is roughly 2.5x the
 * combined "skip zone" (box's own 0.2 full width + wall's 0.1 thickness = 0.3), so BEFORE this
 * substep integrates, the box's PRE-step pose ([4.4,4.6] on the approach axis) doesn't overlap
 * the wall ([4.95,5.05]), and its naive POST-step pose ([5.233,5.433]) doesn't either -- the box
 * would cross from entirely-before to entirely-after the wall within one substep with no
 * ordinary (non-speculative) contact ever generated to stop it. This is confirmed by hand
 * computation, not assumed: the ordinary narrowphase test at the pre-step pose provably finds no
 * overlap (4.6 < 4.95), so without the speculative path this substep produces zero manifolds and
 * the box would simply integrate straight through. The fast-pair gate must fire here (box's own
 * displacement 0.833 >> its own extent 0.1) and produce a speculative contact that clamps the
 * closing velocity before integration, stopping the box on the near side.
 */
COOPA_TEST(fast_box_does_not_tunnel_through_thin_wall) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f)); // isolate the CCD effect from gravity

    dynamics::Body wall;
    wall.type = dynamics::BodyType::Static;
    wall.position = glm::vec3(5.0f, 0.0f, 0.0f);
    dynamics::BodyId wall_id = world.add_body(wall, collision::Shape::make_box(glm::vec3(0.05f, 2.0f, 2.0f)));

    dynamics::BodyId box_id = add_dynamic_box(world, glm::vec3(4.5f, 0.0f, 0.0f), glm::vec3(0.1f));
    world.get_body(box_id)->linear_velocity = glm::vec3(50.0f, 0.0f, 0.0f);

    world.step_fixed(util::k_default_fixed_dt);

    const dynamics::Body* box = world.get_body(box_id);
    // Naive (no-collision-response) integration would have put it at x ~= 5.333, past the
    // wall's far face (5.05) -- must not have crossed even the wall's CENTER, let alone its far
    // side, and must be markedly slower than the un-constrained 50 m/s it started with.
    ASSERT_TRUE(box->position.x < 5.0f);
    ASSERT_TRUE(box->linear_velocity.x < 50.0f);

    // Let it settle fully and confirm a physically sane final state: resting against the wall's
    // near face (4.95), not embedded past it, and eventually at rest.
    for (int i = 0; i < 300; ++i) world.step_fixed(util::k_default_fixed_dt);
    const dynamics::Body* settled = world.get_body(box_id);
    ASSERT_TRUE(settled->position.x < 4.951f); // near face at 4.95; small slop tolerance
    ASSERT_TRUE(glm::length(settled->linear_velocity) < 0.05f);
    (void)wall_id;
}

/** @brief Ten flush-stacked unit boxes must settle without drifting sideways, every box flat
 *         (not balanced on an edge, with full 4-point face manifolds) and asleep. */
COOPA_TEST(ten_box_stack_settles_flat_and_sleeps) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(5.0f, 5.0f, 0.5f));

    const float half = 0.5f;
    std::vector<dynamics::BodyId> ids;
    for (int i = 0; i < 10; ++i) {
        glm::vec3 pos(0.0f, 0.0f, half * (2 * i + 1));
        ids.push_back(add_dynamic_box(world, pos, glm::vec3(half)));
    }

    for (int i = 0; i < 1000; ++i) world.step_fixed(util::k_default_fixed_dt);

    for (auto id : ids) {
        const dynamics::Body* b = world.get_body(id);
        EXPECT_LT(std::abs(b->position.x), 0.01f);
        EXPECT_LT(std::abs(b->position.y), 0.01f);
        EXPECT_GT(best_up_alignment(b->orientation), 0.999f);
        EXPECT_FALSE(b->awake);
    }
}

COOPA_TEST(friction_holds_above_tan_angle_and_slides_below) {
    const float angle_deg = 30.0f;
    glm::quat slope_rot = glm::angleAxis(glm::radians(angle_deg), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 slope_normal = slope_rot * glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 slope_tangent = glm::normalize(glm::vec3(slope_normal.z, 0.0f, -slope_normal.x)); // along the incline

    auto run = [&](float friction) {
        dynamics::PhysicsMaterial mat;
        mat.dynamic_friction = friction;
        mat.static_friction = friction;
        mat.friction_combine = dynamics::CombineMode::Minimum;

        PhysicsWorld world;
        add_static_box(world, glm::vec3(0.0f), glm::vec3(5.0f, 5.0f, 0.5f), slope_rot, &mat);
        glm::vec3 box_pos = slope_normal * 0.75f; // slope half-extent (0.5) + box half-extent (0.25)
        dynamics::BodyId id = add_dynamic_box(world, box_pos, glm::vec3(0.25f), slope_rot, &mat);

        glm::vec3 start = box_pos;
        for (int i = 0; i < 300; ++i) world.step_fixed(util::k_default_fixed_dt); // 5s
        const dynamics::Body* b = world.get_body(id);
        return glm::dot(b->position - start, slope_tangent);
    };

    float slid_high_friction = run(0.8f);   // > tan(30) ~= 0.577 -> should stay put
    float slid_low_friction = run(0.2f);    // < tan(30) -> should slide noticeably

    ASSERT_TRUE(std::abs(slid_high_friction) < 0.05f);
    ASSERT_TRUE(std::abs(slid_low_friction) > 0.3f); // slides a meaningful distance down the incline
}

COOPA_TEST(mass_ratio_100_to_1_stack_stays_stable) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(5.0f, 5.0f, 0.5f));

    dynamics::Body light;
    light.position = glm::vec3(0.0f, 0.0f, 0.5f);
    light.mass = 1.0f;
    light.inv_mass = 1.0f;
    light.inv_inertia_local = dynamics::box_inverse_inertia(glm::vec3(0.5f), 1.0f);
    dynamics::BodyId light_id = world.add_body(light, collision::Shape::make_box(glm::vec3(0.5f)));

    dynamics::Body heavy;
    heavy.position = glm::vec3(0.0f, 0.0f, 1.5f);
    heavy.mass = 100.0f;
    heavy.inv_mass = 1.0f / 100.0f;
    heavy.inv_inertia_local = dynamics::box_inverse_inertia(glm::vec3(0.5f), 100.0f);
    dynamics::BodyId heavy_id = world.add_body(heavy, collision::Shape::make_box(glm::vec3(0.5f)));

    for (int i = 0; i < 500; ++i) {
        world.step_fixed(util::k_default_fixed_dt);
        const dynamics::Body* l = world.get_body(light_id);
        const dynamics::Body* h = world.get_body(heavy_id);
        ASSERT_TRUE(std::isfinite(l->position.x) && std::isfinite(l->position.z));
        ASSERT_TRUE(std::isfinite(h->position.z));
        ASSERT_TRUE(glm::length(l->linear_velocity) < 50.0f);
        ASSERT_TRUE(glm::length(h->linear_velocity) < 50.0f);
    }
    const dynamics::Body* l = world.get_body(light_id);
    const dynamics::Body* h = world.get_body(heavy_id);
    ASSERT_TRUE(l->position.z > 0.3f && l->position.z < 0.7f);
    ASSERT_TRUE(h->position.z > 1.3f && h->position.z < 1.7f);
}

/** @brief A lone capsule lying along X rests at exactly its radius; a five-capsule pile beside
 *         it stays finite, bounded and above the floor every step while it settles. */
COOPA_TEST(capsules_rest_at_radius_and_pile_without_exploding) {
    PhysicsWorld world;
    add_static_box(world, glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(5.0f, 5.0f, 0.5f));

    const float radius = 0.3f;
    dynamics::BodyId lone = add_dynamic_capsule(world, glm::vec3(3.0f, 0.0f, 1.5f), radius, 0.5f, 0 /*axis X*/);

    std::vector<dynamics::BodyId> pile;
    for (int i = 0; i < 5; ++i) {
        glm::vec3 pos(0.1f * i, 0.0f, 1.0f + 0.5f * i);
        pile.push_back(add_dynamic_capsule(world, pos, 0.25f, 0.4f, 0));
    }

    for (int i = 0; i < 500; ++i) {
        world.step_fixed(util::k_default_fixed_dt);
        for (auto id : pile) {
            const dynamics::Body* b = world.get_body(id);
            ASSERT_TRUE(std::isfinite(b->position.x) && std::isfinite(b->position.z));
            ASSERT_LT(glm::length(b->linear_velocity), 50.0f);
            ASSERT_GT(b->position.z, -1.0f); // never tunnels through the floor
        }
    }

    const dynamics::Body* b = world.get_body(lone);
    EXPECT_NEAR(b->position.z, radius, 0.05f);
    EXPECT_LT(glm::length(b->linear_velocity), 0.1f);
}
