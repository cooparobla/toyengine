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
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/job/engine.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
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
    // (res-1)*res horizontal + the same vertical, and the two diagonals per cell in their OWN
    // list: a weave's threads and its shear are different material properties, held at different
    // compliances (ClothParams::shear_compliance).
    const std::size_t structural = 2u * (res - 1) * res;
    const std::size_t shear = 2u * (res - 1) * (res - 1);
    ASSERT_TRUE(c.stretch.size() == structural);
    ASSERT_TRUE(c.shear.size() == shear);
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
    ASSERT_TRUE(batches_disjoint(c.shear, c.shear_batches, c.particles.size()));
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

// ---------------------------------------------------------------------------------------------
// Skin cloth: the painted-channel stages (3b skin pins, anim drive, 5b max-distance/backstop).
// Builder topology for the mesh path is cloth_mesh_builder_test.cpp.
// ---------------------------------------------------------------------------------------------

#include <physxcoopa/cloth/cloth_mesh_builder.h>

#include <cstring>
#include <limits>

namespace {

/** @brief A res x res welded patch (XY plane, z == `z`) through the MESH builder, with the given
 *         channels. Mirrors cloth_mesh_builder_test.cpp's make_patch. */
cloth::Cloth make_skin_patch(uint32_t res, float size, float z, cloth::ClothSkinChannels channels,
                             cloth::ClothParams params = {}) {
    cloth::MeshClothDesc desc;
    const float spacing = size / static_cast<float>(res - 1);
    for (uint32_t y = 0; y < res; ++y) {
        for (uint32_t x = 0; x < res; ++x) {
            desc.positions.push_back(glm::vec3(static_cast<float>(x) * spacing,
                                               static_cast<float>(y) * spacing, z));
        }
    }
    for (uint32_t y = 0; y + 1 < res; ++y) {
        for (uint32_t x = 0; x + 1 < res; ++x) {
            const uint32_t i00 = y * res + x, i10 = y * res + x + 1;
            const uint32_t i01 = (y + 1) * res + x, i11 = (y + 1) * res + x + 1;
            desc.triangles.insert(desc.triangles.end(), {i00, i10, i11});
            desc.triangles.insert(desc.triangles.end(), {i00, i11, i01});
        }
    }
    desc.channels = std::move(channels);
    desc.params = params;
    return cloth::make_mesh_cloth(desc);
}

constexpr float k_skin_h = 1.0f / 60.0f;
const glm::vec3 k_skin_gravity(0.0f, 0.0f, -9.81f);

} // namespace

/**
 * @brief Stage 3b and 5b: painted-zero particles track their targets exactly, and every free
 *        particle stays inside its painted max-distance sphere -- under gravity AND while the
 *        targets travel, which is the case that matters for clothing on a moving character.
 */
COOPA_TEST(skin_pins_track_targets_and_max_distance_holds) {
    const uint32_t res = 7;
    const std::size_t n = res * res;
    cloth::ClothSkinChannels ch;
    ch.max_distance.assign(n, 0.3f);
    for (uint32_t x = 0; x < res; ++x) ch.max_distance[(res - 1) * res + x] = 0.0f;

    cloth::ClothParams params;
    params.anim_drive_compliance = 0.0f; // no drive: isolate the clamp
    cloth::Cloth c = make_skin_patch(res, 2.0f, 2.0f, ch, params);
    const std::vector<glm::vec3> rest = [&] {
        std::vector<glm::vec3> r;
        for (const cloth::ClothParticle& p : c.particles) r.push_back(p.position);
        return r;
    }();

    cloth::ClothSolverScratch scratch;
    const std::vector<cloth::ClothCollider> no_colliders;
    for (int frame = 0; frame < 120; ++frame) {
        // Targets travel +X at 1.5 m/s after half a second at rest.
        const float t = static_cast<float>(frame) * k_skin_h;
        const float travel = (t > 0.5f) ? (t - 0.5f) * 1.5f : 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            c.skin_targets.position[i] = rest[i] + glm::vec3(travel, 0.0f, 0.0f);
        }
        cloth::solve_cloth(c, no_colliders, k_skin_gravity, 4, 1, k_skin_h, scratch);
    }

    for (std::size_t i = 0; i < n; ++i) {
        const float d = glm::length(c.particles[i].position - c.skin_targets.position[i]);
        if (c.skin.max_distance[i] <= 1e-6f) {
            ASSERT_TRUE(d < 1e-4f); // skin-driven: written outright from the target
        } else {
            ASSERT_TRUE(d <= 0.3f + 1e-3f); // free: leashed to the painted radius
        }
    }
}

