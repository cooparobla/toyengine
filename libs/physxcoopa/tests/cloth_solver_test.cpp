/**
 * @file cloth_solver_test.cpp
 * @brief The XPBD cloth solver: grid topology and parallel-safe batches, analytic free fall on the
 *        frame clock, anchors and tethers, kinematic anchors across uneven frames, self-collision,
 *        sleep/wake, determinism, handles, and ClothComponent binding. Collider projection is
 *        cloth_collision_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/cloth/cloth_builder.h>
#include <physxcoopa/cloth/cloth_solver.h>
#include <physxcoopa/components/cloth.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/job/engine.h>
#include <memory>
#include "support/cloth_fixtures.h"

COOPA_TEST_SUITE("cloth_solver");

using namespace coopa::physx;
using namespace physxtest;

/**
 * @brief Grid topology counts, plus the property the whole parallel story rests on: every
 *        ConstraintBatch's particle set is pairwise disjoint. If that ever regresses, a
 *        job-parallel dispatcher would silently produce nondeterministic results rather than
 *        crashing, so it is asserted structurally here rather than being left to a race.
 */
COOPA_TEST(grid_builder_topology_and_disjoint_batches) {
    const uint32_t res = 9;
    cloth::Cloth c = make_test_sheet(res, glm::vec3(0.0f, 0.0f, 2.0f));

    ASSERT_TRUE(c.particles.size() == res * res);
    ASSERT_TRUE(c.columns == res && c.rows == res);
    // (res-1)*res horizontal + the same vertical + two diagonals per cell.
    const std::size_t structural = 2u * (res - 1) * res;
    const std::size_t shear = 2u * (res - 1) * (res - 1);
    ASSERT_TRUE(c.stretch.size() == structural + shear);
    ASSERT_TRUE(c.bend.size() == 2u * (res - 2) * res);
    ASSERT_TRUE(c.triangles.size() == (res - 1) * (res - 1) * 6);

    // Rest lengths must match the authored spacing exactly (4 m over res-1 gaps).
    const float spacing = 4.0f / static_cast<float>(res - 1);
    ASSERT_NEAR(c.stretch[0].rest_length, spacing, 1e-5f);

    auto batches_disjoint = [](const std::vector<cloth::ClothConstraint>& list,
                               const std::vector<cloth::ConstraintBatch>& batches,
                               std::size_t particle_count) {
        for (const cloth::ConstraintBatch& b : batches) {
            std::vector<bool> seen(particle_count, false);
            for (uint32_t i = b.begin; i < b.end; ++i) {
                if (seen[list[i].a] || seen[list[i].b]) return false;
                seen[list[i].a] = true;
                seen[list[i].b] = true;
            }
        }
        return true;
    };
    ASSERT_TRUE(batches_disjoint(c.stretch, c.stretch_batches, c.particles.size()));
    ASSERT_TRUE(batches_disjoint(c.bend, c.bend_batches, c.particles.size()));

    // Every constraint must land in exactly one batch -- a gap would silently stop solving part
    // of the sheet, which no visual check would obviously catch.
    std::size_t covered = 0;
    for (const cloth::ConstraintBatch& b : c.stretch_batches) covered += b.end - b.begin;
    ASSERT_TRUE(covered == c.stretch.size());
    covered = 0;
    for (const cloth::ConstraintBatch& b : c.bend_batches) covered += b.end - b.begin;
    ASSERT_TRUE(covered == c.bend.size());
}

/** @brief An unpinned sheet in free space falls exactly 0.5*g*t^2 -- the integrator sanity check,
 *         mirroring free_fall_matches_analytic_while_static_and_kinematic_hold for rigid bodies. Constraints are all
 *         satisfied at rest, so they contribute nothing and the analytic result holds exactly. */
