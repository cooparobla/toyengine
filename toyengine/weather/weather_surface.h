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
bool takes_splashes(const coopa::scene::SceneObject* obj);

/** @brief Shows a precipitation system's landings out to `radius` (driven by the weather). */
class WeatherDistantLandings : public coopa::scene::Component {
public:
    float radius = 45.0f;
    std::vector<std::string> targets;   ///< Sub emitter target names to fire; empty = all of them.
    std::string type_name() const override { return "WeatherDistantLandings"; }
};

/** @brief Registers "WeatherSurface" and "WeatherDistantLandings". */
void register_weather_surface_components();

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_WEATHER_SURFACE_H
