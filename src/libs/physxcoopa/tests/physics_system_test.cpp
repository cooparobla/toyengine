/**
 * @file physics_system_test.cpp
 * @brief PhysicsSystem: binding scene components (colliders, Rigidbody, compound colliders, joint
 *        components) to PhysicsWorld bodies, per-frame reconciliation of runtime changes, pivot vs
 *        center-of-mass conversion, job-parallel hierarchical write-back, and the scene query
 *        wrappers. Cloth binding is in cloth_solver_test.cpp; collision callbacks in contact_events_test.cpp.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/convex_collider.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/hinge_joint.h>
#include <physxcoopa/components/ball_joint.h>
#include <physxcoopa/components/cone_twist_joint.h>
#include <physxcoopa/dynamics/joint.h>
#include <physxcoopa/geometry/convex_hull.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/job/engine.h>
#include <memory>
#include "support/physics_fixtures.h"

COOPA_TEST_SUITE("physics_system");

using namespace coopa::physx;
using namespace physxtest;

/**
 * The binding smoke test plus the pivot/center-of-mass conversion. A ground and a box bind to
 * exactly one body each, lazily on the first execute().
 *
 * The box is a corner-origin mesh (cube.000's [0,1]^3 local vertices) compensated for via
 * `BoxCollider center: {0.5,0.5,0.5}` on a dynamic Rigidbody, and must settle flat. Every dynamics
 * formula assumes body.position IS the center of mass, so if body.position stayed pinned to the
 * Transform's raw pivot while the shape (and mass/inertia) sat 0.5 units away in each axis,
 * gravity would produce a persistent spurious torque about the wrong point and the stack would
 * tumble and never settle. PhysicsSystem folds the collider's center offset into body.position
 * as the true center of mass (see create_compound_body_()'s doc) and converts back to the pivot
 * every frame in write_transforms_back_(). Unlike contact_solver's tumbled-box and stack tests
 * (headless PhysicsWorld, no Scene/Collider::center() involved), this test goes through the actual
 * Scene/PhysicsSystem binding path with a nonzero collider center, which is where that folding
 * happens.
 */
COOPA_TEST(offset_box_collider_binds_and_settles_flat_on_its_pivot) {
    using namespace coopa::scene;

    Scene scene("PhysicsCenterOffsetTest");

    auto ground = std::make_unique<SceneObject>("ground");
    ground->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, -0.5f));
    ground->add_component<components::BoxCollider>()->set_size(glm::vec3(8.0f, 8.0f, 1.0f));
    scene.add_root_object(std::move(ground));

    // Pivot ("Transform position") is the box's corner, matching cube.000's convention; the
    // collider's center offset re-centers the 1x1x1 shape onto that pivot exactly like
    // physics_test's stack_1/2/3 objects (BoxCollider size:{1,1,1} center:{0.5,0.5,0.5}).
    auto box = std::make_unique<SceneObject>("box");
    auto* transform = box->add_component<TransformComponent>();
    transform->transform().set_position(glm::vec3(0.0f, 0.0f, 2.5f));
    glm::quat tilt = glm::angleAxis(glm::radians(25.0f), glm::normalize(glm::vec3(1.0f, 0.3f, 0.0f)));
    transform->transform().set_rotation_quat(tilt);
    auto* box_collider = box->add_component<components::BoxCollider>();
    box_collider->set_size(glm::vec3(1.0f));
    box_collider->set_center(glm::vec3(0.5f));
    box->add_component<components::RigidbodyComponent>();
    scene.add_root_object(std::move(box));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    EXPECT_EQ(sys->bound_count(), 0u); // not gathered until the first execute()

    for (int i = 0; i < 600; ++i) { // 10s -- generous settle budget, matching the headless tests
        scene.update(util::k_default_fixed_dt);
        scene.late_update(util::k_default_fixed_dt);
    }
    EXPECT_EQ(sys->bound_count(), 2u); // ground + box each bind to exactly one body

    SceneObject* box_obj = scene.find_object("box");
    ASSERT_TRUE(box_obj != nullptr);
    dynamics::BodyId id = box_obj->get_component<components::BoxCollider>()->body_id();
    const dynamics::Body* b = sys->world().get_body(id);
    ASSERT_TRUE(b != nullptr);

    EXPECT_TRUE(is_settled(*b));
    // "Flat" means one of the box's local axes ends up within ~2 degrees of world +Z.
    EXPECT_GT(best_up_alignment(b->orientation), 0.999f);

    // Ground top at z=0; resting flat, the pivot (Transform position, NOT the center of mass)
    // must land back at z~=0 -- exactly cube.000's corner-origin convention -- confirming
    // write_transforms_back_() correctly converts true-center space back to the authored pivot.
    float pivot_z = box_obj->get_transform()->transform().position().z;
    EXPECT_NEAR(pivot_z, 0.0f, 0.05f);
}

