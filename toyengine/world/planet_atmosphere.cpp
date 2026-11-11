#include <toyengine/world/planet_atmosphere.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <glm/glm.hpp>

#include <coopa/debug/logger.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/components/volume.h>
#include <glm/gtc/constants.hpp>

#include <toyengine/world/terrain_component.h>
#include <toyengine/world/terrain_system.h>

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
                               29.0f * radius_base_scale + 31.0f * moon_intensity +
                               37.0f * cloud_altitude_peak_scale + 41.0f * cloud_altitude_radius_scale +
                               43.0f * cloud_thickness_radius_scale + 47.0f * cloud_thickness_altitude_scale +
                               53.0f * limb_optical_depth;
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

    // The density too, when the scene asks for a depth rather than a per-metre figure. Without
    // this the one un-proportional number in the profile rides along unchanged from whatever
    // globe it was tuned on, and the shaped atmosphere comes out either invisible or opaque.
    // The divisor is the grazing-path integral through an exponential atmosphere,
    // sqrt(2*pi * H * R) -- see the field's doc.
    if (limb_optical_depth > 0.0f) {
        const float grazing = std::sqrt(2.0f * glm::pi<float>() * volume->height_falloff * planet_radius);
        volume->density = limb_optical_depth / std::max(grazing, 1e-3f);
    }

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

CloudDeck planet_cloud_deck(coopa::scene::Scene& scene, const env::PlanetFrame& planet) {
    /// Must match sky_cloud_density.glsl's WEATHER_PERIOD.
    constexpr float k_weather_period = 13000.0f;
    /// Weather systems around the globe: enough for distinct fronts, few enough to read as
    /// continent-sized rather than as noise.
    constexpr float k_periods = 3.0f;

    const TerrainComponent* terrain = find_planet_terrain(scene);
    const float peak = terrain != nullptr ? planet_peak(*terrain) : 0.0f;

    // The scene's atmosphere, not one of its cloud decks: `day_sky_altitude_scale > 0` is the
    // existing "I am the air, and I decide what the sky looks like" marker (day_sky_factor()
    // returns 0 below it), so this reuses that contract instead of adding a flag beside it.
    const PlanetAtmosphere* atmos = nullptr;
    for (const PlanetAtmosphere* a : scene.get_components<PlanetAtmosphere>()) {
        if (a->day_sky_altitude_scale > 0.0f) { atmos = a; break; }
    }
    const PlanetAtmosphere defaults;
    const PlanetAtmosphere& knobs = atmos != nullptr ? *atmos : defaults;

    CloudDeck deck;
    // The base clears the terrain's measured peak, with a radius-keyed floor so a low-relief or
    // ocean world still gets its clouds off the ground; the depth is keyed to the radius so the
    // deck reads as a layer rather than a film from orbit. The largest term wins in each, the
    // same rule the shell's own thickness uses and for the same reason.
    deck.altitude  = std::max(peak * std::max(knobs.cloud_altitude_peak_scale, 0.0f),
                              planet.radius * std::max(knobs.cloud_altitude_radius_scale, 0.0f));
    deck.thickness = std::max(planet.radius * std::max(knobs.cloud_thickness_radius_scale, 0.0f),
                              deck.altitude * std::max(knobs.cloud_thickness_altitude_scale, 0.0f));
    deck.thickness = std::max(deck.thickness, 0.1f);

    // The deck lives INSIDE the air it is weather in. Two reasons, and the second is the one
    // that bites: a layer of cloud above the atmosphere reads as cloud in vacuum, and the
    // volumetric march bounds its ray by the shell's own outer radius (volumetrics_march.frag),
    // so a deck beyond it gets none of the shell's in-scatter and sits in front of the limb
    // instead of within it.
    //
    // A deck that does not fit is thinned, not lowered. The base is what the altitude knobs are
    // FOR: it is what clears the mountains, and what a camera has to get under for the deck to
    // read as a layer overhead at all -- a planet camera's closest orbit is only
    // `peak + min_clearance` up (planet_camera_fit.h). The depth only decides how much of a
    // layer it reads as from outside. Scaling both together drags the base below the peak and
    // below that closest orbit, so the camera ends up inside the slab at full zoom with no
    // position left that sees the deck from beneath.
    if (atmos != nullptr && atmos->shell_outer() > atmos->shell_inner()) {
        const float head = atmos->shell_outer() - planet.radius;   // air there is to put it in
        if (deck.altitude + deck.thickness > head) {
            // The same radius-keyed figure the thickness floor uses: below this the layer stops
            // reading as one from orbit, and giving up base clearance is the better trade.
            const float thinnest = std::max(
                planet.radius * std::max(knobs.cloud_thickness_radius_scale, 0.0f), 0.1f);
            const float room = head - deck.altitude;
            if (room >= thinnest) {
                deck.thickness = room;
            } else {
                // Not even the base fits with a layer on top of it: the air is genuinely too
                // thin for a deck, and only here is lowering it the lesser evil.
                deck.thickness = std::max(std::min(thinnest, head), 0.1f);
                deck.altitude  = std::max(head - deck.thickness, 0.0f);
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    coopa::debug::Logger("clouds").warn(
                        "the cloud deck's base reached " + std::to_string(planet.radius + deck.altitude) +
                        " but the atmosphere ends at " + std::to_string(atmos->shell_outer()) +
                        "; dropped it back inside. Raise PlanetAtmosphere's radius_thickness_scale "
                        "(or thickness_scale) to give it room.");
                }
            }
        }
    }

    const float deck_radius = planet.radius + deck.altitude + deck.thickness * 0.5f;
    deck.scale = std::max(2.0f * glm::pi<float>() * deck_radius / (k_periods * k_weather_period), 1e-4f);
    return deck;
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
