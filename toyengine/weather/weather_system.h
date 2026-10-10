/**
 * @file weather_system.h
 * @brief The scene-global weather and time of day: a clock, the active weather condition with
 *        smooth transitions between conditions, and everything that follows from them -- sky,
 *        ambient light, fog, the sun / moon, runtime effects (rain, snow, mist) and a state any
 *        component can read.
 *
 * One WeatherSystem per scene (installed by Engine::prepare_scene_(), configured from the
 * scene's `scene.settings.weather`, see weather_profile.h). It does nothing until enabled.
 *
 * Each execute() (order 40: before the Behaviour walk, so gameplay reads this frame's weather):
 *  1. Clock. time_of_day advances by dt * 24 h / day length; on_hour / on_phase / on_new_day fire
 *     as it crosses them. In edit mode the clock (and the schedule) stand still unless the editor
 *     turns on its preview (set_editor_preview()).
 *  2. Schedule. Fixed stays put; Random and Cycle move on when the active condition's duration
 *     runs out (Random: a weighted pick among its `next`, or all conditions).
 *  3. Blend. A transition goes from a SNAPSHOT of whatever was showing when it started (so
 *     interrupting a transition never jumps) to the target condition's profile, over the target's
 *     `transition` seconds, eased. Numbers lerp; wind lerps as a vector; each effect's intensity
 *     lerps (the outgoing condition's rain fades out while the incoming one's snow fades in).
 *  4. Atmosphere. The clock's sky (sky_model.h) is layered with the blend: clouds grey and darken
 *     the sky, the condition scales the sun and ambient, fog takes the condition's values with
 *     its colour darkened by the time of day, and lightning flashes brighten everything briefly.
 *     The result is atmosphere(); Engine copies it into the live render config every frame (the
 *     render keys in controlled_render_keys()), and puts the config's own values back when the
 *     weather is disabled.
 *  5. Sun. With drive_sun, the scene's first directional light follows the sun by day and the
 *     moon by night (its authored values come back when the weather lets go). A scene without one
 *     gets a runtime "Sun" light.
 *  6. Effects. Every effect with intensity > 0 is an instance of its object asset under one
 *     runtime root, "Weather" (marked toy::scene::RuntimeObject: never saved, locked in the
 *     editor). Particle emission rates and volume densities are scaled by the effect's intensity,
 *     particles drift with the wind, volumes are tinted by daylight; instances follow the camera
 *     (or sit under it on the ground). An effect faded to zero is destroyed once its particles
 *     have died out.
 *
 * Reading it from gameplay:
 * @code
 * const toy::weather::WeatherState& w = toy::weather::current();   // any component, any time
 * if (w.is_night() || w.precipitation > 0.3f) lamp->intensity = 40.0f;
 *
 * auto* sys = toy::weather::find(*scene);                          // the scene's system
 * conn_ = sys->on_condition_changed.connect_scoped([](const std::string& from, const std::string& to) { ... });
 * sys->set_condition("storm");                                      // blends over storm's transition
 * sys->set_time(21.5f);                                             // jump the clock
 * @endcode
 * The WeatherReactor component (weather_reactor.h) covers the common cases with no code.
 */

#ifndef TOYENGINE_WEATHER_WEATHER_SYSTEM_H
#define TOYENGINE_WEATHER_WEATHER_SYSTEM_H

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>


#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/volume.h>

#include <toyengine/weather/ground_probe.h>
#include <toyengine/weather/sky_model.h>
#include <toyengine/weather/weather_surface.h>

namespace toy {
namespace weather {

/** @brief Order the system runs at: before kinematic control (50) and the Behaviour walk (200). */
inline constexpr int k_weather_system_order = 40;

/** @brief Name of the runtime root every weather object hangs under. */
inline constexpr const char* k_weather_root_name = "Weather";

/** @brief Coarse part of the day, from the sun's height. */
enum class DayPhase { Night, Dawn, Day, Dusk };

const char* phase_name(DayPhase p);

/**
 * @brief What the weather is right now: the clock, the condition, and the blended profile.
 *        Everything a component may want to react to.
 */
struct WeatherState {
    bool enabled = false;

    // --- Clock ---
    float hour = 12.0f;               ///< [0, 24).
    int day = 0;                      ///< Days since the scene started (increments at midnight).
    DayPhase phase = DayPhase::Day;
    float daylight = 1.0f;            ///< 0 night .. 1 full day.
    float sun_height = 1.0f;          ///< sin(sun elevation).
    glm::vec3 sun_direction{0, 0, 1}; ///< Unit direction TO the sun.
    glm::vec3 moon_direction{0, 0, -1};

