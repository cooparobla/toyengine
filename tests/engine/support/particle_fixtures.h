#pragma once

/**
 * @file particle_fixtures.h
 * @brief A device-free particle scene: ParticleSystems on their own objects (configured before
 *        start), the simulation system installed by hand, and a fixed-step runner -- plus a
 *        two-triangle emission surface with known areas.
 */

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>
#include <toyengine/particles/particle_shape.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/particle_system_runner.h>

namespace toy::test {

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::particles::ParticleSystem;

/** @brief A scene of ParticleSystems on their own objects (configure each before start). */
struct PScene {
    std::unique_ptr<Scene> scene;
    toy::particles::ParticleSimulationSystem* runner = nullptr;
};

inline ParticleSystem* add_system(PScene& ps, const std::string& name, const glm::vec3& pos,
                           const std::function<void(ParticleSystem&)>& configure) {
    if (!ps.scene) ps.scene = std::make_unique<Scene>("particles_cpu");
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>()->transform().set_position(pos);
    auto* sys = obj->add_component<ParticleSystem>();
    if (configure) configure(*sys);
    ps.scene->add_root_object(std::move(obj));
    return sys;
}

inline void start(PScene& ps) {
    ps.scene->start();
    ps.runner = toy::particles::install_particle_system(*ps.scene);
}

inline void run(PScene& ps, float seconds, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < static_cast<int>(std::lround(seconds / dt)); ++i) {
        ps.scene->update(dt);
        ps.scene->late_update(dt);
    }
}

/** @brief Two triangles in the z = 0 plane: [0,1]x[0,1] lower-left half (area 0.5) and a big one
 *         over x in [2,5] (area 4.5), with +Z normals. */
inline toy::particles::MeshSurface two_triangle_surface() {
    toy::particles::MeshSurface s;
    std::vector<glm::vec3> p = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {5, 0, 0}, {2, 3, 0}};
    std::vector<glm::vec3> n(6, glm::vec3(0, 0, 1));
    s.build(p, n, {}, {});
    return s;
}

} // namespace toy::test
