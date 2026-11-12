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
 *   frame   numpad . / . / F (selection; F in Object Mode), Home (everything)
 *   views   numpad 1/3/7 or the nav gizmo's axis balls (front/right/top; Ctrl = opposite):
 *           orthographic, back to perspective once orbited (Blender's Auto Perspective);
 *           numpad 5 toggles ortho
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
    /// Blender's Auto Perspective: an axis view went orthographic on its own, so orbiting out
    /// of it returns to perspective. Cleared by an explicit projection toggle.
    bool  auto_ortho = false;
    float fov = 50.0f;
    // Edit > Preferences > Navigation.
    float orbit_speed = 1.0f;   ///< Degrees per pixel of drag, x 0.35.
    float zoom_speed = 1.0f;    ///< Scales every dolly step.
    float fly_speed = 1.0f;

    /** @brief Creates the camera object as a root of `scene` (the editor UI scene). */
    void create(coopa::scene::Scene& scene);

    coopa::gfx::engine::components::CameraComponent* camera() { return camera_; }

    /** @brief Writes the pose into the camera's transform; call after changing any field. */
    void apply();

    /** @brief Makes this the main camera (edit mode does this every frame). */
    void make_main() { if (camera_) camera_->make_main(); }

    glm::vec3 offset_dir() const;
    glm::vec3 position() const { return focus + offset_dir() * distance; }
    glm::vec3 forward() const { return -offset_dir(); }
    glm::vec3 right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 0, 1))); }
    glm::vec3 up() const { return glm::normalize(glm::cross(right(), forward())); }

    void orbit(glm::vec2 mouse_delta) { turn(-mouse_delta.x * 0.35f * orbit_speed, mouse_delta.y * 0.35f * orbit_speed); }

    /** @brief Turns the view by exact angles (numpad 2/4/6/8), whatever the orbit speed. */
    void turn(float yaw_delta_deg, float pitch_delta_deg);

    /** @brief Pans so the point under the cursor follows it (`viewport_h` in window pixels). */
    void pan(glm::vec2 mouse_delta, float viewport_h);

    void dolly(float wheel);

    /** @brief Flies the focus with WASD/QE: `move` is (right, forward, up) in -1..1. */
    void fly(glm::vec3 move, float dt);

    /** @brief Frames a world-space box. */
    void frame(const glm::vec3& lo, const glm::vec3& hi);

    /** @brief Explicit perspective / orthographic switch (numpad 5, the nav button). */
    void set_ortho(bool o);

    /**
     * @brief Snaps to an axis view: 'f'ront (-Y), 'r'ight (+X), 't'op (+Z); `opposite` flips.
     *        Goes orthographic as Blender's Auto Perspective does (unless already), and
     *        orbiting away goes back to perspective.
     */
    void axis_view(char which, bool opposite);

private:
    coopa::scene::SceneObject* object_ = nullptr;
    coopa::gfx::engine::components::CameraComponent* camera_ = nullptr;
};

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_EDITOR_CAMERA_H