    // --- Condition ---
    std::string condition;            ///< The condition the weather is in or moving to.
    std::string previous;             ///< The one before it.
    float transition = 1.0f;          ///< 0 just started blending toward `condition` .. 1 fully there.
    float condition_time = 0.0f;      ///< Seconds since `condition` was chosen.

    // --- Blended profile ---
    float cloud_cover = 0.0f;
    float precipitation = 0.0f;
    float temperature = 18.0f;
    float wetness = 0.0f;             ///< Creeps toward the condition's (wets faster than it dries).
    float snow_cover = 0.0f;          ///< 0..1 lying snow: builds while it snows below freezing, melts above it.
    float snow_depth = 0.0f;          ///< Metres of deep snow now (snow_cover * Settings::snow_max_depth).
    glm::vec3 wind{0.0f};             ///< m/s, horizontal, gusts included.
    float fog_density = 0.0f;
    float lightning_flash = 0.0f;     ///< 0..1 while a flash lights the scene.

    bool is_night() const { return phase == DayPhase::Night; }
    bool is_day() const { return phase == DayPhase::Day; }
    float wind_speed() const { return glm::length(wind); }
};

/** @brief What the weather writes into the render config (see controlled_render_keys()). */
struct Atmosphere {
    glm::vec3 sky_zenith{0.0f}, sky_horizon{0.0f}, sky_ground{0.0f};
    float ambient_intensity = 1.0f;
    float sky_intensity = 1.0f;
    float fog_density = 0.0f;
    glm::vec3 fog_color{0.5f};
    float fog_sky_blend = 0.0f;
    float fog_max_opacity = 1.0f;
    float fog_height_falloff = 0.0f;
    float fog_sun_amount = 0.0f;
    float exposure_scale = 1.0f;   ///< Multiplies the config's own exposure (darker nights).
};

/** @brief A condition's profile as numbers that blend. */
struct ProfileMix {
    float cloud_cover = 0.0f, sun = 1.0f, ambient = 1.0f;
    glm::vec3 sky_tint{1.0f};
    float fog_density = 0.0f;
    glm::vec3 fog_color{0.5f};
    float fog_height_falloff = 0.0f, fog_sky_blend = 0.0f, fog_max_opacity = 1.0f, fog_sun_amount = 0.0f;
    glm::vec2 wind{0.0f};
    float wind_gust = 0.0f, temperature = 18.0f, precipitation = 0.0f, wetness = 0.0f, lightning = 0.0f;
    std::map<std::string, float> effects;   ///< prefab -> intensity.

    static ProfileMix of(const Condition& c);
    static ProfileMix lerp(const ProfileMix& a, const ProfileMix& b, float t);
};

/**
 * @class WeatherSystem
 * @brief See the file doc.
 */
class WeatherSystem : public coopa::scene::ISceneSystem {
public:
    WeatherSystem() = default;
    ~WeatherSystem() override {
        if (active_slot_() == this) active_slot_() = nullptr;
    }

    const char* system_name() const override { return "Weather"; }
    /// Runs in the editor so the scene previews its weather (clock and schedule held -- see the file doc).
    bool runs_in_edit_mode() const override { return true; }

    // --- Signals (main thread, from inside execute()) ---
    coopa::event::Signal<const std::string&, const std::string&> on_condition_changed;  ///< (from, to), as a transition starts.
    coopa::event::Signal<int> on_hour;               ///< The clock crossed a whole hour (0..23).
    coopa::event::Signal<DayPhase> on_phase;         ///< Night / dawn / day / dusk began.
    coopa::event::Signal<int> on_new_day;            ///< Midnight: the new day's index.
    coopa::event::Signal<> on_lightning;             ///< A lightning strike (a flash starts this frame).

    /** @brief The system of the scene that updated most recently (the active scene's), or null. */
    static WeatherSystem* active() { return active_slot_(); }

    // --- Configuration ---

    /** @brief Whether precipitation at this temperature falls (and settles) as snow. */
    static bool snows(float precipitation, float temperature) { return precipitation > 0.01f && temperature < 1.0f; }

    /**
     * @brief One step of the lying-snow model: below freezing, snowfall builds cover at
     *        precipitation / accumulate_time per second; above +1 C it melts at
     *        1 / melt_time per second at +5 C, scaled with the warmth (rain on snow melts it
     *        faster still). Between, cover stays as it is.
     */
    static float advance_snow_cover(float cover, float precipitation, float temperature, float dt,
                                    float accumulate_time, float melt_time);

