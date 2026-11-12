/**
 * @file camera_component.h
 * @brief Component representing a scene camera with perspective or orthographic projection.
 *
 * Stores camera parameters as loaded from the scene YAML's Camera component.
 * Supports PERSP (perspective) and ORTHO (orthographic/isometric) projections.
 * The view matrix is derived from the owning object's TransformComponent.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_CAMERA_COMPONENT_H
#define GFXCOOPA_ENGINE_COMPONENTS_CAMERA_COMPONENT_H

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @enum CameraType
 * @brief Projection mode for the camera.
 */
enum class CameraType {
    Perspective,  /**< Perspective projection (PERSP in YAML). */
    Orthographic  /**< Orthographic/isometric projection (ORTHO in YAML). */
};

/**
 * @class CameraComponent
 * @brief Camera with perspective or orthographic projection.
 *
 * Parameters match the Blender YAML export format:
 *   type:         PERSP | ORTHO
 *   fov:          Vertical field of view in degrees (PERSP)
 *   ortho_scale:  Orthographic scale factor (ORTHO)
 *   clip_start:   Near clip plane
 *   clip_end:     Far clip plane
 *   lens:         Focal length in mm -- read by DofPass's thin-lens CoC (see
 *                 aperture/focus_distance below) when a render config's
 *                 dof_focal_length override is <= 0; otherwise still informational
 *   sensor_width: Sensor width in mm -- same DofPass fallback role as lens
 *   sensor_height:Sensor height in mm (informational; DofPass only needs sensor_width)
 *   aperture:     f-stop; <= 0 inherits the render config's dof_aperture
 *   focus_distance: Metres to the sharp plane; <= 0 inherits dof_focus_distance
 *   focus_object: ':'-separated scene path (e.g. "sdf_blob:sdf_blob_sphere") to keep in
 *                 focus; non-empty puts THIS camera in object-focus mode regardless of the
 *                 render config's dof_focus_mode, and overrides focus_distance above --
 *                 see ToyRenderPipeline::resolve_dof_focus_() for the resolution order
 *   motion_blur:  false keeps this camera's image free of motion blur even when the render
 *                 config turns it on (default true)
 *
 * For isometric rendering, use CameraType::Orthographic. The isometric
 * viewing angle is baked into the scene object's Transform, not the projection.
 */
class CameraComponent : public coopa::scene::Component {
public:
    CameraComponent() = default;

    /**
     * @brief Clears the main-camera singleton if this instance currently holds it.
     *
     * Required so a scene reload (or any other destruction of the main
     * camera) never leaves main() pointing at a freed component.
     */
    ~CameraComponent() override {
        if (main_camera_() == this) main_camera_() = nullptr;
    }

    std::string type_name() const override { return "Camera"; }

    // --- Camera parameters ---

    CameraType type             = CameraType::Perspective; /**< Projection type. */
    float fov                   = 60.0f;   /**< Vertical FOV in degrees (Perspective mode). */
    float orthographic_size     = 3.0f;    /**< Orthographic half-height in world units (Unity Camera.orthographicSize). */
    float clip_start            = 0.1f;    /**< Near clip distance (alias: near_clip_plane). */
    float clip_end              = 1000.0f; /**< Far clip distance (alias: far_clip_plane). */
    float lens                  = 50.0f;   /**< Lens focal length in mm; read by DofPass as a fallback (see class doc). */
    float sensor_width          = 36.0f;   /**< Sensor width in mm; read by DofPass as a fallback (see class doc). */
    float sensor_height         = 24.0f;   /**< Sensor height in mm (informational). */
    float aperture              = 0.0f;    /**< f-stop; <= 0 inherits the render config's dof_aperture. */
    float focus_distance        = 0.0f;    /**< Metres to the sharp plane; <= 0 inherits dof_focus_distance. */
    std::string focus_object    = "";      /**< ':'-separated scene path to keep in focus; empty inherits the render config (see class doc). */
    bool motion_blur            = true;    /**< false opts this camera out of the renderer's motion blur (an editor viewport camera, a UI camera). */
    bool is_main                = false;   /**< Claims the main-camera singleton in start() (alias: main). */

    // Alias expressing orthographic size as a full height rather than a half-height.
    float get_ortho_scale() const { return orthographic_size * 2.0f; }
    void set_ortho_scale(float scale) { orthographic_size = scale * 0.5f; }

    // --- Configuration API (Unity-style) ---

    /**
     * @brief Sets perspective projection mode with vertical FOV in degrees.
     *
     * Safe to call at runtime, not just from the YAML parser: this only
     * mutates plain fields, which the render pipeline re-reads every frame.
     */
    void set_perspective(float fov_deg, float near_clip = 0.1f, float far_clip = 1000.0f) {
        type = CameraType::Perspective;
        fov = fov_deg;
        clip_start = near_clip;
        clip_end = far_clip;
    }

