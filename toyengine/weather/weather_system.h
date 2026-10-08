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

#include <coopa/event/signal.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>

#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/volume.h>

#include <toyengine/particles/particle_math.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/weather/ground_probe.h>
#include <toyengine/weather/sky_model.h>
#include <toyengine/weather/weather_surface.h>
#include <toyengine/weather/weather_profile.h>

namespace toy {
namespace weather {

/** @brief Order the system runs at: before kinematic control (50) and the Behaviour walk (200). */
inline constexpr int k_weather_system_order = 40;

/** @brief Name of the runtime root every weather object hangs under. */
inline constexpr const char* k_weather_root_name = "Weather";

/** @brief Coarse part of the day, from the sun's height. */
enum class DayPhase { Night, Dawn, Day, Dusk };

inline const char* phase_name(DayPhase p) {
    switch (p) {
        case DayPhase::Night: return "night";
        case DayPhase::Dawn: return "dawn";
        case DayPhase::Day: return "day";
        case DayPhase::Dusk: return "dusk";
    }
    return "day";
}

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

    static ProfileMix of(const Condition& c) {
        ProfileMix m;
        m.cloud_cover = c.cloud_cover; m.sun = c.sun; m.ambient = c.ambient; m.sky_tint = c.sky_tint;
        m.fog_density = c.fog_density; m.fog_color = c.fog_color; m.fog_height_falloff = c.fog_height_falloff;
        m.fog_sky_blend = c.fog_sky_blend; m.fog_max_opacity = c.fog_max_opacity; m.fog_sun_amount = c.fog_sun_amount;
        const float a = glm::radians(c.wind_heading);
        m.wind = glm::vec2(std::cos(a), std::sin(a)) * c.wind_strength;
        m.wind_gust = c.wind_gust; m.temperature = c.temperature; m.precipitation = c.precipitation;
        m.wetness = c.wetness; m.lightning = c.lightning;
        for (const auto& e : c.effects) m.effects[e.prefab] += e.intensity;
        return m;
    }
    static ProfileMix lerp(const ProfileMix& a, const ProfileMix& b, float t) {
        ProfileMix m;
        auto L = [t](float x, float y) { return x + (y - x) * t; };
        m.cloud_cover = L(a.cloud_cover, b.cloud_cover); m.sun = L(a.sun, b.sun); m.ambient = L(a.ambient, b.ambient);
        m.sky_tint = glm::mix(a.sky_tint, b.sky_tint, t);
        m.fog_density = L(a.fog_density, b.fog_density); m.fog_color = glm::mix(a.fog_color, b.fog_color, t);
        m.fog_height_falloff = L(a.fog_height_falloff, b.fog_height_falloff); m.fog_sky_blend = L(a.fog_sky_blend, b.fog_sky_blend);
        m.fog_max_opacity = L(a.fog_max_opacity, b.fog_max_opacity); m.fog_sun_amount = L(a.fog_sun_amount, b.fog_sun_amount);
        m.wind = glm::mix(a.wind, b.wind, t);
        m.wind_gust = L(a.wind_gust, b.wind_gust); m.temperature = L(a.temperature, b.temperature);
        m.precipitation = L(a.precipitation, b.precipitation); m.wetness = L(a.wetness, b.wetness);
        m.lightning = L(a.lightning, b.lightning);
        for (const auto& [k, v] : a.effects) m.effects[k] += v * (1.0f - t);
        for (const auto& [k, v] : b.effects) m.effects[k] += v * t;
        return m;
    }
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
                                    float accumulate_time, float melt_time) {
        if (dt <= 0.0f) return cover;
        if (snows(precipitation, temperature)) {
            cover += dt * precipitation / std::max(accumulate_time, 1e-3f);
        } else if (temperature > 1.0f) {
            const float warmth = std::clamp((temperature - 1.0f) / 4.0f, 0.25f, 4.0f) * (1.0f + precipitation);
            cover -= dt * warmth / std::max(melt_time, 1e-3f);
        }
        return std::clamp(cover, 0.0f, 1.0f);
    }

