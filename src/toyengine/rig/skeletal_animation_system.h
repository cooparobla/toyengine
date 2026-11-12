/**
 * @file skeletal_animation_system.h
 * @brief The Animation-phase ISceneSystem for SkeletalAnimators.
 */

#ifndef TOYENGINE_RIG_SKELETAL_ANIMATION_SYSTEM_H
#define TOYENGINE_RIG_SKELETAL_ANIMATION_SYSTEM_H

#include <toyengine/rig/skeletal_animator.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <memory>

namespace toy {
namespace rig {

/**
 * @class SkeletalAnimationSystem
 * @brief Registered at coopa::scene::UpdatePhase::Animation (300) — the sole driver of every
 *        SkeletalAnimator, via the friend-only advance_()/apply_() pair.
 *
 * Runs at the same numeric order as toy::anim::AnimationSystem: a scene's property
 * Animators and skeletal SkeletalAnimators are two independent systems at the same phase,
 * each driving its own component type, with no shared state between them.
 */
class SkeletalAnimationSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "SkeletalAnimation"; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        for (SkeletalAnimator* a : scene.get_components<SkeletalAnimator>()) {
            a->advance_(ctx.delta_time);
            a->apply_(ctx.delta_time);
        }
    }
};

/**
 * @brief Constructs a SkeletalAnimationSystem and registers it at UpdatePhase::Animation.
 * @return Non-owning pointer to the installed system.
 */
inline SkeletalAnimationSystem* install_skeletal_animation_system(coopa::scene::Scene& scene) {
    auto sys = std::make_unique<SkeletalAnimationSystem>();
    SkeletalAnimationSystem* raw = sys.get();
    scene.add_system(std::move(sys), coopa::scene::UpdatePhase::Animation);
    return raw;
}

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_SKELETAL_ANIMATION_SYSTEM_H