    /**
     * @brief Sets orthographic projection mode with half-height size in world units.
     *
     * Safe to call at runtime, not just from the YAML parser: this only
     * mutates plain fields, which the render pipeline re-reads every frame.
     */
    void set_orthographic(float ortho_size, float near_clip = 0.1f, float far_clip = 1000.0f) {
        type = CameraType::Orthographic;
        orthographic_size = ortho_size;
        clip_start = near_clip;
        clip_end = far_clip;
    }

    // --- Main-camera singleton ---

    /**
     * @brief Claims the main-camera singleton unconditionally, evicting any
     * previous holder.
     *
     * Call this to switch the active camera at runtime (e.g. a cutscene
     * camera taking over). YAML-driven claiming happens automatically in
     * start() instead -- see main()'s doc for the rule a loaded scene follows.
     */
    void make_main() {
        is_main = true;
        declared_main_ = false;
        main_camera_() = this;
    }

    /**
     * @brief Called once the scene finishes loading. Claims the main-camera
     * singleton if `is_main` was set by YAML, or if no camera has claimed it
     * yet -- so a scene always has a main camera, even if every Camera
     * component in it omits `main: true`.
     *
     * If another camera already claimed the singleton and this one also
     * requests it via `is_main`, this one wins and a warning is printed:
     * exactly one `main: true` per scene is the intended usage.
     */
    void start() override {
        if (is_main) {
            // Only two *declared* claims conflict. A camera that took over through make_main()
            // (a cutscene, an editor viewport camera) is a runtime override, not a second
            // `main: true`, so being displaced from it is expected and silent.
            if (main_camera_() && main_camera_() != this && main_camera_()->declared_main_) {
                std::cerr << "[gfxcoopa] Warning: multiple cameras marked main; "
                             "the most recently started one wins.\n";
            }
            declared_main_ = true;
            main_camera_() = this;
        } else if (!main_camera_()) {
            main_camera_() = this;
        }
    }

    /**
     * @brief Returns the scene's current main camera, or nullptr if none has
     * started yet.
     *
     * A camera becomes main either by declaring `main: true` in YAML, or by
     * being the first CameraComponent::start() to run in a scene where none
     * did -- so a loaded scene always has a main camera once it has started.
     *
     * @return The main CameraComponent, or nullptr.
     */
    static CameraComponent* main() { return main_camera_(); }

    // --- Matrix computation ---

    /**
     * @brief Computes and returns the view matrix from the owner's Transform.
     *
     * The view matrix is the inverse of the owner's world matrix.
     * Returns the identity matrix if the owner has no TransformComponent.
     *
     * @return View matrix as glm::mat4.
     */
    glm::mat4 get_view_matrix() const {
        if (!owner) return glm::mat4(1.0f);
        const auto* tc = owner->get_transform();
        if (!tc) return glm::mat4(1.0f);
        // View = inverse of world transform
        return glm::inverse(tc->get_world_matrix());
    }

    /**
     * @brief Computes and returns the projection matrix.
     *
     * @param aspect_ratio Viewport width / height.
     * @return Projection matrix as glm::mat4 (Vulkan NDC: Y-flipped).
     */
    glm::mat4 get_projection_matrix(float aspect_ratio) const {
        glm::mat4 proj;
        if (type == CameraType::Perspective) {
            proj = glm::perspective(
                glm::radians(fov),
                aspect_ratio,
                clip_start,
                clip_end
            );
        } else {
            // Unity orthographic: orthographic_size is half-height.
            float half_h = orthographic_size;
            float half_w = half_h * aspect_ratio;
            proj = glm::ortho(-half_w, half_w, -half_h, half_h, clip_start, clip_end);
        }
        // Y-flip is handled by the negative viewport height (VK_KHR_maintenance1)
        // instead of flipping the projection matrix, which avoids reversing
        // triangle winding order and keeps face culling correct.
        return proj;
    }

    /**
     * @brief Returns the world-space camera position (for lighting calculations).
     */
    glm::vec3 get_world_position() const {
        if (!owner) return glm::vec3(0.0f);
        const auto* tc = owner->get_transform();
        if (!tc) return glm::vec3(0.0f);
        glm::mat4 world = tc->get_world_matrix();
        return glm::vec3(world[3]); // Translation column
    }

private:
    bool declared_main_ = false;   ///< Claimed main in start() from `is_main` (not via make_main()).

    /**
     * @brief Function-local static holding the main-camera singleton.
     *
     * Same idiom as coopa::scene::SceneLoader's parser registry
     * (a private static accessor returning a reference to a function-local
     * static) rather than uicoopa's Meyers-singleton `instance()` shape --
     * the state here is a bare non-owning pointer, not an object to construct.
     */
    static CameraComponent*& main_camera_() {
        static CameraComponent* camera = nullptr;
        return camera;
    }
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_CAMERA_COMPONENT_H
