/**
 * @file transform_system.h
 * @brief The TransformResolve-phase ISceneSystem: a top-down world-matrix
 *        resolve pass over the whole scene hierarchy.
 */

#ifndef COOPA_SCENE_SYSTEMS_TRANSFORM_SYSTEM_H
#define COOPA_SCENE_SYSTEMS_TRANSFORM_SYSTEM_H

#include <coopa/job/parallel_for.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <coopa/util/transform.h>

#include <cstddef>
#include <memory>

namespace coopa {
namespace scene {

/**
 * @class TransformSystem
 * @brief Registered at UpdatePhase::TransformResolve; recomputes every dirty
 *        Transform's world matrix, top-down, once per frame.
 *
 * Closes a real gap that exists without it: coopa::anim::AnimationSystem's
 * apply_() step (phase Animation = 300) writes directly onto animated
 * properties -- including, commonly, a TransformComponent's position/
 * rotation/scale -- which marks the affected Transform (and its descendants)
 * dirty via Transform::mark_dirty(), but does not recompute it. Without a
 * resolve pass running AFTER Animation and BEFORE LateBehaviour/the
 * renderer, that dirty flag would not be noticed until BehaviourSystem's
 * *next* frame walk (each TransformComponent::update() only recomputes
 * itself when reached during that phase-200 pass) -- meaning this frame's
 * late_update() and render would read a stale world matrix for anything
 * animated this frame.
 *
 * Also the only place in libcoopa that resolves world matrices in a way
 * that is safe to run concurrently across multiple Scenes, and (within one
 * Scene) fans out across independent root subtrees -- see Transform's
 * thread-safety doc for why get_world_matrix()/recompute() are NOT
 * themselves safe to call concurrently on transforms that might share a
 * parent chain, and why Transform::world_matrix() (a pure read) exists for
 * readers -- e.g. a render-list build --
 * that run after this system every frame.
 *
 * Not auto-installed by Scene (unlike BehaviourSystem/LateBehaviourSystem):
 * install it explicitly via install_transform_system() alongside
 * coopa::anim::install_animation_system(), same pattern.
 */
class TransformSystem : public ISceneSystem {
public:
    const char* system_name() const override { return "TransformResolve"; }
    /// Keeps world matrices current for a non-simulating (editor) scene too.
    bool runs_in_edit_mode() const override { return true; }

    void execute(Scene& scene, const FrameContext& ctx) override {
        auto& roots = scene.root_objects();
        if (roots.empty()) return;

        if (ctx.jobs && roots.size() > 1) {
            // Grain of 1: fan out one job per root. A root's own subtree may
            // be arbitrarily large, so batching multiple roots per chunk
            // would only shrink the parallelism this system exists to offer.
            ctx.jobs->parallel_for_blocking(roots.size(), 1,
                [&roots](size_t start, size_t end) {
                    for (size_t i = start; i < end; ++i) {
                        resolve_subtree(*roots[i]);
                    }
                },
                0, coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        } else {
            for (auto& root : roots) resolve_subtree(*root);
        }
    }

    /**
     * @brief Recomputes `obj`'s Transform if dirty, then always recurses into
     *        every child regardless -- mark_dirty() only ever propagates
     *        downward from whichever node actually changed, so a clean
     *        parent can still have an independently dirty child (e.g. a
     *        child animated directly, with its parent untouched this frame).
     *        Pruning on this node's own dirty flag would silently skip that
     *        child.
     */
    static void resolve_subtree(SceneObject& obj) {
        if (TransformComponent* tc = obj.get_transform()) {
            coopa::util::Transform& t = tc->transform();
            if (t.is_dirty()) t.recompute();
        }
        for (auto& child : obj.children()) {
            resolve_subtree(*child);
        }
    }
};

/**
 * @brief Resolves every dirty Transform in `scene` right now, on the calling thread: the
 *        TransformResolve pass, outside the update cycle.
 *
 * For a caller that builds or restructures a scene between an update and a render -- loading a
 * scene, pushing one (the editor's Play), spawning or rebuilding an object from a UI callback --
 * and so leaves world matrices dirty with no resolve pass left to run before the readers that
 * take the cached Transform::world_matrix() (skinning, the render gather). Idempotent and cheap
 * on an already-resolved scene: one tree walk, recomputing only what is dirty.
 */
inline void resolve_transforms(Scene& scene) {
    for (auto& root : scene.root_objects()) TransformSystem::resolve_subtree(*root);
}

/**
 * @brief Constructs a TransformSystem and registers it at UpdatePhase::TransformResolve.
 * @return Non-owning pointer to the installed system.
 */
inline TransformSystem* install_transform_system(Scene& scene) {
    auto sys = std::make_unique<TransformSystem>();
    TransformSystem* raw = sys.get();
    scene.add_system(std::move(sys), UpdatePhase::TransformResolve);
    return raw;
}

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SYSTEMS_TRANSFORM_SYSTEM_H