/** @brief Anim drive pulls the sheet toward its targets against gravity: full drive hangs far
 *         closer to the skinned pose than zero drive, and stiffer-than-painted never exceeds the
 *         max-distance leash. */
COOPA_TEST(skin_anim_drive_pulls_toward_targets) {
    const uint32_t res = 7;
    const std::size_t n = res * res;

    auto sag = [&](float drive_gain) {
        cloth::ClothSkinChannels ch;
        ch.max_distance.assign(n, 1.0f); // loose leash so the drive, not the clamp, decides
        for (uint32_t x = 0; x < res; ++x) ch.max_distance[(res - 1) * res + x] = 0.0f;
        if (drive_gain > 0.0f) ch.anim_drive.assign(n, drive_gain);

        cloth::ClothParams params;
        params.anim_drive_compliance = 1e-4f;
        cloth::Cloth c = make_skin_patch(res, 2.0f, 2.0f, ch, params);

        cloth::ClothSolverScratch scratch;
        const std::vector<cloth::ClothCollider> no_colliders;
        for (int frame = 0; frame < 180; ++frame) {
            cloth::solve_cloth(c, no_colliders, k_skin_gravity, 4, 1, k_skin_h, scratch);
        }
        float worst = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            worst = std::max(worst,
                             glm::length(c.particles[i].position - c.skin_targets.position[i]));
        }
        return worst;
    };

    const float driven = sag(1.0f);
    const float undriven = sag(0.0f);
    ASSERT_TRUE(driven < 0.08f);           // full drive at 1e-4 m/N holds the pose closely
    ASSERT_TRUE(undriven > driven * 3.0f); // and that is the drive's doing, not the leash's
}

/**
 * @brief Stage 5b backstop: with a backstop sphere tucked behind the skinned surface, gravity
 *        cannot pull the sheet below its targets' plane; without one it sags to the leash.
 */
COOPA_TEST(skin_backstop_keeps_cloth_off_the_body) {
    const uint32_t res = 7;
    const std::size_t n = res * res;

    auto lowest = [&](bool backstop) {
        cloth::ClothSkinChannels ch;
        ch.max_distance.assign(n, 0.5f);
        for (uint32_t x = 0; x < res; ++x) ch.max_distance[(res - 1) * res + x] = 0.0f;
        if (backstop) {
            ch.backstop_distance.assign(n, 0.0f);
            ch.backstop_radius.assign(n, 0.5f);
        }
        cloth::ClothParams params;
        params.anim_drive_compliance = 0.0f;
        cloth::Cloth c = make_skin_patch(res, 2.0f, 2.0f, ch, params);
        if (backstop) {
            // Skinned surface faces +Z, so the backstop sphere sits beneath each target.
            c.skin_targets.normal.assign(n, glm::vec3(0.0f, 0.0f, 1.0f));
        }

        cloth::ClothSolverScratch scratch;
        const std::vector<cloth::ClothCollider> no_colliders;
        for (int frame = 0; frame < 180; ++frame) {
            cloth::solve_cloth(c, no_colliders, k_skin_gravity, 4, 1, k_skin_h, scratch);
        }
        float low = std::numeric_limits<float>::max();
        for (const cloth::ClothParticle& p : c.particles) low = std::min(low, p.position.z);
        return low;
    };

    const float with_backstop = lowest(true);
    const float without = lowest(false);
    ASSERT_TRUE(with_backstop > 2.0f - 5e-3f); // held at the targets' z == 2 plane
    ASSERT_TRUE(without < 2.0f - 0.2f);        // control: gravity really was pulling
}

/** @brief Serial and job-parallel solves of a skin cloth -- channels, drive, backstop, moving
 *         targets -- produce bit-identical particle state, extending the determinism contract to
 *         the new stages. */
