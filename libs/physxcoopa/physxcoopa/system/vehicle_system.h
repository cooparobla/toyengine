/**
 * @file vehicle_system.h
 * @brief VehicleSystem + install_vehicle_system() -- binds every VehicleComponent to its chassis
 *        body, steps them inside the physics substep, and places their wheel visuals.
 *
 * Two stages, for the same reason RagdollSystem has three:
 *
 *   - **PrePhysics, order 96** (after the weather's wind at 95, before Physics at 100): finds
 *     each vehicle's chassis Rigidbody, resolves its wheel anchors into the body's
 *     centre-of-mass frame, and keeps one connection to PhysicsWorld::on_substep for the whole
 *     scene. Also zeroes the controls of any `player_input` vehicle that is not `active`, so a
 *     host that only writes to the vehicle being driven cannot leave another one holding the
 *     throttle.
 *   - **PostPhysics, order 150** (after Physics, before the Behaviour walk at 200): writes each
 *     wheel child's local Transform from the state the step just produced -- suspension travel,
 *     steer angle and roll -- and emits the frame's signals. Gameplay reading the vehicle in its
 *     own update() therefore sees this frame's state, and acts on the next one.
 *
 * The substep callback is where the actual work happens (vehicle::simulate()); everything here
 * is binding and presentation. Signals are emitted in PostPhysics rather than from inside the
 * substep, because a slot that destroyed an object mid-solve would do so halfway through a step.
 *
 * A vehicle binds on the first frame its Rigidbody has a body, which is the frame AFTER the one
 * it was spawned on (PhysicsSystem's reconcile pass at order 100 is what creates the body, and
 * this runs at 96). It simulates from the following step; nothing needs to know.
 */

#ifndef PHYSXCOOPA_SYSTEM_VEHICLE_SYSTEM_H
#define PHYSXCOOPA_SYSTEM_VEHICLE_SYSTEM_H

#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/vehicle.h>
#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/vehicle/vehicle_sim.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace system {

/** @brief Before Physics (100), after the host's wind (95). */
inline constexpr int k_vehicle_pre_physics_order = 96;
/** @brief After Physics (100), before the Behaviour walk (200). */
inline constexpr int k_vehicle_post_physics_order = 150;

/**
 * @class VehicleSystem
 * @brief See the file doc. Installed twice, once per Stage.
 */
class VehicleSystem : public coopa::scene::ISceneSystem {
public:
    enum class Stage {
        PrePhysics,
        PostPhysics,
    };

    explicit VehicleSystem(Stage stage) : stage_(stage) {}

    const char* system_name() const override {
        return stage_ == Stage::PrePhysics ? "Vehicle" : "VehiclePost";
    }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) override {
        if (stage_ == Stage::PrePhysics) {
            bind_(scene);
            connect_(scene);
        } else {
            present_(scene);
        }
    }

