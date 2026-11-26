/**
 * @file sky_state.h
 * @brief What the physical sky (render `sky_model: physical`) needs from outside the renderer
 *        each frame: the atmosphere's media, the sun and moon, and the light levels the CPU
 *        atmosphere model (toyengine/weather/atmosphere_model.h) worked out for them.
 *
 * Plain data, like WaterFrameState / SurfaceFrameState: Engine fills it every frame
 * (Engine::sync_sky_render_state_()) and hands it over with ToyRenderPipeline::set_sky_state(),
 * so render/ never depends on the weather module.
 */

#ifndef TOYENGINE_RENDER_SKY_STATE_H
#define TOYENGINE_RENDER_SKY_STATE_H

#include <array>

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/env/world_frame.h>

namespace toy {
namespace render {

/**
 * @brief The atmosphere's scattering media, in kilometres (Hillaire 2020's Earth defaults).
 *
 * Shared by the GPU LUT passes (as SkyAtmospherePass::AtmosphereGpu) and the CPU model, so the
 * two can never describe different skies.
 */
struct SkyAtmosphereMedia {
    glm::vec3 rayleigh_scattering{5.802e-3f, 13.558e-3f, 33.1e-3f};  ///< 1/km at sea level.
    float     rayleigh_scale_height = 8.0f;                          ///< km.
    glm::vec3 mie_scattering{3.996e-3f};                             ///< 1/km at sea level.
    glm::vec3 mie_extinction{4.40e-3f};                              ///< 1/km at sea level.
    float     mie_scale_height = 1.2f;                               ///< km.
    float     mie_g = 0.8f;                                          ///< Forward-scattering asymmetry.
    glm::vec3 ozone_absorption{0.650e-3f, 1.881e-3f, 0.085e-3f};     ///< 1/km at the layer's peak (25 km).
    float     bottom_radius = 6360.0f;                               ///< Ground radius, km.
    float     top_radius = 6460.0f;                                  ///< Top of the atmosphere, km.
    glm::vec3 ground_albedo{0.3f};

    /**
     * @brief The Earth defaults with render `atmosphere_density` (haze: multiplies the aerosols,
     *        and a little of the air) and `ozone` (multiplies the ozone layer) applied.
     */
    static SkyAtmosphereMedia earth(float density, float ozone) {
        SkyAtmosphereMedia m;
        const float d = glm::clamp(density, 0.0f, 20.0f);
        m.mie_scattering *= d;
        m.mie_extinction *= d;
        // Thick haze also scatters more broadly (a whiter, lower-contrast sky).
        m.mie_g = glm::mix(0.8f, 0.7f, glm::clamp((d - 1.0f) / 9.0f, 0.0f, 1.0f));
        m.rayleigh_scattering *= glm::mix(1.0f, glm::clamp(d, 0.5f, 2.0f), 0.25f);
        m.ozone_absorption *= glm::clamp(ozone, 0.0f, 10.0f);
        return m;
    }

    bool operator==(const SkyAtmosphereMedia& o) const {
        return rayleigh_scattering == o.rayleigh_scattering && rayleigh_scale_height == o.rayleigh_scale_height &&
               mie_scattering == o.mie_scattering && mie_extinction == o.mie_extinction &&
               mie_scale_height == o.mie_scale_height && mie_g == o.mie_g && ozone_absorption == o.ozone_absorption &&
               bottom_radius == o.bottom_radius && top_radius == o.top_radius && ground_albedo == o.ground_albedo;
    }
    bool operator!=(const SkyAtmosphereMedia& o) const { return !(*this == o); }
};

/**
 * @brief One additional sun disc in the sky, beyond the built-in `sun_to` pair.
 */
struct SkySunState {
    glm::vec3 dir{0.0f, 0.0f, 1.0f};  ///< Unit direction TO the sun, world space.
    glm::vec3 color{1.0f};            ///< The disc's tint.
    float disc_cos = 1.0f;            ///< cos of the disc's angular radius.
    float disc_radiance = 0.0f;       ///< The disc's brightness.
};

/**
 * @brief One additional moon disc in the sky: a lit sphere with a phase and an optional face
 *        sampled from the sky-body atlas.
 */
struct SkyMoonState {
    glm::vec3 dir{0.0f, 0.0f, -1.0f};       ///< Unit direction TO the moon, world space.
    glm::vec3 tint{0.95f, 0.96f, 1.0f};     ///< The disc's tint.
    float disc_cos = 1.0f;                  ///< cos of the disc's angular radius.
    float disc_radiance = 0.0f;             ///< The disc's brightness before its phase shading.
    glm::vec3 lit_from{0.0f, 0.0f, 1.0f};   ///< Unit direction from the moon TO its sun.
    /// The moon's face in the sky-body atlas (`Engine::update_sky_body_atlas`): xy = uv offset,
    /// zw = uv size. zw == 0 draws a procedural face.
    glm::vec4 atlas_rect{0.0f};
    /// The procedural fallback face when `atlas_rect.zw` is zero: false = cratered maria,
    /// true = latitude bands (a body with no solid surface).
    bool banded = false;
    /// The thin scattering shell hugging the body's limb: a tinted rim inside the disc's edge
    /// and a soft halo just past it, brightest when the body is backlit. `atmo_thickness` is the
    /// shell's thickness as a fraction of the disc's radius; 0 (the default) draws no shell.
    glm::vec3 atmo_tint{0.0f};
    float atmo_thickness = 0.0f;
};

/**
 * @brief One frame of the physical sky. `active` false (the default) draws the gradient sky.
 */
struct SkyFrameState {
    bool active = false;              ///< The physical sky is on and these values are this frame's.
    SkyAtmosphereMedia media;

