#include <toyengine/weather/weather_reactor.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/weather/weather_profile.h>
#include <toyengine/weather/weather_surface.h>

namespace toy {
namespace weather {

bool WeatherReactor::matches(const WeatherState& w) const {
    if (use_hours) {
        const bool in = from_hour <= to_hour ? (w.hour >= from_hour && w.hour < to_hour)
                                             : (w.hour >= from_hour || w.hour < to_hour);
        if (!in) return false;
    }
    if (!phases.empty() && std::find(phases.begin(), phases.end(), phase_name(w.phase)) == phases.end()) return false;
    if (!conditions.empty() && std::find(conditions.begin(), conditions.end(), w.condition) == conditions.end()) return false;
    return w.precipitation >= min_precipitation;
}

void WeatherReactor::start() {
    if (!owner) return;
    lights_.clear();
    owner->for_each_recursive([this](coopa::scene::SceneObject& o) {
        Light l;
        l.flicker = o.get_component<toy::particles::LightFlicker>();
        if (auto* pl = o.get_component<coopa::gfx::engine::components::PointLightComponent>()) { l.point = pl; l.base = pl->intensity; }
        else if (auto* sl = o.get_component<coopa::gfx::engine::components::SpotLightComponent>()) { l.spot = sl; l.base = sl->intensity; }
        if (l.point || l.spot) lights_.push_back(l);
    });
    on_ = matches(current()) != invert;
    level_ = on_ ? 1.0f : 0.0f;
    apply_(true);
}

void WeatherReactor::update(float dt) {
    if (!owner) return;
    const bool now = matches(current()) != invert;
    const bool changed = now != on_;
    on_ = now;
    const float goal = on_ ? 1.0f : 0.0f;
    level_ = fade > 0.0f ? goal + (level_ - goal) * std::max(0.0f, 1.0f - dt / fade) : goal;
    if (std::abs(level_ - goal) < 1e-3f) level_ = goal;
    apply_(changed);
}

void WeatherReactor::apply_(bool switched) {
    if (target_children && switched) {
        for (const auto& c : owner->children()) c->set_active(on_);
    }
    // Off: kept stopped every frame (a system's first-frame init would otherwise start it);
    // on: started once, at the switch, so gameplay may still stop it itself.
    if (target_effects && (switched || !on_)) {
        owner->for_each_recursive([this](coopa::scene::SceneObject& o) {
            if (auto* ps = o.get_component<toy::particles::ParticleSystem>()) {
                if (on_ && !ps->is_playing()) ps->play();
                else if (!on_ && ps->is_playing()) ps->stop();
            }
        });
    }
    if (target_lights) {
        for (Light& l : lights_) {
            // A flickering light is dimmed through its flicker, which writes the intensity.
            if (l.flicker) l.flicker->dimmer = level_;
            else if (l.point) l.point->intensity = l.base * level_;
            else if (l.spot) l.spot->intensity = l.base * level_;
            if (l.flicker && l.point && level_ <= 0.0f) l.point->intensity = 0.0f;
        }
    }
}

void register_weather_components() {
    using coopa::scene::SceneLoader;
    register_weather_surface_components();
    SceneLoader::register_component_parser("WeatherReactor",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            using namespace yaml_detail;
            auto* r = obj.add_component<WeatherReactor>();
            if (node.contains("hours")) {   // [from, to] or {x: from, y: to} (the editor's vec2)
                const fkyaml::node& h = node.at("hours");
                if (h.is_sequence() && h.size() >= 2) {
                    r->use_hours = true;
                    r->from_hour = num(h[0], r->from_hour);
                    r->to_hour = num(h[1], r->to_hour);
                } else if (h.is_mapping()) {
                    r->use_hours = true;
                    r->from_hour = f(h, "x", r->from_hour);
                    r->to_hour = f(h, "y", r->to_hour);
                }
            }
            auto strings = [&](const char* key, std::vector<std::string>& out) {
                if (!node.contains(key) || !node.at(key).is_sequence()) return;
                for (const auto& s : node.at(key)) if (s.is_string()) out.push_back(s.get_value<std::string>());
            };
            strings("phases", r->phases);
            strings("conditions", r->conditions);
            r->min_precipitation = f(node, "min_precipitation", r->min_precipitation);
            r->invert = b(node, "invert", r->invert);
            if (node.contains("target")) {
                std::vector<std::string> t;
                if (node.at("target").is_string()) t.push_back(node.at("target").get_value<std::string>());
                else strings("target", t);
                auto has = [&](const char* k) { return std::find(t.begin(), t.end(), k) != t.end(); };
                const bool all = has("all");
                r->target_lights = all || has("lights");
                r->target_effects = all || has("effects");
                r->target_children = all || has("children");
            }
            r->fade = std::max(0.0f, f(node, "fade", r->fade));
        });
}

} // namespace weather
} // namespace toy
