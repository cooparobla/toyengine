/**
 * @file planet_atmosphere.h
 * @brief Sizes a sibling `Volume` (VolumeShape::Shell) into a sky whose HEIGHT is proportional
 *        to how tall the planet's terrain actually bakes, instead of a fixed guess.
 *
 * A planet's `height_scale` is a nominal amplitude, not a measured bound -- the world a
 * specific bake actually produces can sit anywhere under it (an archipelago is mostly near sea
 * level; a mountainous seed is not). A shell `Volume` sized to a fixed radius either clips
 * through an unusually tall peak or sits a visibly oversized distance above a flat one, and a
 * fixed *thickness* reads as a thin skin once the terrain is tall enough to stand most of the
 * way through it. This component re-measures the terrain's own baked height whenever it changes
 * and keeps the shell's radii and scale height matched to it.
 *
 * The profile it writes, given the terrain's measured `peak` above the datum and the planet's
 * nominal radius `R`:
 *
 * | Volume field     | Value                                     | Meaning for VolumeShape::Shell |
 * |------------------|-------------------------------------------|--------------------------------|
 * | `height_base`    | `R`                                       | inner radius: densest altitude |
 * | `height_falloff` | `thickness * scale_height_fraction`       | exponential scale height       |
 * | `extent`         | `base + thickness`                        | outer radius: the march's bound |
 *
 * where
 *
 *     thickness = max(min_thickness, peak * thickness_scale, R * radius_thickness_scale)
 *     base      = R + max(peak * base_altitude_scale, R * radius_base_scale)
 *
 * so a mountainous world gets a proportionally taller sky, a large or low-relief one gets a
 * sky proportional to the globe it wraps, and a near-flat small one still gets
 * `min_thickness` of air. Taking the LARGEST is what lets a planet of any radius work from
 * the same authored numbers.
 *
 * Example YAML, as a child of the planet object, beside the `Volume` it drives:
 * @code
 * - name: atmosphere
 *   components:
 *     - type: Transform
 *     - type: Volume
 *       kind: fog
 *       shape: shell
 *       occlusion: 0.0      # additive only: the night side stays transparent to the stars
 *     - type: PlanetAtmosphere
 *       thickness_scale: 1.8
 * @endcode
 */

#ifndef TOYENGINE_WORLD_PLANET_ATMOSPHERE_H
#define TOYENGINE_WORLD_PLANET_ATMOSPHERE_H

#include <cstdint>

#include <glm/glm.hpp>

#include <coopa/scene/component.h>

namespace coopa {
namespace scene {
class Scene;
}
}

namespace toy {
namespace world {

class TerrainComponent;

/** @brief Default order for install_planet_atmosphere_system(): after TerrainSystem (60). */
inline constexpr int k_planet_atmosphere_system_order = 61;

/**
 * @class PlanetAtmosphere
 * @brief Drives a sibling Volume's `extent`/`height_base`/`height_falloff` from the nearest
 *        ancestor planet terrain's measured peak height.
 *
 * Finds its terrain by walking the owner's parent chain for the nearest `TerrainComponent` --
 * the natural fit for an atmosphere authored as a child of the planet object it belongs to,
 * with no name reference to keep in sync. The work itself is a single cheap revision compare
 * most frames: it only recomputes when the terrain's own `TerraData::height_revision()` has
 * moved past what was last applied, so a re-bake or a sculpt is picked up automatically and an
 * unchanged terrain costs one integer comparison.
 *
 * Driven by install_planet_atmosphere_system(), NOT by Component::update(): the Behaviour walk
 * does not tick in edit mode, and an atmosphere that only exists once you press play is one you
 * cannot author. See that function.
 */
class PlanetAtmosphere : public coopa::scene::Component {
public:
    std::string type_name() const override { return "PlanetAtmosphere"; }

    /**
     * @brief Sky height above the tallest peak, as a multiple of that peak's own height.
     *
     * The sky scales with the world: a terrain whose mountains rise 40 units gets 72 units of
     * air above them at the default 1.8, where a fixed margin would leave the peaks standing
     * most of the way through their own atmosphere.
     */
    float thickness_scale = 1.8f;

    /** @brief Floor on the sky's height, so a near-flat world still gets an atmosphere. */
    float min_thickness = 30.0f;

    /**
     * @brief Sky height as a fraction of the PLANET'S RADIUS, taken when it is the largest.
     *
     * What makes a planet of any size work. `thickness_scale` keys the sky to the terrain's
     * relief, which says nothing about how big the world is: a 2600-unit globe with 40-unit
     * hills would get the same 72 units of air as a 100-unit one, a skin far too thin to read
     * as a limb from orbit. The thickness is the LARGEST of the three terms, so a mountainous
     * world still gets its peak-proportional sky and a large or low-relief one gets this.
     */
    float radius_thickness_scale = 0.12f;

    /**
     * @brief Altitude of the shell's BASE as a fraction of the planet's radius.
     *
     * The radius-keyed counterpart of `base_altitude_scale`, and 0 by default so an
     * atmosphere still starts at the datum. A cloud deck on a low-relief planet wants it:
     * keyed only to the peak, the deck of an ocean world would sit on the sea.
     */
    float radius_base_scale = 0.0f;

    /**
     * @brief Altitude of the shell's BASE, as a multiple of the measured peak. 0 = on the ground.
     *
     * What lets one component serve both layers a planet wants: at 0 the shell starts at the
     * datum and is an atmosphere, and at ~1 it starts just clear of the tallest peak and is a
     * cloud deck floating over the terrain. Both then track the same measured height, so a
     * re-bake that raises the mountains lifts the clouds with them instead of burying them.
     */
    float base_altitude_scale = 0.0f;

