#include <toyengine/world/planet_atmosphere.h>

#include <algorithm>
#include <memory>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/components/volume.h>
#include <toyengine/world/terrain_component.h>

namespace toy {
namespace world {

void PlanetAtmosphere::apply() {
    if (owner == nullptr) return;
    // Resolved lazily rather than in start(): the component has to work the same in edit mode,
    // where the terrain it measures may only appear once the generator has run.
    if (terrain_ == nullptr) {
        for (coopa::scene::SceneObject* p = owner->parent(); p != nullptr; p = p->parent()) {
            if (auto* t = p->get_component<TerrainComponent>()) {
                terrain_ = t;
                break;
            }
        }
        if (terrain_ == nullptr) return;
    }
    if (terrain_->field.empty()) return;
    TerraData* data = terrain_->field.data();
    // The knobs as well as the bake: an inspector drag or a scene reload changes the profile
    // with the terrain's height untouched, and would otherwise not be picked up at all.
    const float settings_key = thickness_scale + 7.0f * min_thickness + 13.0f * base_altitude_scale +
                               17.0f * scale_height_fraction + 23.0f * radius_thickness_scale +
                               29.0f * radius_base_scale + 31.0f * moon_intensity;
    if (data == nullptr ||
        (data->height_revision() == applied_height_revision_ && settings_key == applied_settings_key_)) {
        return;
    }

    auto* volume = owner->get_component<coopa::gfx::engine::components::VolumeComponent>();
    if (volume == nullptr) return;

    // The terrain's own measured peak, not params.height_scale (a nominal amplitude the bake
    // may fall well short of, or -- with enough noise detail -- exceed).
    const float peak = std::max(data->height_range().y, 0.0f);
    const float planet_radius = terrain_->params.layout().radius();
    // The largest of the three terms, so one set of authored numbers serves a planet of any
    // size: relief-keyed where the terrain is tall, radius-keyed where the globe is large or
    // its relief is low, and an absolute floor for a small near-flat world. See the header.
    const float thickness = std::max({min_thickness,
                                      peak * thickness_scale,
                                      planet_radius * std::max(radius_thickness_scale, 0.0f)});
    const float base = planet_radius + std::max(peak * std::max(base_altitude_scale, 0.0f),
                                                planet_radius * std::max(radius_base_scale, 0.0f));
    const float outer = base + thickness;

    volume->extent         = glm::vec3(outer, outer, outer);
    volume->height_base    = base;            // VolumeShape::Shell's inner radius
    volume->height_falloff = std::max(thickness * scale_height_fraction, 0.1f);

    inner_ = base;
    outer_ = outer;
    peak_  = peak;
    applied_height_revision_ = data->height_revision();
    applied_settings_key_ = settings_key;
}

float PlanetAtmosphere::day_sky_factor(const glm::vec3& camera_pos, const glm::vec3& sun_to) const {
    // A layer that is not the atmosphere (a cloud deck) has no business recolouring the sky.
    if (day_sky_altitude_scale <= 0.0f) return 0.0f;
    if (outer_ <= inner_ || owner == nullptr) return 0.0f;
    auto* tf = owner->get_transform();
    if (tf == nullptr) return 0.0f;
    const glm::vec3 centre = glm::vec3(tf->get_world_matrix()[3]);
    const glm::vec3 radial = camera_pos - centre;
    const float r = glm::length(radial);
    if (r < 1e-4f) return 0.0f;

    // Full sky at or below day_sky_altitude, fading back to stars by the shell's outer radius.
    const float span    = outer_ - inner_;
    const float full    = glm::clamp(peak_ * day_sky_altitude_scale, 0.0f, span * 0.9f);
    const float depth   = 1.0f - glm::smoothstep(full, span, r - inner_);
    // The same soft terminator the shell's own density gate uses, so the backdrop and the
    // volumetric glow fade in together instead of one leading the other across the dusk band.
    const float day = glm::smoothstep(-0.25f, 0.35f, glm::dot(radial / r, sun_to));
    return depth * depth * day;
}

namespace {

/**
 * @class PlanetAtmosphereSystem
 * @brief Drives every PlanetAtmosphere; see install_planet_atmosphere_system().
 */
class PlanetAtmosphereSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "PlanetAtmosphere"; }
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) override {
        for (PlanetAtmosphere* a : scene.get_components<PlanetAtmosphere>()) a->apply();
    }
};

} // namespace

void install_planet_atmosphere_system(coopa::scene::Scene& scene, int order) {
    scene.add_system(std::make_unique<PlanetAtmosphereSystem>(), order);
}

} // namespace world
} // namespace toy
