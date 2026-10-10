#include "editor/viewport/editor_camera.h"

namespace toy {
namespace editor {

void EditorCamera::create(coopa::scene::Scene& scene) {
    auto obj = std::make_unique<coopa::scene::SceneObject>("__EditorCamera");
    obj->add_component<coopa::scene::TransformComponent>();
    camera_ = obj->add_component<coopa::gfx::engine::components::CameraComponent>();
    camera_->set_perspective(fov, 0.05f, 2000.0f);
    // The scene view never motion-blurs (orbiting/flying would smear the whole viewport); Play
    // hands main to the game's camera, which follows the render settings.
    camera_->motion_blur = false;
    object_ = scene.add_root_object(std::move(obj));
    scene.adopt(*object_);
    apply();
}

void EditorCamera::apply() {
    if (!object_) return;
    pitch_deg = std::clamp(pitch_deg, -89.5f, 89.5f);
    distance = std::clamp(distance, 0.05f, 5000.0f);
    auto& t = object_->get_transform()->transform();
    t.set_position(position());
    t.set_rotation(glm::vec3(90.0f - pitch_deg, 0.0f, yaw_deg));
    const float near_clip = std::max(0.01f, distance * 0.005f), far_clip = std::max(2000.0f, distance * 50.0f);
    if (ortho) camera_->set_orthographic(distance * std::tan(glm::radians(fov * 0.5f)), near_clip, far_clip);
    else       camera_->set_perspective(fov, near_clip, far_clip);
}

glm::vec3 EditorCamera::offset_dir() const {
    const float e = glm::radians(pitch_deg), p = glm::radians(yaw_deg);
    return {std::cos(e) * std::sin(p), -std::cos(e) * std::cos(p), std::sin(e)};
}

void EditorCamera::turn(float yaw_delta_deg, float pitch_delta_deg) {
    if (auto_ortho && (yaw_delta_deg != 0.0f || pitch_delta_deg != 0.0f)) { ortho = false; auto_ortho = false; }
    yaw_deg += yaw_delta_deg;
    pitch_deg += pitch_delta_deg;
    apply();
}

void EditorCamera::pan(glm::vec2 mouse_delta, float viewport_h) {
    const float world_per_px = 2.0f * distance * std::tan(glm::radians(fov * 0.5f)) / std::max(1.0f, viewport_h);
    focus += (-right() * mouse_delta.x + up() * mouse_delta.y) * world_per_px;
    apply();
}

void EditorCamera::dolly(float wheel) {
    distance *= std::pow(0.88f, wheel * zoom_speed);
    apply();
}

void EditorCamera::fly(glm::vec3 move, float dt) {
    const float speed = std::max(2.0f, distance) * 1.2f * fly_speed;
    focus += (right() * move.x + forward() * move.y + glm::vec3(0, 0, 1) * move.z) * speed * dt;
    apply();
}

void EditorCamera::frame(const glm::vec3& lo, const glm::vec3& hi) {
    focus = (lo + hi) * 0.5f;
    const float radius = std::max(0.25f, glm::length(hi - lo) * 0.5f);
    distance = radius / std::sin(glm::radians(fov * 0.5f)) * 1.1f;
    apply();
}

void EditorCamera::set_ortho(bool o) {
    ortho = o;
    auto_ortho = false;
    apply();
}

void EditorCamera::axis_view(char which, bool opposite) {
    if (!ortho) { ortho = true; auto_ortho = true; }
    switch (which) {
        case 'f': yaw_deg = opposite ? 180.0f : 0.0f; pitch_deg = 0.0f; break;
        case 'r': yaw_deg = opposite ? -90.0f : 90.0f; pitch_deg = 0.0f; break;
        case 't': yaw_deg = 0.0f; pitch_deg = opposite ? -89.5f : 89.5f; break;
        default: break;
    }
    apply();
}

} // namespace editor
} // namespace toy
