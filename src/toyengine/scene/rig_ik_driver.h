/**
 * @file rig_ik_driver.h
 * @brief A component that steers rig-space IK each frame (FootIK, Rider) -- the toyengine-side
 *        counterpart of toy::anim::IkDriver, for drivers that query physics or a
 *        CharacterController and so cannot live in libcoopa's toy::rig module (see
 *        toyengine/rig/rig_pose_math.h's own doc on why that math is free functions, not a base
 *        class a driver could inherit alongside IkComponent).
 *
 * toy::rig::IkSystem (order 320) has no driver concept of its own -- it is purely a sorted
 * list of TwoBoneIK/ChainIK/LookAtIK solvers. RigIkDriverSystem brackets it instead: installed
 * at 319 and 321, each run gathers every RigIkDriver in the scene fresh, sorts by `priority`
 * (ties: scene pre-order, via a stable sort), and calls ik_pre_solve()/ik_post_solve() in that
 * order -- so a FootIK (priority 0) adjusts a rig's own TwoBoneIK targets before 320 solves
 * them, and a Rider (priority 100) steers on top of that, then both tilt/finish after 320 runs.
 */

#ifndef TOYENGINE_SCENE_RIG_IK_DRIVER_H
#define TOYENGINE_SCENE_RIG_IK_DRIVER_H

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace toy {
namespace scene {

/** @brief Order: lower first, ties broken by scene pre-order (a stable sort). Mirrors
 *         toy::anim::IkDriver: both hooks default to no-op, so a driver overrides only the
 *         one(s) it needs (FootIK uses both; Rider only ik_pre_solve). */
class RigIkDriver {
public:
    int priority = 0;
    virtual void ik_pre_solve(float /*dt*/) {}
    virtual void ik_post_solve(float /*dt*/) {}
    virtual ~RigIkDriver() = default;
};

/** @brief Runs every RigIkDriver's pre- or post-solve hook, bracketing toy::rig::IkSystem
 *         (order 320) -- install at 319 (Pre) and 321 (Post). See the file doc. */
class RigIkDriverSystem : public coopa::scene::ISceneSystem {
public:
    enum class Mode { Pre, Post };
    explicit RigIkDriverSystem(Mode mode) : mode_(mode) {}

    const char* system_name() const override { return mode_ == Mode::Pre ? "RigIkDriverPre" : "RigIkDriverPost"; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        std::vector<RigIkDriver*> drivers;
        for (const auto& root : scene.root_objects()) {
            root->for_each_recursive([&drivers](const coopa::scene::SceneObject& obj) {
                if (!obj.active()) return;
                for (const auto& comp : obj.components()) {
                    if (auto* d = dynamic_cast<RigIkDriver*>(comp.get())) drivers.push_back(d);
                }
            });
        }
        std::stable_sort(drivers.begin(), drivers.end(),
                         [](const RigIkDriver* a, const RigIkDriver* b) { return a->priority < b->priority; });
        const float dt = ctx.delta_time;
        if (mode_ == Mode::Pre) {
            for (RigIkDriver* d : drivers) d->ik_pre_solve(dt);
        } else {
            for (RigIkDriver* d : drivers) d->ik_post_solve(dt);
        }
    }

private:
    Mode mode_;
};

constexpr int k_rig_ik_driver_pre_order = 319;
constexpr int k_rig_ik_driver_post_order = 321;

/** @brief Installs both halves of the bracket (see the file doc). Call once at startup. */
inline void install_rig_ik_driver_system(coopa::scene::Scene& scene) {
    scene.add_system(std::make_unique<RigIkDriverSystem>(RigIkDriverSystem::Mode::Pre), k_rig_ik_driver_pre_order);
    scene.add_system(std::make_unique<RigIkDriverSystem>(RigIkDriverSystem::Mode::Post), k_rig_ik_driver_post_order);
}

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_RIG_IK_DRIVER_H
