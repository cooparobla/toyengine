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
 * | `extent`         | `R + peak + thickness`                    | outer radius: the march's bound |
 *
 * where `thickness = max(min_thickness, peak * thickness_scale)` -- so a mountainous world gets
 * a proportionally taller sky and a flat one still gets `min_thickness` of air.
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
     * @brief Exponential scale height, as a fraction of the total thickness.
     *
     * Density at altitude `a` above the datum is `exp(-a / (thickness * this))`. Small values
     * pack the air into a dense skin at sea level (and white out a camera flying through it);
     * the default spreads it over most of the sky's height, which is what makes the limb read
     * as a gradient rather than a band.
     */
    float scale_height_fraction = 0.45f;

    /** @brief Recomputes the sibling Volume if the terrain's height has changed. Idempotent. */
    void apply();

private:
    /** @brief Nearest ancestor TerrainComponent, resolved lazily; null until one exists. */
    TerrainComponent* terrain_ = nullptr;
    std::uint64_t applied_height_revision_ = 0;
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
