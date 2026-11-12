#include <toyengine/world/planet_camera_fit.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <coopa/debug/logger.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/terrain_system.h>

namespace toy {
namespace world {

void PlanetCameraFit::apply() {
    if (owner == nullptr || scene == nullptr) return;

    // Resolved lazily rather than in start(), for two reasons: this has to work in edit mode,
    // where start() never runs at all, and a planet whose world is still baking has no peak to
    // measure yet -- the revision gate below picks it up the frame the bake lands.
    if (controller_ == nullptr) controller_ = owner->get_component<toy::scene::CameraController>();
    if (camera_ == nullptr) {
        camera_ = owner->get_component<coopa::gfx::engine::components::CameraComponent>();
    }
    if (terrain_ == nullptr) terrain_ = find_planet_terrain(*scene);
    // find_planet_terrain(), not find_planet_frame(): a default WorldFrame IS the flat world,
    // which is a fine answer to "which way is up" and a useless one to "is there a planet". A
    // scene with no globe must leave the camera exactly as authored.
    if (controller_ == nullptr || terrain_ == nullptr) return;
    // The derivation is an orbit rig's: a zoom range, a zoom rate and a fitted distance mean
    // nothing to a fly or first-person camera.
    if (controller_->mode != toy::scene::CameraControlMode::Orbit) return;
    if (terrain_->field.empty()) return;
    const TerraData* data = terrain_->field.data();
    if (data == nullptr) return;

    const float fov = camera_ != nullptr ? camera_->fov : 60.0f;
    // The fov as well as the knobs: the fit is a function of it, and the inspector can drag it.
    const float settings_key = frame_margin + 7.0f * max_distance_scale + 13.0f * min_clearance +
                               17.0f * zoom_notches + 23.0f * far_clip_scale +
                               29.0f * near_clip_ratio + 31.0f * fov;
    if (applied_once_ && data->height_revision() == applied_height_revision_ &&
        settings_key == applied_settings_key_) {
        return;
    }

    const float radius = terrain_->params.layout().radius();
    const float peak   = planet_peak(*terrain_);
    if (radius <= 0.0f) return;

    // The distance at which a sphere of `radius` exactly fills the vertical field of view, times
    // a margin: EditorCamera::frame()'s rule, reused rather than re-derived.
    const float half_fov = glm::radians(std::max(fov, 1.0f) * 0.5f);
    fitted_ = radius / std::max(std::sin(half_fov), 1e-3f) * std::max(frame_margin, 0.0f);

    const float max_distance = fitted_ * std::max(max_distance_scale, 0.0f);
    // Above the measured PEAK, not above the datum: on a globe whose mountains stand a third of
    // its radius tall, clearing only the datum puts the camera inside the terrain at full zoom.
    const float min_distance = radius + peak + std::max(min_clearance, 0.0f);
    if (max_distance <= min_distance) {
        // Writing these anyway would collapse the zoom range to nothing, and with it the
        // normalised zoom that drives the pitch and look-tilt curves -- the camera would lock at
        // its min-distance pitch with no sign of why.
        static bool warned = false;
        if (!warned) {
            warned = true;
            coopa::debug::Logger("camera").warn(
                "PlanetCameraFit: the fitted max_distance (" + std::to_string(max_distance) +
                ") is not clear of min_distance (" + std::to_string(min_distance) +
                "); leaving the camera as authored. Raise max_distance_scale or frame_margin.");
        }
        return;
    }

    // All three together, never some of them. CameraController's zoom-coupled pitch and look
    // tilt both normalise the zoom as (distance - min) / (max - min), so deriving a subset would
    // re-map both curves and move where an authored pitch_at_*_distance_deg is reached. Derived
    // from one radius, every ratio those curves read stays where the scene put it.
    controller_->min_distance = min_distance;
    controller_->max_distance = max_distance;
    controller_->zoom_speed   = (max_distance - min_distance) / std::max(zoom_notches, 1.0f);

    if (camera_ != nullptr && far_clip_scale > 0.0f) {
        // The far plane has to clear the whole globe from the far end of the zoom; the rest is
        // headroom for the atmosphere shell and the stars behind it. Safe at runtime: the
        // projection is rebuilt from these fields every frame (CameraComponent::set_perspective).
        camera_->clip_end = (max_distance + radius) * far_clip_scale;
        if (near_clip_ratio > 0.0f) {
            camera_->clip_start = std::max(camera_->clip_end * near_clip_ratio, 1e-3f);
        }
    }

    if (set_distance && !applied_once_) {
        // Play opens with the globe held whole. Snapped, not eased: start() has already seeded
        // the applied pose from the authored distance, so without this the camera would visibly
        // slide from there to here over the first second.
        controller_->distance = max_distance;
        controller_->snap_to_targets();
    }
    // A later re-derive -- a world that finished baking mid-play -- deliberately does NOT snap:
    // easing into the corrected framing reads better than a jump.

    applied_height_revision_ = data->height_revision();
    applied_settings_key_    = settings_key;
    applied_once_            = true;
}

namespace {

/**
 * @class PlanetCameraFitSystem
 * @brief Drives every PlanetCameraFit; see install_planet_camera_fit_system().
 */
class PlanetCameraFitSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "PlanetCameraFit"; }
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) override {
        for (PlanetCameraFit* f : scene.get_components<PlanetCameraFit>()) f->apply();
    }
};

} // namespace

void install_planet_camera_fit_system(coopa::scene::Scene& scene, int order) {
    scene.add_system(std::make_unique<PlanetCameraFitSystem>(), order);
}

} // namespace world
} // namespace toy