    /**
     * @brief Applies a scene's weather settings. The first call starts the clock at
     *        time_of_day and snaps to `condition`; later calls (live edits) keep the running
     *        state, except that a changed time_of_day or starting condition jumps there.
     */
    void set_settings(const Settings& s);
    const Settings& settings() const { return settings_; }
    /** @brief Sets the lying snow directly (editor preview, gameplay); the weather carries on from it. */
    void set_snow_cover(float cover);
    bool enabled() const { return configured_ && settings_.enabled; }

    // --- Control ---

    /**
     * @brief Moves to condition `name` over `transition_seconds` (negative: the condition's own
     *        `transition`; 0: at once). Returns false for an unknown name.
     */
    bool set_condition(const std::string& name, float transition_seconds = -1.0f);
    /** @brief The condition the weather is in or moving to. */
    const std::string& condition() const { return target_; }

    /** @brief Sets the clock (hours, wrapped to [0, 24)); fires no hour / phase signals. */
    void set_time(float hour);
    float time_of_day() const { return hour_; }
    void set_day(int day) { day_ = day; }

    /** @brief Stops (or restarts) the clock without touching day_length_minutes. */
    void set_clock_paused(bool paused) { clock_paused_ = paused; }
    bool clock_paused() const { return clock_paused_; }

    /**
     * @brief Speeds every transition up (a tool's fast-forward; 1 = as authored, <= 0 = instant).
     *        Not saved -- the editor's Preview uses it.
     */
    void set_transition_speed(float speed) { transition_speed_ = speed; }
    float transition_speed() const { return transition_speed_; }
    /** @brief Lands the running transition on its target now. */
    void finish_transition() { t_ = 1.0f; }
    /** @brief True while blending toward condition(). */
    bool transitioning() const { return t_ < 1.0f; }

    /**
     * @brief The renderer draws the physical sky (render sky_model: physical; Engine sets it each
     *        frame). The sun then shines white from above the atmosphere and the renderer colours
     *        it by the air it crosses (atmosphere_model.h), instead of the hand-tuned sunrise ramp.
     */
    void set_physical_sky(bool on) { physical_sky_ = on; }
    bool physical_sky() const { return physical_sky_; }

    /** @brief Edit mode only: lets the clock and the schedule run as they would in play. */
    void set_editor_preview(bool run) { editor_preview_ = run; }
    bool editor_preview() const { return editor_preview_; }
    /**
     * @brief Edit mode only: shows the weather's effects (rain, snow, mist...). Off by default --
     *        in the editor the effects wait for Play, while the sky, sun, fog and lying snow
     *        still preview. Not saved.
     */
    void set_preview_effects(bool show) { preview_effects_ = show; }
    bool preview_effects() const { return preview_effects_; }

    // --- Results ---

    const WeatherState& state() const { return state_; }
    const Atmosphere& atmosphere() const { return atmosphere_; }
    /** @brief The live blended profile (for tools). */
    const ProfileMix& blend() const { return current_; }
    /** @brief The precipitation height map around the viewer (see ground_probe.h). */
    const GroundProbe& ground_probe() const { return probe_; }
    /** @brief The runtime root effects hang under, or null while none exists. */
    coopa::scene::SceneObject* root() const { return root_; }

    // --- ISceneSystem ---

    void on_detach(coopa::scene::Scene& /*scene*/) override { release_sun_(); }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

private:
    // --- Clock ---------------------------------------------------------------------------

    void advance_clock_(float dt);

    // --- Schedule ------------------------------------------------------------------------

    void pick_duration_(const Condition& c) {
        duration_s_ = 60.0f * rng_.range(c.min_minutes, std::max(c.min_minutes, c.max_minutes));
    }

    void advance_schedule_(float dt);

    void snap_to_(const std::string& name);

    // --- Blend ---------------------------------------------------------------------------

    void advance_blend_(float dt);

    void advance_lightning_(float dt);
    /** @brief A strike's brightness: a sharp flash and a weaker second one ~0.2 s later. */
    float flash_() const;

    // --- State and atmosphere ------------------------------------------------------------

    void compute_state_(float dt);

    // --- Sun -----------------------------------------------------------------------------

    void drive_sun_(coopa::scene::Scene& scene);

    /** @brief Hands the authored light back as it was (a runtime sun is removed with the root). */
    void release_sun_();

    // --- Effects -------------------------------------------------------------------------

