/**
 * @file fire.cpp
 * @brief Component bodies and YAML parsers for Fire and Burnable. See fire.h.
 */
#include <toyengine/fire/fire.h>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/components/transform_component.h>

namespace toy {
namespace fire {
namespace {

using coopa::scene::SceneLoader;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

float num(const fkyaml::node& n, const char* k, float def) {
    if (!n.is_mapping() || !n.contains(k)) return def;
    const fkyaml::node& v = n.at(k);
    if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return def;
}

bool flag(const fkyaml::node& n, const char* k, bool def) {
    if (!n.is_mapping() || !n.contains(k) || !n.at(k).is_boolean()) return def;
    return n.at(k).get_value<bool>();
}

std::string str(const fkyaml::node& n, const char* k) {
    if (!n.is_mapping() || !n.contains(k) || !n.at(k).is_string()) return {};
    return n.at(k).get_value<std::string>();
}

glm::mat4 world_of(const SceneObject* obj) {
    if (!obj) return glm::mat4(1.0f);
    const auto* tc = obj->get_transform();
    return tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
}

} // namespace

void Burnable::ignite_at(const glm::vec3& world_point) {
    // Stored in LOCAL space so a burning object that is moved or turned carries its burn.
    origin_local_ = glm::vec3(glm::inverse(world_of(owner)) * glm::vec4(world_point, 1.0f));
    burning_ = true;
    spent_ = false;
    age_ = 0.0f;
    front_ = 0.0f;
}

void Burnable::ignite() {
    origin_local_ = glm::vec3(0.0f);
    burning_ = true;
    spent_ = false;
    age_ = 0.0f;
    front_ = 0.0f;
}

void Burnable::reset() {
    burning_ = false;
    spent_ = false;
    age_ = 0.0f;
    front_ = 0.0f;
    origin_local_ = glm::vec3(0.0f);
}

glm::vec3 Burnable::origin_world() const {
    return glm::vec3(world_of(owner) * glm::vec4(origin_local_, 1.0f));
}

void register_fire_components() {
    SceneLoader::register_component_parser("Burnable",
        [](const fkyaml::node& n, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* b = obj.add_component<Burnable>();
            b->spread_rate = num(n, "spread_rate", b->spread_rate);
            b->burn_time = num(n, "burn_time", b->burn_time);
            b->overshoot = num(n, "overshoot", b->overshoot);
            const std::string on_burnt = str(n, "on_burnt");
            b->on_burnt = (on_burnt == "remove") ? BurntResult::Remove : BurntResult::Char;
            // `ignited: true` lights it at load, at its own origin -- a scene that wants
            // something already alight does not need a script to do it.
            if (flag(n, "ignited", false)) b->ignite();
        });

    SceneLoader::register_component_parser("FireIgniter",
        [](const fkyaml::node& n, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* g = obj.add_component<FireIgniter>();
            const std::string b = str(n, "button");
            if (!b.empty()) g->button = b;
            if (n.is_mapping() && n.contains("reset_key")) g->reset_key = str(n, "reset_key");
            g->max_distance = num(n, "max_distance", g->max_distance);
        });
}

} // namespace fire
} // namespace toy