COOPA_TEST(free_fall_matches_analytic) {
    PhysicsWorld world;
    // Zero the two things that would otherwise (correctly) break the analytic result: velocity
    // damping, and the aerodynamic drag/lift that are on by default -- a horizontal sheet falling
    // flat presents maximum area to the flow, so default air_drag alone slows this fall by ~27%.
    cloth::ClothParams params;
    params.damping = 0.0f;
    params.air_drag = 0.0f;
    params.air_lift = 0.0f;
    cloth::Cloth c = make_test_sheet(5, glm::vec3(0.0f, 0.0f, 10.0f), params);
    const glm::vec3 start = c.particles[0].position;
    cloth::ClothId id = world.add_cloth(std::move(c));

    const float h = util::k_default_fixed_dt;
    const int steps = 60;
    for (int i = 0; i < steps; ++i) world.step(h);

    const cloth::Cloth* sim = world.get_cloth(id);
    const float t = static_cast<float>(steps) * h;
    // Semi-implicit Euler over N substeps of hs accumulates 0.5*g*t^2 + 0.5*g*t*hs.
    const float hs = h / static_cast<float>(world.config().cloth_substeps);
    const float expected_drop = 0.5f * 9.81f * t * t + 0.5f * 9.81f * t * hs;
    ASSERT_NEAR(start.z - sim->particles[0].position.z, expected_drop, 0.05f);
    // Damping is the only thing acting laterally, and there is none: X/Y must not drift at all.
    ASSERT_NEAR(sim->particles[0].position.x, start.x, 1e-4f);
    ASSERT_NEAR(sim->particles[0].position.y, start.y, 1e-4f);
}

/** @brief A frame too short to run a rigid substep still advances the cloth.
 *
 *  The other half of the same rule: cloth is on the frame clock, so it moves whenever a frame
 *  does, not only when the accumulator happens to clear fixed_dt. Two half-substeps -- the first
 *  runs no substep at all -- and the sheet must have fallen by both of them. */
COOPA_TEST(advances_when_no_rigid_substep_runs) {
    PhysicsWorld world;
    cloth::ClothParams params;
    params.damping = 0.0f;
    params.air_drag = 0.0f;
    params.air_lift = 0.0f;
    cloth::Cloth c = make_test_sheet(5, glm::vec3(0.0f, 0.0f, 10.0f), params);
    const float start_z = c.particles[0].position.z;
    cloth::ClothId id = world.add_cloth(std::move(c));

    const float half = util::k_default_fixed_dt * 0.5f;
    world.step(half); // accumulator below fixed_dt: zero rigid substeps
    const float after_one = world.get_cloth(id)->particles[0].position.z;
    ASSERT_TRUE(start_z - after_one > 1e-4f);

    world.step(half);
    ASSERT_TRUE(after_one - world.get_cloth(id)->particles[0].position.z > 1e-4f);
    // Two half-frames of free fall land exactly where one whole frame would: the sheet advances on
    // wall-clock time, not on substep count. Same analytic form free_fall_matches_analytic
    // uses (semi-implicit Euler carries a 0.5*g*t*hs term), and `hs` is unchanged at
    // 1/240 s because step_cloths_() sizes the substep count to hold that length -- each half
    // frame runs 2 substeps where a whole one runs 4.
    const float t = util::k_default_fixed_dt;
    const float hs = t / static_cast<float>(world.config().cloth_substeps);
    const float drop = start_z - world.get_cloth(id)->particles[0].position.z;
    ASSERT_NEAR(drop, 0.5f * 9.81f * t * t + 0.5f * 9.81f * t * hs, 1e-5f);
}

/** @brief Statically pinned particles never move, and tethers cap every particle's distance from
 *         its anchor at the geodesic rest length times tether_scale, even under a hard sideways
 *         yank that distance constraints alone would need far more iterations to resist. */
