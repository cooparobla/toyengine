#pragma once

/**
 * @file character_rig.h
 * @brief A headless physics scene for character tests: static/kinematic/dynamic boxes, one
 *        CharacterController ("player"), and the order-50 control system + physics installed the
 *        way Engine::prepare_scene_() does. Shared by the character_controller, camera_controller,
 *        foot_ik and animation_rig suites -- everything that needs a body walking on colliders.
 */

#include <algorithm>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/character/character_motor.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/kinematic_control_system.h>

namespace toy::test {

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

struct CharacterRig {
    Scene scene{"character_demo"};
    toy::scene::CharacterController* cc = nullptr;
    SceneObject* player = nullptr;
    coopa::physx::system::PhysicsSystem* physics = nullptr;

    SceneObject* box(const std::string& name, const glm::vec3& pos, const glm::vec3& size,
                     const glm::vec3& rot_deg = glm::vec3(0.0f), int kind = 0 /* 0 static, 1 kinematic, 2 dynamic */,
                     float mass = 10.0f) {
        auto obj = std::make_unique<SceneObject>(name);
        auto& t = obj->add_component<TransformComponent>()->transform();
        t.set_position(pos);
        t.set_rotation(rot_deg);
        obj->add_component<coopa::physx::components::BoxCollider>()->set_size(size);
        if (kind != 0) {
            auto* rb = obj->add_component<coopa::physx::components::RigidbodyComponent>();
            rb->is_kinematic = kind == 1;
            rb->use_gravity = kind == 2;
            rb->mass = mass;
        }
        SceneObject* raw = obj.get();
        scene.add_root_object(std::move(obj));
        return raw;
    }

    void spawn(const glm::vec3& feet) {
        auto obj = std::make_unique<SceneObject>("player");
        obj->add_component<TransformComponent>()->transform().set_position(feet);
        cc = obj->add_component<toy::scene::CharacterController>();
        player = obj.get();
        scene.add_root_object(std::move(obj));
    }

    void start() {
        scene.start();
        toy::scene::install_kinematic_control_system(scene);
        physics = coopa::physx::system::install_physics_system(scene);
    }

    void step(int frames, const glm::vec2& move = glm::vec2(0.0f)) {
        for (int i = 0; i < frames; ++i) {
            cc->move_input = move;
            scene.update(1.0f / 60.0f);
            scene.late_update(1.0f / 60.0f);
        }
    }

    glm::vec3 feet() const { return player->get_transform()->transform().position(); }

    /** @brief Deepest overlap of the character's capsule with anything but itself. */
    float penetration() const {
        coopa::physx::query::QueryFilter f;
        f.include_triggers = false;
        f.ignore = player->get_component<coopa::physx::components::RigidbodyComponent>()->body_id();
        const glm::vec3 centre = feet() + glm::vec3(0.0f, 0.0f, 0.5f * cc->height);
        float deepest = 0.0f;
        for (const auto& p : physics->world().compute_penetration(coopa::physx::character::motor_capsule(cc->motor_settings()),
                                                                  centre, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), f))
            deepest = std::max(deepest, p.depth);
        return deepest;
    }
};

/** @brief The default ground for character tests: a 60 m slab, top at z = 0. */
inline void character_ground(CharacterRig& rig) { rig.box("ground", glm::vec3(0.0f, 0.0f, -0.5f), glm::vec3(60.0f, 60.0f, 1.0f)); }

} // namespace toy::test