COOPA_TEST(collider_parameter_change_updates_shape_in_place) {
    using namespace coopa::scene;

    Scene scene("PhysicsRevisionTest");
    auto obj = std::make_unique<SceneObject>("sphere");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 5.0f));
    auto* sphere = obj->add_component<components::SphereCollider>();
    sphere->set_radius(0.5f);
    obj->add_component<components::RigidbodyComponent>();
    scene.add_root_object(std::move(obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    scene.update(util::k_default_fixed_dt); // first execute() binds the body

    SceneObject* sphere_obj = scene.find_object("sphere");
    auto* collider = sphere_obj->get_component<components::SphereCollider>();
    dynamics::BodyId id = collider->body_id();
    ASSERT_TRUE(id.is_valid());
    ASSERT_NEAR(sys->world().get_shape(id)->radius, 0.5f, 1e-5f);

    collider->set_radius(1.5f); // no refresh() call -- must ride the per-frame revision check
    scene.update(util::k_default_fixed_dt);

    ASSERT_NEAR(sys->world().get_shape(id)->radius, 1.5f, 1e-5f);
    ASSERT_TRUE(collider->body_id() == id); // same body, shape updated in place -- not rebuilt
}

/**
 * RigidbodyComponent's fields are re-read every frame, not only at bind time
 * (create_compound_body_()): a runtime `rb->mass = 5.0f` or `rb->is_kinematic = true` takes
 * effect, just as Collider's own parameters (collider_parameter_change_updates_shape_in_place) do via the per-frame revision
 * check. update_changed_rigidbodies_() does this; the test drives mass, use_gravity, and an
 * is_kinematic round-trip through the actual Scene/PhysicsSystem binding path, then the
 * component's sleep/wake API.
 */
COOPA_TEST(rigidbody_component_changes_reach_the_bound_body) {
    using namespace coopa::scene;

    Scene scene("PhysicsRigidbodyReconcileTest");
    auto obj = std::make_unique<SceneObject>("sphere");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 5.0f));
    obj->add_component<components::SphereCollider>()->set_radius(0.5f);
    auto* rb = obj->add_component<components::RigidbodyComponent>();
    scene.add_root_object(std::move(obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    scene.update(util::k_default_fixed_dt); // first execute() binds the body

    SceneObject* sphere_obj = scene.find_object("sphere");
    dynamics::BodyId id = sphere_obj->get_component<components::SphereCollider>()->body_id();
    ASSERT_TRUE(id.is_valid());
    const dynamics::Body* body = sys->world().get_body(id);
    ASSERT_NEAR(body->mass, 1.0f, 1e-5f);
    glm::vec3 inertia_at_mass_1 = body->inv_inertia_local;
    ASSERT_TRUE(body->use_gravity);
    ASSERT_TRUE(body->type == dynamics::BodyType::Dynamic);

    // Mass: inv_mass and inv_inertia_local must both be recomputed from the new value, not just
    // the mass field itself.
    rb->mass = 4.0f;
    rb->use_gravity = false;
    scene.update(util::k_default_fixed_dt);
    body = sys->world().get_body(id);
    ASSERT_NEAR(body->mass, 4.0f, 1e-5f);
    ASSERT_NEAR(body->inv_mass, 0.25f, 1e-5f);
    ASSERT_TRUE(body->inv_inertia_local.x < inertia_at_mass_1.x); // heavier -> smaller inverse inertia
    ASSERT_TRUE(!body->use_gravity);

    // is_kinematic: a BodyType flip, not just a scalar -- round-trip it and confirm mass/inertia
    // come back correctly on the way back to Dynamic (set_body_type()'s own responsibility).
    rb->is_kinematic = true;
    scene.update(util::k_default_fixed_dt);
    ASSERT_TRUE(sys->world().get_body(id)->type == dynamics::BodyType::Kinematic);

    rb->is_kinematic = false;
    scene.update(util::k_default_fixed_dt);
    body = sys->world().get_body(id);
    ASSERT_TRUE(body->type == dynamics::BodyType::Dynamic);
    ASSERT_NEAR(body->mass, 4.0f, 1e-5f);
    ASSERT_NEAR(body->inv_mass, 0.25f, 1e-5f);

    // sleep()/wake()/is_sleeping() and set_velocity()'s wake-on-write reach the bound Body too
    // (the headless wake rules are physics_world_test's; this is the component delegation).
    EXPECT_FALSE(rb->is_sleeping()); // bound bodies start awake
    rb->sleep();
    ASSERT_TRUE(rb->is_sleeping());
    ASSERT_VEC_NEAR(rb->velocity(), glm::vec3(0.0f), 1e-6f);
    rb->wake();
    ASSERT_FALSE(rb->is_sleeping());

    rb->sleep();
    ASSERT_TRUE(rb->is_sleeping());
    rb->set_velocity(glm::vec3(1.0f, 0.0f, 0.0f)); // must wake the body, not just set velocity
    ASSERT_FALSE(rb->is_sleeping());
    ASSERT_VEC_NEAR(rb->velocity(), glm::vec3(1.0f, 0.0f, 0.0f), 1e-6f);
}

/**
 * RigidbodyComponent::center_of_mass_override/inertia_tensor_override -- Unity's
 * Rigidbody.centerOfMass/Rigidbody.inertiaTensor. Checked directly against the bound Body right
 * after bind (no simulation needed): the override composes with the collider `center` via
 * center_offset_for_(), and body.position/inv_inertia_local reflect it exactly.
 * local_center_of_mass()/world_center_of_mass() mirror Binding::center_offset and the live
 * Body::position -- the pair a caller needs to map a point authored in the owner's local frame onto
 * the live (substep) body pose -- and add_impulse_at_position()/velocity_at_point() delegate to
 * the bound Body.
 */
COOPA_TEST(center_of_mass_and_inertia_overrides_fold_into_the_body) {
    using namespace coopa::scene;

    Scene scene("PhysicsComOverrideTest");
    auto obj = std::make_unique<SceneObject>("box");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 5.0f));
    auto* box = obj->add_component<components::BoxCollider>();
    box->set_size(glm::vec3(1.0f));
    box->set_center(glm::vec3(0.0f, 0.2f, 0.0f));
    auto* rb = obj->add_component<components::RigidbodyComponent>();
    rb->center_of_mass_override = glm::vec3(0.3f, 0.0f, 0.0f);
    rb->inertia_tensor_override = glm::vec3(9.0f, 9.0f, 9.0f); // deliberately far from the box's natural value
    scene.add_root_object(std::move(obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    sys->world().set_gravity(glm::vec3(0.0f)); // isolate the fold-in check from one substep of fall
    scene.update(util::k_default_fixed_dt);

    dynamics::BodyId id = box->body_id();
    ASSERT_TRUE(id.is_valid());
    const dynamics::Body* body = sys->world().get_body(id);

    // Pivot was (0,0,5), identity rotation -- true center is pivot + collider center + override.
    EXPECT_VEC_NEAR(body->position, glm::vec3(0.3f, 0.2f, 5.0f), 1e-5f);
    EXPECT_VEC_NEAR(body->inv_inertia_local, glm::vec3(9.0f, 9.0f, 9.0f), 1e-5f);

    EXPECT_VEC_NEAR(rb->local_center_of_mass(), glm::vec3(0.3f, 0.2f, 0.0f), 1e-5f);
    EXPECT_VEC_NEAR(rb->world_center_of_mass(), glm::vec3(0.3f, 0.2f, 5.0f), 1e-5f);

    glm::vec3 com = rb->world_center_of_mass();
    rb->add_impulse_at_position(glm::vec3(1.0f, 0.0f, 0.0f), com);
    EXPECT_VEC_NEAR(rb->velocity(), glm::vec3(1.0f, 0.0f, 0.0f), 1e-5f); // mass 1
    EXPECT_VEC_NEAR(rb->velocity_at_point(com + glm::vec3(0.0f, 0.0f, 1.0f)), rb->velocity(), 1e-5f);
}

/**
 * Compound colliders: one Rigidbody (on the ROOT object, which deliberately has NO
 * Collider of its own -- exercises Binding::sync_owner's edge case) owning two CHILD
 * SceneObjects' BoxColliders, one bigger than the other, offset so they genuinely overlap in
 * one region. Verifies every piece create_compound_body_() is responsible for: composite mass
 * (volume-weighted split of the authored total), composite center of mass (hand-computed, not
 * just "some value"), individual raycast attribution to the correct CHILD collider (not just
 * the body), and that the two children's genuine geometric overlap never produces a
 * self-collision manifold.
 *
 * Geometry (root at world (0,0,5), no rotation):
 * - child_a: BoxCollider size {2,1,1} (half-extents 1,0.5,0.5), local offset (0,0,0) -- volume
 *   2.0, world center (0,0,5), spans x[-1,1] y[-0.5,0.5] z[4.5,5.5].
 * - child_b: BoxCollider size {1,1,1} (half-extents 0.5,0.5,0.5), local offset (0,0.7,0) --
 *   volume 1.0, world center (0,0.7,5), spans x[-0.5,0.5] y[0.2,1.2] z[4.5,5.5].
 * - Overlap region: x[-0.5,0.5] y[0.2,0.5] z[4.5,5.5] -- genuinely overlapping.
 * - Rigidbody mass = 3.0 -> volume-split masses 2.0/1.0 -> composite center of mass (local) =
 *   (2*(0,0,0) + 1*(0,0.7,0)) / 3 = (0, 0.23333, 0) -> world true center (0, 0.23333, 5).
 */
COOPA_TEST(compound_collider_composes_mass_and_attributes_hits_per_child) {
    using namespace coopa::scene;

    Scene scene("PhysicsCompoundColliderTest");

    auto root = std::make_unique<SceneObject>("compound_root");
    root->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 5.0f));
    auto* rb = root->add_component<components::RigidbodyComponent>();
    rb->mass = 3.0f;
    SceneObject* root_ptr = root.get();

    auto child_a = std::make_unique<SceneObject>("child_a");
    auto* child_a_tc = child_a->add_component<TransformComponent>();
    child_a_tc->set_parent_transform(&root_ptr->get_transform()->transform());
    child_a_tc->transform().set_position(glm::vec3(0.0f, 0.0f, 0.0f));
    auto* collider_a = child_a->add_component<components::BoxCollider>();
    collider_a->set_size(glm::vec3(2.0f, 1.0f, 1.0f));
    SceneObject* child_a_ptr = root_ptr->add_child(std::move(child_a));
    (void)child_a_ptr;

    auto child_b = std::make_unique<SceneObject>("child_b");
    auto* child_b_tc = child_b->add_component<TransformComponent>();
    child_b_tc->set_parent_transform(&root_ptr->get_transform()->transform());
    child_b_tc->transform().set_position(glm::vec3(0.0f, 0.7f, 0.0f));
    auto* collider_b = child_b->add_component<components::BoxCollider>();
    collider_b->set_size(glm::vec3(1.0f, 1.0f, 1.0f));
    root_ptr->add_child(std::move(child_b));

    scene.add_root_object(std::move(root));
    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    sys->world().set_gravity(glm::vec3(0.0f)); // isolate bind-time correctness from any fall
    scene.update(util::k_default_fixed_dt);

    // Both children share the SAME body, resolved via the primary collider (whichever one
    // regather_() picked -- root has no Collider of its own, so it's whichever child was
    // gathered first; either is fine, this test only needs them to AGREE).
    dynamics::BodyId id = collider_a->body_id();
    ASSERT_TRUE(id.is_valid());
    ASSERT_TRUE(collider_b->body_id() == id); // same compound body

    const dynamics::Body* body = sys->world().get_body(id);
    ASSERT_TRUE(body != nullptr);
    ASSERT_TRUE(body->type == dynamics::BodyType::Dynamic);

    // Composite mass: total, not just one child's.
    ASSERT_NEAR(body->mass, 3.0f, 1e-4f);
    ASSERT_NEAR(body->inv_mass, 1.0f / 3.0f, 1e-4f);

    // Composite center of mass -- hand-computed above, not a placeholder check.
    ASSERT_VEC_NEAR(body->position, glm::vec3(0.0f, 0.23333f, 5.0f), 1e-3f);

    // Inertia composition produced something sane (positive, finite) on all three axes --
    // exact values aren't hand-verified here (see compose_mass_properties()'s own doc for the
    // diagonal-only approximation), just that composition didn't degenerate to zero/garbage.
    ASSERT_TRUE(body->inv_inertia_local.x > 0.0f && body->inv_inertia_local.y > 0.0f &&
                body->inv_inertia_local.z > 0.0f);

    // Raycast attribution: a point unambiguously inside ONLY child_a, and one unambiguously
    // inside ONLY child_b (see the geometry comment above) -- each must resolve to the
    // correct, DISTINCT Collider*, not just "the compound body somehow got hit."
    system::PhysicsSystem::RaycastHit hit_a;
    geometry::Ray ray_a;
    ray_a.origin = glm::vec3(0.8f, 0.0f, 10.0f);
    ray_a.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray_a.max_distance = 100.0f;
    ASSERT_TRUE(sys->raycast(ray_a, hit_a));
    ASSERT_TRUE(hit_a.collider == collider_a);

    system::PhysicsSystem::RaycastHit hit_b;
    geometry::Ray ray_b;
    ray_b.origin = glm::vec3(0.0f, 1.0f, 10.0f);
    ray_b.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray_b.max_distance = 100.0f;
    ASSERT_TRUE(sys->raycast(ray_b, hit_b));
    ASSERT_TRUE(hit_b.collider == collider_b);

    // No self-collision: child_a and child_b genuinely overlap geometrically, but they're
    // siblings of the SAME body -- narrowphase must never produce a manifold with both sides
    // resolving to this body (discover_pairs_()'s owner-based self-rejection, see its doc).
    for (int i = 0; i < 5; ++i) scene.update(util::k_default_fixed_dt);
    for (const auto& m : sys->world().manifolds()) {
        ASSERT_TRUE(!(m.a == id && m.b == id));
    }
}

