/**
 * @file ik_system.h
 * @brief IkSystem: solves every ChainIK / TwoBoneIK / LookAtIK in the scene each frame, between the
 *        Animation phase (300, which writes the animated pose) and TransformResolve (350, which
 *        resolves world matrices for rendering).
 *
 * Order within a frame:
 *   1. IkDriver::ik_pre_solve() on every driver (e.g. FootIK: ground probes, pelvis offset,
 *      leg targets), by `priority` (lower first; scene pre-order among equals) -- a driver that
 *      reads a rig another driver poses (a rider seated on a horse whose FootIK pitches its body)
 *      takes a higher priority,
 *   2. every solver, by `priority` (lower first); equal priorities run every ChainIK, then every
 *      TwoBoneIK, then every LookAtIK, each in scene pre-order -- a spine settles before the arms
 *      hanging from it, and a head turn rides on both,
 *   3. IkDriver::ik_post_solve() (e.g. FootIK aligning the feet to the ground normal), in the
 *      same order as step 1.
 *
 * Solvers read world positions through TransformComponent::get_world_matrix(), which re-walks
 * any parent chain dirtied by the Animator or an earlier solve -- so each chain sees the pose as
 * it stands at that moment, before TransformResolve has run.
 *
 * The component lists are gathered with one scene walk per frame -- one dynamic_cast per
 * component, to their common IkComponent base -- so spawned rigs are picked up without a refresh
 * call; a scene with no IK pays only that walk (and only while simulating: not in edit mode).
 */

#ifndef COOPA_ANIMATION_IK_SYSTEM_H
#define COOPA_ANIMATION_IK_SYSTEM_H

#include <coopa/animation/ik_components.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace coopa {
namespace anim {

/** @brief IkSystem's update order: between Animation (300) and TransformResolve (350). */
constexpr int k_ik_update_order = 320;

/**
 * @class IkSystem
 * @brief Runs IK drivers and solvers once per simulated frame (see file doc).
 */
class IkSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "IK"; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        drivers_.clear();
        solvers_.clear();
        chain_count_ = two_bone_count_ = look_at_count_ = 0;
        for (const auto& root : scene.root_objects()) {
            root->for_each_recursive([this](const coopa::scene::SceneObject& obj) {
                if (!obj.active()) return;
                for (const auto& comp : obj.components()) {
                    auto* ik = dynamic_cast<IkComponent*>(comp.get());
                    if (!ik) continue;
                    switch (ik->ik_kind()) {
                        case IkComponent::Kind::Driver: drivers_.push_back(static_cast<IkDriver*>(ik)); break;
                        case IkComponent::Kind::Chain: ++chain_count_; solvers_.push_back(ik); break;
                        case IkComponent::Kind::TwoBone: ++two_bone_count_; solvers_.push_back(ik); break;
                        case IkComponent::Kind::LookAt: ++look_at_count_; solvers_.push_back(ik); break;
                    }
                }
            });
        }
        // Stable: scene pre-order among drivers of equal priority...
        std::stable_sort(drivers_.begin(), drivers_.end(), [](const IkDriver* a, const IkDriver* b) {
            return a->priority < b->priority;
        });
        // ...and within a (priority, kind) group of solvers.
        std::stable_sort(solvers_.begin(), solvers_.end(), [](const IkComponent* a, const IkComponent* b) {
            if (a->priority != b->priority) return a->priority < b->priority;
            return static_cast<int>(a->ik_kind()) < static_cast<int>(b->ik_kind());
        });
        const float dt = ctx.delta_time;
        for (IkDriver* d : drivers_) d->ik_pre_solve(dt);
        for (IkComponent* s : solvers_) s->solve(dt);
        for (IkDriver* d : drivers_) d->ik_post_solve(dt);
    }

    size_t chain_count() const { return chain_count_; }
    size_t two_bone_count() const { return two_bone_count_; }
    size_t look_at_count() const { return look_at_count_; }
    size_t driver_count() const { return drivers_.size(); }
    /** @brief This frame's drivers in pre-solve order. */
    const std::vector<IkDriver*>& driver_order() const { return drivers_; }
    /** @brief This frame's solvers in solve order. */
    const std::vector<IkComponent*>& solve_order() const { return solvers_; }

private:
    std::vector<IkDriver*>    drivers_;
    std::vector<IkComponent*> solvers_;
    size_t chain_count_ = 0, two_bone_count_ = 0, look_at_count_ = 0;
};

/**
 * @brief Constructs an IkSystem and registers it at k_ik_update_order (320).
 * @return Non-owning pointer to the installed system.
 */
inline IkSystem* install_ik_system(coopa::scene::Scene& scene) {
    auto sys = std::make_unique<IkSystem>();
    IkSystem* raw = sys.get();
    scene.add_system(std::move(sys), k_ik_update_order);
    return raw;
}

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_IK_SYSTEM_H