COOPA_TEST(skin_solve_is_deterministic_serial_and_parallel) {
    const uint32_t res = 13;
    const std::size_t n = res * res;

    auto run = [&](coopa::job::JobEngine* jobs) {
        cloth::ClothSkinChannels ch;
        ch.max_distance.assign(n, 0.4f);
        for (uint32_t x = 0; x < res; ++x) ch.max_distance[(res - 1) * res + x] = 0.0f;
        ch.anim_drive.assign(n, 0.5f);
        ch.backstop_distance.assign(n, 0.02f);
        ch.backstop_radius.assign(n, 0.3f);

        cloth::Cloth c = make_skin_patch(res, 2.0f, 2.0f, ch);
        c.skin_targets.normal.assign(n, glm::vec3(0.0f, 0.0f, 1.0f));
        const std::vector<glm::vec3> rest = c.skin_targets.position;

        cloth::ClothSolverScratch scratch;
        const std::vector<cloth::ClothCollider> no_colliders;
        for (int frame = 0; frame < 90; ++frame) {
            const float travel = static_cast<float>(frame) * k_skin_h * 1.2f;
            for (std::size_t i = 0; i < n; ++i) {
                c.skin_targets.position[i] = rest[i] + glm::vec3(travel, 0.3f * travel, 0.0f);
            }
            if (jobs) {
                auto dispatch = [jobs](std::size_t count, auto&& fn) {
                    if (count == 0) return;
                    jobs->parallel_for_blocking(
                        count, 0, [&fn](std::size_t b, std::size_t e) { fn(b, e); }, 0,
                        coopa::job::Priority::Normal, coopa::job::Priority::Normal);
                };
                cloth::solve_cloth(c, no_colliders, k_skin_gravity, 4, 1, k_skin_h, scratch,
                                   dispatch);
            } else {
                cloth::solve_cloth(c, no_colliders, k_skin_gravity, 4, 1, k_skin_h, scratch);
            }
        }
        return c.particles;
    };

    const std::vector<cloth::ClothParticle> serial = run(nullptr);
    coopa::job::JobEngine jobs(4);
    const std::vector<cloth::ClothParticle> parallel = run(&jobs);
    ASSERT_TRUE(serial.size() == parallel.size());
    ASSERT_TRUE(std::memcmp(serial.data(), parallel.data(),
                            serial.size() * sizeof(cloth::ClothParticle)) == 0);
}

/**
 * @brief An authored compliance describes a FABRIC, not a mass: the same bend compliance hangs a
 *        1 kg sheet and a 10 kg sheet of the same size into the same shape.
 *
 * This is the property `project_distance()` folds the constraint's inverse mass in for, and the
 * one the whole documented compliance scale rests on. Without it a compliance in m/N is measured
 * against `1/m`, which for a 1.6 g cloth particle is ~1250 -- so every value small enough to read
 * as "stiff" comes out rigid, a sheet cannot crease, and an inextensible sheet that cannot crease
 * cannot drape over anything convex at all.
 *
 * Shape, not sag: a heavier sheet under the same gravity reaches the same hanging shape (gravity
 * scales with mass too), so the two pinned sheets must agree closely. The guard against the test
 * passing vacuously is the second half -- a compliance that is NOT mass-normalised would make the
 * 10x sheet visibly stiffer, which is what the loose bound would catch.
 */
