/**
 * @file weather_profile.h
 * @brief The weather module's data: a scene's weather settings (clock, sky palette, schedule)
 *        and its catalogue of conditions, each with the profile that applies while it is active.
 *
 * A scene keeps its weather under `scene.settings.weather` -- beside the render / physics
 * overrides, so it reaches the Engine through the same per-scene settings path (load, play mode,
 * live editor edits). It is not a config.yaml section: weather is per scene.
 *
 * @code
 * scene:
 *   settings:
 *     weather:
 *       enabled: true
 *       time_of_day: 9.5          # hours, where the clock starts
 *       day_length_minutes: 24    # real minutes per game day; 0 stops the clock
 *       condition: clear          # the condition the scene starts in
 *       schedule: random          # fixed | random | cycle
 *       conditions:               # omitted: default_conditions()
 *         - name: rain
 *           weight: 1.0           # random schedule: how likely it is picked
 *           duration: [3, 8]      # real minutes it lasts (random schedule / cycle)
 *           transition: 25        # seconds to blend in
 *           next: [overcast, storm]   # what may follow it (empty: anything)
 *           cloud_cover: 0.9
 *           sun: 0.3              # x the clock's sun
 *           ...
 *           effects:
 *             - {prefab: objects/weather_rain, follow: camera, offset: {x: 0, y: 0, z: 4}}
 * @endcode
 *
 * Everything here is plain data with YAML in and out; weather_system.h does the work.
 */

#ifndef TOYENGINE_WEATHER_WEATHER_PROFILE_H
#define TOYENGINE_WEATHER_WEATHER_PROFILE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

namespace toy {
namespace weather {

/** @brief Where a weather effect sits. */
enum class EffectAnchor {
    Camera,   ///< At the viewer + offset (rain and snow fall around whoever is looking).
    Ground,   ///< Under the viewer, at the weather's ground height + offset.z (mist banks).
    World,    ///< Fixed at offset.
};

/** @brief One runtime effect a condition spawns: an object asset, faded by the condition's weight. */
struct EffectSpec {
    std::string prefab;               ///< Object asset, e.g. "objects/weather_rain".
    float intensity = 1.0f;           ///< Scales particle emission and volume density.
    EffectAnchor follow = EffectAnchor::Camera;
    glm::vec3 offset{0.0f};
    float wind_influence = 1.0f;      ///< How much of the wind is added to particle velocity / volume drift.
};

/** @brief A weather condition and its profile. Numbers blend linearly between conditions. */
struct Condition {
    std::string name = "clear";

    // --- Schedule ---
    float weight = 1.0f;              ///< Random schedule: relative chance of being picked (0 = only on request).
    float min_minutes = 4.0f;         ///< How long it lasts before the schedule moves on, real minutes...
    float max_minutes = 10.0f;        ///< ...picked between these.
    float transition = 20.0f;         ///< Seconds to blend in from whatever was active.
    std::vector<std::string> next;    ///< Conditions that may follow it; empty = any.

    // --- Sky and light ---
    float cloud_cover = 0.0f;         ///< 0..1: greys and darkens the sky; drives render cloud_coverage (physical sky clouds).
    float sun = 1.0f;                 ///< Multiplier on the clock's sun / moon light.
    float ambient = 1.0f;             ///< Multiplier on the sky's ambient light.
    glm::vec3 sky_tint{1.0f};         ///< Multiplies the sky gradient.

    // --- Fog (global, render fog_*; colours are daylight colours, the clock darkens them) ---
    float fog_density = 0.0034f;
    glm::vec3 fog_color{0.68f, 0.75f, 0.84f};
    /// Metres over which the fog thins by e above the render config's fog_height_base; 0 = uniform
    /// fog. Uniform fog also fills the sky at every elevation (sky pixels integrate to
    /// fog_sky_distance), so a condition keeps a falloff unless it means to white out the sky.
    float fog_height_falloff = 40.0f;
    float fog_sky_blend = 0.6f;
    float fog_max_opacity = 1.0f;
    float fog_sun_amount = 0.4f;

