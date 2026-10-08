/**
 * @file runtime_object.h
 * @brief Marks scene objects a SYSTEM creates at runtime (weather effects, ...) rather than the
 *        scene file: never saved, never editable, but visible to tools.
 *
 * A system that spawns objects into a scene puts a RuntimeObject component on each root it
 * creates. The marker says who owns it (`owner`, a system name such as "Weather") and why it
 * exists (`note`), and is inherited by everything under that root (is_runtime_object() walks the
 * parents). Nothing here changes how the object updates or renders.
 *
 * The editor reads the marker: marked roots are listed in the Hierarchy under "Runtime", locked
 * (greyed, with a lock icon), selectable for a read-only look at their components, and never
 * written to the scene document. A gameplay system that spawns objects can use it the same way:
 * @code
 * coopa::scene::SceneObject* root = toy::scene::spawn_runtime_root(scene, "Fireworks", "MyShow",
 *                                                                  "Created by the fireworks show");
 * root->add_child(std::move(rocket));
 * @endcode
 *
 * The marker has no YAML parser on purpose: a scene file cannot author a runtime object.
 */

#ifndef TOYENGINE_SCENE_RUNTIME_OBJECT_H
#define TOYENGINE_SCENE_RUNTIME_OBJECT_H

#include <memory>
#include <string>
#include <utility>

#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

namespace toy {
namespace scene {

/**
 * @class RuntimeObject
 * @brief Marker component: this object (and its subtree) was created by `owner` at runtime.
 */
class RuntimeObject : public coopa::scene::Component {
public:
    RuntimeObject() = default;
    RuntimeObject(std::string owner_name, std::string why) : owner_system(std::move(owner_name)), note(std::move(why)) {}

    std::string type_name() const override { return "RuntimeObject"; }

    std::string owner_system;  ///< The system that created it and alone may change it ("Weather").
    std::string note;          ///< One line for tools: what it is and how to change it.
};

/** @brief The RuntimeObject marker on `obj` or its nearest marked ancestor, or null. */
inline const RuntimeObject* runtime_marker(const coopa::scene::SceneObject& obj) {
    for (const coopa::scene::SceneObject* o = &obj; o; o = o->parent()) {
        if (auto* m = const_cast<coopa::scene::SceneObject*>(o)->get_component<RuntimeObject>()) return m;
    }
    return nullptr;
}

/** @brief True if `obj` is, or sits under, an object a system created at runtime. */
inline bool is_runtime_object(const coopa::scene::SceneObject& obj) { return runtime_marker(obj) != nullptr; }

/**
 * @brief Creates a marked root object (with a Transform at the origin) in `scene`, adopted and
 *        started so its components see the scene at once. Children added later must be adopted
 *        too (Scene::adopt()), as for any object added at runtime.
 */
inline coopa::scene::SceneObject* spawn_runtime_root(coopa::scene::Scene& scene, std::string name,
                                                     std::string owner_system, std::string note) {
    auto obj = std::make_unique<coopa::scene::SceneObject>(std::move(name));
    obj->add_component<coopa::scene::TransformComponent>();
    obj->add_component<RuntimeObject>(std::move(owner_system), std::move(note));
    coopa::scene::SceneObject* raw = scene.add_root_object(std::move(obj));
    scene.adopt(*raw);
    raw->start();
    return raw;
}

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_RUNTIME_OBJECT_H