/**
 * The full YAML-authoring-equivalent path: a HingeJointComponent on a "door" object, naming a
 * static "frame" object via `connected_object`, with only ONE anchor/axis authored (in the
 * door's own local frame) -- regather_joints_()/create_hinge_joint_for_() must resolve the
 * frame side automatically (Unity's autoConfigureConnectedAnchor default, see
 * HingeJointComponent's own doc) and produce the same physically correct swing-to-limit
 * behavior the headless hinge_door_swings_and_stops_at_its_limit already verified
 * directly against PhysicsWorld -- this test instead exercises the Scene/PhysicsSystem binding
 * layer on top of it (component gather, connected_object name resolution, Collider->BodyId
 * lookup), the part a hand-built PhysicsWorld test can't reach at all.
 */
COOPA_TEST(hinge_joint_component_resolves_connected_object_and_swings_to_limit) {
    using namespace coopa::scene;

    Scene scene("PhysicsHingeJointTest");

    auto frame = std::make_unique<SceneObject>("frame");
    frame->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 0.0f));
    frame->add_component<components::BoxCollider>()->set_size(glm::vec3(0.2f, 2.0f, 2.0f));
    scene.add_root_object(std::move(frame));

    auto door = std::make_unique<SceneObject>("door");
    door->add_component<TransformComponent>()->transform().set_position(glm::vec3(1.0f, 0.0f, 0.0f));
    door->add_component<components::BoxCollider>()->set_size(glm::vec3(2.0f, 1.0f, 0.2f));
    auto* rb = door->add_component<components::RigidbodyComponent>();
    rb->initial_angular_velocity = glm::vec3(0.0f, 0.0f, 5.0f); // fast push, well past the limit if unconstrained
    auto* hinge = door->add_component<components::HingeJointComponent>();
    hinge->connected_object = "frame";
    hinge->anchor = glm::vec3(-1.0f, 0.0f, 0.0f); // door's own hinge edge, in the door's local frame
    hinge->axis = glm::vec3(0.0f, 0.0f, 1.0f);
    hinge->use_limits = true;
    hinge->min_angle_deg = 0.0f;
    hinge->max_angle_deg = 90.0f;
    scene.add_root_object(std::move(door));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    sys->world().set_gravity(glm::vec3(0.0f));

    for (int i = 0; i < 300; ++i) { // 5s -- generous settle budget, matching the headless version
        scene.update(util::k_default_fixed_dt);
        scene.late_update(util::k_default_fixed_dt);
    }

    SceneObject* door_obj = scene.find_object("door");
    ASSERT_TRUE(door_obj != nullptr);
    auto* hinge_comp = door_obj->get_component<components::HingeJointComponent>();
    ASSERT_TRUE(hinge_comp != nullptr);
    dynamics::JointId joint_id = hinge_comp->joint_id();
    ASSERT_TRUE(joint_id.is_valid());

    const dynamics::HingeJoint* j = sys->world().get_joint(joint_id);
    ASSERT_TRUE(j != nullptr && j->valid);
    const dynamics::Body* frame_body = sys->world().get_body(j->a);
    const dynamics::Body* door_body = sys->world().get_body(j->b);
    ASSERT_TRUE(frame_body != nullptr && door_body != nullptr);

    float angle = dynamics::hinge_current_angle(frame_body->orientation, door_body->orientation, j->local_axis_a,
                                                 j->rest_relative_rotation);
    ASSERT_NEAR(angle, glm::radians(90.0f), glm::radians(3.0f));
    ASSERT_TRUE(glm::length(door_body->angular_velocity) < 0.05f);

    glm::vec3 world_anchor_a = frame_body->position + frame_body->orientation * j->local_anchor_a;
    glm::vec3 world_anchor_b = door_body->position + door_body->orientation * j->local_anchor_b;
    ASSERT_NEAR(glm::length(world_anchor_b - world_anchor_a), 0.0f, 0.05f);
}

