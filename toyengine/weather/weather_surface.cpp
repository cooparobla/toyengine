#include <toyengine/weather/weather_surface.h>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

namespace toy {
namespace weather {

bool takes_splashes(const coopa::scene::SceneObject* obj) {
    for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
        if (auto* s = const_cast<coopa::scene::SceneObject*>(o)->get_component<WeatherSurface>()) return s->splashes;
    }
    return false;
}

void register_weather_surface_components() {
    using coopa::scene::SceneLoader;
    SceneLoader::register_component_parser("WeatherSurface",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* s = obj.add_component<WeatherSurface>();
            if (node.contains("splashes") && node.at("splashes").is_boolean()) s->splashes = node.at("splashes").get_value<bool>();
        });
    SceneLoader::register_component_parser("WeatherDistantLandings",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<WeatherDistantLandings>();
            if (node.contains("radius")) {
                const fkyaml::node& r = node.at("radius");
                if (r.is_float_number()) d->radius = static_cast<float>(r.get_value<double>());
                else if (r.is_integer()) d->radius = static_cast<float>(r.get_value<int64_t>());
            }
            if (node.contains("targets") && node.at("targets").is_sequence()) {
                for (const auto& t : node.at("targets")) if (t.is_string()) d->targets.push_back(t.get_value<std::string>());
            }
        });
}

} // namespace weather
} // namespace toy
