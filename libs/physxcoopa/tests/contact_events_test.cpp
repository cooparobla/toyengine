/**
 * @file contact_events_test.cpp
 * @brief Contact events: the world-level trigger Enter/Stay/Exit sequence, and the scene-level
 *        Collider collision callbacks with their contact payload.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <memory>

COOPA_TEST_SUITE("contact_events");

using namespace coopa::physx;

COOPA_TEST(trigger_fires_enter_and_exit_once_with_stays_between) {
    PhysicsWorld world;
    world.set_gravity(glm::vec3(0.0f)); // isolate the trigger pass from falling

    dynamics::Body trigger_body;
    trigger_body.type = dynamics::BodyType::Static;
    collision::Shape trigger_shape = collision::Shape::make_sphere(1.0f);
    trigger_shape.is_trigger = true;
    dynamics::BodyId trigger_id = world.add_body(trigger_body, trigger_shape);

    // A dynamic probe (triggers only ever pair with a Dynamic body -- see
    // generate_manifolds_()'s either_dynamic check) flies straight through the trigger volume.
    dynamics::Body probe;
    probe.type = dynamics::BodyType::Dynamic;
    probe.position = glm::vec3(-3.0f, 0.0f, 0.0f);
    probe.linear_velocity = glm::vec3(2.0f, 0.0f, 0.0f);
    probe.mass = 1.0f;
    probe.inv_mass = 1.0f;
    probe.inv_inertia_local = dynamics::sphere_inverse_inertia(0.1f, probe.mass);
    dynamics::BodyId probe_id = world.add_body(probe, collision::Shape::make_sphere(0.1f));

    int enter_count = 0, stay_count = 0, exit_count = 0;
    for (int i = 0; i < 200; ++i) { // 2 m/s over 6 units (x: -3 -> 3) takes 3s = 180 steps
        world.clear_events();
        world.step_fixed(util::k_default_fixed_dt);
        for (const auto& ev : world.events()) {
            if (!ev.is_trigger) continue;
            bool is_this_pair = (ev.a.index == trigger_id.index && ev.b.index == probe_id.index) ||
                                 (ev.a.index == probe_id.index && ev.b.index == trigger_id.index);
            if (!is_this_pair) continue;
            if (ev.type == collision::ContactEvent::Type::Enter) ++enter_count;
            else if (ev.type == collision::ContactEvent::Type::Stay) ++stay_count;
            else ++exit_count;
        }
    }

    ASSERT_TRUE(enter_count == 1);
    ASSERT_TRUE(exit_count == 1);
    ASSERT_TRUE(stay_count > 0); // fires every step while inside
}

COOPA_TEST(collision_callbacks_carry_contact_payload_and_exit_clears_it) {
    using namespace coopa::scene;

    Scene scene("PhysicsCollisionPayloadTest");

    auto ground = std::make_unique<SceneObject>("ground");
    ground->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, -0.5f));
    ground->add_component<components::BoxCollider>()->set_size(glm::vec3(8.0f, 8.0f, 1.0f));
    scene.add_root_object(std::move(ground));

    auto sphere_obj = std::make_unique<SceneObject>("sphere");
    sphere_obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 3.0f));
    auto* sphere_collider = sphere_obj->add_component<components::SphereCollider>();
    sphere_collider->set_radius(0.5f);
    auto bouncy = std::make_shared<dynamics::PhysicsMaterial>();
    bouncy->restitution = 0.9f;
    sphere_collider->set_material(bouncy);
    sphere_obj->add_component<components::RigidbodyComponent>();
    scene.add_root_object(std::move(sphere_obj));

    scene.start();
    system::PhysicsSystem* sys = system::install_physics_system(scene);

    int enter_count = 0, exit_count = 0;
    bool enter_had_contacts = false, enter_had_impulse = false;
    float enter_normal_z = 0.0f;
    bool exit_had_zero_contacts = true;

    sphere_collider->on_collision_enter.connect([&](components::Collider&, const components::Collision& c) {
        ++enter_count;
        enter_had_contacts = c.contact_count > 0;
        enter_had_impulse = glm::length(c.impulse) > 0.0f;
        enter_normal_z = c.normal.z;
        EXPECT_TRUE(c.collider != nullptr); // non-fatal: a throw would unwind through the signal emit
        EXPECT_TRUE(c.object != nullptr && c.object->name() == "ground");
    });
    sphere_collider->on_collision_exit.connect([&](components::Collider&, const components::Collision& c) {
        ++exit_count;
        if (c.contact_count != 0) exit_had_zero_contacts = false;
    });

    for (int i = 0; i < 300; ++i) {
        scene.update(util::k_default_fixed_dt);
        scene.late_update(util::k_default_fixed_dt);
    }

    ASSERT_TRUE(enter_count >= 1);
    ASSERT_TRUE(exit_count >= 1); // combined restitution ~0.45 must separate at least once
    ASSERT_TRUE(enter_had_contacts);
    ASSERT_TRUE(enter_had_impulse);
    ASSERT_TRUE(enter_normal_z < 0.0f); // points from sphere (self) down toward the ground (other)
    ASSERT_TRUE(exit_had_zero_contacts);
    (void)sys;
}