COOPA_TEST(bend_compliance_describes_the_fabric_not_the_mass) {
    auto hang = [](float total_mass) {
        cloth::ClothParams params;
        params.bend_compliance = 5e-3f;
        params.air_drag = 0.0f;
        params.air_lift = 0.0f;
        cloth::GridClothDesc desc;
        desc.columns = desc.rows = 9;
        desc.width = desc.height = 1.0f;
        desc.center = glm::vec3(0.0f, 0.0f, 2.0f);
        desc.total_mass = total_mass;
        desc.params = params;
        cloth::Cloth c = cloth::make_grid_cloth(desc);
        // Pin one edge; the rest folds down over it.
        for (uint32_t x = 0; x < desc.columns; ++x) {
            c.particles[x].inv_mass = 0.0f;
            cloth::ClothAnchor a;
            a.particle = x;
            a.world_position = c.particles[x].position;
            a.prev_world_position = a.world_position;
            c.anchors.push_back(a);
        }
        cloth::ClothSolverScratch scratch;
        const std::vector<cloth::ClothCollider> none;
        for (int frame = 0; frame < 240; ++frame) {
            cloth::solve_cloth(c, none, glm::vec3(0.0f, 0.0f, -9.81f), 4, 1, 1.0f / 60.0f, scratch);
        }
        return c;
    };

    const cloth::Cloth light = hang(1.0f);
    const cloth::Cloth heavy = hang(10.0f);
    float worst = 0.0f;
    for (std::size_t i = 0; i < light.particles.size(); ++i) {
        worst = std::max(worst, glm::length(light.particles[i].position - heavy.particles[i].position));
    }
    // 1 cm over a 1 m sheet. Against a raw (non-normalised) compliance the 10x sheet's constraints
    // are 10x softer relative to it and the two settle centimetres apart.
    ASSERT_TRUE(worst < 0.01f);
}

/**
 * @brief A weave gives way by SHEARING, not by its threads changing length.
 *
 * The distinction is the whole reason shear has a compliance of its own. A flat sheet cannot be
 * laid on a sphere isometrically, so conforming to one means losing circumference somewhere, and
 * where it is lost decides what the result looks like. Lost to shear -- warp and weft scissoring
 * toward each other -- every thread keeps its length and the sheet folds onto the ball. Lost to
 * the threads themselves, the sheet SHRINKS onto it: measured at 12% mean thread compression on
 * the ball demo, with the hem riding up above the equator and the ball showing underneath.
 *
 * Measured on the constraint lengths directly, so it cannot be confused with a collision or a
 * leash doing the work.
 */
COOPA_TEST(a_weave_shears_rather_than_changing_thread_length) {
    cloth::ClothParams params;
    params.stretch_compliance = 0.0f;
    params.bend_compliance = 5e-3f;
    cloth::Cloth c = make_test_sheet(9, glm::vec3(0.0f, 0.0f, 2.0f), params);
    // Pin the four corners and let the middle fall: the sheet goes into tension radially and
    // has to give way in the hoop direction around the sag.
    for (const uint32_t i : {0u, 8u, 72u, 80u}) {
        c.particles[i].inv_mass = 0.0f;
        cloth::ClothAnchor a;
        a.particle = i;
        a.world_position = c.particles[i].position;
        a.prev_world_position = a.world_position;
        c.anchors.push_back(a);
    }
    cloth::ClothSolverScratch scratch;
    const std::vector<cloth::ClothCollider> none;
    for (int frame = 0; frame < 240; ++frame) {
        cloth::solve_cloth(c, none, glm::vec3(0.0f, 0.0f, -9.81f), 4, 1, 1.0f / 60.0f, scratch);
    }

    auto strain_range = [&](const std::vector<cloth::ClothConstraint>& list) {
        glm::vec2 range(0.0f);
        for (const cloth::ClothConstraint& k : list) {
            const float e = glm::length(c.particles[k.a].position - c.particles[k.b].position) /
                                k.rest_length - 1.0f;
            range.x = std::max(range.x, e);
            range.y = std::min(range.y, e);
        }
        return range;
    };
    const glm::vec2 thread = strain_range(c.stretch);
    const glm::vec2 diagonal = strain_range(c.shear);
    std::cout << "    thread " << thread.x << " / " << thread.y << "   diagonal " << diagonal.x
              << " / " << diagonal.y << "\n";

    // The threads hold. The few percent of stretch that remains is not compliance -- stretch
    // compliance is 0 here -- but small-substep XPBD's convergence at one iteration per substep.
    ASSERT_TRUE(thread.x < 0.05f);
    // And they do NOT shorten, which is the failure this exists for.
    ASSERT_TRUE(thread.y > -0.01f);
    // The give is in the shear, by an order of magnitude.
    ASSERT_TRUE(std::abs(diagonal.y) > std::abs(thread.y) * 10.0f);
}

