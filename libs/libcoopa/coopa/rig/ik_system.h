/**
 * @file ik_system.h
 * @brief IkSystem: solves every rig-space ChainIK / TwoBoneIK / LookAtIK each frame, between
 *        the Animation phase (300, where SkeletalAnimator writes the animated pose) and
 *        RigPoseSystem/BoneAttachment/TransformResolve.
 *
 * Order within a frame: every solver, by `priority` (lower first); equal priorities run
 * every ChainIK, then every TwoBoneIK, then every LookAtIK, each in scene pre-order -- a
 * spine settles before the arms hanging from it, and a head turn rides on both. Mirrors
 * coopa::anim::IkSystem's ordering exactly; there is no driver pass here (toyengine's FootIK
 * stays an coopa::anim::IkDriver, since its pre/post-solve split is about ground probes, not
 * about this module's bone addressing -- see toyengine/scene/foot_ik.h).
 */

#ifndef COOPA_RIG_IK_SYSTEM_H
#define COOPA_RIG_IK_SYSTEM_H

#include <coopa/rig/ik_components.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace coopa {
namespace rig {

/** @brief IkSystem's update order: between the Animation phase (300) and RigPoseSystem (340). */
constexpr int k_ik_update_order = 320;

/**
 * @class IkSystem
 * @brief Runs every rig-space IK solver once per simulated frame (see file doc).
 */
class IkSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "RigIK"; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        _solvers.clear();
        for (const auto& root : scene.root_objects()) {
            root->for_each_recursive([this](const coopa::scene::SceneObject& obj) {
                if (!obj.active()) return;
                for (const auto& comp : obj.components()) {
                    if (auto* ik = dynamic_cast<IkComponent*>(comp.get())) _solvers.push_back(ik);
                }
            });
        }
        std::stable_sort(_solvers.begin(), _solvers.end(), [](const IkComponent* a, const IkComponent* b) {
            if (a->priority != b->priority) return a->priority < b->priority;
            return static_cast<int>(a->ik_kind()) < static_cast<int>(b->ik_kind());
        });
        const float dt = ctx.delta_time;
        for (IkComponent* s : _solvers) s->solve(dt);
    }

    /** @brief This frame's solvers in solve order. */
    const std::vector<IkComponent*>& solve_order() const { return _solvers; }

private:
    std::vector<IkComponent*> _solvers;
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

} // namespace rig
} // namespace coopa

#endif // COOPA_RIG_IK_SYSTEM_H