    glm::vec3 sun_to{0.0f, 0.0f, 1.0f};    ///< Unit direction TO the sun.
    glm::vec3 moon_to{0.0f, 0.0f, -1.0f};  ///< Unit direction TO the moon (the sun's opposite).
    float sky_illuminance = 1.0f;     ///< Sun illuminance the sky-view LUT is scaled by (engine units).
    float moon_ratio = 0.0f;          ///< Moon illuminance / sun illuminance, for the LUT's moonlit sky.
    float sun_disc_radiance = 0.0f;   ///< The sun disc's brightness above the atmosphere.
    float moon_disc_radiance = 0.0f;  ///< The moon disc's brightness above the atmosphere.
    float sun_disc_cos = 1.0f;        ///< cos of the sun disc's angular radius.
    float moon_disc_cos = 1.0f;       ///< cos of the moon disc's angular radius.
    float star_visibility = 0.0f;     ///< 0 daylight / overcast .. 1 a clear night.
    float night_floor = 0.0f;         ///< Airglow floor so a moonless night sky is not pure black.

    /// Multiplies the directional light's colour wherever the renderer reads it (lighting, fog,
    /// volumetrics, water): the air's transmittance toward the light, and the cloud cover's dimming.
    glm::vec3 light_tint{1.0f};

    float time = 0.0f;                ///< Seconds; star twinkle.

    /// The star backdrop's structure: the galactic band's pole (world space, unit) and the
    /// field's variation seed (any value in [0, 1024)). The defaults give every scene the same
    /// tilted band; a sky-state hook may retune both per world.
    glm::vec3 backdrop_to{0.853f, 0.201f, 0.482f};
    float backdrop_seed = 0.0f;

    /// At most this many additional suns / moons are drawn.
    static constexpr int k_max_suns = 4;
    static constexpr int k_max_moons = 8;

    /// Additional sun and moon discs, filled by the sky-state hook. Counts of zero (the default)
    /// keep the single `sun_to` / `moon_to` pair above authoritative -- exactly the behaviour a
    /// scene that never touches these gets.
    std::array<SkySunState, k_max_suns> suns{};
    int sun_count = 0;
    std::array<SkyMoonState, k_max_moons> moons{};
    int moon_count = 0;

    /// WORLD-space unit direction TO the brightest moon, for the night-side ground light; zero
    /// keeps the anti-solar fallback (the single moon sits opposite the sun).
    glm::vec3 moonlight_to{0.0f};
    /// The night-side fill's tint, and with it the AGGREGATE hue of whatever is reflecting.
    ///
    /// Read by the terms that take a scalar and a tint rather than a direction -- the sea's night
    /// sheen mirrors the whole sky, so a direction means nothing to it. A producer publishing
    /// `reflected` should set this to the summed radiance's hue, or those terms keep this default
    /// while the ground beside them is lit the colour of the body that actually cast the light.
    glm::vec3 moonlight_tint{0.55f, 0.75f, 1.3f};