COOPA_TEST(scene_queries_resolve_collider_and_object) {
    using namespace coopa::scene;

    Scene scene("PhysicsQueryTest");
    auto obj = std::make_unique<SceneObject>("target");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 0.0f));
    obj->add_component<components::SphereCollider>()->set_radius(1.0f);
    scene.add_root_object(std::move(obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    scene.update(util::k_default_fixed_dt); // first execute() binds the body

    geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray.max_distance = 100.0f;

    system::PhysicsSystem::RaycastHit hit;
    ASSERT_TRUE(sys->raycast(ray, hit));
    ASSERT_TRUE(hit.collider != nullptr);
    ASSERT_TRUE(hit.object != nullptr && hit.object->name() == "target");
    ASSERT_NEAR(hit.point.z, 1.0f, 1e-4f);

    auto overlapping = sys->overlap_sphere(glm::vec3(0.0f), 1.5f);
    ASSERT_TRUE(overlapping.size() == 1);
    ASSERT_TRUE(overlapping[0] == hit.collider);
}

/**
 * A ground Collider under a deactivated ANCESTOR (not deactivated itself) must leave the
 * physics world -- regather_()'s gather now walks up to the root checking active(), matching
 * the ancestor-active check WaterSystem::refresh_bodies() already had. Without that walk, a
 * scene that deactivates one subtree of several terrain-flavour objects (the toyengine demo
 * this guards) leaves the hidden flavour's colliders solid and a player stands on a terrain
 * that isn't being drawn. refresh() must be called after set_active() for the gather to notice
 * -- nothing dirties the system on activation by itself.
 */
COOPA_TEST(a_collider_under_a_deactivated_ancestor_leaves_the_world) {
    using namespace coopa::scene;

    Scene scene("PhysicsAncestorActiveTest");

    auto group = std::make_unique<SceneObject>("group");
    group->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f));
    SceneObject* group_ptr = group.get();

    auto ground = std::make_unique<SceneObject>("ground");
    auto* ground_tc = ground->add_component<TransformComponent>();
    ground_tc->set_parent_transform(&group_ptr->get_transform()->transform());
    ground_tc->transform().set_position(glm::vec3(0.0f));
    ground->add_component<components::BoxCollider>()->set_size(glm::vec3(8.0f, 8.0f, 1.0f));
    group_ptr->add_child(std::move(ground));

    scene.add_root_object(std::move(group));
    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    scene.update(util::k_default_fixed_dt); // first execute() binds the ground body

    geometry::Ray down;
    down.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    down.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    down.max_distance = 100.0f;

    system::PhysicsSystem::RaycastHit hit;
    ASSERT_TRUE(sys->raycast(down, hit)); // solid while the group is active

    group_ptr->set_active(false);
    sys->refresh();
    scene.update(util::k_default_fixed_dt);
    ASSERT_TRUE(!sys->raycast(down, hit)); // the ancestor-active walk drops it from the gather

    group_ptr->set_active(true);
    sys->refresh();
    scene.update(util::k_default_fixed_dt);
    ASSERT_TRUE(sys->raycast(down, hit)); // reactivating puts it straight back
}

