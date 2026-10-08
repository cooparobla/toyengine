/**
 * @file weather_surface.h
 * @brief Per-object precipitation reactions: which surfaces rain splashes on (and snow settles
 *        on), and how far from the camera those landings are shown.
 *
 *  - `WeatherSurface` on an object opts it -- and everything under it -- into ground effects.
 *    Rain still LANDS on every surface the precipitation map finds (a roof keeps the ground
 *    under it dry, splashes or not); only surfaces marked here, or under a marked parent, make
 *    splash rings, spray or settling snow. Unmarked surfaces take the drops silently.
 *    @code
 *    - name: ground
 *      components:
 *        - type: WeatherSurface
 *          splashes: true
 *    @endcode
 *  - `WeatherDistantLandings` on a precipitation system's object (the rain prefab's drops)
 *    continues its landings past its own wrap box: the weather scatters landings on
 *    splash-taking surfaces out to `radius`, at the same rate per square metre as the real drops
 *    land, firing the listed sub emitters (splash rings) -- so splashes are seen across the whole
 *    street, not only in the ~18 m the drops are simulated in.
 *    @code
 *    - type: WeatherDistantLandings
 *      radius: 45                     # m from the camera
 *      targets: [weather_rain_splash] # which of the drops' sub emitters (empty: all)
 *    @endcode
 */

#ifndef TOYENGINE_WEATHER_WEATHER_SURFACE_H
#define TOYENGINE_WEATHER_WEATHER_SURFACE_H

#include <string>
#include <vector>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <fkYAML/node.hpp>

namespace toy {
namespace weather {

/** @brief Opts an object (and its subtree) into precipitation ground effects. */
class WeatherSurface : public coopa::scene::Component {
public:
    bool splashes = true;   ///< Rain splashes / sprays, snow settles here.
    std::string type_name() const override { return "WeatherSurface"; }
};

/** @brief True if `obj` or an ancestor carries a WeatherSurface that takes splashes. */
inline bool takes_splashes(const coopa::scene::SceneObject* obj) {
    for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
        if (auto* s = const_cast<coopa::scene::SceneObject*>(o)->get_component<WeatherSurface>()) return s->splashes;
    }
    return false;
}

/** @brief Shows a precipitation system's landings out to `radius` (driven by the weather). */
class WeatherDistantLandings : public coopa::scene::Component {
public:
    float radius = 45.0f;
    std::vector<std::string> targets;   ///< Sub emitter target names to fire; empty = all of them.
    std::string type_name() const override { return "WeatherDistantLandings"; }
};

/** @brief Registers "WeatherSurface" and "WeatherDistantLandings". */
inline void register_weather_surface_components() {
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

#endif // TOYENGINE_WEATHER_WEATHER_SURFACE_H
