#include <toyengine/save/saveable.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

namespace toy {
namespace save {

std::string object_path(const coopa::scene::SceneObject& obj) {
    std::string path = obj.name();
    for (const coopa::scene::SceneObject* p = obj.parent(); p; p = p->parent()) path = p->name() + ":" + path;
    return path;
}

std::string SaveId::key() const {
    if (!id.empty() || !owner) return id;
    return object_path(*owner);
}

coopa::scene::SceneObject* find_by_save_id(const coopa::scene::Scene& scene, const std::string& id) {
    coopa::scene::SceneObject* found = nullptr;
    for (const auto& root : scene.root_objects()) {
        root->for_each_recursive([&](coopa::scene::SceneObject& obj) {
            if (found) return;
            if (auto* sid = obj.get_component<SaveId>(); sid && sid->key() == id) found = &obj;
        });
        if (found) break;
    }
    return found;
}

} // namespace save
} // namespace toy