    /// A sphere that may stand between a shaded point and the directional light.
    struct Occluder {
        glm::vec3 center{0.0f};   ///< WORLD space -- the frame the lighting pass shades in.
        float radius = 0.0f;      ///< 0 is an empty slot.
    };
    /// At most this many are shadowed against; see `LightUBO::dir_occluders`.
    static constexpr int k_max_occluders = 4;
    /// Spheres shadowing the directional light ANALYTICALLY -- no map, and so no range limit.
    /// The cascades are fitted to the camera's own surroundings; an eclipse is cast by something
    /// tens of thousands of units away, and the two problems have nothing in common. A count of
    /// 0 (the default) costs the lighting pass one compare.
    std::array<Occluder, k_max_occluders> occluders{};
    int occluder_count = 0;
    /// The directional light's own angular radius, radians. 0 (the default) disables the term --
    /// a light with no disc has no penumbra and nothing to partially cover.
    float light_angular_radius = 0.0f;
    /// The smallest fraction of the directional light an occluder may leave, 0..1.
    ///
    /// The occluder spheres above cover the light's DISC, so a sphere wide enough to cover all of
    /// it takes the direct term to exactly zero -- correct for a world with no air, and wrong for
    /// one with air, where the shaded point still sees a sky lit by the light the sphere never
    /// covered. On a world whose horizon is far away that lit sky is most of the hemisphere even
    /// at totality, which is why a total eclipse reads as deep twilight rather than as night.
    ///
    /// Scales with how much air there is, so 0 (the default) is an airless world and a flat scene
    /// alike, and the term stays exactly what it was before.
    float occluder_light_floor = 0.0f;

    /// One body bouncing the directional light back onto the world being shaded.
    ///
    /// The night-side fill, generalised from one source to several. `moonlight_to`,
    /// `moonlight_tint` and `night_floor` above are the single-source form and keep working
    /// exactly as they did: a count of 0 (the default) leaves them authoritative, which is what
    /// every flat scene gets and costs it one compare.
    ///
    /// Each slot shades as a light in its own right, so a body low in a point's sky lights it
    /// less than one overhead -- which a single averaged fill cannot say -- and carries its own
    /// colour rather than a fixed tint. WORLD space, never rotated into the sky's camera frame:
    /// the lighting pass shades world positions.
    struct ReflectedLight {
        glm::vec3 to{0.0f};        ///< Unit direction TO the reflecting body.
        glm::vec3 radiance{0.0f};  ///< Its reflected light here, tinted. 0 is an empty slot.
    };
    /// At most this many are shaded; see `LightUBO::sky_reflected_dirs`.
    static constexpr int k_max_reflected = 4;
    std::array<ReflectedLight, k_max_reflected> reflected{};
    int reflected_count = 0;
};

/// The cloud layer's two looks (render cloud_type).
enum class CloudType { Volumetric, Flat };

/**
 * @brief One frame of the cloud layer (render clouds), with either sky model: the engine fills
 *        it from the render config, the weather and the scene's light every frame.
 */
struct CloudFrameState {
    bool active = false;              ///< Draw the layer (and its shadows) this frame.
    CloudType type = CloudType::Volumetric;
    float coverage = 0.0f;            ///< 0..1.
    float altitude = 1500.0f;         ///< m; the layer's base. World z on a flat world, ALTITUDE above the datum on a curved one.
    float thickness = 1500.0f;        ///< m.
    float density = 1.0f;             ///< Volumetric extinction multiplier.
    float scale = 1.0f;               ///< Volumetric: the cloud field's size relative to a real sky's.
    glm::dvec2 offset{0.0};           ///< Accumulated wind drift (m), unwrapped: each use wraps it to its own periods.
    /// The wind driving that drift: unit direction in world XY, and its speed (m/s) after the
    /// weather's own gust scaling. Published because a layer that cannot SLIDE needs the same
    /// wind expressed its own way -- a shell turns about its pole at this speed -- and
    /// recomputing it from the config would drift from the offset above.
    glm::vec2 wind_dir{1.0f, 0.0f};
    float wind_speed = 0.0f;
    float time = 0.0f;                ///< Seconds; the clouds' evolution.
    glm::vec3 light_to{0.0f, 0.0f, 1.0f};  ///< The light the clouds are lit by (sun, or the moon at night).
    glm::vec3 light_color{0.0f};           ///< Its illuminance at the layer (engine units).
    /// The physical sky's transmittance table colours light_color by the air below the layer
    /// (light_color is then the light above the atmosphere); false: light_color is used as is.
    bool atmosphere_lut = false;
    float fade = 1.0f;                ///< Camera fade (render cloud_camera_fade): 0 hidden .. 1 shown. Shadows ignore it.