/**
 * @brief A collider that starts moving nudges the cloth lying on it; it does not launch it.
 *
 * The failure this pins down is specific to position-based dynamics and invisible to every
 * interpenetration check there is. Velocity at stage 8 is `(p - prev) / hs`, so a correction is
 * read back as motion -- which is only meaningful if the correction itself was. Hand the solver
 * one displacement for the whole frame and hold the shape at the FAR END of it for every substep,
 * and a sphere travelling 4 m/s arrives 6.7 cm inside the sheet on substep one; projecting that
 * out inside a 1/240 s substep reports 16 m/s of cloth, and the sheet is thrown off the body it
 * was resting on. Measured on the ball demo before this: 13.7 m/s on the first frame the ball
 * moved, with the sheet's coverage of the ball down from 91% to 10% within two seconds.
 *
 * Both halves are asserted, because only the comparison shows the test has teeth: the swept
 * collider must stay near its own speed, and the teleported one must not.
 */
COOPA_TEST(a_moving_collider_arrives_instead_of_teleporting) {
    collision::Shape sphere = collision::Shape::make_sphere(1.0f);

    // A small sheet dropped onto the crown of a sphere at the origin, left to settle.
    auto settled = [&]() {
        cloth::ClothParams params;
        params.bend_compliance = 5e-3f;
        params.air_drag = 0.0f;
        params.air_lift = 0.0f;
        cloth::GridClothDesc desc;
        desc.columns = desc.rows = 9;
        desc.width = desc.height = 1.0f;
        desc.center = glm::vec3(0.0f, 0.0f, 1.15f);
        desc.params = params;
        cloth::Cloth c = cloth::make_grid_cloth(desc);

        cloth::ClothCollider still;
        still.shape = &sphere;
        still.bounds = collision::world_bounds(sphere, glm::vec3(0.0f), still.orientation);
        const std::vector<cloth::ClothCollider> colliders{still};
        cloth::ClothSolverScratch scratch;
        for (int frame = 0; frame < 180; ++frame) {
            cloth::solve_cloth(c, colliders, glm::vec3(0.0f, 0.0f, -9.81f), 4, 1, 1.0f / 60.0f,
                               scratch);
        }
        return c;
    };

    // One frame of a 4 m/s sideways move, described two ways.
    const float dt = 1.0f / 60.0f;
    const glm::vec3 step(4.0f * dt, 0.0f, 0.0f);
    auto drive = [&](cloth::Cloth c, bool swept) {
        cloth::ClothSolverScratch scratch;
        glm::vec3 centre(0.0f);
        float peak = 0.0f;
        for (int frame = 0; frame < 10; ++frame) {
            centre += step;
            cloth::ClothCollider collider;
            collider.shape = &sphere;
            // Swept: the pose at the top of the step plus the displacement across it. Teleported:
            // the end pose alone, which is what PhysicsWorld used to hand over.
            collider.position = swept ? centre - step : centre;
            collider.sweep = swept ? step : glm::vec3(0.0f);
            collider.linear_velocity = step / dt;
            collider.bounds = collision::world_bounds(sphere, centre - step, collider.orientation);
            const geometry::AABB end = collision::world_bounds(sphere, centre, collider.orientation);
            collider.bounds.min = glm::min(collider.bounds.min, end.min);
            collider.bounds.max = glm::max(collider.bounds.max, end.max);
            const std::vector<cloth::ClothCollider> colliders{collider};
            cloth::solve_cloth(c, colliders, glm::vec3(0.0f, 0.0f, -9.81f), 4, 1, dt, scratch);
            peak = std::max(peak, cloth::max_particle_speed(c));
        }
        return peak;
    };

    const cloth::Cloth rest = settled();
    ASSERT_TRUE(cloth::max_particle_speed(rest) < 0.05f);   // it really did settle first

    const float arriving = drive(rest, true);
    const float teleporting = drive(rest, false);
    std::cout << "    swept " << arriving << " m/s   teleported " << teleporting
              << " m/s   (sphere doing 4)\n";

    // Twice the sphere's own speed: fabric a sphere slides under genuinely moves, and the loose
    // edges flick, but it stays the same order as the thing pushing it.
    ASSERT_TRUE(arriving < 8.0f);
    // And the comparison is the point -- without the sweep the same frame is several times worse.
    ASSERT_TRUE(teleporting > arriving * 2.0f);
}