COOPA_TEST(static_anchors_hold_and_tethers_cap_distance) {
    PhysicsWorld world;

    cloth::ClothParams params;
    params.tether_scale = 1.02f;
    params.external_acceleration = glm::vec3(200.0f, 0.0f, 0.0f); // a violent lateral yank
    cloth::Cloth c = make_test_sheet(11, glm::vec3(0.0f, 0.0f, 5.0f), params);
    cloth::pin_static(c, glm::vec3(0.0f, 0.0f, 5.0f), 0.5f);
    cloth::build_tethers(c);
    ASSERT_TRUE(!c.tethers.empty());
    cloth::ClothId id = world.add_cloth(std::move(c));

    for (int i = 0; i < 180; ++i) world.step(util::k_default_fixed_dt);

    const cloth::Cloth* sim = world.get_cloth(id);
    for (const cloth::ClothTether& t : sim->tethers) {
        const float d = glm::length(sim->particles[t.particle].position -
                                    sim->particles[t.anchor].position);
        ASSERT_TRUE(d <= t.max_length + 1e-3f);
    }
    // ...and the statically pinned particles themselves never moved under that yank.
    ASSERT_FALSE(sim->anchors.empty());
    for (const cloth::ClothAnchor& a : sim->anchors) {
        EXPECT_VEC_NEAR(sim->particles[a.particle].position, a.world_position, 1e-5f);
        EXPECT_EQ(sim->particles[a.particle].inv_mass, 0.0f);
    }
}

/**
 * @brief The sheet keeps its grip on a kinematic body across UNEVEN frames -- the regression test
 *        for the cloth/ball jitter.
 *
 * Cloth is solved once per frame in step(), not inside step_fixed() on the 1/60 s grid, because a
 * kinematic body is posed on the wall clock every frame. On the fixed grid, whenever a frame's dt
 * fell short of the substep length the accumulator would run ZERO substeps, so the sheet would
 * stand perfectly still in world space while the body travelled a whole frame -- and the next
 * frame would run two substeps and snap it back. Rendered, that beat is jitter.
 *
 * The dt sequence below alternates 0.9h/1.1h precisely to drive the accumulator across that
 * boundary repeatedly (the pattern a vsynced display beating against a 60 Hz fixed step produces
 * for real). The anchored particles are written outright from the body's pose, so "did the cloth
 * get stepped this frame" is measurable to the micrometre at the anchor: the offset from the body
 * centre must not move. Solved on the fixed grid it would drift by up to speed*dt (~6 cm) on every
 * zero-substep frame.
 */
COOPA_TEST(anchor_tracks_kinematic_body_across_uneven_frames) {
    PhysicsWorld world;

    dynamics::Body ball;
    ball.type = dynamics::BodyType::Kinematic;
    ball.position = glm::vec3(0.0f, 0.0f, 3.0f);
    ball.inv_mass = 0.0f;
    ball.use_gravity = false;
    dynamics::BodyId ball_id = world.add_body(ball, collision::Shape::make_sphere(1.0f));

    cloth::Cloth c = make_test_sheet(15, glm::vec3(0.0f, 0.0f, 4.05f));
    ASSERT_TRUE(cloth::pin_to_body(c, ball_id, *world.get_body(ball_id),
                                   glm::vec3(0.0f, 0.0f, 4.0f), 0.4f) > 0);
    cloth::build_tethers(c);
    cloth::ClothId id = world.add_cloth(std::move(c));

    const float h = util::k_default_fixed_dt;
    for (int i = 0; i < 120; ++i) world.step(h); // let it drape first

    // Offsets the anchors hold while the ball is still -- exactly what must survive the drive.
    const cloth::Cloth* sim = world.get_cloth(id);
    std::vector<glm::vec3> rest_offsets;
    for (const cloth::ClothAnchor& a : sim->anchors) {
        rest_offsets.push_back(sim->particles[a.particle].position - world.get_body(ball_id)->position);
    }

    const float speed = 4.0f; // assets/scenes/cloth_test's KinematicController move_speed
    for (int frame = 0; frame < 240; ++frame) {
        const float dt = (frame % 2 == 0) ? h * 0.9f : h * 1.1f;
        // What PhysicsSystem::sync_transforms_in_() does for a kinematic body: hard-set the pose
        // the wall clock asks for, derive the velocity from the delta.
        dynamics::Body* b = world.get_body(ball_id);
        b->position.x += speed * dt;
        b->linear_velocity = glm::vec3(speed, 0.0f, 0.0f);
        world.step(dt);

        const cloth::Cloth* live = world.get_cloth(id);
        for (std::size_t i = 0; i < live->anchors.size(); ++i) {
            const glm::vec3 offset =
                live->particles[live->anchors[i].particle].position - world.get_body(ball_id)->position;
            ASSERT_VEC_NEAR(offset, rest_offsets[i], 1e-3f);
        }
    }
}