    /**
     * @brief Exponential scale height, as a fraction of the total thickness.
     *
     * Density at altitude `a` above the datum is `exp(-a / (thickness * this))`. Small values
     * pack the air into a dense skin at sea level (and white out a camera flying through it);
     * the default spreads it over most of the sky's height, which is what makes the limb read
     * as a gradient rather than a band.
     */
    float scale_height_fraction = 0.45f;

    /**
     * @brief Colour the star backdrop turns into in full daylight, deep inside the atmosphere.
     *
     * The raymarched shell alone cannot do this. A daytime sky hides the stars by being far
     * BRIGHTER than them, not by being opaque -- and any shell thick enough to actually occlude
     * them from inside is, by construction, at least as thick seen from orbit, which would veil
     * the planet itself. So the backdrop is faded toward this colour instead, exactly where a
     * real sky would be lit: see day_sky_factor().
     */
    glm::vec3 day_sky_color = glm::vec3(0.16f, 0.36f, 0.78f);

    /**
     * @brief Light the night side receives, relative to the sun. 0 = a black night (the default).
     *
     * A planet lit by one sun has a hemisphere facing away from it, and nothing in the engine
     * lights that hemisphere: the clouds' sun term is gated to zero past the terminator and the
     * only fill is the scene's `sky_zenith` family, which a `space` sky leaves near-black. The
     * result is a globe that reads as a lit crescent with its dark half simply missing -- the
     * cloud deck is still there, still occluding the stars at the limb, but emitting nothing.
     *
     * This is the moon. It lights the night hemisphere's ground, water and clouds together, so
     * the dark side reads as a moonlit world rather than a void. **0 is the default and
     * reproduces the black night side exactly**, so no existing scene changes until it asks.
     *
     * Reaches the shaders as `LightUBO::sky_extra.z` (the night floor), which both the lighting
     * pass and the cloud march already bind.
     *
     * The moon sits opposite the sun -- a full moon over the anti-solar point, which lights the
     * night hemisphere evenly and is the useful shape when the point is "the dark side is not a
     * void". Its direction is therefore derived, not authored: in world space it is simply the
     * sun's own travel direction. An independently placed moon, with a terminator of its own,
     * would need a world-space direction plumbed to both the lighting pass and the cloud march
     * and is deliberately out of scope here.
     */
    float moon_intensity = 0.0f;

    /**
     * @brief Colour the night sky fills the cloud deck with, scaled by `moon_intensity`.
     *
     * The ambient counterpart of `day_sky_color`: what a cloud sees looking around it on the
     * night side, where `day_sky_color` is what it sees at noon. The deck blends between the
     * two across the terminator, which is also what keeps the day/night edge a gradient rather
     * than a line.
     */
    glm::vec3 night_sky_color = glm::vec3(0.04f, 0.06f, 0.12f);

    /**
     * @brief Altitude at which the backdrop is FULLY daytime sky, as a multiple of the peak.
     *
     * 0 disables the backdrop fade entirely for this layer -- which is what a cloud deck wants:
     * only the atmosphere itself should be deciding what the sky looks like.
     *
     * Above it the sky fades back to stars, reaching space at the shell's outer radius. The
     * default just clears the tallest terrain, so anywhere a camera can sit on the ground reads
     * as a full sky and climbing out of the world is what brings the stars back. Keyed to the
     * peak rather than the scale height on purpose: the physical air column at a playable
     * altitude on a planet this small is thin enough to show stars, and this is the knob that
     * says "treat it as a sky anyway" instead of inflating the shell until orbit looks wrong.
     */
    float day_sky_altitude_scale = 1.2f;

    /**
     * @brief How much daytime sky lies between `camera_pos` and space, 0..1.
     *
     * The product of two terms: how deep the camera sits inside the shell (0 at the outer
     * radius, 1 at the datum) and how lit its own patch of sky is (a soft terminator on the
     * local up against the sun), so flying out of the atmosphere or around onto the night side
     * each bring the stars back, and doing both brings them back faster.
     *
     * @param camera_pos Camera position, world space.
     * @param sun_to     Unit direction TOWARD the sun, world space.
     * @return 0 when the backdrop should stay the authored star field, 1 at full daylight.
     */
    float day_sky_factor(const glm::vec3& camera_pos, const glm::vec3& sun_to) const;

    /** @brief Recomputes the sibling Volume if the terrain's height has changed. Idempotent. */
    void apply();

private:
    /** @brief Nearest ancestor TerrainComponent, resolved lazily; null until one exists. */
    TerrainComponent* terrain_ = nullptr;
    std::uint64_t applied_height_revision_ = 0;
    /** @brief Hash of the knobs apply() last used, so an edited one re-applies. */
    float applied_settings_key_ = 0.0f;
    /** @brief Shell radii from the last apply(); outer_ <= 0 means nothing has been measured. */
    float inner_ = 0.0f;
    float outer_ = 0.0f;
    float peak_  = 0.0f;
};

/**
 * @brief Installs the system that drives every PlanetAtmosphere in `scene`.
 *
 * Runs in EDIT MODE as well as play, like TerrainSystem and MapGeneratorSystem: the sky is part
 * of the thing being authored, so it has to be visible in the viewport without pressing play.
 * At order 61 it sits just after TerrainSystem (60), so a bake that lands this frame is already
 * measurable when the shell is sized from it.
 *
 * @param scene Scene to install into.
 * @param order UpdatePhase order; defaults to k_planet_atmosphere_system_order.
 */
void install_planet_atmosphere_system(coopa::scene::Scene& scene,
                                      int order = k_planet_atmosphere_system_order);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_PLANET_ATMOSPHERE_H
