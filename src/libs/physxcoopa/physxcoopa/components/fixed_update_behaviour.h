/**
 * @file fixed_update_behaviour.h
 * @brief Base class for gameplay code that needs a per-substep callback, without a libcoopa
 *        Component::fixed_update hook -- which does not exist and should not be added just
 *        for this.
 */

#ifndef PHYSXCOOPA_COMPONENTS_FIXED_UPDATE_BEHAVIOUR_H
#define PHYSXCOOPA_COMPONENTS_FIXED_UPDATE_BEHAVIOUR_H

#include <physxcoopa/system/physics_system.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/event/signal.h>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class FixedUpdateBehaviour
 * @brief Subclass and override fixed_update() for Unity-style FixedUpdate() ergonomics.
 *
 * On its first update(), looks up `scene->find_system("Physics")` and connects to the
 * world's on_substep signal; disconnects in its destructor. Requires PhysicsSystem to
 * already be installed by the time the first update() runs (true for any object present at
 * scene load, since install_physics_system() is called right after SceneLoader::load()).
 */
class FixedUpdateBehaviour : public coopa::scene::Component {
public:
    ~FixedUpdateBehaviour() override {
        connection_.disconnect();
    }

    void update(float) override {
        if (connected_) return;
        connected_ = true;
        if (!scene) return;
        auto* sys = dynamic_cast<system::PhysicsSystem*>(scene->find_system("Physics"));
        if (!sys) return;
        connection_ = sys->world().on_substep.connect(
            [this](PhysicsWorld& world, float h) { fixed_update(world, h); });
    }

    /** @brief Called once per fixed substep -- see PhysicsWorld::on_substep's doc for exactly
     *         where in the step this fires, and which mutations are immediate vs. deferred. */
    virtual void fixed_update(PhysicsWorld& world, float h) = 0;

private:
    bool connected_ = false;
    coopa::event::Connection connection_;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_FIXED_UPDATE_BEHAVIOUR_H
