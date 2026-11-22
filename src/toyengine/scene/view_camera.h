#pragma once

/**
 * @file view_camera.h
 * @brief Which camera a camera-relative system follows, and how a component overrides it.
 *
 * Terrain streaming, snow focus, water focus, weather, view blockers and instance painting all
 * need to know "where is the viewer". Reading
 * `coopa::gfx::engine::components::CameraComponent::main()` for that is a process-global, and it
 * is wrong as soon as more than one scene is simulated and rendered in the same frame: every
 * scene would stream around whichever camera happened to claim the singleton.
 *
 * So the camera is PUSHED in, by whoever owns the frame, the same contract
 * `toy::core::Engine`'s `drive_*` functions already use for input (see
 * `toyengine/scene/README.md`). A system that was never pushed a camera falls back to `main()`,
 * which is what keeps the single-scene and editor paths behaving exactly as they did: the
 * editor's viewport camera lives in its own UI scene rather than in the scene being edited, so
 * "the scene's own camera" is NOT a safe default and is deliberately not what this resolves to.
 *
 * A component may name a camera of its own -- a terrain that streams around a chase camera, a
 * map view that follows its own -- through `CameraRef`.
 */

#include <string>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/camera_component.h>

namespace toy::scene {

/**
 * @struct CameraRef
 * @brief Which camera a camera-relative component follows.
 *
 * Authored as two keys so the inspector can show a checkbox that enables a name field:
 *
 * ```yaml
 * use_main_camera: true     # follow whatever camera the host is rendering this scene from
 * camera: chase_cam         # ...or this object's camera, when use_main_camera is false
 * ```
 *
 * The default follows the host's camera, which is what almost every scene wants.
 */
struct CameraRef {
    /// Follow the camera the host pushed for this scene (the default).
    bool use_main_camera = true;
    /// Object name holding the camera to follow when `use_main_camera` is false. An empty or
    /// unresolvable name falls back to the host's camera rather than to no camera at all,
    /// because a misspelled name must not silently park a terrain at the world origin.
    std::string camera;
};

/**
 * @brief The camera a scene's systems should follow, given what the host pushed.
 *
 * @param pushed What the host pushed for this scene, or null if nothing was.
 * @return `pushed` when it is non-null, else the process-global main camera.
 */
inline const coopa::gfx::engine::components::CameraComponent* scene_view_camera(
    const coopa::gfx::engine::components::CameraComponent* pushed) {
    return pushed != nullptr ? pushed
                             : coopa::gfx::engine::components::CameraComponent::main();
}

/**
 * @class ViewCameraAware
 * @brief Mixin for a system whose behaviour depends on where the viewer is.
 *
 * Carries the pushed camera and nothing else. A system that mixes this in keeps working
 * untouched when nobody pushes one, because `scene_view_camera()` falls back to the global main
 * camera -- so adding this to a system is not a behaviour change on its own.
 */
class ViewCameraAware {
public:
    /**
     * @brief Sets the camera this system's scene is being viewed from.
     *
     * Called by the host once per frame, BEFORE the scene updates, the same way input is
     * pushed. Pass null to go back to following the global main camera.
     *
     * @param cam The camera, or null.
     */
    void set_view_camera(const coopa::gfx::engine::components::CameraComponent* cam) {
        view_camera_ = cam;
    }

    /** @brief The pushed camera, or null if none was pushed. */
    const coopa::gfx::engine::components::CameraComponent* pushed_view_camera() const {
        return view_camera_;
    }

protected:
    /// What the host pushed. Read through scene_view_camera()/resolve_view_camera(), never raw,
    /// so the fallback to main() is never accidentally skipped.
    const coopa::gfx::engine::components::CameraComponent* view_camera_ = nullptr;
};

/**
 * @brief Resolves one component's camera: its own named one, else the scene's.
 *
 * @param ref The component's authored choice.
 * @param scene The scene the component belongs to, searched for `ref.camera`.
 * @param pushed What the host pushed for this scene, or null.
 * @return The camera to follow, or null when there is none at all.
 */
inline const coopa::gfx::engine::components::CameraComponent* resolve_view_camera(
    const CameraRef& ref, const coopa::scene::Scene& scene,
    const coopa::gfx::engine::components::CameraComponent* pushed) {
    using coopa::gfx::engine::components::CameraComponent;
    if (!ref.use_main_camera && !ref.camera.empty()) {
        if (coopa::scene::SceneObject* obj = scene.find_object(ref.camera)) {
            if (auto* cam = obj->get_component<CameraComponent>()) return cam;
        }
        // Falls through to the scene's camera on purpose -- see CameraRef::camera.
    }
    return scene_view_camera(pushed);
}

/**
 * @brief Reads `use_main_camera` and `camera` into a CameraRef, leaving absent keys alone.
 *
 * @param node The component's YAML mapping.
 * @param ref The reference to fill.
 */
template <typename Node>
void parse_camera_ref(const Node& node, CameraRef& ref) {
    if (node.contains("use_main_camera")) {
        ref.use_main_camera = node.at("use_main_camera").template get_value<bool>();
    }
    if (node.contains("camera")) {
        ref.camera = node.at("camera").template get_value<std::string>();
        // Naming a camera is the whole point of naming one: an author who writes `camera:` and
        // nothing else means it, so the flag follows rather than making them write both.
        if (!ref.camera.empty() && !node.contains("use_main_camera")) ref.use_main_camera = false;
    }
}

}  // namespace toy::scene
