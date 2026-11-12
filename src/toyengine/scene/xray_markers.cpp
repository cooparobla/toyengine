#include <toyengine/scene/xray_markers.h>

#include <coopa/scene/scene_object.h>

namespace toy {
namespace scene {
namespace {

/// The nearest ancestor-or-self marker of type T, or null. Nearest rather than any, so a
/// child can opt back out of a marked parent's subtree with `enabled: false`.
template <typename T>
const T* nearest_marker(const coopa::scene::SceneObject* obj) {
    for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
        if (auto* m = const_cast<coopa::scene::SceneObject*>(o)->get_component<T>()) return m;
    }
    return nullptr;
}

} // namespace

bool is_xray_subject(const coopa::scene::SceneObject* obj) {
    const XraySubject* s = nearest_marker<XraySubject>(obj);
    return s != nullptr && s->enabled;
}

bool allows_xray(const coopa::scene::SceneObject* obj) {
    const XrayOccluder* o = nearest_marker<XrayOccluder>(obj);
    return o != nullptr && o->enabled;
}

XrayRole xray_role(const coopa::scene::SceneObject* obj) {
    if (is_xray_subject(obj)) return XrayRole::Subject;
    if (allows_xray(obj)) return XrayRole::Occluder;
    return XrayRole::None;
}

} // namespace scene
} // namespace toy
