/**
 * @file destruction_system.h
 * @brief Runs destruction: fractures a Destructible's mesh on a worker, and turns the object
 *        into physical chunks when it breaks.
 *
 * The policy half of destruction, split from the Destructible component the way TerrainSystem
 * is split from TerrainComponent, and for the same reasons: it needs the device, the allocator,
 * the asset manager, the job engine and the physics world, none of which a scene-authored
 * component should know about, and its work is per frame over every Destructible rather than
 * per object.
 *
 * What it does each pass, at UpdatePhase order **70** -- after terrain streaming (60) and
 * before physics (100), so a chunk spawned this frame is simulated this frame rather than
 * hanging in the air for one:
 *
 *  1. **Fracture.** A Destructible with `prefracture` gets its mesh cut into cells on the job
 *     engine as soon as its mesh asset has loaded. The cells are kept on this system (not on
 *     the component), because they are geometry, and several objects sharing one mesh and one
 *     seed share one fracture -- a wall of fifty identical crates is cut once.
 *  2. **Break.** A Destructible whose `pending()` is set (a hard enough hit, or shatter())
 *     has its chunks spawned: one object each, with a MeshRenderer over the cell's own mesh, a
 *     BoxCollider of its bounds and a Rigidbody weighed from its volume. The original object
 *     is deactivated, not deleted, so whatever referenced it still resolves.
 *  3. **Clean up.** Chunks past their lifetime sink and are removed, so a scene that is broken
 *     over and over does not accumulate bodies for ever.
 *
 * `DESTRUCT_DEBUG=1` in the environment prints one line per break (what broke, into how many
 * chunks) -- the first thing to look at when something refuses to come apart.
 *
 * **Chunk colliders are boxes -- oriented ones.** physxcoopa has no convex-hull shape
 * (collision/shape.h), and a triangle-mesh collider cannot be dynamic. Each cell is therefore
 * boxed in its OWN best-fit frame rather than along the world axes, and a shard that fills too
 * little of that box (Destructible::collider_fill) is cut into up to `max_boxes` parts, spawned
 * as child objects whose colliders PhysicsSystem gathers into one compound body. A wedge then
 * rests on its face instead of hanging off a corner of a box that is mostly air. It is still
 * not the shape to build a puzzle on.
 *
 * A child collider's Transform must be parented to the chunk's by hand (SceneObject::add_child
 * links ownership only): a compound's shapes are placed from the child's world transform
 * relative to the root's, so an unparented child puts its shape a whole chunk position away
 * from the body, and the chunk falls through the floor it should be resting on.
 */

#ifndef TOYENGINE_SCENE_DESTRUCTION_SYSTEM_H
#define TOYENGINE_SCENE_DESTRUCTION_SYSTEM_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/job/engine.h>
#include <coopa/scene/scene.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/destruct/fracture.h>

namespace coopa::asset { class AssetManager; }
namespace coopa::gfx::core { class Device; }
namespace coopa::gfx::memory { class Allocator; }

namespace toy {
namespace scene {

class Destructible;

/**
 * @class DestructionSystem
 * @brief Fractures breakable meshes and spawns their chunks (see the file doc).
 */
class DestructionSystem : public coopa::scene::ISceneSystem {
public:
    DestructionSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                      coopa::asset::AssetManager& assets, coopa::job::JobEngine* jobs)
        : device_(device), allocator_(allocator), assets_(assets), jobs_(jobs) {}

    ~DestructionSystem() override;

    const char* system_name() const override { return "Destruction"; }
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /** @brief Fractures done so far -- one per distinct mesh + settings (tests, diagnostics). */
    std::size_t fractures() const { return cache_.size(); }
    /** @brief Chunk objects alive right now. */
    std::size_t live_chunks() const { return chunks_.size(); }

private:
    /// One mesh broken one way: the cells, and the job cutting them if it is still running.
    struct Fracture {
        std::shared_ptr<std::vector<destruct::FracturePiece>> pieces;
        coopa::job::JobHandle job;
        bool ready = false;
    };

    /// A chunk in flight: what to remove, and when.
    struct Chunk {
        coopa::scene::SceneObject* object = nullptr;
        float age = 0.0f;
        float lifetime = 0.0f;
        float fade = 0.0f;
    };

    /// Keyed on the mesh and the settings that shape the break, so identical crates share one.
    static std::string cache_key_(const std::string& mesh_path, const Destructible& d);

    /** @brief Starts (or advances) the fracture a Destructible needs; true once its cells exist. */
    bool ensure_fracture_(Destructible& d, const std::string& mesh_path, Fracture*& out);

    /** @brief Spawns one rigid chunk per cell and deactivates the original. */
    int spawn_chunks_(coopa::scene::Scene& scene, Destructible& d, const Fracture& fracture);

    /** @brief Ages chunks out: sink, then remove. */
    void age_chunks_(coopa::scene::Scene& scene, float dt);

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;
    coopa::job::JobEngine*         jobs_ = nullptr;

    std::unordered_map<std::string, Fracture> cache_;
    std::vector<Chunk> chunks_;
    std::uint64_t      spawned_ = 0;   ///< Names chunk objects and their meshes uniquely.
    bool               physics_dirty_ = false;
};

/**
 * @brief Installs a DestructionSystem on `scene` (idempotent); returns it.
 *
 * @param jobs Job engine for the fracture work; null fractures inline on the owner thread,
 *             which is what the device-free tests do.
 */
DestructionSystem* install_destruction_system(coopa::scene::Scene& scene,
                                              coopa::gfx::core::Device& device,
                                              coopa::gfx::memory::Allocator& allocator,
                                              coopa::asset::AssetManager& assets,
                                              coopa::job::JobEngine* jobs);

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_DESTRUCTION_SYSTEM_H