/**
 * Hierarchical dynamic bodies (a ragdoll's shape: each link a CHILD of the previous one, each
 * with its own Rigidbody + Collider, jointed with a BallJoint and a ConeTwistJoint component)
 * must be written back as correct LOCAL poses -- computed from the parent's new body pose, not
 * its stale world matrix -- on the job-parallel path too. Checked every frame: each link's
 * Transform world position equals its body's pivot, and no link ever "teleports" (the
 * write-back and the next frame's teleport check agree), so the chain swings smoothly.
 */
COOPA_TEST(hierarchical_bodies_write_back_correct_local_poses_in_parallel) {
    using namespace coopa::scene;
    coopa::job::JobEngine jobs(4);
    Scene scene("PhysicsHierarchyWriteBack");
    scene.set_job_engine(&jobs);

    auto anchor = std::make_unique<SceneObject>("anchor");
    anchor->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 3.0f));
    anchor->add_component<components::SphereCollider>()->set_radius(0.05f);
    SceneObject* parent = anchor.get();
    scene.add_root_object(std::move(anchor));

    const char* names[3] = {"link0", "link1", "link2"};
    for (int i = 0; i < 3; ++i) {
        auto link = std::make_unique<SceneObject>(names[i]);
        // Each link's pivot sits 0.5 m along +X from its parent's (horizontal start: it swings).
        link->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.5f, 0.0f, 0.0f));
        auto* box = link->add_component<components::BoxCollider>();
        box->set_size(glm::vec3(0.4f, 0.1f, 0.1f));
        box->set_center(glm::vec3(0.25f, 0.0f, 0.0f));
        link->add_component<components::RigidbodyComponent>()->mass = 1.0f;
        if (i == 1) {
            auto* cone = link->add_component<components::ConeTwistJointComponent>();
            cone->connected_object = names[i - 1];
            cone->axis = glm::vec3(1.0f, 0.0f, 0.0f);
            cone->swing_limit_deg = 70.0f;
        } else {
            auto* ball = link->add_component<components::BallJointComponent>();
            ball->connected_object = i == 0 ? "anchor" : names[i - 1];
        }
        SceneObject* raw = link.get();
        raw->get_transform()->set_parent_transform(&parent->get_transform()->transform());
        parent->add_child(std::move(link));
        parent = raw;
    }

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    sys->set_parallel_threshold(1); // force the job-parallel write-back path

    float max_error = 0.0f;
    glm::vec3 start_tip(0.0f);
    for (int frame = 0; frame < 120; ++frame) {
        scene.update(util::k_default_fixed_dt);
        scene.late_update(util::k_default_fixed_dt);
        for (int i = 0; i < 3; ++i) {
            SceneObject* obj = scene.find_object(names[i]);
            auto* col = obj->get_component<components::BoxCollider>();
            const dynamics::Body* body = sys->world().get_body(col->body_id());
            ASSERT_TRUE(body != nullptr);
            const glm::mat4 world = obj->get_transform()->transform().get_world_matrix();
            // last_written_*: what the write-back wrote (interpolated, when the body interpolates).
            const glm::vec3 pivot = body->last_written_position - body->last_written_orientation * glm::vec3(0.25f, 0.0f, 0.0f);
            max_error = std::max(max_error, glm::length(glm::vec3(world[3]) - pivot));
        }
        if (frame == 0) start_tip = glm::vec3(scene.find_object("link2")->get_transform()->transform().get_world_matrix()[3]);
    }
    ASSERT_TRUE(max_error < 0.02f);
    // It swung down under gravity (the joints did not lock it, the write-back did not freeze it).
    glm::vec3 end_tip(scene.find_object("link2")->get_transform()->transform().get_world_matrix()[3]);
    ASSERT_TRUE(end_tip.z < start_tip.z - 0.3f);
    // ...and the links stayed linked (pivot-to-pivot distance ~0.5 m).
    for (int i = 1; i < 3; ++i) {
        glm::vec3 p0(scene.find_object(names[i - 1])->get_transform()->transform().get_world_matrix()[3]);
        glm::vec3 p1(scene.find_object(names[i])->get_transform()->transform().get_world_matrix()[3]);
        ASSERT_NEAR(glm::length(p1 - p0), 0.5f, 0.03f);
    }
}

