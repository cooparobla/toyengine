/**
 * @file planet_camera_fit.h
 * @brief Frames the scene's planet: derives an orbit camera's zoom range, its zoom rate and its
 *        clip planes from the globe's own radius and its terrain's measured peak.
 *
 * A planet's radius is derived, never authored -- `2 * face_tiles * tile_size / pi` -- so every
 * camera number tuned against it is a magic multiple that silently stops framing the world the
 * moment `face_tiles` changes. planet_demo was authored that way: `distance` and `max_distance`
 * at 4.3 radii, `min_distance` at the datum plus four units, `far_clip_plane` at fifteen radii.
 * Doubling `face_tiles` leaves all four describing a planet half the size of the one on screen.
 *
 * This component takes those over. What the scene keeps authoring is the SHAPE of the rig --
 * `pitch_at_min_distance_deg`, `tangent_at_max_distance`, the look button, the smoothing -- and
 * what it hands over is the SCALE:
 *
 * | CameraController / Camera | Derived as |
 * |---------------------------|------------|
 * | `max_distance`            | `R / sin(fov/2) * frame_margin * max_distance_scale` |
 * | `min_distance`            | `R + peak + min_clearance` |
 * | `zoom_speed`              | `(max_distance - min_distance) / zoom_notches` |
 * | `distance`                | `max_distance`, on the first apply only |
 * | `clip_end` / `clip_start` | `(max_distance + R) * far_clip_scale`, and that times `near_clip_ratio` |
 *
 * The fit itself is the editor viewport's own rule (`editor/viewport/editor_camera.cpp`,
 * `EditorCamera::frame()`): the distance at which a sphere of radius `R` exactly fills the
 * vertical field of view, times a margin.
 *
 * **Why those three distances move together.** `CameraController`'s zoom-coupled pitch and look
 * tilt both normalise the zoom as `(distance - min_distance) / (max_distance - min_distance)`.
 * Deriving only some of the three would re-map both curves, so an authored
 * `pitch_at_max_distance_deg: 70` would be reached at a different zoom than the scene meant.
 * All three come from the same `R` in one apply, which keeps every ratio those curves read
 * exactly where the scene put it. Do not author `min_distance`/`max_distance`/`zoom_speed`
 * alongside this component: it overwrites them.
 *
 * `shadow_distance` and `volumetrics_max_distance` are deliberately NOT derived. They are render
 * config rather than components, reachable only from the Engine, and they depend on the
 * atmosphere's thickness -- which no camera can see. Author them in the scene.
 *
 * Example YAML, on the camera object beside its Camera and CameraController:
 * @code
 * - name: camera
 *   components:
 *     - type: Transform
 *     - type: Camera
 *       fov: 50
 *       main: true
 *     - type: CameraController
 *       mode: orbit
 *       pitch_at_min_distance_deg: 8      # shape: kept
 *       pitch_at_max_distance_deg: 70
 *     - type: PlanetCameraFit             # scale: derived
 * @endcode
 */

#ifndef TOYENGINE_WORLD_PLANET_CAMERA_FIT_H
#define TOYENGINE_WORLD_PLANET_CAMERA_FIT_H

#include <cstdint>

#include <coopa/scene/component.h>

namespace coopa {
namespace scene {
class Scene;
}
namespace gfx {
namespace engine {
namespace components {
class CameraComponent;
}
}
}
}

namespace toy {
namespace scene {
class CameraController;
}
namespace world {

class TerrainComponent;

/** @brief Default order for install_planet_camera_fit_system(): after the other planet sizers. */
inline constexpr int k_planet_camera_fit_system_order = 65;

/**
 * @class PlanetCameraFit
 * @brief Writes a sibling CameraController's and Camera's scale fields from the scene's planet.
 *
 * Finds the planet with `world::find_planet_terrain()` and is a strict no-op without one, so the
 * component can sit on a camera in a scene that has no globe. Re-derives only when the terrain's
 * `TerraData::height_revision()` moves or one of its own knobs (or the camera's `fov`) changes,
 * so an unchanged scene costs one comparison a frame and a world that finishes baking mid-play
 * is picked up the frame it lands.
 *
 * Driven by install_planet_camera_fit_system(), NOT by Component::update(), for the same reason
 * PlanetAtmosphere is: the Behaviour walk does not tick in edit mode, and a framing that only
 * exists once you press play is one you cannot author against.
 */
class PlanetCameraFit : public coopa::scene::Component {
public:
    std::string type_name() const override { return "PlanetCameraFit"; }

