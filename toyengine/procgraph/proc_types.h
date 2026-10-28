/**
 * @file proc_types.h
 * @brief This domain's socket types, registered with the nodegraph library's TypeRegistry.
 *
 * The library owns no types of its own, so this is where `ProceduralGraph` says what can travel
 * along a link. `register_proc_types()` is called once at startup and is idempotent; the ids are
 * then reachable from anywhere via `types()`.
 *
 * ### `ObjectRef` and `ObjectSet` are values, not pointers
 *
 * A graph can read the surrounding scene, but the bake runs on a worker thread and `Scene` is
 * main-thread-only. So a scene binding is **snapshotted** on the main thread into one of these
 * plain value types -- a name, a transform, bounds, optionally geometry -- and a node never
 * receives a `SceneObject*`. The absence of a pointer is what makes the thread-safety contract
 * enforceable rather than a convention: a node cannot follow a reference back into the scene
 * because it never has one.
 *
 * ### The conversions
 *
 * Mirrors Blender's implicit widening (`Int -> Float -> Vector`, `Float -> Color`), so authoring
 * does not need an explicit conversion node for the obvious cases. `Vector -> Float` deliberately
 * does *not* exist: there is no single right answer (length? x? mean?) and silently picking one
 * hides mistakes.
 */

#ifndef TOYENGINE_PROCGRAPH_PROC_TYPES_H
#define TOYENGINE_PROCGRAPH_PROC_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/nodegraph/type_registry.h>
#include <coopa/nodegraph/value.h>
#include <toyengine/procgraph/geometry_set.h>

namespace toy {
namespace procgraph {

using coopa::nodegraph::TypeId;
using coopa::nodegraph::Value;

/**
 * @struct ObjectRef
 * @brief A snapshot of one scene object: enough to place geometry relative to it.
 *
 * Captured on the main thread by a scene-input resolver. `geometry` is filled only when the
 * binding asked for it (`{object: X, geometry: true}`), because pulling a mesh off a renderer is
 * far more expensive than reading a transform.
 */
struct ObjectRef {
    std::string name;
    glm::vec3   position{0.0f};
    glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3   scale{1.0f};
    glm::vec3   bounds_min{0.0f};
    glm::vec3   bounds_max{0.0f};
    GeometrySet geometry;              ///< Empty unless the binding requested it.
    bool        valid = false;         ///< False when the named object was not found.

    /// @brief The object's world matrix as captured.
    glm::mat4 matrix() const;

    bool operator==(const ObjectRef& o) const {
        return name == o.name && valid == o.valid && position == o.position &&
               rotation == o.rotation && scale == o.scale;
    }
};

/**
 * @struct ObjectSet
 * @brief Several object snapshots, shared by refcount so a socket copy stays cheap.
 *
 * A `tag` binding over a busy scene can match thousands of objects, and `bake: on_change`
 * re-resolves bindings every frame -- so this must not deep-copy when it crosses a link.
 */
struct ObjectSet {
    std::shared_ptr<const std::vector<ObjectRef>> objects;

    size_t size() const { return objects ? objects->size() : 0; }
    bool   empty() const { return size() == 0; }
    const ObjectRef& at(size_t i) const {
        static const ObjectRef none;
        return objects && i < objects->size() ? (*objects)[i] : none;
    }

    static ObjectSet from(std::vector<ObjectRef> refs) {
        ObjectSet out;
        out.objects = std::make_shared<const std::vector<ObjectRef>>(std::move(refs));
        return out;
    }

    bool operator==(const ObjectSet& o) const { return objects == o.objects; }
};

/**
 * @struct ComponentSpec
 * @brief One component an Add Component node emits, on its way into a BakedObject.
 *
 * Deliberately the same shape as `BakedComponent`: a registered type name plus the keys a scene
 * file would write. That is what lets the Output nodes emit any component type in the engine --
 * including a game project's own -- with no per-type code anywhere in this module.
 */
struct ComponentSpec {
    std::string  type;
    fkyaml::node props;

    ComponentSpec() : props(fkyaml::node::mapping()) {}
    ComponentSpec(std::string t, fkyaml::node p)
        : type(std::move(t)), props(p.is_mapping() ? std::move(p) : fkyaml::node::mapping()) {}

    bool operator==(const ComponentSpec& o) const {
        return type == o.type && fkyaml::node::serialize(props) == fkyaml::node::serialize(o.props);
    }
};

/**
 * @struct ObjectSpec
 * @brief What an Object Output node produces: a named object with geometry and components.
 *
 * Shared by refcount, like the other aggregates, so nesting Object Output nodes to build a tree
 * does not copy the subtree at every link.
 */
struct ObjectSpec {
    struct Data {
        std::string                name;
        glm::vec3                  position{0.0f};
        glm::quat                  rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3                  scale{1.0f};
        GeometrySet                geometry;
        std::vector<ComponentSpec> components;
        std::vector<ObjectSpec>    children;
        uint8_t                    collider = 0;   ///< Matches BakedObject::Collider.
    };
    std::shared_ptr<const Data> data;

    bool empty() const { return !data; }
    const Data& get() const {
        static const Data none;
        return data ? *data : none;
    }
    bool operator==(const ObjectSpec& o) const { return data == o.data; }
};

/**
 * @struct ProcTypeIds
 * @brief Every socket type id this domain registers.
 */
struct ProcTypeIds {
    TypeId float_t = 0;
    TypeId int_t = 0;
    TypeId bool_t = 0;
    TypeId vector_t = 0;
    TypeId color_t = 0;      ///< glm::vec4, so alpha survives.
    TypeId rotation_t = 0;   ///< glm::quat.
    TypeId string_t = 0;
    TypeId geometry_t = 0;
    TypeId object_t = 0;
    TypeId object_set_t = 0;
    TypeId component_t = 0;
    TypeId object_spec_t = 0;
};

/// @brief Registers every socket type and conversion. Idempotent; safe to call more than once.
const ProcTypeIds& register_proc_types();

/// @brief The registered ids. Registers on first use, so a caller never gets zeros.
inline const ProcTypeIds& types() { return register_proc_types(); }

// --- Small helpers, so node bodies read as arithmetic rather than as plumbing ---

inline Value make_float(float v) { return Value::make(types().float_t, v); }
inline Value make_int(int32_t v) { return Value::make(types().int_t, v); }
inline Value make_bool(bool v) { return Value::make(types().bool_t, v); }
inline Value make_vector(const glm::vec3& v) { return Value::make(types().vector_t, v); }
inline Value make_color(const glm::vec4& v) { return Value::make(types().color_t, v); }
inline Value make_rotation(const glm::quat& v) { return Value::make(types().rotation_t, v); }
inline Value make_string(std::string v) { return Value::make(types().string_t, std::move(v)); }
inline Value make_geometry(GeometrySet v) { return Value::make(types().geometry_t, std::move(v)); }
inline Value make_object(ObjectRef v) { return Value::make(types().object_t, std::move(v)); }
inline Value make_object_set(ObjectSet v) { return Value::make(types().object_set_t, std::move(v)); }
inline Value make_component(ComponentSpec v) { return Value::make(types().component_t, std::move(v)); }
inline Value make_object_spec(ObjectSpec v) { return Value::make(types().object_spec_t, std::move(v)); }

/// @brief An empty GeometrySet, for an `as<T>(fallback)` call site.
inline const GeometrySet& empty_geometry() {
    static const GeometrySet none;
    return none;
}

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_PROC_TYPES_H
