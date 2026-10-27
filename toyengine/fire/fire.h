/**
 * @file fire.h
 * @brief An object that catches fire at a point and burns across itself.
 *
 * Fire here is **per object**. There is no world fire state, no field, no spreading between
 * objects, and no coupling to the weather or the environment field -- a thing is either alight
 * or it is not, and what a burning surface looks like is drawn by
 * `assets/shaders/gfx/surface/burn.glsl` -- char behind an advancing incandescent edge.
 *
 * | | For |
 * |---|---|
 * | `Burnable` | an object that catches at a POINT and burns outward across itself |
 * | `FireIgniter` | opts a scene into click-to-ignite |
 *
 * A purely VISUAL fire -- a candle, a campfire -- is not a component here at all. It is a
 * particle prefab (`objects/props/campfire`), which is the engine's existing, editable, fully
 * authorable path for exactly that; a component that spawned its emitters at runtime would be
 * the same thing with less control and a second way to do it.
 *
 * `Burnable` is the interesting one. Ignition takes a world-space point -- click a chair on its
 * seat and the fire starts on the seat -- and a front then travels outward over the object at
 * `spread_rate`, charring what it has passed and leaving an incandescent rim at its edge. The
 * front is a sphere growing from the ignition point, which is an approximation: it crosses the
 * gap between a chair's seat and its leg rather than travelling the surface between them. True
 * geodesic spread would need mesh connectivity, and at flame scale nobody can tell.
 */
#ifndef TOYENGINE_FIRE_FIRE_H
#define TOYENGINE_FIRE_FIRE_H

#include <string>

#include <glm/glm.hpp>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

namespace toy {
namespace fire {

class FireSystem;

/** @brief What is left of a Burnable once the fire has finished with it. */
enum class BurntResult {
    Char,   ///< It stays, blackened by the burn layer.
    Remove  ///< It is switched off once consumed.
};

/**
 * @class Burnable
 * @brief An object that catches fire at a point and burns outward across itself.
 *
 * ```yaml
 * - type: Burnable
 *   spread_rate: 0.25   # m/s the front travels over the surface
 *   burn_time: 14       # s from ignition to fully consumed
 *   on_burnt: char
 * ```
 */
class Burnable : public coopa::scene::Component {
public:
    float spread_rate = 0.25f;  ///< Metres per second the front travels outward.
    float burn_time = 14.0f;    ///< Seconds from ignition until it is consumed.
    BurntResult on_burnt = BurntResult::Char;
    /// Metres the front keeps advancing past the object's own extent before it is done. A small
    /// margin, so the last corner is reached rather than left green.
    float overshoot = 0.3f;

    std::string type_name() const override { return "Burnable"; }

    /**
     * @brief Lights it at a world-space point -- the point a click landed on.
     *
     * The point is stored in the object's LOCAL space, so a burning object that is then moved
     * or turned carries its burn with it.
     */
    void ignite_at(const glm::vec3& world_point);

    /** @brief Lights it at its own origin. */
    void ignite();

    /** @brief Puts it out and leaves it as it is -- part burnt, if it was part burnt. */
    void extinguish() { burning_ = false; }

    /** @brief Puts it back to untouched. */
    void reset();

    bool burning() const { return burning_; }       ///< Alight right now.
    bool spent() const { return spent_; }           ///< Finished burning.
    float age() const { return age_; }              ///< Seconds since ignition.
    /** @brief 0 untouched .. 1 consumed. */
    float progress() const { return burn_time > 0.0f ? glm::clamp(age_ / burn_time, 0.0f, 1.0f) : 1.0f; }
    /** @brief Current front radius, in world metres from the ignition point. */
    float front() const { return front_; }
    /** @brief The ignition point in world space. Meaningless before ignition. */
    glm::vec3 origin_world() const;

private:
    friend class FireSystem;
    bool burning_ = false;
    bool spent_ = false;
    coopa::scene::SceneObject* effects_root_ = nullptr;
    float age_ = 0.0f;
    float front_ = 0.0f;
    glm::vec3 origin_local_{0.0f};
};

/**
 * @class FireIgniter
 * @brief Opts a scene into click-to-ignite: a click lights whatever Burnable is under it.
 *
 * The ray is PUSHED in by `Engine::drive_fire_igniters_()` rather than pulled from an Input
 * here -- the same contract FreeMover and Turntable use, which is what keeps these components
 * testable with no live window and lets NO_INPUT=1 silence them.
 *
 * ```yaml
 * - type: FireIgniter
 *   button: left
 *   reset_key: R
 * ```
 */
class FireIgniter : public coopa::scene::Component {
public:
    std::string button = "left";   ///< Mouse button that ignites: left / right / middle.
    std::string reset_key = "R";   ///< Key that puts every Burnable back; empty disables it.
    float max_distance = 1000.0f;  ///< Metres to search along the ray.

    std::string type_name() const override { return "FireIgniter"; }

    /// @name Written by Engine each frame, before the scene updates.
    /// @{
    bool ignite_requested = false;
    bool reset_requested = false;
    glm::vec3 ray_origin{0.0f};
    glm::vec3 ray_direction{0.0f, 1.0f, 0.0f};
    /// @}
};

/** @brief Registers the YAML parsers for `Burnable` and `FireIgniter`. */
void register_fire_components();

} // namespace fire
} // namespace toy

#endif // TOYENGINE_FIRE_FIRE_H
