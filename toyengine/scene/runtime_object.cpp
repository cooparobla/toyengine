#include <toyengine/scene/runtime_object.h>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

namespace toy {
namespace scene {

const RuntimeObject* runtime_marker(const coopa::scene::SceneObject& obj) {
    for (const coopa::scene::SceneObject* o = &obj; o; o = o->parent()) {
        if (auto* m = const_cast<coopa::scene::SceneObject*>(o)->get_component<RuntimeObject>()) return m;
    }
    return nullptr;
}

coopa::scene::SceneObject* spawn_runtime_root(coopa::scene::Scene& scene, std::string name,
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
