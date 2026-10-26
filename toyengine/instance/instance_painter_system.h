/**
 * @file instance_painter_system.h
 * @brief `InstancePainterSystem` (order 62) -- builds every `InstancePainter`'s chunks, hands the
 *        renderer their instanced batches, and streams colliders for the prototypes that ask.
 *
 * Order **62** is after the two terrain systems (60 Tiles, 61 Heightmap), so a chunk is built
 * against finished ground, and before `Physics` (**100**), so a collider created this frame is
 * picked up by the physics reconcile pass in the same frame rather than the next one. It
 * `runs_in_edit_mode()`, which is what makes the editor's Instance Paint mode usable at all:
 * instances appear, cull, light and collide as they are painted.
 *
 * ### What it owns
 *
 * | | |
 * |---|---|
 * | prototype resolution | once per prototype, lazily -- see `resolve_prototype()` |
 * | the chunk cache | `InstanceRenderCache`, streamed around the camera |
 * | render batches | appended to the frame's mesh batches by `collect_render()` |
 * | colliders | one runtime child object per collidable instance within `collision_radius` |
 *
 * Nothing here is saved: the records are the authored data (in their `.instances` file) and
 * everything in this file is derived from them.
 */

#ifndef TOYENGINE_INSTANCE_INSTANCE_PAINTER_SYSTEM_H
#define TOYENGINE_INSTANCE_INSTANCE_PAINTER_SYSTEM_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/instance/instance_cache.h>
#include <toyengine/instance/instance_painter.h>
#include <toyengine/render/particle_types.h>

namespace toy {
namespace instance {

/** @brief Runs after the terrain systems and before physics -- see the file doc. */
inline constexpr int k_instance_painter_order = 62;

/**
 * @class InstancePainterSystem
 * @brief See the file comment.
 */
class InstancePainterSystem : public coopa::scene::ISceneSystem {
public:
    explicit InstancePainterSystem(coopa::asset::AssetManager* assets) : assets_(assets) {}

    const char* system_name() const override { return "InstancePainter"; }

    /** @brief Instances must appear as they are painted, so this runs while editing too. */
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /**
     * @brief Appends this frame's visible chunk batches to `out`.
     *
     * Takes the scene rather than a list remembered from `execute()`, because the editor
     * rebuilds an edited object in its `post_late_update` hook -- which runs BETWEEN the scene
     * update and this call. A painter pointer cached at order 62 is freed memory by the time
     * the renderer asks for its batches, so the live components are looked up again here.
     *
     * The matrices the batches point at belong to the chunk cache, which only `execute()`
     * rewrites, so they stay valid until the next frame's update.
     *
     * @param camera Where thinning and culling are measured from.
     * @param cull   Returns whether a world-space AABB is worth drawing (the frustum test).
     */
    void collect_render(coopa::scene::Scene& scene, const glm::vec3& camera,
                        const std::function<bool(const glm::vec3&, const glm::vec3&)>& cull,
                        render::ParticleFrameState& out);

    /** @brief Total instances in built chunks across every painter -- for the debug overlay. */
    std::size_t instance_count() const;

private:
    /**
     * @struct Painted
     * @brief One painter's derived state, keyed by its **instance set's uid**.
     *
     * Not by the component pointer: the editor destroys and recreates an `InstancePainter` on
     * every inspector keystroke, and a new one is routinely allocated at the dead one's
     * address. Keying on the component would hand the rebuilt painter the old painter's cache
     * one keystroke and throw a good cache away the next. The set outlives every rebuild (it
     * lives in the `InstanceSetStore`), so its uid is the identity that actually means "this
     * painter" -- and keying on it means a keystroke no longer costs a chunk rebuild or a
     * round of collider churn either.
     */
    struct Painted {
        InstanceRenderCache cache;
        /** @brief The collider parent per chunk: a runtime object holding one child per instance. */
        std::map<std::pair<int, int>, coopa::scene::SceneObject*> collision;
        /** @brief Which data revision each collider chunk was built from. */
        std::map<std::pair<int, int>, std::uint64_t> collision_revision;
        /**
         * @brief The object those collider children hang off.
         *
         * Compared, never dereferenced: when the editor rebuilds the painter's object the old
         * one is gone and took its runtime children with it, so the bookkeeping is dropped
         * rather than used to detach children of an object that no longer exists.
         */
        const coopa::scene::SceneObject* collision_owner = nullptr;
        std::uint64_t key = 0;   ///< Stable hash for this painter's static batch keys.
        bool bounds_ready = false;
    };

    /** @brief The derived state for `painter`, keyed by its set's uid; null without a set. */
    Painted* painted_for_(InstancePainter& painter);

    void update_painter_(coopa::scene::Scene& scene, InstancePainter& painter, Painted& p, const glm::vec3& camera);
    void update_collision_(coopa::scene::Scene& scene, InstancePainter& painter, Painted& p, const glm::vec3& camera);
    void drop_collision_(InstancePainter& painter, Painted& p);
    /** @brief Builds the collider children for one chunk; null when it needs none. */
    coopa::scene::SceneObject* build_collision_chunk_(InstancePainter& painter, ChunkCoord coord);
    void log_once_(const std::string& line);

    coopa::asset::AssetManager* assets_ = nullptr;
    /// Keyed by InstanceSetData::uid() -- see Painted.
    std::unordered_map<std::uint64_t, Painted> painted_;
    bool physics_dirty_ = false;
};

/**
 * @brief Installs the system, or returns the one already there.
 * @param assets Resolves prototype physics materials and collider meshes; may be null, which
 *        turns `collision: mesh` and `physics_material:` into warnings.
 */
InstancePainterSystem* install_instance_painter_system(coopa::scene::Scene& scene,
                                                       coopa::asset::AssetManager* assets,
                                                       int order = k_instance_painter_order);

/** @brief The scene's painter system, or null. */
InstancePainterSystem* find_instance_painter_system(coopa::scene::Scene& scene);

} // namespace instance
} // namespace toy

#endif // TOYENGINE_INSTANCE_INSTANCE_PAINTER_SYSTEM_H