    /**
     * @brief Slack around the globe at full zoom-out. 1.0 fills the frame exactly.
     *
     * The margin in the editor viewport's own framing rule, which this reuses rather than
     * re-inventing. Below 1 the limb is cropped; well above it the planet floats in a lot of sky.
     */
    float frame_margin = 1.1f;

    /**
     * @brief Multiplies the fitted distance to set `max_distance`. 1.0 is the globe framed.
     *
     * Raise it to pull the far end of the zoom back into space -- a scene that wants the planet
     * small in frame at full zoom-out rather than filling it.
     */
    float max_distance_scale = 1.0f;

    /**
     * @brief World units of clearance above the MEASURED peak at full zoom-in.
     *
     * Above the peak, not above the datum: a globe whose mountains stand a third of its radius
     * tall will otherwise put the camera inside the terrain at closest zoom, which is what
     * `min_distance: R + 4` did on planet_demo.
     */
    float min_clearance = 4.0f;

    /**
     * @brief Scroll notches the full zoom range takes. Keys `zoom_speed` to the planet's size.
     *
     * A better knob than a fraction of the radius: what a user notices is how many turns of the
     * wheel cross the range, and that stays the same on a globe of any size.
     */
    float zoom_notches = 25.0f;

    /**
     * @brief Sets the sibling Camera's far clip to `(max_distance + R) * this`. 0 leaves it alone.
     *
     * The far plane has to clear the whole globe seen from the far end of the zoom, which is that
     * sum; the multiplier is the headroom beyond it for the atmosphere shell and the stars.
     */
    float far_clip_scale = 2.0f;

    /**
     * @brief Sets the near clip to `clip_end * this`. 0 (the default) leaves it alone.
     *
     * Expressed as a ratio because depth precision is a function of the near-to-far ratio, not of
     * either plane alone. Around 2e-4 is the usual rule of thumb.
     */
    float near_clip_ratio = 0.0f;

    /**
     * @brief Also set `distance` to `max_distance`, so play opens with the globe held whole.
     *
     * First apply only, with CameraController::snap_to_targets() so the camera starts there
     * rather than easing in from the authored value. A later re-derive (a world that finished
     * baking) leaves `distance` alone and lets the framing ease, which reads better mid-play.
     */
    bool set_distance = true;

    /** @brief Re-derives the sibling camera fields if the planet or a knob has changed. Idempotent. */
    void apply();

    /** @brief The fitted full-zoom-out distance from the last apply(); 0 before the first. */
    float fitted_distance() const { return fitted_; }

private:
    /** @brief Resolved lazily; null until the scene has a planet and the siblings exist. */
    TerrainComponent* terrain_ = nullptr;
    toy::scene::CameraController* controller_ = nullptr;
    coopa::gfx::engine::components::CameraComponent* camera_ = nullptr;
    std::uint64_t applied_height_revision_ = 0;
    /** @brief Hash of the knobs and the fov apply() last used, so an edited one re-applies. */
    float applied_settings_key_ = 0.0f;
    bool applied_once_ = false;
    float fitted_ = 0.0f;
};

/**
 * @brief Installs the system that drives every PlanetCameraFit in `scene`.
 *
 * Runs in EDIT MODE as well as play, like PlanetAtmosphere: the framing is part of the thing
 * being authored. At order 65 it sits after the terrain (60) and the other planet sizers, and far
 * ahead of the `Behaviour` walk (200) that ticks CameraController -- so the fields it writes are
 * this frame's by the time the camera reads them.
 *
 * @param scene Scene to install into.
 * @param order UpdatePhase order; defaults to k_planet_camera_fit_system_order.
 */
void install_planet_camera_fit_system(coopa::scene::Scene& scene,
                                      int order = k_planet_camera_fit_system_order);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_PLANET_CAMERA_FIT_H
