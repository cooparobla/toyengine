#include <toyengine/scene/scene_link.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

SceneLinkRequests& SceneLinkRequests::instance() {
    static SceneLinkRequests q;
    return q;
}

void SceneLinkRequests::push(SceneLinkRequest r) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(r));
}

std::vector<SceneLinkRequest> SceneLinkRequests::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SceneLinkRequest> out;
    out.swap(queue_);
    return out;
}

void SceneLink::trigger() {
    if (fired_ || target_scene.empty()) return;
    fired_ = true;
    SceneLinkRequest r;
    r.target_scene = target_scene;
    r.transition = transition;
    r.loading_screen = loading_screen;
    r.spawn_point = spawn_point;
    r.color = color;
    r.fade_time = fade_time;
    r.min_display_time = min_display_time;
    r.origin = scene;
    SceneLinkRequests::instance().push(std::move(r));
}

void SceneLink::update(float) {
    if (fired_ || target_scene.empty() || !scene || !owner) return;
    const bool inside = character_inside_();
    // The first look only records where the player is: arriving inside a link is not entering it.
    if (armed_ && inside && !was_inside_) trigger();
    was_inside_ = inside;
    armed_ = true;
}

bool SceneLink::character_inside_() const {
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"));
    auto* tc = owner->get_transform();
    if (!physics || !tc) return false;
    const glm::mat4 world = tc->get_world_matrix();
    const glm::vec3 axis_x(world[0]), axis_y(world[1]), axis_z(world[2]);
    const glm::vec3 scale(glm::length(axis_x), glm::length(axis_y), glm::length(axis_z));
    if (scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f) return false;
    coopa::physx::geometry::OBB box;
    box.center = glm::vec3(world[3]);
    box.half_extents = glm::abs(size) * scale * 0.5f;
    box.orientation = glm::quat_cast(glm::mat3(axis_x / scale.x, axis_y / scale.y, axis_z / scale.z));
    for (auto* col : physics->overlap_box(box, ~0u, /*include_triggers=*/false)) {
        if (col && col->owner && col->owner->get_component<CharacterController>()) return true;
    }
    return false;
}

} // namespace scene
} // namespace toy