    /// The scene's curved world, or a flat frame (the default). When curved the layer is a real
    /// spherical shell about its centre rather than one pinned under the camera, and
    /// `altitude` is measured from the datum. See toyengine/env/world_frame.h.
    env::WorldFrame world_frame{};
    /// Curved worlds only: the deck turns rigidly about this axis (the pole) by `spin_angle`,
    /// which is how a wind moves clouds on a sphere -- `offset` above is a flat world's
    /// translation of the same field and the two are never both live. `spin_delta` is this
    /// frame's step, which the temporal reconstruction reprojects the history by.
    glm::vec3 spin_axis{0.0f, 0.0f, 1.0f};
    double spin_angle = 0.0;
    float spin_delta = 0.0f;
    /// Curved worlds only: the colours the deck's ambient blends between across the terminator --
    /// what a cloud sees looking around it at noon, and on the night side. On a `space` sky the
    /// scene's own sky_zenith family is the near-black STAR backdrop, which lights the deck
    /// with nothing and leaves the day side lit by direct sun alone; these come from the
    /// whatever fills them through the cloud-state hook instead.
    glm::vec3 day_fill{0.0f};
    glm::vec3 night_fill{0.0f};

    // --- What the clouds are MADE of --------------------------------------------------------
    //
    // Every default below is what the layer drew before these existed, so a scene that sets none
    // of them is untouched: vec3(0.9) is the scalar albedo the march carried, and a zero band
    // count or storm count skips that term entirely.

    /// Single-scattering albedo where a cloud is thin and lit -- the colour of its bright edge.
    glm::vec3 albedo{0.9f};
    /// The albedo the blend reaches: a band's own colour on a giant, a thick base's grey on an
    /// earthlike world.
    glm::vec3 albedo_deep{0.9f};
    /// How fast a sample's own density carries it from one albedo to the other. 0 = never, which
    /// leaves `albedo` alone everywhere. Unused on a banded world, where the BAND chooses.
    float deep_onset = 0.0f;

    /// x = belts from pole to pole (0 = none, and bands and jets both switch off), y = contrast
    /// of a belt against a zone, z = sharpness (>1 squares them off), w = jet shear: how much
    /// faster a belt turns than the deck as a whole. Curved worlds only -- a flat world has no
    /// pole to band about.
    glm::vec4 bands{0.0f};

    /// A cyclone: a swirl, a raised wall and a cleared eye. The same four numbers are a
    /// hurricane on an earthlike world and a Great Red Spot on a giant. Curved worlds only.
    struct Storm {
        glm::vec3 direction{0.0f, 0.0f, 1.0f};  ///< Unit, from the world's centre to the eye.
        float angular_radius = 0.0f;            ///< Radians of arc from the eye to the rim.
        float swirl = 0.0f;                     ///< How far the field winds round, at the wall (rad).
        float lift = 0.0f;                      ///< Coverage the storm adds over its whole disc.
        float eye = 0.3f;                       ///< Fraction of the radius that is cleared eye.
        float wall = 0.0f;                      ///< Extra coverage in the ring around that eye.
    };
    /// Live cyclones; `SkyCloudPass::kMaxStorms` of them reach the march, nearest-first.
    std::vector<Storm> storms;

    /// Seeds the weather map that PLACES the clouds. Changing it re-bakes a 512^2 compute and
    /// cuts the temporal history for a frame; 0 is the map the engine always baked, so two
    /// worlds in one scene can wear different weather instead of one pattern at two scales.
    std::uint32_t weather_seed = 0;

    // Shadows (render cloud_shadows).
    bool shadows = false;
    float shadow_strength = 1.0f;
    float shadow_distance = 4000.0f;  ///< m, the shadow map's side, centred on the camera.

    // Flat look.
    float flat_size = 30.0f;
    float flat_opacity = 0.92f;
    float flat_light_bands = 3.0f;
    float flat_outline = 0.5f;
    float flat_turbulence = 0.5f;
    float flat_evolve = 1.0f;
    float flat_phase = 0.0f;          ///< The puffs' evolution, 0..1 (wraps seamlessly): time * flat_evolve, accumulated.
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_SKY_STATE_H