/**
 * A kinematic body's velocity is derived from how far its Transform moved, so there is no way to
 * tell a platform travelling fast from a body that was simply PUT somewhere -- a spawn dropped on
 * the ground, a character teleported, an animated bone whose clip cut to a new pose. The derived
 * figure for a placement is the distance over one frame, which is hundreds of m/s, and the solver
 * believes it: whatever dynamic body is in contact gets launched. Past
 * PhysicsConfig::max_kinematic_speed, the move is read as the placement it is -- the body is
 * placed, and its velocity left at zero.
 */
COOPA_TEST(a_placed_kinematic_body_reports_no_velocity) {
    using namespace coopa::scene;

    Scene scene("PhysicsKinematicTeleportTest");
    auto obj = std::make_unique<SceneObject>("platform");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f));
    obj->add_component<components::BoxCollider>()->set_size(glm::vec3(1.0f));
    auto* rb = obj->add_component<components::RigidbodyComponent>();
    rb->is_kinematic = true;
    rb->use_gravity = false;
    scene.add_root_object(std::move(obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);
    const float dt = util::k_default_fixed_dt;
    scene.update(dt);

    SceneObject* platform = scene.find_object("platform");
    coopa::util::Transform& t = platform->get_transform()->transform();
    const dynamics::BodyId id = platform->get_component<components::BoxCollider>()->body_id();
    ASSERT_TRUE(id.is_valid());

    // Driven: 3 m/s along +x is a platform moving, and the derivation is what makes a character
    // standing on it ride along.
    t.set_position(glm::vec3(3.0f * dt, 0.0f, 0.0f));
    scene.update(dt);
    ASSERT_VEC_NEAR(sys->world().get_body(id)->linear_velocity, glm::vec3(3.0f, 0.0f, 0.0f), 1e-3f);

    // Placed: 40 m in one frame is 2400 m/s, which is nothing moving -- it is a teleport.
    t.set_position(glm::vec3(0.0f, 40.0f, 0.0f));
    scene.update(dt);
    const dynamics::Body* body = sys->world().get_body(id);
    ASSERT_VEC_NEAR(body->linear_velocity, glm::vec3(0.0f), 1e-6f);
    ASSERT_VEC_NEAR(body->angular_velocity, glm::vec3(0.0f), 1e-6f);
    ASSERT_VEC_NEAR(body->position, glm::vec3(0.0f, 40.0f, 0.0f), 1e-4f);   // but it IS there

    // ...and it is moving again on the next ordinary step, from where it was placed.
    t.set_position(glm::vec3(0.0f, 40.0f + 2.0f * dt, 0.0f));
    scene.update(dt);
    ASSERT_VEC_NEAR(sys->world().get_body(id)->linear_velocity, glm::vec3(0.0f, 2.0f, 0.0f), 1e-3f);

    // A spin no body turns at is the same story: both velocities go, not just the one that broke
    // its cap, since a placement moves and turns at once.
    t.set_position(glm::vec3(0.0f, 40.0f + 2.0f * dt, 0.0f));
    t.set_rotation_quat(glm::angleAxis(2.0f, glm::vec3(0.0f, 0.0f, 1.0f)));
    scene.update(dt);
    body = sys->world().get_body(id);
    ASSERT_VEC_NEAR(body->linear_velocity, glm::vec3(0.0f), 1e-6f);
    ASSERT_VEC_NEAR(body->angular_velocity, glm::vec3(0.0f), 1e-6f);

    // Turning the detection off restores the raw derivation.
    sys->world().config().max_kinematic_speed = 0.0f;
    sys->world().config().max_kinematic_spin = 0.0f;
    t.set_position(glm::vec3(0.0f, 0.0f, 0.0f));
    scene.update(dt);
    ASSERT_TRUE(glm::length(sys->world().get_body(id)->linear_velocity) > 100.0f);
}

