/**
 * @file keyframe.h
 * @brief A single timed value on an AnimationCurve, plus the easing applied to
 *        the segment leaving it.
 */

#ifndef COOPA_ANIMATION_KEYFRAME_H
#define COOPA_ANIMATION_KEYFRAME_H

#include <cstdint>
#include <string>

namespace coopa {
namespace anim {

/**
 * @enum Interpolation
 * @brief How to blend from one keyframe's value toward the next.
 *
 * Applies to the segment LEAVING the keyframe it is set on — the last
 * keyframe's easing is therefore never used. A uint8_t enum rather than a
 * full Hermite-tangent representation: it covers linear motion and the four
 * standard eased shapes at a fraction of the implementation cost, and being
 * a small enum means a future Hermite mode (with tangents in an optional
 * parallel array) is a non-breaking format addition rather than a rewrite.
 */
enum class Interpolation : uint8_t {
    Linear = 0,  ///< Straight lerp.
    Step,        ///< Holds this key's value for the whole segment; jumps at the end.
    EaseIn,      ///< Starts slow, accelerates: t^2.
    EaseOut,     ///< Starts fast, decelerates: t*(2-t).
    EaseInOut,   ///< Slow-fast-slow: smoothstep.
};

/**
 * @brief Parses an Interpolation from its YAML spelling.
 *
 * Accepts "linear", "step", "ease_in", "ease_out", "ease_in_out"
 * (case-sensitive, matching the clip YAML schema). Unrecognized strings fall
 * back to Interpolation::Linear.
 *
 * @param s The YAML `easing:` value.
 * @return The matching Interpolation, or Linear if unrecognized.
 */
inline Interpolation parse_interpolation(const std::string& s) {
    if (s == "step") return Interpolation::Step;
    if (s == "ease_in") return Interpolation::EaseIn;
    if (s == "ease_out") return Interpolation::EaseOut;
    if (s == "ease_in_out") return Interpolation::EaseInOut;
    return Interpolation::Linear;
}

/**
 * @brief Applies an easing curve to a normalized segment parameter.
 *
 * @param mode Which easing shape to apply.
 * @param t Normalized position within the segment, expected in [0, 1].
 * @return The eased parameter, still nominally in [0, 1].
 */
inline float apply_easing(Interpolation mode, float t) {
    switch (mode) {
        case Interpolation::Step:      return 0.0f;
        case Interpolation::EaseIn:    return t * t;
        case Interpolation::EaseOut:   return t * (2.0f - t);
        case Interpolation::EaseInOut: return (t < 0.5f) ? 2.0f * t * t
                                                         : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        case Interpolation::Linear:
        default:                       return t;
    }
}

/**
 * @struct Keyframe
 * @brief A single timed value, up to 4 components, on an AnimationCurve.
 *
 * value[] is pre-initialized to the glm-idiomatic xyzw/rgba identity {0,0,0,1}
 * so a clip that only ever writes fewer than 4 components (e.g. a 3-float
 * position, or a color with no explicit alpha) still has an unsurprising
 * value in the unused slots.
 */
struct Keyframe {
    float         time     = 0.0f;
    float         value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Interpolation easing   = Interpolation::Linear; ///< Applies to the segment LEAVING this key.
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_KEYFRAME_H
