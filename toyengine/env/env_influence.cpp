#include <toyengine/env/env_influence.h>

#include <algorithm>

#include <coopa/animation/animator.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/env/env_system.h>

namespace toy {
namespace env {

namespace {

/** @brief The object's world position, or the origin when it has no transform. */
glm::vec3 world_position_(const coopa::scene::SceneObject* obj) {
    if (!obj) return glm::vec3(0.0f);
    const auto* tc = obj->get_transform();
    return tc ? glm::vec3(tc->transform().get_world_matrix()[3]) : glm::vec3(0.0f);
}

/** @brief Scales every channel of `s` by `k`, leaving the shape alone. */
void scale_channels_(EnvSource& s, float k) {
    s.wind *= k;
    s.outward *= k;
    s.vertical *= k;
    s.turbulence *= k;
    s.wetness *= k;
    s.temperature *= k;
    s.snow *= k;
    s.snow_clear *= k;
    s.water_disturb *= k;
}

/** @brief Reads a float key, accepting both YAML integers and floats. */
void num_(const fkyaml::node& node, const char* key, float& out) {
    if (!node.contains(key)) return;
    const fkyaml::node& v = node.at(key);
    if (v.is_float_number()) out = static_cast<float>(v.get_value<double>());
    else if (v.is_integer()) out = static_cast<float>(v.get_value<int64_t>());
}

void vec3_(const fkyaml::node& node, const char* key, glm::vec3& out) {
    if (!node.contains(key)) return;
    const fkyaml::node& v = node.at(key);
    if (!v.is_mapping()) return;
    num_(v, "x", out.x);
    num_(v, "y", out.y);
    num_(v, "z", out.z);
}

void bool_(const fkyaml::node& node, const char* key, bool& out) {
    if (node.contains(key) && node.at(key).is_boolean()) out = node.at(key).get_value<bool>();
}

std::string lower_(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void shape_(const fkyaml::node& node, EnvShape& out) {
    if (!node.contains("shape") || !node.at("shape").is_string()) return;
    const std::string v = lower_(node.at("shape").get_value<std::string>());
    if (v == "disc" || v == "cylinder" || v == "column") out = EnvShape::Disc;
    else if (v == "sphere" || v == "ball") out = EnvShape::Sphere;
}

void mode_(const fkyaml::node& node, EnvMode& out) {
    if (!node.contains("mode") || !node.at("mode").is_string()) return;
    const std::string v = lower_(node.at("mode").get_value<std::string>());
    if (v == "radial") out = EnvMode::Radial;
    else if (v == "directional" || v == "direction") out = EnvMode::Directional;
}

void blend_(const fkyaml::node& node, EnvBlend& out) {
    if (!node.contains("blend") || !node.at("blend").is_string()) return;
    const std::string v = lower_(node.at("blend").get_value<std::string>());
    if (v == "max") out = EnvBlend::Max;
    else if (v == "override") out = EnvBlend::Override;
    else if (v == "add") out = EnvBlend::Add;
}

/** @brief Parses the shape and channel keys shared by both components. */
void parse_source_(const fkyaml::node& node, EnvSource& s) {
    shape_(node, s.shape);
    mode_(node, s.mode);
    blend_(node, s.blend);
    num_(node, "radius", s.radius);
    num_(node, "height", s.height);
    num_(node, "falloff", s.falloff);
    vec3_(node, "wind", s.wind);
    num_(node, "outward", s.outward);
    num_(node, "vertical", s.vertical);
    num_(node, "turbulence", s.turbulence);
    num_(node, "wetness", s.wetness);
    num_(node, "temperature", s.temperature);
    num_(node, "snow", s.snow);
    num_(node, "snow_clear", s.snow_clear);
    num_(node, "water_disturb", s.water_disturb);
}

} // namespace

EnvSource EnvInfluence::resolved() const {
    EnvSource s = source;
    s.position = world_position_(owner);
    scale_channels_(s, enabled ? std::clamp(strength, 0.0f, 1.0f) : 0.0f);
    return s;
}

bool EnvInfluence::contributes() const {
    if (!enabled || strength <= 0.0f || source.radius <= 0.0f) return false;
    const EnvSource& s = source;
    // An Override source contributes by REPLACING, so it matters even with every channel at
    // zero -- a sheltered courtyard is exactly "zero wind here".
    if (s.blend == EnvBlend::Override) return true;
    return glm::dot(s.wind, s.wind) > 0.0f || s.outward != 0.0f || s.vertical != 0.0f ||
           s.turbulence != 0.0f || s.wetness != 0.0f || s.temperature != 0.0f ||
           s.snow != 0.0f || s.snow_clear != 0.0f || s.water_disturb != 0.0f;
}

void EnvPulseEmitter::start() {
    if (event.empty() || !owner) return;
    if (auto* animator = owner->get_component<coopa::anim::Animator>()) {
        anim_ = animator->on_event.connect_scoped([this](const coopa::anim::AnimationEvent& ev) {
            if (ev.name == event) fire();
        });
    }
}

void EnvPulseEmitter::fire() {
    if (!owner || strength <= 0.0f) return;
    EnvPulse p = pulse;
    const auto* tc = owner->get_transform();
    const glm::mat4 m = tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
    p.source.position = glm::vec3(m * glm::vec4(offset, 1.0f));
    scale_channels_(p.source, std::clamp(strength, 0.0f, 1.0f));
    if (scene && emit(*scene, p)) on_fired.emit(p);
}

void register_env_preset_component();

void register_env_components() {
    using coopa::scene::SceneLoader;
    register_env_preset_component();

    SceneLoader::register_component_parser("EnvInfluence",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* e = obj.add_component<EnvInfluence>();
            bool_(node, "enabled", e->enabled);
            num_(node, "strength", e->strength);
            parse_source_(node, e->source);
        });

    SceneLoader::register_component_parser("EnvPulseEmitter",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* e = obj.add_component<EnvPulseEmitter>();
            if (node.contains("event") && node.at("event").is_string())
                e->event = node.at("event").get_value<std::string>();
            vec3_(node, "offset", e->offset);
            num_(node, "strength", e->strength);
            parse_source_(node, e->pulse.source);
            num_(node, "attack", e->pulse.attack);
            num_(node, "hold", e->pulse.hold);
            num_(node, "decay", e->pulse.decay);
        });
}

} // namespace env
} // namespace toy
