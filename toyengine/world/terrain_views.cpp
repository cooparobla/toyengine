#include <toyengine/world/terrain_views.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/terrain_component.h>

namespace toy {
namespace world {

std::vector<TerrainSceneView> terrain_views(const coopa::scene::Scene& scene) {
    std::vector<TerrainSceneView> out;
    // One loop, both modes: a terrain answers through its field whichever model it carries.
    for (TerrainComponent* t : const_cast<coopa::scene::Scene&>(scene).get_components<TerrainComponent>()) {
        if (t->owner == nullptr || !t->owner->active()) continue;
        t->sync_field();
        if (t->field.empty()) continue;
        out.push_back(TerrainSceneView{t->field_view(), t->owner});
    }
    return out;
}

TerrainSceneView terrain_view_at(const coopa::scene::Scene& scene, const glm::vec3& world) {
    for (const TerrainSceneView& v : terrain_views(scene)) {
        if (v.view.contains(world)) return v;
    }
    return TerrainSceneView{};
}

} // namespace world
} // namespace toy