/**
 * @brief A long frame buys more substeps, and never a longer one.
 *
 * `cloth::frame_substeps()` is the whole claim, and it is worth a test of its own because both
 * failures it sits between are silent. Nothing in the solver notices a 15.6 ms substep -- it simply
 * stops holding the margins the collision stage is sized against, and the sheet slides off the body
 * it was draped over (measured on the ball demo at a 0.25 s frame: upper-cap coverage 81% -> 43%).
 * And nothing notices a TRUNCATED frame either, which was the first attempt at a fix and is worse:
 * the anchors still carry a whole frame of motion, so completing a 1 m anchor move inside a 0.067 s
 * solve read back as 47.5 m/s and tore the sheet off entirely. Both numbers come from
 * `cloth_probe_test.cpp` swept over `CLOTH_PROBE_DT`.
 */
COOPA_TEST(a_long_frame_buys_more_substeps_not_longer_ones) {
    const float internal_h = (1.0f / 60.0f) / 4.0f;   // the shipped default: 1/240 s

    // The common case, unchanged: one substep per 1/240 s of frame, exactly as the plain clamp gave.
    ASSERT_TRUE(cloth::frame_substeps(1.0f / 60.0f, internal_h) == 4u);
    ASSERT_TRUE(cloth::frame_substeps(1.0f / 30.0f, internal_h) == 8u);
    ASSERT_TRUE(cloth::frame_substeps(1.0f / 15.0f, internal_h) == 16u);
    ASSERT_TRUE(cloth::frame_substeps(1.0f / 144.0f, internal_h) == 2u);
    // Never zero, however short the frame -- a 1000 fps host still steps the cloth.
    ASSERT_TRUE(cloth::frame_substeps(0.001f, internal_h) == 1u);

    // The invariant that matters: across every frame length step() can hand over, the substep stays
    // at or under the authored one. This is what failed before, from 1/15 s upward.
    for (const float dt : {1.0f / 300.0f, 1.0f / 60.0f, 1.0f / 15.0f, 0.1f, 0.2f,
                           util::k_max_frame_time}) {
        const float hs = dt / static_cast<float>(cloth::frame_substeps(dt, internal_h));
        ASSERT_TRUE(hs <= internal_h + 1e-6f);
    }

    // And what that buys downstream: a collider doing 4 m/s cannot advance further than the
    // standoff in one substep, at any frame length. The stretched substep broke exactly this.
    cloth::ClothParams defaults;
    const float worst_hs = util::k_max_frame_time /
                           static_cast<float>(cloth::frame_substeps(util::k_max_frame_time, internal_h));
    std::cout << "    worst-case substep " << worst_hs * 1000.0f << " ms over "
              << cloth::frame_substeps(util::k_max_frame_time, internal_h)
              << " substeps; a 4 m/s collider advances " << 4.0f * worst_hs * 100.0f
              << " cm against a " << defaults.thickness * 100.0f << " cm standoff\n";
    ASSERT_TRUE(4.0f * worst_hs < defaults.thickness);

    // The absolute ceiling still binds, so a pathological cloth_substeps cannot ask for unbounded
    // work -- it just reverts to a stretched substep, which is the lesser of the two evils there.
    ASSERT_TRUE(cloth::frame_substeps(util::k_max_frame_time, 1e-5f) == util::k_max_cloth_substeps);
}