/**
 * ConvexCollider on a non-kinematic Rigidbody must bind and simulate normally -- unlike
 * MeshCollider, where exactly this configuration throws (see create_compound_body_()'s mesh-
 * collider-static-only check). A dynamic hull body is the entire reason ConvexCollider exists,
 * so this is the regression guard that the TriangleMesh-specific throw never widens to catch it.
 */
COOPA_TEST(convex_collider_on_a_dynamic_rigidbody_binds_and_settles_flat) {
    using namespace coopa::scene;

    glm::vec3 pts[8];
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) pts[k++] = glm::vec3(float(sx), float(sy), float(sz)) * 0.5f;
    auto hull = std::make_shared<geometry::ConvexHull>();
    ASSERT_TRUE(geometry::build_convex_hull(pts, 8, *hull));

    Scene scene("ConvexColliderDynamicTest");

    auto ground = std::make_unique<SceneObject>("ground");
    ground->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, -0.5f));
    ground->add_component<components::BoxCollider>()->set_size(glm::vec3(8.0f, 8.0f, 1.0f));
    scene.add_root_object(std::move(ground));

    auto obj = std::make_unique<SceneObject>("hull");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 2.0f));
    obj->add_component<components::ConvexCollider>()->set_hull(hull);
    obj->add_component<components::RigidbodyComponent>(); // non-kinematic: must NOT throw
    scene.add_root_object(std::move(obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);

    for (int i = 0; i < 300; ++i) {
        scene.update(util::k_default_fixed_dt);
        scene.late_update(util::k_default_fixed_dt);
    }
    EXPECT_EQ(sys->bound_count(), 2u);

    SceneObject* obj_ptr = scene.find_object("hull");
    ASSERT_TRUE(obj_ptr != nullptr);
    dynamics::BodyId id = obj_ptr->get_component<components::ConvexCollider>()->body_id();
    const dynamics::Body* b = sys->world().get_body(id);
    ASSERT_TRUE(b != nullptr);
    EXPECT_TRUE(b->type == dynamics::BodyType::Dynamic);
    EXPECT_TRUE(is_settled(*b));
    EXPECT_NEAR(b->position.z, 0.5f, 0.02f);
    EXPECT_GT(best_up_alignment(b->orientation), 0.999f);
}
