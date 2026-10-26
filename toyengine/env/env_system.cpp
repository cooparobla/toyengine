#include <toyengine/env/env_system.h>

#include <coopa/scene/scene_object.h>

#include <algorithm>
#include <iostream>

#include <toyengine/weather/weather_system.h>

namespace toy {
namespace env {

namespace {

/** @brief Whether `obj` and every ancestor of it is active. */
bool active_in_tree_(const coopa::scene::SceneObject* obj) {
    for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
        if (!o->active()) return false;
    }
    return true;
}

} // namespace

EnvSystem*& EnvSystem::active_slot_() {
    static EnvSystem* slot = nullptr;
    return slot;
}

EnvSystem::~EnvSystem() {
    if (active_slot_() == this) active_slot_() = nullptr;
}

void EnvSystem::emit(const EnvPulse& pulse) {
    // A pulse with no lifetime would never be sampled; treat it as a mistake rather than
    // silently queueing something invisible.
    if (env_pulse_lifetime(pulse) <= 0.0f || pulse.source.radius <= 0.0f) return;
    pulses_.push_back({pulse, 0.0f});
    on_pulse.emit(pulse);
}

void EnvSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    active_slot_() = this;

    field_ = weather_only_field(scene);
    gathered_.clear();
    dropped_ = 0;

    // --- Persistent sources, extrapolated one frame (see the header's position contract) ---
    const auto& influences = scene.get_components<EnvInfluence>();
    for (EnvInfluence* e : influences) {
        // Belt and braces: Scene::get_components<T>() already returns only active components,
        // but an influence whose ancestor was switched off must not blow either, and that is
        // load-bearing here -- switching a preset off switches its ROOT off and the influence
        // hangs below it.
        if (!e->owner || !active_in_tree_(e->owner)) continue;
        if (!e->contributes()) continue;
        EnvSource s = e->resolved();
        auto it = prev_pos_.find(e);
        if (it != prev_pos_.end()) {
            const glm::vec3 step = s.position - it->second;
            it->second = s.position;
            s.position += step;   // one frame of linear extrapolation
        } else {
            prev_pos_.emplace(e, s.position);
        }
        gathered_.push_back(s);
    }
    // Forget influences that went away, so the map does not grow with scene churn.
    if (prev_pos_.size() > influences.size()) {
        for (auto it = prev_pos_.begin(); it != prev_pos_.end();) {
            const bool alive = std::find(influences.begin(), influences.end(), it->first) != influences.end();
            it = alive ? std::next(it) : prev_pos_.erase(it);
        }
    }

    // --- One-shot pulses: age, fold at their envelope strength, drop the spent ones ---
    const float dt = std::max(ctx.delta_time, 0.0f);
    for (auto it = pulses_.begin(); it != pulses_.end();) {
        const float strength = env_pulse_strength(it->pulse, it->age);
        it->age += dt;
        if (it->age >= env_pulse_lifetime(it->pulse)) {
            it = pulses_.erase(it);
        } else {
            if (strength > 0.0f) {
                EnvSource s = it->pulse.source;
                s.wind *= strength;
                s.outward *= strength;
                s.vertical *= strength;
                s.turbulence *= strength;
                s.wetness *= strength;
                s.temperature *= strength;
                s.snow_clear *= strength;
                s.water_disturb *= strength;
                gathered_.push_back(s);
            }
            ++it;
        }
    }

    // --- Keep the strongest kMaxEnvSources, by how much air they move over their footprint ---
    if (gathered_.size() > static_cast<std::size_t>(kMaxEnvSources)) {
        const auto weight = [](const EnvSource& s) {
            const float speed = std::max({glm::length(s.wind), std::abs(s.outward), std::abs(s.vertical)});
            return speed * s.radius;
        };
        std::partial_sort(gathered_.begin(), gathered_.begin() + kMaxEnvSources, gathered_.end(),
                          [&](const EnvSource& a, const EnvSource& b) { return weight(a) > weight(b); });
        dropped_ = static_cast<int>(gathered_.size()) - kMaxEnvSources;
        gathered_.resize(kMaxEnvSources);
        // A silent cap reads as "it covered everything"; say so, once per change. Per system,
        // not a function-local static: the editor keeps an edit scene and a play scene alive at
        // once, and one would otherwise silence the other.
        if (dropped_ != reported_drop_) {
            std::cout << "[env] " << dropped_ << " local influence(s) dropped this frame: the field holds "
                      << kMaxEnvSources << ". The weakest by speed x radius are the ones cut.\n";
            reported_drop_ = dropped_;
        }
    }

    field_.count = static_cast<int>(gathered_.size());
    for (int i = 0; i < field_.count; ++i) field_.source[i] = gathered_[i];

    snapshot_ = std::make_shared<const EnvField>(field_);
}

EnvSystem* install_env_system(coopa::scene::Scene& scene, int order) {
    if (auto* existing = find_env(scene)) return existing;
    auto sys = std::make_unique<EnvSystem>();
    EnvSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

const EnvField& current_field() {
    static const EnvField none;
    const EnvSystem* s = EnvSystem::active();
    return s ? s->field() : none;
}

bool emit(coopa::scene::Scene& scene, const EnvPulse& pulse) {
    if (EnvSystem* s = find_env(scene)) {
        s->emit(pulse);
        return true;
    }
    return false;
}

EnvField weather_only_field(const coopa::scene::Scene& scene) {
    EnvField f;
    const weather::WeatherSystem* w = weather::find(scene);
    if (w && w->enabled() && w->state().enabled) {
        const weather::WeatherState& s = w->state();
        f.wind = s.wind;
        f.wind_gust = s.wind_gust;
        f.wind_time = s.wind_time;
        f.wetness = s.wetness;
        f.temperature = s.temperature;
    }
    return f;
}

} // namespace env
} // namespace toy
