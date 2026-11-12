/**
 * @file component.h
 * @brief Abstract base class for all SceneObject components.
 *
 * Components encapsulate behavior attached to SceneObjects, analogous to Unity's
 * MonoBehaviour. Each component receives start() on scene load, update() every
 * frame, and late_update() every frame after every component's update() has run.
 */

#ifndef COOPA_SCENE_COMPONENT_H
#define COOPA_SCENE_COMPONENT_H

#include <string>

namespace coopa {
namespace scene {

// Forward declarations so components can access their owner and scene.
class SceneObject;
class Scene;

/**
 * @class Component
 * @brief Abstract base class for all components attached to a SceneObject.
 *
 * Subclass and override start()/update()/late_update() to add behavior.
 * The owner pointer is set automatically by SceneObject::add_component();
 * the scene pointer is set by Scene::start() (or Scene::adopt()).
 */
class Component {
public:
    virtual ~Component() = default;

    /**
     * @brief Called once when the scene is loaded, after all objects are constructed.
     */
    virtual void start() {}

    /**
     * @brief Called every frame.
     * @param delta_time Time in seconds since the last frame.
     */
    virtual void update(float delta_time) { (void)delta_time; }

    /**
     * @brief Called every frame, after every component's update() has already
     * run this frame across the whole scene — mirrors Unity's LateUpdate.
     *
     * Use this for behavior that must observe this frame's already-updated
     * state before doing its own work (e.g. UI layout reading a color a
     * sibling's update() just changed this same frame). Ordinary per-frame
     * behavior belongs in update() instead.
     *
     * @param delta_time Time in seconds since the last frame.
     */
    virtual void late_update(float delta_time) { (void)delta_time; }

    /**
     * @brief Returns the type name string for this component.
     * @return Human-readable type name (e.g. "Transform", "MeshRenderer").
     */
    virtual std::string type_name() const = 0;

    SceneObject* owner = nullptr; /**< Non-owning pointer to the owning SceneObject. Set by SceneObject. */

    /**
     * Non-owning pointer to the owning Scene, or nullptr until the scene has
     * been started (or this subtree adopted). Lets a component reach
     * scene->events() to emit/listen on the named EventBus without every
     * call site threading a Scene& through by hand.
     */
    Scene* scene = nullptr;
};

/**
 * @brief Strips a leading '!' so a YAML tag-form name ("!MeshRenderer") and a
 *        type-form name ("MeshRenderer") compare equal.
 *
 * Shared by SceneLoader and SceneInheritance, which must agree exactly on how
 * a component node is named for inheritance overrides to match the components
 * they override.
 *
 * @param tag The raw tag or type name.
 * @return The name without a leading '!'.
 */
inline std::string normalize_component_tag(const std::string& tag) {
    return (!tag.empty() && tag[0] == '!') ? tag.substr(1) : tag;
}

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_COMPONENT_H