    // --- Atmosphere (read by gameplay and effects) ---
    float wind_strength = 1.5f;       ///< m/s.
    float wind_heading = 30.0f;       ///< Degrees the wind blows TOWARD, counter-clockwise from +X.
    float wind_gust = 0.2f;           ///< 0..1: how much the wind varies.
    float temperature = 18.0f;        ///< Degrees C.
    float precipitation = 0.0f;       ///< 0..1: how hard it rains / snows.
    float wetness = 0.0f;             ///< 0..1: how wet surfaces get (WeatherState::wetness creeps toward it).
    float lightning = 0.0f;           ///< Flashes per minute.

    std::vector<EffectSpec> effects;
};

/** @brief How the active condition changes by itself. */
enum class Schedule {
    Fixed,    ///< Stays on `condition` until code calls set_condition().
    Random,   ///< After each condition's duration, a weighted pick among its `next` (or all).
    Cycle,    ///< After each condition's duration, the next one in the list.
};

/** @brief A scene's weather: `scene.settings.weather`. */
struct Settings {
    bool enabled = false;

    // --- Clock ---
    float time_of_day = 10.0f;        ///< Hours [0, 24) the clock starts at.
    float day_length_minutes = 24.0f; ///< Real minutes per game day; 0 = time stands still.
    float latitude = 35.0f;           ///< Degrees: tilts the sun's path (0 = overhead at noon).
    float north_offset = 0.0f;        ///< Degrees: turns the whole sun path about Z (sunrise toward +X at 0).
    bool drive_sun = true;            ///< Aim and colour the scene's directional light (sun by day, moon by night).
    float sun_intensity = 1.2f;       ///< The sun's intensity at noon in clear weather.
    float moon_intensity = 0.12f;
    glm::vec3 moon_color{0.55f, 0.65f, 0.9f};

    // --- Sky palette (render sky_*; weather blends these by the sun's height) ---
    glm::vec3 day_zenith{0.05f, 0.18f, 0.55f};
    glm::vec3 day_horizon{0.25f, 0.35f, 0.45f};
    glm::vec3 day_ground{0.05f, 0.045f, 0.04f};
    glm::vec3 twilight_zenith{0.12f, 0.12f, 0.30f};
    glm::vec3 twilight_horizon{0.85f, 0.42f, 0.20f};
    glm::vec3 night_zenith{0.004f, 0.007f, 0.02f};
    glm::vec3 night_horizon{0.015f, 0.022f, 0.045f};
    glm::vec3 night_ground{0.006f, 0.006f, 0.008f};
    float ambient_day = 1.0f;         ///< render ambient_intensity / sky_intensity at noon.
    float ambient_night = 0.25f;      ///< ... at midnight.
    float night_exposure = 0.35f;     ///< Multiplier on render exposure at midnight (keeps auto exposure from lifting night to day).

    // --- Schedule ---
    std::string condition = "clear";  ///< The condition the scene starts in.
    Schedule schedule = Schedule::Fixed;
    uint32_t seed = 0;                ///< Random schedule / lightning / gusts; 0 = fixed default seed.
    float ground_height = 0.0f;       ///< World Z effects treat as the ground (mist, precipitation collision).
    bool surface_collision = true;    ///< Rain / snow stop on real surfaces (roofs, terrain, water) -- see ground_probe.h; off: the ground_height plane.
    bool ground_effects = true;       ///< Splashes where rain lands, snow settling: the effects' sub emitters.
    bool ground_height_splashes = false; ///< The ground_height plane takes splashes where no object is below
                                         ///< (objects opt in with a WeatherSurface component).