    /**
     * @brief Applies a scene's weather settings. The first call starts the clock at
     *        time_of_day and snaps to `condition`; later calls (live edits) keep the running
     *        state, except that a changed time_of_day or starting condition jumps there.
     */
    void set_settings(const Settings& s) {
        const bool first = !configured_;
        const Settings old = settings_;
        settings_ = s;
        configured_ = true;
        if (first || old.seed != s.seed) rng_.reseed(s.seed ? s.seed : 0x5eed5u, 7);
        if (first || std::abs(old.time_of_day - s.time_of_day) > 1e-5f) set_time(s.time_of_day);
        if (first || old.initial_snow_cover != s.initial_snow_cover) snow_cover_ = s.initial_snow_cover;
        if (first || old.condition != s.condition || !settings_.find(target_)) {
            snap_to_(settings_.find(s.condition) ? s.condition : settings_.conditions.front().name);
        }
        if (!s.enabled) state_.enabled = false;
    }
    const Settings& settings() const { return settings_; }
    /** @brief Sets the lying snow directly (editor preview, gameplay); the weather carries on from it. */
    void set_snow_cover(float cover) { snow_cover_ = std::clamp(cover, 0.0f, 1.0f); state_.snow_cover = snow_cover_; }
    bool enabled() const { return configured_ && settings_.enabled; }

    // --- Control ---

    /**
     * @brief Moves to condition `name` over `transition_seconds` (negative: the condition's own
     *        `transition`; 0: at once). Returns false for an unknown name.
     */
    bool set_condition(const std::string& name, float transition_seconds = -1.0f) {
        const Condition* c = settings_.find(name);
        if (!c) return false;
        if (transition_seconds == 0.0f) { snap_to_(name); return true; }
        if (name == target_ && t_ >= 1.0f) return true;
        from_ = current_;
        const std::string was = target_;
        target_ = name;
        t_ = 0.0f;
        transition_s_ = transition_seconds < 0.0f ? c->transition : transition_seconds;
        condition_time_ = 0.0f;
        pick_duration_(*c);
        state_.previous = was;
        if (was != name) on_condition_changed.emit(was, name);
        return true;
    }
    /** @brief The condition the weather is in or moving to. */
    const std::string& condition() const { return target_; }

    /** @brief Sets the clock (hours, wrapped to [0, 24)); fires no hour / phase signals. */
    void set_time(float hour) {
        hour_ = wrap_hour(hour);
        last_hour_ = static_cast<int>(std::floor(hour_));
        phase_known_ = false;
    }
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

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        active_slot_() = this;
        if (!enabled()) {
            if (root_ || driven_sun_) shut_down_(scene);
            return;
        }
        const float dt = std::max(0.0f, ctx.delta_time);
        const bool running = scene.is_simulating() || editor_preview_;

        advance_clock_(running ? dt : 0.0f);
        if (running) advance_schedule_(dt);
        advance_blend_(dt);
        advance_lightning_(dt);
        compute_state_(dt);
        drive_sun_(scene);
        update_effects_(scene, dt, scene.is_simulating() || preview_effects_);
    }

