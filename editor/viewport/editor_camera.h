/**
 * @file editor_camera.h
 * @brief The editor's viewport camera: Blender-style orbit / pan / dolly / fly around a
 *        focus point, living in the editor's own UI scene so scene rebuilds never touch it.
 *
 * Controls (only while the cursor is over the viewport):
 *   orbit   middle-drag, Alt+left-drag, or right-drag
 *   pan     Shift + any orbit gesture
 *   dolly   scroll wheel (trackpad two-finger scroll)
 *   fly     hold right button + W/A/S/D/Q/E
 *   frame   F (selection), Home (everything)
 *   views   numpad 1/3/7 (front/right/top; Ctrl = opposite), numpad 5 toggles ortho
 *
 * Z-up, camera looking down its local -Z: the same pose convention CameraController's
 * orbit uses (position = focus + spherical offset, Euler = (90 - pitch, 0, yaw)).
 */

#ifndef TOYEDITOR_VIEWPORT_EDITOR_CAMERA_H
#define TOYEDITOR_VIEWPORT_EDITOR_CAMERA_H

#include <gfxcoopa/engine/components/camera_component.h>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace toy::editor {

class EditorCamera {
public:
    glm::vec3 focus{0.0f};
    float yaw_deg = 35.0f;
    float pitch_deg = 25.0f;
    float distance = 12.0f;
    bool  ortho = false;
    float fov = 50.0f;

    /** @brief Creates the camera object as a root of `scene` (the editor UI scene). */
    void create(coopa::scene::Scene& scene) {
        auto obj = std::make_unique<coopa::scene::SceneObject>("__EditorCamera");
        obj->add_component<coopa::scene::TransformComponent>();
        camera_ = obj->add_component<coopa::gfx::engine::components::CameraComponent>();
        camera_->set_perspective(fov, 0.05f, 2000.0f);
        object_ = scene.add_root_object(std::move(obj));
        scene.adopt(*object_);
        apply();
    }

    coopa::gfx::engine::components::CameraComponent* camera() { return camera_; }

    /** @brief Writes the pose into the camera's transform; call after changing any field. */
    void apply() {
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

    /** @brief Makes this the main camera (edit mode does this every frame). */
    void make_main() { if (camera_) camera_->make_main(); }

    glm::vec3 offset_dir() const {
        const float e = glm::radians(pitch_deg), p = glm::radians(yaw_deg);
        return {std::cos(e) * std::sin(p), -std::cos(e) * std::cos(p), std::sin(e)};
    }
    glm::vec3 position() const { return focus + offset_dir() * distance; }
    glm::vec3 forward() const { return -offset_dir(); }
    glm::vec3 right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 0, 1))); }
    glm::vec3 up() const { return glm::normalize(glm::cross(right(), forward())); }

    void orbit(glm::vec2 mouse_delta) {
        yaw_deg -= mouse_delta.x * 0.35f;
        pitch_deg += mouse_delta.y * 0.35f;
        apply();
    }

    /** @brief Pans so the point under the cursor follows it (`viewport_h` in window pixels). */
    void pan(glm::vec2 mouse_delta, float viewport_h) {
        const float world_per_px = 2.0f * distance * std::tan(glm::radians(fov * 0.5f)) / std::max(1.0f, viewport_h);
        focus += (-right() * mouse_delta.x + up() * mouse_delta.y) * world_per_px;
        apply();
    }

    void dolly(float wheel) {
        distance *= std::pow(0.88f, wheel);
        apply();
    }

    /** @brief Flies the focus with WASD/QE: `move` is (right, forward, up) in -1..1. */
    void fly(glm::vec3 move, float dt) {
        const float speed = std::max(2.0f, distance) * 1.2f;
        focus += (right() * move.x + forward() * move.y + glm::vec3(0, 0, 1) * move.z) * speed * dt;
        apply();
    }

    /** @brief Frames a world-space box. */
    void frame(const glm::vec3& lo, const glm::vec3& hi) {
        focus = (lo + hi) * 0.5f;
        const float radius = std::max(0.25f, glm::length(hi - lo) * 0.5f);
        distance = radius / std::sin(glm::radians(fov * 0.5f)) * 1.1f;
        apply();
    }

    /** @brief Snaps to an axis view: 'f'ront (-Y), 'r'ight (+X), 't'op (+Z); `opposite` flips. */
    void axis_view(char which, bool opposite) {
        switch (which) {
            case 'f': yaw_deg = opposite ? 180.0f : 0.0f; pitch_deg = 0.0f; break;
            case 'r': yaw_deg = opposite ? -90.0f : 90.0f; pitch_deg = 0.0f; break;
            case 't': yaw_deg = 0.0f; pitch_deg = opposite ? -89.5f : 89.5f; break;
            default: break;
        }
        apply();
    }

private:
    coopa::scene::SceneObject* object_ = nullptr;
    coopa::gfx::engine::components::CameraComponent* camera_ = nullptr;
};

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_EDITOR_CAMERA_H