    // --- Snow cover (WeatherState::snow_cover; drawn by gfx/surface/snow.glsl) ---
    float snow_accumulate_time = 180.0f; ///< Seconds of full snowfall (precipitation 1, below freezing) to full cover.
    float snow_melt_time = 240.0f;       ///< Seconds full cover takes to melt at +5 C (faster when warmer).
    float snow_max_depth = 0.3f;         ///< Metres of deep snow (the `snow` surface shader) at full cover.
    float initial_snow_cover = 0.0f;     ///< Cover the scene starts with (0..1); a snowy starting condition starts at 1.
    bool snow_auto_deformers = false;    ///< Every Rigidbody leaves tracks in deep snow, not only SnowDeformer objects (world/snow_system.h).
    float snow_trench_recover_time = 2.0f; ///< Seconds a full-depth track takes to fill back in (0: only while it snows).
    bool snow_patch_hard = false;        ///< `snow_patch_style: hard` -- round, crisp-edged (toon) patches that grow and
                                         ///< merge; `soft` (default) -- the soft noise-edged cover.
    float snow_patch_size = 1.5f;        ///< Hard patches: typical patch diameter (m).

    std::vector<Condition> conditions;   ///< Empty in YAML: default_conditions().

    /** @brief The condition named `n`, or null. */
    const Condition* find(const std::string& n) const;
    int index_of(const std::string& n) const;
};

/**
 * @brief The config.yaml `render` keys the weather writes every frame while it is enabled. The
 *        editor locks these rows (they would be overwritten anyway) and points at the World tab.
 */
const std::vector<std::string>& controlled_render_keys();
bool controls_render_key(const std::string& key);

// =====================================================================================
// Defaults
// =====================================================================================

namespace detail {
EffectSpec effect(std::string prefab, float intensity, EffectAnchor follow, glm::vec3 offset, float wind = 1.0f);
} // namespace detail

/**
 * @brief The stock catalogue: the weather an RPG world usually wants. clear / cloudy / overcast /
 *        fog / rain / storm cycle among themselves on the random schedule; snow, blizzard and
 *        sandstorm have weight 0 (biome weather: set them from code or give them a weight).
 */
std::vector<Condition> default_conditions();

// =====================================================================================
// YAML
// =====================================================================================

namespace yaml_detail {

float num(const fkyaml::node& n, float def);
inline float f(const fkyaml::node& m, const char* k, float def) { return m.contains(k) ? num(m.at(k), def) : def; }
bool b(const fkyaml::node& m, const char* k, bool def);
inline std::string s(const fkyaml::node& m, const char* k, const std::string& def) {
    return m.contains(k) && m.at(k).is_string() ? m.at(k).get_value<std::string>() : def;
}
/** @brief {r, g, b} or [r, g, b]. */
glm::vec3 rgb(const fkyaml::node& m, const char* k, glm::vec3 def);
glm::vec3 xyz(const fkyaml::node& m, const char* k, glm::vec3 def);
fkyaml::node out_rgb(glm::vec3 c);
fkyaml::node out_xyz(glm::vec3 v);
/** @brief Rounds a float for writing, so 0.1f is saved as 0.1 rather than 0.100000001. */
inline double out_f(float v) { return static_cast<double>(std::round(static_cast<double>(v) * 1e5) / 1e5); }

inline const char* anchor_name(EffectAnchor a) {
    return a == EffectAnchor::Ground ? "ground" : a == EffectAnchor::World ? "world" : "camera";
}
EffectAnchor anchor_of(const std::string& s);
inline const char* schedule_name(Schedule s) {
    return s == Schedule::Random ? "random" : s == Schedule::Cycle ? "cycle" : "fixed";
}
inline Schedule schedule_of(const std::string& s) {
    return s == "random" ? Schedule::Random : s == "cycle" ? Schedule::Cycle : Schedule::Fixed;
}

} // namespace yaml_detail

EffectSpec parse_effect(const fkyaml::node& n);

Condition parse_condition(const fkyaml::node& n);

/** @brief A weather block (`scene.settings.weather`); a null / non-map node gives the defaults, disabled. */
Settings parse_settings(const fkyaml::node& n);

fkyaml::node to_node(const EffectSpec& e);

/** @brief A condition as YAML, every key written (the editor's "add condition" and defaults). */
fkyaml::node to_node(const Condition& c);

/** @brief The whole weather block as YAML (conditions included). */
fkyaml::node to_node(const Settings& st);

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_WEATHER_PROFILE_H