private:
    // --- Clock ---------------------------------------------------------------------------

    void advance_clock_(float dt) {
        if (!clock_paused_ && settings_.day_length_minutes > 0.0f && dt > 0.0f) {
            const float hours = dt * 24.0f / (settings_.day_length_minutes * 60.0f);
            float h = hour_ + hours;
            while (h >= 24.0f) {
                h -= 24.0f;
                ++day_;
                on_new_day.emit(day_);
            }
            hour_ = h;
            const int whole = static_cast<int>(std::floor(hour_));
            if (whole != last_hour_) {
                last_hour_ = whole;
                on_hour.emit(whole);
            }
        }
        sky_ = evaluate_sky(settings_, hour_);
        const DayPhase p = sky_.sun_height > 0.1f ? DayPhase::Day
                         : sky_.sun_height < -0.1f ? DayPhase::Night
                         : (hour_ < 12.0f ? DayPhase::Dawn : DayPhase::Dusk);
        if (!phase_known_) { phase_ = p; phase_known_ = true; }
        else if (p != phase_) { phase_ = p; on_phase.emit(p); }
    }

    // --- Schedule ------------------------------------------------------------------------

    void pick_duration_(const Condition& c) {
        duration_s_ = 60.0f * rng_.range(c.min_minutes, std::max(c.min_minutes, c.max_minutes));
    }

    void advance_schedule_(float dt) {
        condition_time_ += dt;
        if (settings_.schedule == Schedule::Fixed || t_ < 1.0f || condition_time_ < duration_s_) return;
        const Condition* cur = settings_.find(target_);
        if (!cur) return;
        std::string next;
        if (settings_.schedule == Schedule::Cycle) {
            const int i = settings_.index_of(target_);
            next = settings_.conditions[static_cast<size_t>((i + 1) % static_cast<int>(settings_.conditions.size()))].name;
        } else {
            // Weighted pick among `next` (or every condition), excluding zero weights. A
            // condition listing itself may repeat; with no `next` the current one is skipped.
            std::vector<const Condition*> pool;
            if (!cur->next.empty()) {
                for (const auto& n : cur->next) if (const Condition* c = settings_.find(n); c && c->weight > 0.0f) pool.push_back(c);
            } else {
                for (const auto& c : settings_.conditions) if (c.weight > 0.0f && c.name != target_) pool.push_back(&c);
            }
            float total = 0.0f;
            for (const Condition* c : pool) total += c->weight;
            if (pool.empty() || total <= 0.0f) { condition_time_ = 0.0f; pick_duration_(*cur); return; }
            float r = rng_.next01() * total;
            next = pool.back()->name;
            for (const Condition* c : pool) { if ((r -= c->weight) <= 0.0f) { next = c->name; break; } }
        }
        if (next == target_) { condition_time_ = 0.0f; pick_duration_(*cur); return; }
        set_condition(next);
    }

    void snap_to_(const std::string& name) {
        const Condition* c = settings_.find(name);
        if (!c) return;
        const std::string was = target_;
        target_ = name;
        current_ = from_ = ProfileMix::of(*c);
        t_ = 1.0f;
        transition_s_ = 0.0f;
        condition_time_ = 0.0f;
        wetness_ = c->wetness;
        if (snows(c->precipitation, c->temperature)) snow_cover_ = 1.0f;
        pick_duration_(*c);
        if (!was.empty() && was != name) {
            state_.previous = was;
            on_condition_changed.emit(was, name);
        }
    }

    // --- Blend ---------------------------------------------------------------------------

    void advance_blend_(float dt) {
        const Condition* target = settings_.find(target_);
        if (!target) return;
        if (t_ < 1.0f) {
            t_ = (transition_s_ > 0.0f && transition_speed_ > 0.0f)
                ? std::min(1.0f, t_ + dt * transition_speed_ / transition_s_) : 1.0f;
        }
        const float e = t_ * t_ * (3.0f - 2.0f * t_);
        // The target's profile is read fresh every frame, so live edits show at once.
        current_ = t_ >= 1.0f ? ProfileMix::of(*target) : ProfileMix::lerp(from_, ProfileMix::of(*target), e);
    }

    void advance_lightning_(float dt) {
        if (flash_t_ >= 0.0f) {
            flash_t_ += dt;
            if (flash_t_ > 0.9f) flash_t_ = -1.0f;
        }
        const float per_second = current_.lightning / 60.0f;
        if (per_second > 0.0f && dt > 0.0f && flash_t_ < 0.0f && rng_.next01() < per_second * dt) {
            flash_t_ = 0.0f;
            on_lightning.emit();
        }
    }
    /** @brief A strike's brightness: a sharp flash and a weaker second one ~0.2 s later. */
    float flash_() const {
        if (flash_t_ < 0.0f) return 0.0f;
        const float a = std::exp(-flash_t_ * 14.0f);
        const float b = flash_t_ > 0.2f ? 0.7f * std::exp(-(flash_t_ - 0.2f) * 10.0f) : 0.0f;
        return std::clamp(std::max(a, b), 0.0f, 1.0f);
    }

    // --- State and atmosphere ------------------------------------------------------------

    void compute_state_(float dt) {
        const ProfileMix& m = current_;
        gust_clock_ += dt;
        const float g = 0.6f * std::sin(gust_clock_ * 0.7f) + 0.4f * std::sin(gust_clock_ * 1.9f + 1.3f);
        const glm::vec2 wind = m.wind * std::max(0.0f, 1.0f + m.wind_gust * g);
        // Wetness: soaks in over ~40 s, dries over ~3 min.
        const float tau = m.wetness > wetness_ ? 40.0f : 180.0f;
        wetness_ += (m.wetness - wetness_) * (1.0f - std::exp(-dt / tau));
        snow_cover_ = advance_snow_cover(snow_cover_, m.precipitation, m.temperature, dt,
                                         settings_.snow_accumulate_time, settings_.snow_melt_time);
        const float flash = flash_();

        WeatherState& s = state_;
        s.enabled = true;
        s.hour = hour_;
        s.day = day_;
        s.phase = phase_;
        s.daylight = sky_.daylight;
        s.sun_height = sky_.sun_height;
        s.sun_direction = sky_.sun_to;
        s.moon_direction = sky_.moon_to;
        s.condition = target_;
        s.transition = t_;
        s.condition_time = condition_time_;
        s.cloud_cover = m.cloud_cover;
        s.precipitation = m.precipitation;
        s.temperature = m.temperature;
        s.wetness = wetness_;
        s.snow_cover = snow_cover_;
        s.snow_depth = snow_cover_ * settings_.snow_max_depth;
        s.wind = glm::vec3(wind, 0.0f);
        s.fog_density = m.fog_density;
        s.lightning_flash = flash;

        // Sky: clouds pull the gradient toward a darker grey of the same brightness, then the tint.
        auto cloudy = [&](glm::vec3 c) {
            const float lum = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            return glm::mix(c, glm::vec3(lum * 0.85f), std::clamp(m.cloud_cover, 0.0f, 1.0f) * 0.85f) * m.sky_tint;
        };
        const glm::vec3 flash_sky = glm::vec3(0.55f, 0.6f, 0.75f) * flash;
        Atmosphere& a = atmosphere_;
        a.sky_zenith = cloudy(sky_.zenith) + flash_sky;
        a.sky_horizon = cloudy(sky_.horizon) + flash_sky;
        a.sky_ground = cloudy(sky_.ground) + flash_sky * 0.3f;
        a.ambient_intensity = sky_.ambient * m.ambient * (1.0f + 3.0f * flash);
        a.sky_intensity = a.ambient_intensity;
        // Fog colours are daylight colours: darken toward night, warm a little at twilight.
        light_level_ = glm::mix(0.04f, 1.0f, sky_.daylight) * (1.0f - 0.25f * m.cloud_cover);
        const glm::vec3 twilight_tint = glm::mix(glm::vec3(1.0f), settings_.twilight_horizon * 1.4f, sky_.twilight * 0.45f);
        a.fog_color = m.fog_color * light_level_ * twilight_tint + flash_sky * 0.5f;
        a.fog_density = std::max(0.0f, m.fog_density);
        a.fog_sky_blend = std::clamp(m.fog_sky_blend, 0.0f, 1.0f);
        a.fog_max_opacity = std::clamp(m.fog_max_opacity, 0.0f, 1.0f);
        a.fog_height_falloff = m.fog_height_falloff;
        a.fog_sun_amount = std::max(0.0f, m.fog_sun_amount) * (sky_.moon_light ? 0.2f : 1.0f);
        a.exposure_scale = glm::mix(std::max(0.0f, settings_.night_exposure), 1.0f, sky_.daylight);
    }

    // --- Sun -----------------------------------------------------------------------------

    void drive_sun_(coopa::scene::Scene& scene) {
        using coopa::gfx::engine::components::DirectionalLightComponent;
        if (!settings_.drive_sun) { release_sun_(); return; }
        if (!driven_sun_) {
            // The scene's own light first (an authored sun), else a runtime one.
            for (DirectionalLightComponent* l : scene.get_components<DirectionalLightComponent>()) {
                if (l->owner && l->owner->active() && !toy::scene::is_runtime_object(*l->owner)) { driven_sun_ = l; break; }
            }
            if (driven_sun_) {
                sun_saved_ = {driven_sun_->direction, driven_sun_->color, driven_sun_->intensity};
                owns_sun_ = false;
            } else {
                coopa::scene::SceneObject* root = ensure_root_(scene);
                auto obj = std::make_unique<coopa::scene::SceneObject>("Sun");
                obj->add_component<coopa::scene::TransformComponent>();
                driven_sun_ = obj->add_component<DirectionalLightComponent>();
                driven_sun_->cast_shadows = true;
                coopa::scene::SceneObject* raw = root->add_child(std::move(obj));
                scene.adopt(*raw);
                raw->start();
                owns_sun_ = true;
            }
        }
        const ProfileMix& m = current_;
        driven_sun_->direction = sky_.light_dir;
        driven_sun_->color = sky_.light_color;
        // Clouds take the edge off direct light on top of the condition's own multiplier.
        driven_sun_->intensity = sky_.light_intensity * std::max(0.0f, m.sun) * (1.0f + 1.5f * state_.lightning_flash);
    }

    /** @brief Hands the authored light back as it was (a runtime sun is removed with the root). */
    void release_sun_() {
        if (driven_sun_ && !owns_sun_) {
            driven_sun_->direction = sun_saved_.direction;
            driven_sun_->color = sun_saved_.color;
            driven_sun_->intensity = sun_saved_.intensity;
        }
        if (driven_sun_ && owns_sun_ && root_) {
            if (coopa::scene::SceneObject* o = driven_sun_->owner) root_->detach_child(o);
        }
        driven_sun_ = nullptr;
        owns_sun_ = false;
    }

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

    coopa::scene::SceneObject* ensure_root_(coopa::scene::Scene& scene) {
        if (!root_) {
            root_ = toy::scene::spawn_runtime_root(scene, k_weather_root_name, "Weather",
                                                   "Created by the scene's weather (World > Weather): rain, snow, mist and the sun it drives");
        }
        return root_;
    }

    /** @brief The spec an effect is placed by: the target condition's, else any condition's. */
    const EffectSpec* spec_for_(const std::string& prefab) const {
        if (const Condition* c = settings_.find(target_)) {
            for (const auto& e : c->effects) if (e.prefab == prefab) return &e;
        }
        for (const auto& c : settings_.conditions) {
            for (const auto& e : c.effects) if (e.prefab == prefab) return &e;
        }
        return nullptr;
    }

    static glm::vec3 viewer_position_() {
        // Any main camera, not only one of this scene: weather falls around whoever is looking
        // (the editor's viewport camera lives in its own UI scene).
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        if (!cam || !cam->owner) return glm::vec3(0.0f);
        auto* tc = cam->owner->get_transform();
        return tc ? glm::vec3(tc->transform().get_world_matrix()[3]) : glm::vec3(0.0f);
    }

    bool spawn_effect_(coopa::scene::Scene& scene, const std::string& prefab, const EffectSpec& spec) {
        if (failed_.count(prefab)) return false;
        coopa::scene::SceneObject* root = ensure_root_(scene);
        std::unique_ptr<coopa::scene::SceneObject> obj;
        try {
            obj = coopa::scene::SceneLoader::instantiate(prefab, nullptr, root->get_transform());
        } catch (const std::exception& e) {
            std::cerr << "[weather] Could not spawn effect '" << prefab << "': " << e.what() << "\n";
            failed_.insert(prefab);
            return false;
        }
        EffectInstance inst;
        inst.spec = spec;
        obj->for_each_recursive([&](coopa::scene::SceneObject& o) {
            if (auto* ps = o.get_component<toy::particles::ParticleSystem>()) {
                inst.particles.push_back({ps, ps->settings.rate, ps->settings.velocity, ps->settings.ground_height, ps->settings.on_death});
            }
            if (auto* v = o.get_component<coopa::gfx::engine::components::VolumeComponent>()) {
                inst.volumes.push_back({v, v->density, v->speed, v->height_base, v->color, v->direction});
            }
            auto* dl = o.get_component<WeatherDistantLandings>();
            auto* dps = o.get_component<toy::particles::ParticleSystem>();
            if (dl && dps) inst.distant.push_back({dl, dps, 0.0f});
        });
        inst.object = root->add_child(std::move(obj));
        scene.adopt(*inst.object);
        inst.object->start();
        effects_[prefab] = std::move(inst);
        return true;
    }

    /** @param show False (edit mode without the Preview's effects): no effect exists -- the
     *         ones there go at once -- but the precipitation map still runs for lying snow. */
    void update_effects_(coopa::scene::Scene& scene, float dt, bool show) {
        if (!show) {
            for (auto& [k, inst] : effects_) if (root_) root_->detach_child(inst.object);
            effects_.clear();
        } else {
            // Spawn what the blend asks for.
            for (const auto& [prefab, w] : current_.effects) {
                if (w > 1e-3f && !effects_.count(prefab)) {
                    if (const EffectSpec* spec = spec_for_(prefab)) spawn_effect_(scene, prefab, *spec);
                }
            }
        }
        const glm::vec3 eye = viewer_position_();
        // The precipitation height map, while anything collides with it.
        bool colliding = false;
        for (const auto& [k, inst] : effects_) {
            for (const ParticleBase& p : inst.particles) colliding |= p.ps->settings.collide && p.rate > 0.0f;
        }
        // The map reaches as far as anything shows landings (WeatherDistantLandings), at least
        // the drops' own boxes.
        float reach = 24.0f;
        for (const auto& [k, inst] : effects_) for (const auto& d : inst.distant) reach = std::max(reach, d.comp->radius + 1.0f);
        // Lying snow needs the map too: it is what tells the renderer where the sky is open
        // (snow settles on a roof, not under it -- see render/surface_world.h).
        const bool snow = snow_cover_ > 0.0f;
        if (snow) reach = std::max(reach, 32.0f);
        if ((colliding || snow) && settings_.surface_collision) {
            probe_.update(scene, eye, settings_.ground_height, dt, reach, settings_.ground_height_splashes);
        }
        else if (probe_.field()) probe_.reset();
        const glm::vec3 wind = state_.wind;
        const float wind_speed = glm::length(wind);
        for (auto it = effects_.begin(); it != effects_.end();) {
            EffectInstance& inst = it->second;
            auto wit = current_.effects.find(it->first);
            const float w = wit == current_.effects.end() ? 0.0f : std::max(0.0f, wit->second);
            if (const EffectSpec* spec = spec_for_(it->first)) inst.spec = *spec;

            glm::vec3 pos = inst.spec.offset;
            if (inst.spec.follow == EffectAnchor::Camera) pos = eye + inst.spec.offset;
            else if (inst.spec.follow == EffectAnchor::Ground)
                pos = glm::vec3(eye.x + inst.spec.offset.x, eye.y + inst.spec.offset.y, settings_.ground_height + inst.spec.offset.z);
            if (auto* tc = inst.object->get_transform()) tc->transform().set_position(pos);

            const glm::vec3 drift = wind * inst.spec.wind_influence;
            uint32_t alive = 0;
            for (ParticleBase& p : inst.particles) {
                p.ps->settings.rate = p.rate * w;
                // Sub-emitter targets (rate 0: splashes) stay where they land; emitters drift.
                if (p.rate > 0.0f) p.ps->settings.velocity = p.velocity + drift;
                p.ps->settings.ground_height = settings_.ground_height + p.ground_height;
                // Surfaces: the probed height map (roofs, terrain, water) or the flat plane.
                p.ps->ground_field = p.ps->settings.collide && settings_.surface_collision ? probe_.field() : nullptr;
                // Ground effects: splashes / settling snow are the sub emitters.
                if (settings_.ground_effects) { if (p.ps->settings.on_death.size() != p.on_death.size()) p.ps->settings.on_death = p.on_death; }
                else p.ps->settings.on_death.clear();
                alive += p.ps->particle_count();
            }
            for (VolumeBase& v : inst.volumes) {
                v.vol->density = v.density * w;
                v.vol->color = v.color * light_level_;
                v.vol->height_base = (inst.spec.follow == EffectAnchor::Ground ? settings_.ground_height : 0.0f) + v.height_base;
                if (wind_speed > 0.05f && inst.spec.wind_influence > 0.0f) {
                    v.vol->direction = glm::normalize(glm::mix(v.direction, wind / wind_speed, std::min(1.0f, inst.spec.wind_influence)));
                }
                v.vol->speed = v.speed + wind_speed * inst.spec.wind_influence;
            }
            emit_distant_landings_(inst, dt);
            inst.idle = w > 1e-3f ? 0.0f : inst.idle + dt;
            // Faded out: wait for the last particles to land (volumes are already at zero).
            if (inst.idle > 0.25f && (alive == 0 || inst.idle > 30.0f)) {
                if (root_) root_->detach_child(inst.object);
                it = effects_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Landings past a precipitation system's own wrap box: points scattered over the disc
     *        of WeatherDistantLandings::radius (outside the box, where real drops land), at the
     *        rate real drops land per square metre (each drop born in the box lands about once:
     *        rate / box area), each firing the drops' sub emitters on a splash-taking surface of
     *        the precipitation map, lying in its normal.
     */
    void emit_distant_landings_(EffectInstance& inst, float dt) {
        const std::shared_ptr<const toy::particles::GroundField>& field = probe_.field();
        if (!field || !settings_.ground_effects || !settings_.surface_collision || dt <= 0.0f) return;
        for (auto& d : inst.distant) {
            toy::particles::ParticleSystem* ps = d.ps;
            if (!ps->is_playing() || ps->sub_targets.size() != ps->settings.on_death.size()) continue;
            const glm::vec3 box = ps->settings.wrap_box;
            if (box.x <= 0.0f || box.y <= 0.0f || ps->settings.rate <= 0.0f) continue;
            const float r = d.comp->radius;
            const float area = std::max(0.0f, 3.14159265f * r * r - box.x * box.y);
            const float per_m2 = ps->settings.rate / (box.x * box.y);
            float expected = per_m2 * area * dt + d.carry;
            const int n = std::min(static_cast<int>(expected), 4000);
            d.carry = std::min(expected - static_cast<float>(n), 1.0f);
            if (n <= 0 || !ps->owner || !ps->owner->get_transform()) continue;
            const glm::vec3 c = glm::vec3(ps->owner->get_transform()->transform().get_world_matrix()[3]);
            const glm::vec3 vel = ps->settings.velocity;
            for (int k = 0; k < n; ++k) {
                const float rr = r * std::sqrt(rng_.next01());
                const float a = rng_.next01() * 6.28318531f;
                const glm::vec2 p = glm::vec2(c) + glm::vec2(std::cos(a), std::sin(a)) * rr;
                if (std::abs(p.x - c.x) < box.x * 0.5f && std::abs(p.y - c.y) < box.y * 0.5f) continue;   // real drops land there
                if (!field->splashes(p.x, p.y)) continue;
                const glm::vec3 at(p, field->sample(p.x, p.y) + 0.02f);
                const glm::vec3 n_surf = field->normal(p.x, p.y);
                for (size_t s = 0; s < ps->settings.on_death.size(); ++s) {
                    const toy::particles::SubEmitter& sub = ps->settings.on_death[s];
                    toy::particles::ParticleSystem* target = ps->sub_targets[s];
                    if (!target) continue;
                    if (!d.comp->targets.empty() &&
                        std::find(d.comp->targets.begin(), d.comp->targets.end(), sub.target) == d.comp->targets.end()) continue;
                    const int count = static_cast<int>(std::round(sub.count.sample(rng_)));
                    for (int e = 0; e < count; ++e) target->emit_at(at, vel * sub.inherit_velocity, n_surf);
                }
            }
        }
    }

    /** @brief Disabled: the sun goes back, every runtime object goes away. */
    void shut_down_(coopa::scene::Scene& scene) {
        release_sun_();
        effects_.clear();
        if (root_) scene.remove_root_object(root_);
        root_ = nullptr;
        state_.enabled = false;
    }

    static WeatherSystem*& active_slot_() {
        static WeatherSystem* slot = nullptr;
        return slot;
    }

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
inline const WeatherState& current() {
    static const WeatherState none;
    const WeatherSystem* s = WeatherSystem::active();
    return s ? s->state() : none;
}

/** @brief Installs a WeatherSystem configured from a `scene.settings.weather` node (null: disabled). */
inline WeatherSystem* install_weather_system(coopa::scene::Scene& scene, const fkyaml::node& weather_node,
                                             int order = k_weather_system_order) {
    auto sys = std::make_unique<WeatherSystem>();
    WeatherSystem* raw = sys.get();
    raw->set_settings(parse_settings(weather_node));
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_WEATHER_SYSTEM_H