/**
 * @brief Self-collision separates particles that are not topological neighbours, and does nothing
 *        when disabled.
 *
 * Deliberately isolated from the constraint solve: the cloth's stretch/bend constraints are
 * cleared, so the only thing that can move a particle is the self-collision stage itself. Testing
 * it through a naturally-folding drape instead would make the assertion depend on whether that
 * particular sheet happened to fold at all, which is exactly the sort of test that passes for the
 * wrong reason. The real-drape path is still exercised (with self_collision on) by
 * stepping_is_deterministic_serial_and_parallel.
 */
COOPA_TEST(self_collision_separates_non_neighbour_particles) {
    auto run = [](bool self_collision) {
        PhysicsWorld world;

        cloth::ClothParams params;
        params.self_collision = self_collision;
        params.gravity_scale = 0.0f;
        params.air_drag = 0.0f;
        params.air_lift = 0.0f;
        cloth::Cloth c = make_test_sheet(9, glm::vec3(0.0f, 0.0f, 5.0f), params);
        const float d_min = c.params.self_distance;
        ASSERT_TRUE(d_min > 0.0f); // derived from the grid spacing at build time

        c.stretch.clear();
        c.bend.clear();
        c.stretch_batches.clear();
        c.bend_batches.clear();

        // Collapse every particle into a ~1 cm blob, spread deterministically so no two start
        // exactly coincident (a zero separation vector has no defined push direction).
        for (std::size_t i = 0; i < c.particles.size(); ++i) {
            const float f = static_cast<float>(i) * 0.013f;
            c.particles[i].position = glm::vec3(std::sin(f) * 0.01f, std::cos(f) * 0.01f,
                                                5.0f + std::sin(f * 2.0f) * 0.01f);
            c.particles[i].prev_position = c.particles[i].position;
            c.particles[i].velocity = glm::vec3(0.0f);
        }

        const uint32_t cols = c.columns;
        cloth::ClothId id = world.add_cloth(std::move(c));
        for (int i = 0; i < 60; ++i) world.step(util::k_default_fixed_dt);

        const cloth::Cloth* sim = world.get_cloth(id);
        std::size_t violations = 0;
        for (std::size_t i = 0; i < sim->particles.size(); ++i) {
            for (std::size_t j = i + 1; j < sim->particles.size(); ++j) {
                const int32_t xi = static_cast<int32_t>(i % cols), yi = static_cast<int32_t>(i / cols);
                const int32_t xj = static_cast<int32_t>(j % cols), yj = static_cast<int32_t>(j / cols);
                if (std::abs(xi - xj) <= 1 && std::abs(yi - yj) <= 1) continue; // 1-ring is exempt
                if (glm::length(sim->particles[i].position - sim->particles[j].position) < d_min * 0.8f) {
                    ++violations;
                }
            }
        }
        return violations;
    };

    ASSERT_TRUE(run(/*self_collision=*/true) == 0);
    ASSERT_TRUE(run(/*self_collision=*/false) > 0); // the control: without it, they stay collapsed
}

/** @brief A settled cloth sleeps, and a moving anchor body wakes it again -- the cloth analogue of
 *         the kinematic wake rule dynamics/solver.h applies to rigid bodies. */