private:
    /** @brief Binds every vehicle to its chassis body, and drops a binding whose body has gone. */
    void bind_(coopa::scene::Scene& scene) {
        vehicles_.clear();
        for (components::VehicleComponent* v : scene.get_components<components::VehicleComponent>()) {
            if (!v->owner) continue;
            auto* rb = v->owner->get_component<components::RigidbodyComponent>();
            const dynamics::BodyId id = rb ? rb->body_id() : dynamics::BodyId{};
            if (id != v->body_id()) {
                if (id.is_valid() && rb) {
                    v->set_binding(id, rb->local_center_of_mass());
                } else {
                    v->clear_binding();
                }
            }
            if (!v->is_bound()) continue;
            // A host writes only the vehicle it is driving; without this, one it stopped driving
            // mid-corner would keep that steering input for the rest of the scene.
            if (v->player_input && !v->active) v->controls = vehicle::VehicleControls{};
            vehicles_.push_back(v);
        }
    }

    /** @brief One connection to the world's substep for the whole scene, re-made if the
     *         PhysicsSystem is replaced (a scene reload). Mirrors WindSystem::connect_(). */
    void connect_(coopa::scene::Scene& scene) {
        auto* sys = dynamic_cast<PhysicsSystem*>(scene.find_system("Physics"));
        if (sys == physics_) return;
        connection_.disconnect();
        physics_ = sys;
        if (physics_) {
            connection_ = physics_->world().on_substep.connect(
                [this](PhysicsWorld& world, float h) { substep_(world, h); });
        }
    }

    /** @brief `vehicles_` is rebuilt by bind_() at order 96 and consumed by the physics step at
     *         order 100, four slots later in the same frame, so the pointers cannot go stale in
     *         between -- nothing removes components in that window (destruction runs at 70). */
    void substep_(PhysicsWorld& world, float h) {
        for (components::VehicleComponent* v : vehicles_) {
            vehicle::simulate(world, v->body_id(), v->sim_config(), v->controls, v->state_mut(), h);
        }
    }

    /** @brief Wheel visuals, then the frame's signals. */
    void present_(coopa::scene::Scene& scene) {
        for (components::VehicleComponent* v : scene.get_components<components::VehicleComponent>()) {
            if (!v->owner || !v->is_bound()) continue;
            write_wheel_visuals_(*v);
            emit_signals_(*v);
            // One-shot presses, consumed by the step just run.
            v->controls.gear_request = 0;
            v->controls.recover = false;
        }
    }

    /**
     * @brief Places each wheel's child object in the chassis' local frame.
     *
     * The hub sits `hub_offset` below the AUTHORED anchor (VehicleComponent::config, which is
     * origin-relative -- not the centre-of-mass-relative copy the sim runs on). The wheel is
     * yawed by the steer angle and rolled by its accumulated spin, both negative about their
     * axes: with +Y forward and +Z up, turning the wheel to the right rotates its forward
     * direction toward +X, which is a negative rotation about +Z, and rolling forward carries
     * the top of the wheel toward +Y, which is a negative rotation about +X.
     */
    void write_wheel_visuals_(components::VehicleComponent& v) {
        const std::size_t n = std::min(v.config.wheels.size(), v.state().wheels.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (i >= v.wheel_objects.size() || v.wheel_objects[i].empty()) continue;
            coopa::scene::SceneObject* obj = v.owner->find_child(v.wheel_objects[i]);
            if (!obj) obj = v.owner->find_descendant(v.wheel_objects[i]);
            if (!obj || !obj->get_transform()) continue;

            const vehicle::WheelState& ws = v.state().wheels[i];
            coopa::util::Transform& t = obj->get_transform()->transform();
            t.set_position(v.config.wheels[i].anchor - glm::vec3(0.0f, 0.0f, ws.hub_offset));
            t.set_rotation_quat(glm::angleAxis(-ws.steer_angle, glm::vec3(0.0f, 0.0f, 1.0f)) *
                                glm::angleAxis(-ws.spin_angle, glm::vec3(1.0f, 0.0f, 0.0f)));
        }
    }

    void emit_signals_(components::VehicleComponent& v) {
        vehicle::VehicleState& st = v.state_mut();
        if (st.gear_changed) {
            st.gear_changed = false;
            v.on_gear_change.emit(st.gear);
        }
        if (st.landing_impact > 0.0f) {
            const float impact = st.landing_impact;
            st.landing_impact = 0.0f;
            v.on_landed.emit(impact);
        }
        if (st.slip_started_mask != 0) {
            const uint32_t mask = st.slip_started_mask;
            st.slip_started_mask = 0;
            for (int i = 0; i < 32; ++i) {
                if ((mask >> i) & 1u) v.on_wheel_slip.emit(i);
            }
        }
    }

    Stage stage_;
    PhysicsSystem* physics_ = nullptr;
    coopa::event::Connection connection_;
    std::vector<components::VehicleComponent*> vehicles_;
};

/**
 * @brief Installs both VehicleSystem stages. Call once per scene, after install_physics_system()
 *        or before it -- the system finds the PhysicsSystem by name each frame, so the order of
 *        the two install calls does not matter.
 *
 * @param scene Scene to install into.
 */
inline void install_vehicle_system(coopa::scene::Scene& scene) {
    scene.add_system(std::make_unique<VehicleSystem>(VehicleSystem::Stage::PrePhysics),
                     k_vehicle_pre_physics_order);
    scene.add_system(std::make_unique<VehicleSystem>(VehicleSystem::Stage::PostPhysics),
                     k_vehicle_post_physics_order);
}

} // namespace system
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_SYSTEM_VEHICLE_SYSTEM_H