COOPA_TEST(every_cloth_tunable_survives_the_yaml_round_trip) {
    using namespace coopa::scene;
    coopa::asset::AssetManager assets;
    register_physics_components(assets);
    // SceneLoader's parser table is process-global: unregister even when a check below ends the
    // test early, so no later test in this process inherits it.
    struct ParserReset {
        ~ParserReset() { SceneLoader::clear_component_parsers(); }
    } reset_parsers;

    std::istringstream iss(
        "scene:\n"
        "  scene_name: ClothKeyTest\n"
        "  root_objects:\n"
        "    - name: sheet\n"
        "      components:\n"
        "        - { type: Transform, position: { x: 0, y: 0, z: 2 } }\n"
        "        - type: Cloth\n"
        "          resolution: { x: 7, y: 5 }\n"
        "          size: { x: 3.0, y: 2.0 }\n"
        "          mass: 2.5\n"
        "          shear: false\n"
        "          stretch_compliance: 1.0e-4\n"
        "          shear_compliance: 2.0e-3\n"
        "          compression_compliance: 3.0e-3\n"
        "          bend_compliance: 4.0e-3\n"
        "          damping: 0.11\n"
        "          thickness: 0.07\n"
        "          friction: 0.33\n"
        "          gravity_scale: 0.5\n"
        "          max_velocity: 42.0\n"
        "          external_acceleration: { x: 1.0, y: 2.0, z: 3.0 }\n"
        "          wind: { x: 0.5, y: 0.25, z: 0.125 }\n"
        "          wind_turbulence: 0.6\n"
        "          weather_wind: 0.4\n"
        "          air_drag: 0.08\n"
        "          air_lift: 0.09\n"
        "          collide_surface: false\n"
        "          self_collision: true\n"
        "          self_distance: 0.04\n"
        "          tether_scale: 1.05\n"
        "          substeps: 6\n"
        "          iterations: 3\n"
        "          sleep_threshold: 0.02\n"
        "          sleep_time: 1.5\n");
    fkyaml::node root = fkyaml::node::deserialize(iss);
    Scene scene = SceneLoader::load_from_node(root, "");

    auto* cc = scene.find_object("sheet")->get_component<components::ClothComponent>();
    ASSERT_TRUE(cc != nullptr);
    ASSERT_TRUE(cc->columns == 7 && cc->rows == 5);
    ASSERT_NEAR(cc->width, 3.0f, 1e-6f);
    ASSERT_NEAR(cc->height, 2.0f, 1e-6f);
    ASSERT_NEAR(cc->mass, 2.5f, 1e-6f);
    ASSERT_TRUE(cc->shear == false);
    ASSERT_NEAR(cc->weather_wind, 0.4f, 1e-6f);

    const cloth::ClothParams& p = cc->params;
    ASSERT_NEAR(p.stretch_compliance, 1.0e-4f, 1e-9f);
    ASSERT_NEAR(p.shear_compliance, 2.0e-3f, 1e-9f);
    ASSERT_NEAR(p.compression_compliance, 3.0e-3f, 1e-9f);
    ASSERT_NEAR(p.bend_compliance, 4.0e-3f, 1e-9f);
    ASSERT_NEAR(p.damping, 0.11f, 1e-6f);
    ASSERT_NEAR(p.thickness, 0.07f, 1e-6f);
    ASSERT_NEAR(p.friction, 0.33f, 1e-6f);
    ASSERT_NEAR(p.gravity_scale, 0.5f, 1e-6f);
    ASSERT_NEAR(p.max_velocity, 42.0f, 1e-6f);
    ASSERT_NEAR(p.external_acceleration.x, 1.0f, 1e-6f);
    ASSERT_NEAR(p.external_acceleration.z, 3.0f, 1e-6f);
    ASSERT_NEAR(p.wind.x, 0.5f, 1e-6f);
    ASSERT_NEAR(p.wind.z, 0.125f, 1e-6f);
    ASSERT_NEAR(p.wind_turbulence, 0.6f, 1e-6f);
    ASSERT_NEAR(p.air_drag, 0.08f, 1e-6f);
    ASSERT_NEAR(p.air_lift, 0.09f, 1e-6f);
    ASSERT_TRUE(p.collide_surface == false);   // default true, so a parsed false proves the key
    ASSERT_TRUE(p.self_collision == true);
    ASSERT_NEAR(p.self_distance, 0.04f, 1e-6f);
    ASSERT_NEAR(p.tether_scale, 1.05f, 1e-6f);
    ASSERT_TRUE(p.substeps == 6u);
    ASSERT_TRUE(p.iterations == 3u);
    ASSERT_NEAR(p.sleep_threshold, 0.02f, 1e-6f);
    ASSERT_NEAR(p.sleep_time, 1.5f, 1e-6f);
}