    struct ParticleBase {
        toy::particles::ParticleSystem* ps = nullptr;
        float rate = 0.0f;
        glm::vec3 velocity{0.0f};
        float ground_height = 0.0f;
        std::vector<toy::particles::SubEmitter> on_death;   ///< Its splashes; cleared while ground effects are off.
    };
    struct VolumeBase {
        coopa::gfx::engine::components::VolumeComponent* vol = nullptr;
        float density = 0.0f, speed = 0.0f, height_base = 0.0f;
        glm::vec3 color{1.0f}, direction{1, 0, 0};
    };
    struct EffectInstance {
        coopa::scene::SceneObject* object = nullptr;
        EffectSpec spec;
        std::vector<ParticleBase> particles;
        std::vector<VolumeBase> volumes;
        struct Distant { WeatherDistantLandings* comp = nullptr; toy::particles::ParticleSystem* ps = nullptr; float carry = 0.0f; };
        std::vector<Distant> distant;   ///< Landings shown past the drops' own box (weather_surface.h).
        float idle = 0.0f;   ///< Seconds at zero intensity.
    };

    coopa::scene::SceneObject* ensure_root_(coopa::scene::Scene& scene);

    /** @brief The spec an effect is placed by: the target condition's, else any condition's. */
    const EffectSpec* spec_for_(const std::string& prefab) const;

    static glm::vec3 viewer_position_();

    bool spawn_effect_(coopa::scene::Scene& scene, const std::string& prefab, const EffectSpec& spec);

    /** @param show False (edit mode without the Preview's effects): no effect exists -- the
     *         ones there go at once -- but the precipitation map still runs for lying snow. */
    void update_effects_(coopa::scene::Scene& scene, float dt, bool show);

    /**
     * @brief Landings past a precipitation system's own wrap box: points scattered over the disc
     *        of WeatherDistantLandings::radius (outside the box, where real drops land), at the
     *        rate real drops land per square metre (each drop born in the box lands about once:
     *        rate / box area), each firing the drops' sub emitters on a splash-taking surface of
     *        the precipitation map, lying in its normal.
     */
    void emit_distant_landings_(EffectInstance& inst, float dt);

    /** @brief Disabled: the sun goes back, every runtime object goes away. */
    void shut_down_(coopa::scene::Scene& scene);

    static WeatherSystem*& active_slot_();

    Settings settings_;
    bool configured_ = false;

    float hour_ = 10.0f;
    int day_ = 0;
    int last_hour_ = 10;
    DayPhase phase_ = DayPhase::Day;
    bool phase_known_ = false;
    bool clock_paused_ = false;
    bool editor_preview_ = false;
    bool preview_effects_ = false;
    bool physical_sky_ = false;   ///< See set_physical_sky().
    SkyFrame sky_;

    std::string target_;
    ProfileMix from_, current_;
    float t_ = 1.0f;
    float transition_s_ = 0.0f;
    float condition_time_ = 0.0f;
    float duration_s_ = 300.0f;
    float transition_speed_ = 1.0f;

    toy::particles::Rng rng_{0x5eed5u, 7};
    float gust_clock_ = 0.0f;
    float wetness_ = 0.0f;
    float snow_cover_ = 0.0f;
    float flash_t_ = -1.0f;
    float light_level_ = 1.0f;

    WeatherState state_;
    Atmosphere atmosphere_;

    coopa::scene::SceneObject* root_ = nullptr;
    std::map<std::string, EffectInstance> effects_;
    std::unordered_set<std::string> failed_;
    GroundProbe probe_;

    struct SavedLight { glm::vec3 direction{0, 0, -1}; glm::vec3 color{1.0f}; float intensity = 1.0f; };
    coopa::gfx::engine::components::DirectionalLightComponent* driven_sun_ = nullptr;
    bool owns_sun_ = false;
    SavedLight sun_saved_;
};

/** @brief `scene`'s weather system, or null. */
inline WeatherSystem* find(const coopa::scene::Scene& scene) {
    return dynamic_cast<WeatherSystem*>(scene.find_system("Weather"));
}

/**
 * @brief The active scene's weather right now; a default state (disabled, noon, clear) when no
 *        scene has weather. Safe from any component at any time on the main thread.
 */
const WeatherState& current();

/** @brief Installs a WeatherSystem configured from a `scene.settings.weather` node (null: disabled). */
WeatherSystem* install_weather_system(coopa::scene::Scene& scene, const fkyaml::node& weather_node,
                                             int order = k_weather_system_order);

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_WEATHER_SYSTEM_H