COOPA_TEST(sleeps_when_settled_and_wakes_on_anchor_motion) {
    PhysicsWorld world;

    dynamics::Body post;
    post.type = dynamics::BodyType::Kinematic;
    post.position = glm::vec3(0.0f, 0.0f, 5.0f);
    post.inv_mass = 0.0f;
    post.use_gravity = false;
    dynamics::BodyId post_id = world.add_body(post, collision::Shape{}); // no collider needed

    cloth::ClothParams params;
    params.damping = 4.0f;          // settle fast, so the test stays short
    params.sleep_threshold = 0.25f;
    params.sleep_time = 0.25f;
    cloth::Cloth c = make_test_sheet(9, glm::vec3(0.0f, 0.0f, 5.0f), params);
    cloth::pin_to_body(c, post_id, *world.get_body(post_id), glm::vec3(0.0f, 0.0f, 5.0f), 3.0f);
    cloth::ClothId id = world.add_cloth(std::move(c));

    for (int i = 0; i < 600; ++i) world.step(util::k_default_fixed_dt);
    ASSERT_TRUE(!world.get_cloth(id)->awake);

    // A sleeping cloth must be perfectly frozen, not merely slow.
    const glm::vec3 before = world.get_cloth(id)->particles.back().position;
    for (int i = 0; i < 60; ++i) world.step(util::k_default_fixed_dt);
    ASSERT_VEC_NEAR(world.get_cloth(id)->particles.back().position, before, 1e-6f);

    world.get_body(post_id)->position.x += 0.5f;
    world.step(util::k_default_fixed_dt);
    ASSERT_TRUE(world.get_cloth(id)->awake);
}

/** @brief Cloth particle state is part of world_state_hash(), and the whole step is invariant to
 *         whether a JobEngine is installed -- the same oracle
 *         stepping_is_deterministic_across_runs_and_serial_vs_parallel uses for rigid bodies, extended
 *         to cover the cloth pass. Also asserts the hash actually MOVES when cloth exists, so a
 *         hash that silently ignored particles could not pass. */
COOPA_TEST(stepping_is_deterministic_serial_and_parallel) {
    auto build_and_run = [](coopa::job::JobEngine* jobs) {
        PhysicsWorld world;
        world.set_parallel_threshold(1);
        world.set_job_engine(jobs);

        dynamics::Body ball;
        ball.type = dynamics::BodyType::Static;
        ball.position = glm::vec3(0.0f, 0.0f, 3.0f);
        ball.inv_mass = 0.0f;
        world.add_body(ball, collision::Shape::make_sphere(1.0f));

        cloth::ClothParams params;
        params.self_collision = true;
        params.wind = glm::vec3(0.4f, 0.0f, 0.0f);
        params.wind_turbulence = 0.6f;
        cloth::Cloth c = make_test_sheet(13, glm::vec3(0.0f, 0.0f, 4.05f), params);
        cloth::pin_static(c, glm::vec3(0.0f, 0.0f, 4.05f), 0.4f);
        cloth::build_tethers(c);
        world.add_cloth(std::move(c));

        for (int i = 0; i < 180; ++i) world.step(util::k_default_fixed_dt);
        return world.world_state_hash();
    };

    const uint64_t serial_hash = build_and_run(nullptr);
    coopa::job::JobEngine jobs(4);
    const uint64_t parallel_hash = build_and_run(&jobs);
    ASSERT_TRUE(serial_hash == parallel_hash);

    // Rebuilding the same scene twice must reproduce the hash exactly (turbulence is derived from
    // the cloth's own accumulated time, never rand()).
    ASSERT_TRUE(build_and_run(nullptr) == serial_hash);

    PhysicsWorld empty;
    ASSERT_TRUE(empty.world_state_hash() != serial_hash);
}

/** @brief ClothId generational handles reject a stale handle after the slot is recycled, matching
 *         stale_body_handle_is_rejected_after_slot_reuse. */
