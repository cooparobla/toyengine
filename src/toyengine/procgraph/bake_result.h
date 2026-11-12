/**
 * @file bake_result.h
 * @brief `BakeResult` -- the object subtree a graph produces.
 *
 * A graph's terminal output is **not a mesh, it is a scene subtree**: objects with components and
 * children. Geometry is the most important thing a graph can emit but not the only one, so this
 * is general from the start rather than widened later.
 *
 * ### Why components are `{type, fkyaml::node}`
 *
 * `coopa::scene::SceneLoader::build_object()` builds an object and its subtree from an
 * already-resolved YAML node, dispatching each component through the process-wide
 * `register_component_parser` table. So expressing a component as its registered type name plus
 * the keys a scene file would have written means a graph can emit a `Light`, a `Rigidbody`, an
 * `AudioSource`, a `ParticleSystem` or a game project's own `TOY_MODULE()` component **with no
 * new plumbing per component type**. Nothing in the bake knows what a `Light` is.
 *
 * The payloads YAML cannot carry -- a built mesh, a collision shape, an instance set -- ride
 * alongside as pointers and are attached by `proc_publish` after `build_object()` returns.
 *
 * Pure data: no Vulkan, no `Scene`, no `AssetManager`, so a bake is unit-testable without a device.
 */

#ifndef TOYENGINE_PROCGRAPH_BAKE_RESULT_H
#define TOYENGINE_PROCGRAPH_BAKE_RESULT_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <toyengine/procgraph/geometry_set.h>

namespace toy {
namespace procgraph {

/**
 * @struct BakedComponent
 * @brief One component to attach: its registered type name, and the keys a scene file would write.
 */
struct BakedComponent {
    std::string  type;   ///< Any name in the SceneLoader parser table: "Light", "Rigidbody", ...
    fkyaml::node props;  ///< A mapping. Exactly what the scene YAML would have carried.

    BakedComponent() : props(fkyaml::node::mapping()) {}
    BakedComponent(std::string t, fkyaml::node p)
        : type(std::move(t)), props(p.is_mapping() ? std::move(p) : fkyaml::node::mapping()) {}
};

/**
 * @struct BakedObject
 * @brief One object a graph emits, and its subtree.
 *
 * The transform is kept as explicit fields rather than folded into `components` so
 * `proc_publish` can write it straight onto the spawned object's `TransformComponent`. Round
 * tripping a quaternion through YAML Euler degrees would lose precision for no benefit.
 */
struct BakedObject {
    std::string                 name;
    glm::vec3                   position{0.0f};
    glm::quat                   rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3                   scale{1.0f};
    std::vector<BakedComponent> components;
    std::vector<BakedObject>    children;

    /// @brief Geometry for this object. `proc_publish` uploads it and attaches a MeshRenderer.
    std::shared_ptr<const MeshGeometry> mesh;
    /// @brief Instance groups. `proc_publish` turns these into an InstancePainter + backing set.
    std::shared_ptr<const std::vector<InstanceGroup>> instances;
    /// @brief When set, `mesh` also becomes a collider. Convex uses a hull instead of a trimesh.
    enum class Collider : uint8_t { None, Trimesh, Convex };
    Collider collider = Collider::None;

    /// @brief A child, by name, or nullptr.
    BakedObject* find_child(const std::string& child_name) {
        for (auto& c : children) {
            if (c.name == child_name) return &c;
        }
        return nullptr;
    }

    /// @brief This object plus every descendant.
    size_t total_objects() const {
        size_t n = 1;
        for (const auto& c : children) n += c.total_objects();
        return n;
    }

    /// @brief Whether anything here or below carries geometry, instances or a component.
    bool carries_anything() const {
        if (mesh || instances || !components.empty()) return true;
        for (const auto& c : children) {
            if (c.carries_anything()) return true;
        }
        return false;
    }
};

/**
 * @struct BakeResult
 * @brief What one bake produced.
 */
struct BakeResult {
    BakedObject              root;
    std::vector<std::string> errors;    ///< Already formatted, node id included.
    std::vector<std::string> warnings;

    /**
     * @brief Hash of every resolved input, the seed and the compiled graph's revision.
     *
     * The system republishes only when this moves, which is what makes `bake: on_change`
     * affordable: an unrelated object moving in the scene must not rebuild a 200k-vertex mesh.
     */
    uint64_t input_hash = 0;

    bool ok() const { return errors.empty(); }

    /// @brief Every message on one line each, for a log or an inspector panel.
    std::string messages() const {
        std::string out;
        for (const auto& e : errors) out += "[error] " + e + "\n";
        for (const auto& w : warnings) out += "[warn]  " + w + "\n";
        return out;
    }
};

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_BAKE_RESULT_H
