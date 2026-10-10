#pragma once

/**
 * @file water_fixtures.h
 * @brief Shared water inputs: a sloped strip mesh (flow bake, flowing bodies) and a CPU-only
 *        water scene -- one WaterBody plus buoyant boxes, WaterSystem and physics installed by
 *        hand, no device (a WaterSystem with no device bakes the query but publishes no mesh).
 */

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_system.h>

namespace toy::test {

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

/** @brief A strip `length` x `width` along +X (local), resolution `nx` x `ny`, height z(x). */
template <typename HeightFn>
inline void make_strip(float length, float width, int nx, int ny, HeightFn z, std::vector<glm::vec3>& pos,
                std::vector<glm::vec2>& uv, std::vector<uint32_t>& idx) {
    pos.clear();
    uv.clear();
    idx.clear();
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            float x = length * static_cast<float>(i) / nx;
            float y = width * (static_cast<float>(j) / ny - 0.5f);
            pos.push_back({x, y, z(x)});
            uv.push_back({x, y});
        }
    }
    const uint32_t row = static_cast<uint32_t>(nx + 1);
    for (uint32_t j = 0; j < static_cast<uint32_t>(ny); ++j) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(nx); ++i) {
            uint32_t a = j * row + i;
            idx.insert(idx.end(), {a, a + 1, a + row + 1, a, a + row + 1, a + row});
        }
    }
}

/** @brief Mean flow over the strip's centre row for x in [x0, x1]. */
inline glm::vec3 mean_flow(const std::vector<glm::vec3>& pos, const std::vector<glm::vec3>& flow, float x0, float x1) {
    glm::vec3 sum(0.0f);
    int n = 0;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        if (pos[i].x < x0 || pos[i].x > x1 || std::fabs(pos[i].y) > 0.6f) continue;
        sum += flow[i];
        ++n;
    }
    return n ? sum / static_cast<float>(n) : sum;
}

struct BuoyScene {
    std::unique_ptr<Scene> scene;
    toy::water::WaterSystem* water = nullptr;
    coopa::physx::system::PhysicsSystem* physics = nullptr;
    toy::water::WaterBody* body = nullptr;
};

/** @brief A scene with one WaterBody (calm 40x40 m planar at z = 0 unless configured). */
inline BuoyScene make_water_scene(const std::function<void(toy::water::WaterBody&)>& configure = {}) {
    BuoyScene bs;
    bs.scene = std::make_unique<Scene>("water_test_cpu");
    auto obj = std::make_unique<SceneObject>("water");
    obj->add_component<TransformComponent>();
    bs.body = obj->add_component<toy::water::WaterBody>();
    bs.body->size = glm::vec2(40.0f);
    bs.body->resolution = 20;
    if (configure) configure(*bs.body);
    bs.scene->add_root_object(std::move(obj));
    return bs;
}

/** @brief Adds a dynamic box with a Buoyancy component. */
inline SceneObject* add_box(BuoyScene& bs, const std::string& name, const glm::vec3& pos, const glm::vec3& size,
                     float mass) {
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>()->transform().set_position(pos);
    obj->add_component<coopa::physx::components::BoxCollider>()->set_size(size);
    obj->add_component<coopa::physx::components::RigidbodyComponent>()->mass = mass;
    obj->add_component<toy::water::Buoyancy>();
    SceneObject* raw = obj.get();
    bs.scene->add_root_object(std::move(obj));
    return raw;
}

inline void start(BuoyScene& bs) {
    bs.scene->start();
    bs.water = toy::water::install_water_system(*bs.scene);
    bs.physics = coopa::physx::system::install_physics_system(*bs.scene);
}

inline void run(BuoyScene& bs, float seconds) {
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        bs.scene->update(dt);
        bs.scene->late_update(dt);
    }
}

inline glm::vec3 com_of(SceneObject* o) {
    return o->get_component<coopa::physx::components::RigidbodyComponent>()->world_center_of_mass();
}

} // namespace toy::test