COOPA_TEST(stale_cloth_handle_is_rejected_after_slot_reuse) {
    PhysicsWorld world;
    cloth::ClothId a = world.add_cloth(make_test_sheet(5, glm::vec3(0.0f)));
    ASSERT_TRUE(world.is_valid(a));
    ASSERT_TRUE(world.cloth_count() == 1);

    world.remove_cloth(a);
    ASSERT_TRUE(!world.is_valid(a));
    ASSERT_TRUE(world.get_cloth(a) == nullptr);
    ASSERT_TRUE(world.cloth_count() == 0);

    cloth::ClothId b = world.add_cloth(make_test_sheet(5, glm::vec3(0.0f)));
    ASSERT_TRUE(b.index == a.index);   // slot recycled
    ASSERT_TRUE(b.generation != a.generation);
    ASSERT_TRUE(world.is_valid(b) && !world.is_valid(a));

    // An empty cloth is refused rather than occupying a slot.
    ASSERT_TRUE(!world.add_cloth(cloth::Cloth{}).is_valid());
}

/** @brief ClothComponent binds through PhysicsSystem, resolves an anchor named by object, follows
 *         that object's kinematic Rigidbody, and unbinds when the component goes away. */
COOPA_TEST(cloth_component_binds_follows_its_anchor_and_unbinds) {
    using namespace coopa::scene;

    Scene scene("ClothBindTest");

    auto ball = std::make_unique<SceneObject>("ball");
    ball->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 3.0f));
    ball->add_component<components::SphereCollider>()->set_radius(1.0f);
    auto* rb = ball->add_component<components::RigidbodyComponent>();
    rb->is_kinematic = true;
    rb->use_gravity = false;
    SceneObject* ball_ptr = ball.get();
    scene.add_root_object(std::move(ball));

    auto sheet = std::make_unique<SceneObject>("cloth");
    sheet->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 4.05f));
    auto* cc = sheet->add_component<components::ClothComponent>();
    cc->columns = 15;
    cc->rows = 15;
    cc->width = 4.0f;
    cc->height = 4.0f;
    components::ClothAnchorSpec spec;
    spec.object = "ball";
    spec.point = glm::vec3(0.0f, 0.0f, 1.0f); // the ball's own local frame: its crown
    spec.radius = 0.4f;
    cc->anchors.push_back(spec);
    scene.add_root_object(std::move(sheet));

    scene.start();
    system::install_physics_system(scene);

    const float h = util::k_default_fixed_dt;
    for (int i = 0; i < 180; ++i) {
        scene.update(h);
        scene.late_update(h);
    }

    const cloth::Cloth* sim = cc->cloth();
    ASSERT_TRUE(sim != nullptr);
    ASSERT_TRUE(sim->particles.size() == 15u * 15u);
    ASSERT_TRUE(!sim->anchors.empty());
    for (const cloth::ClothParticle& p : sim->particles) {
        ASSERT_TRUE(glm::length(p.position - glm::vec3(0.0f, 0.0f, 3.0f)) >= 1.0f);
    }

    for (int i = 0; i < 120; ++i) {
        coopa::util::Transform& t = ball_ptr->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(2.0f * h, 0.0f, 0.0f));
        scene.update(h);
        scene.late_update(h);
    }

    sim = cc->cloth();
    const float ball_x = ball_ptr->get_transform()->transform().position().x;
    ASSERT_TRUE(ball_x > 3.0f);
    float mean_x = 0.0f;
    for (const cloth::ClothParticle& p : sim->particles) mean_x += p.position.x;
    mean_x /= static_cast<float>(sim->particles.size());
    ASSERT_TRUE(std::abs(mean_x - ball_x) < 0.6f);

    // Removing the component releases the cloth on the next refresh.
    cc->owner->remove_component(cc);
    dynamic_cast<system::PhysicsSystem*>(scene.find_system("Physics"))->refresh();
    scene.update(h);
    scene.late_update(h);
    ASSERT_TRUE(dynamic_cast<system::PhysicsSystem*>(scene.find_system("Physics"))->world().cloth_count() == 0);
}
