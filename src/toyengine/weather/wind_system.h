#pragma once

/**
 * @file wind_system.h
 * @brief WindSystem: the weather's wind (wind.h) on physics -- cloth and rigidbodies.
 *
 * Order 95: after the weather (40) has computed this frame's wind, before Physics (100) steps.
 * Every frame it
 *   - writes each bound Cloth's live params.wind = its authored `wind` + the field at the cloth
 *     x ClothComponent::weather_wind (default 1), so flags and banners flutter with the weather
 *     and its gusts (a scene without weather keeps exactly the authored wind);
 *   - lists the dynamic Rigidbodies with wind_influence > 0, then pushes them every physics
 *     substep (PhysicsWorld::on_substep) with a drag force influence * |rel| * rel, rel = the
 *     wind at the body minus its horizontal velocity. A sleeping body is woken only when that
 *     force is a real fraction of its weight, so crates in a breeze stay asleep.
 * Simulation only (nothing moves in edit mode).
 */

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/event/signal.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/env/env_field.h>
#include <physxcoopa/dynamics/body.h>

namespace coopa::physx {
class PhysicsWorld;
namespace system { class PhysicsSystem; }
}

namespace toy {
namespace weather {

constexpr int k_wind_system_order = 95;

/** @brief The wind force (N) on a body: influence * |rel| * rel, rel = (wind - body velocity) horizontally. */
inline glm::vec3 wind_drag_force(float influence, const glm::vec3& wind, const glm::vec3& velocity) {
    const glm::vec3 rel(wind.x - velocity.x, wind.y - velocity.y, 0.0f);
    return influence * glm::length(rel) * rel;
}

class WindSystem : public coopa::scene::ISceneSystem {
public:
    ~WindSystem() override;
    const char* system_name() const override { return "Wind"; }
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;
    void on_detach(coopa::scene::Scene& scene) override;

    /** @brief Rigidbodies the wind pushes now (for tests / stats). */
    std::size_t pushed_bodies() const { return bodies_.size(); }

private:
    void connect_(coopa::scene::Scene& scene);
    void substep_(coopa::physx::PhysicsWorld& world, float h);

    struct Entry { coopa::physx::dynamics::BodyId id; float influence = 0.0f; };
    std::vector<Entry> bodies_;
    /// This frame's environment, cached in execute() and read in substep_() (which runs on
    /// the physics stack at order 100, strictly after this system's 95, so it is never stale).
    env::EnvField env_;
    coopa::physx::system::PhysicsSystem* physics_ = nullptr;
    coopa::event::Connection connection_;
};

/** @brief Installs the scene's WindSystem (once). */
WindSystem* install_wind_system(coopa::scene::Scene& scene, int order = k_wind_system_order);

} // namespace weather
} // namespace toy
