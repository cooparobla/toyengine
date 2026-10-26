/**
 * @file env_influence.h
 * @brief `EnvInfluence` -- a local environmental influence authored on an object, and
 *        `EnvPulseEmitter` -- a one-shot one fired by an animation event.
 *
 * `EnvInfluence` is the persistent kind: a rotor's downwash, a steam vent, a leaking pipe, a
 * sheltered courtyard. It takes its position from the object's transform, so moving the
 * object moves the influence.
 *
 * @code
 * - type: EnvInfluence
 *   shape: disc            # sphere | disc (a cylinder about +Z: a rotor column, a vent plume)
 *   radius: 7              # m; the influence is exactly zero at and beyond it
 *   height: 10             # disc only: m either side of the origin (0 = an unbounded column)
 *   falloff: 0.45          # 0 a hard edge .. 1 a soft bowl
 *   mode: radial           # directional | radial (radial aims away from the +Z axis)
 *   wind: {x: 0, y: 0, z: 0}   # directional: m/s
 *   outward: 11            # radial: m/s away from the axis at the rim
 *   vertical: -16          # radial: m/s along +Z -- NEGATIVE is a downwash
 *   turbulence: 0.7        # 0..1 extra foliage flutter and particle noise
 *   wetness: 0.0           # 0..1 added surface wetness (negative = a local dry zone)
 *   temperature: 0.0       # degrees C offset
 *   snow_clear: 1.0        # 0..1 how hard it scours lying snow
 *   water_disturb: 0.9     # 0..1 ripple rings on water beneath it
 *   blend: add             # add | max | override
 *   strength: 1.0          # 0..1 master scale on every channel
 *   enabled: true
 * @endcode
 *
 * `EnvPulseEmitter` fires a short-lived influence instead of holding one. Point it at an
 * animation event name and a sword swing gusts the ferns with no game code -- the clip
 * already carries the event:
 *
 * @code
 * - type: EnvPulseEmitter
 *   event: swing           # the Animator clip event that fires it ("" = code only, call fire())
 *   offset: {x: 0, y: 0, z: 1.2}   # where the pulse is born, in the object's own space
 *   radius: 2.8
 *   mode: radial
 *   outward: 10
 *   turbulence: 0.9
 *   attack: 0.04           # s to full
 *   hold: 0.05             # s at full
 *   decay: 0.3             # s back to nothing -- the ferns snap, then recover
 * @endcode
 *
 * Both are gathered by `EnvSystem` (order 45, env_system.h) into the frame's `EnvField`.
 */

#ifndef TOYENGINE_ENV_ENV_INFLUENCE_H
#define TOYENGINE_ENV_ENV_INFLUENCE_H

#include <string>

#include <glm/glm.hpp>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>

#include <toyengine/env/env_source.h>

namespace toy {
namespace env {

/**
 * @class EnvInfluence
 * @brief A persistent local influence. See the file doc for the YAML.
 */
class EnvInfluence : public coopa::scene::Component {
public:
    bool enabled = true;
    float strength = 1.0f;   ///< 0..1 master scale on every channel.

    /// The shape and channels, as authored. `position` is overwritten from the transform each
    /// frame, so authoring it has no effect.
    EnvSource source;

    std::string type_name() const override { return "EnvInfluence"; }

    /**
     * @brief This object's influence as the gather wants it: positioned at the object, with
     *        `strength` already folded into every channel.
     *
     * @return EnvSource The source; its channels are all zero when the component is off.
     */
    EnvSource resolved() const;

    /** @brief Whether it would contribute anything at all this frame. */
    bool contributes() const;
};

/**
 * @class EnvPulseEmitter
 * @brief Fires a one-shot influence, driven by an animation event or by code.
 */
class EnvPulseEmitter : public coopa::scene::Component {
public:
    std::string event;          ///< Animator clip event name that fires it; empty = code only.
    glm::vec3 offset{0.0f};     ///< Where the pulse is born, in the object's own space.
    EnvPulse pulse;             ///< The shape, channels and envelope.
    float strength = 1.0f;

    std::string type_name() const override { return "EnvPulseEmitter"; }

    /// Connects to a sibling Animator's events when `event` is set.
    void start() override;

    /** @brief Emits the pulse now, at the object's current world position plus `offset`. */
    void fire();

    /** @brief Fires each time this emitter emits -- for audio, screen shake, gameplay. */
    coopa::event::Signal<const EnvPulse&> on_fired;

private:
    coopa::event::ScopedConnection anim_;
};

/** @brief Registers the environment's component parsers: "EnvInfluence", "EnvPulseEmitter". */
void register_env_components();

} // namespace env
} // namespace toy

#endif // TOYENGINE_ENV_ENV_INFLUENCE_H
