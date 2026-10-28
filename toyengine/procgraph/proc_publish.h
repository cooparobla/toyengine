/**
 * @file proc_publish.h
 * @brief Turning a `BakeResult` into real scene objects. The only engine-touching half.
 *
 * `proc_bake` is pure CPU and knows nothing about Vulkan, `Scene` or `AssetManager`; everything
 * that does live here. Main thread only -- component parsers are not thread-safe and the scene is
 * single-owner.
 *
 * ### Three steps, and the first one does most of the work
 *
 * 1. The baked tree is written as an `fkyaml::node` (`name` / `components` / `children`) and
 *    handed to `coopa::scene::SceneLoader::build_object()`, which dispatches every component
 *    through the process-wide `register_component_parser` table. **That one call is why a graph
 *    can emit any component type in the engine with no per-type code anywhere in this module** --
 *    a `Light`, a `Rigidbody`, an `AudioSource`, or a game project's own `TOY_MODULE()` component.
 * 2. The two trees are walked in lockstep and the payloads YAML cannot carry are attached: a built
 *    mesh is uploaded and published under a **stable** synthetic asset id, a collider shape is
 *    handed to `MeshCollider::set_mesh()`, and instance transforms go into an `InstanceSetData`.
 * 3. Every spawned object is marked with `toy::scene::RuntimeObject`, so the editor groups them
 *    under "Runtime" and never writes them into the scene file.
 *
 * ### Why the asset id must be stable
 *
 * A re-bake republishes into the same synthetic id, so `AssetManager::create()` retires the
 * previous payload with its in-flight grace period rather than leaking a slot per bake. Deriving
 * the id from the graph and the object's path through the tree (rather than from a counter) is
 * what makes that work -- the `water_system.cpp` pattern.
 */

#ifndef TOYENGINE_PROCGRAPH_PROC_PUBLISH_H
#define TOYENGINE_PROCGRAPH_PROC_PUBLISH_H

#include <string>
#include <vector>

#include <toyengine/procgraph/bake_result.h>

namespace coopa {
namespace asset {
class AssetManager;
}
namespace gfx {
namespace core {
class Device;
}
namespace memory {
class Allocator;
}
}
namespace scene {
class Scene;
class SceneObject;
}
}

namespace toy {
namespace procgraph {

/**
 * @struct PublishContext
 * @brief What publishing needs. Main thread; every pointer must be live.
 */
struct PublishContext {
    coopa::scene::Scene*            scene = nullptr;
    /// @brief The object carrying the component; the baked tree is parented under it.
    coopa::scene::SceneObject*      owner = nullptr;
    coopa::asset::AssetManager*     assets = nullptr;
    coopa::gfx::core::Device*       device = nullptr;
    coopa::gfx::memory::Allocator*  allocator = nullptr;
    /// @brief Part of every synthetic asset id, so two graphs cannot collide.
    std::string                     id_prefix;
    /// @brief For relative asset paths inside emitted components, as a scene file's own path is.
    std::string                     scene_path;
};

/**
 * @struct PublishedGraph
 * @brief What one publish produced, so a re-bake can retire it.
 */
struct PublishedGraph {
    /// @brief The spawned root, owned by the scene. Null when nothing was published.
    coopa::scene::SceneObject* root = nullptr;
    /// @brief Every synthetic asset id published, for unloading on teardown.
    std::vector<std::string>   asset_ids;
    /// @brief Every InstanceSetStore key claimed, for the same reason.
    std::vector<std::string>   instance_keys;
    std::vector<std::string>   errors;

    bool ok() const { return errors.empty(); }
};

/**
 * @brief Spawns a baked tree into the scene, replacing whatever `previous` published.
 *
 * @param result   The bake to realize.
 * @param ctx      Scene, assets and device access.
 * @param previous The prior publish for this component, destroyed first. May be empty.
 * @return What was published, to be handed back as `previous` on the next re-bake.
 */
PublishedGraph publish(const BakeResult& result, const PublishContext& ctx,
                       const PublishedGraph& previous);

/// @brief Destroys a published tree and releases its synthetic assets and instance sets.
void unpublish(const PublishedGraph& published, const PublishContext& ctx);

/// @brief The baked tree as an object node, for a test that wants to inspect it without a device.
fkyaml::node object_to_node(const BakedObject& obj);

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_PROC_PUBLISH_H
