/**
 * @file attenuation.h
 * @brief Distance-based gain rolloff curves.
 */

#ifndef SFXCOOPA_SPATIAL_ATTENUATION_H
#define SFXCOOPA_SPATIAL_ATTENUATION_H

#include <cmath>
#include <functional>

namespace coopa {
namespace sfx {
namespace spatial {

/** @brief Shape of the distance -> gain rolloff curve. */
enum class RolloffMode {
    Inverse,
    Linear,
    Logarithmic,
    Custom
};

/**
 * @struct AttenuationCurve
 * @brief Maps source-to-listener distance to a linear gain multiplier.
 *
 * Gain is always exactly 1.0 at or inside min_distance and exactly 0.0 at
 * or beyond max_distance, regardless of mode -- only the shape of the
 * curve between those two endpoints differs.
 */
struct AttenuationCurve {
    float min_distance = 1.0f;   /**< Distance at and inside which gain is 1.0 (no attenuation). */
    float max_distance = 500.0f; /**< Distance at and beyond which gain is 0.0 (inaudible). */
    RolloffMode mode = RolloffMode::Inverse;
    float rolloff = 1.0f;                 /**< Curve steepness for Inverse mode. */
    std::function<float(float)> custom;   /**< Used only when mode == Custom. */

    /** @brief Evaluates the curve at the given distance, returning a linear gain in [0,1]. */
    float evaluate(float distance) const {
        if (distance <= min_distance) return 1.0f;
        if (distance >= max_distance) return 0.0f;

        switch (mode) {
            case RolloffMode::Inverse:
                return min_distance / (min_distance + rolloff * (distance - min_distance));
            case RolloffMode::Linear:
                return 1.0f - (distance - min_distance) / (max_distance - min_distance);
            case RolloffMode::Logarithmic: {
                const float t = (distance - min_distance) / (max_distance - min_distance);
                return 1.0f - std::log10(1.0f + 9.0f * t);
            }
            case RolloffMode::Custom:
                return custom ? custom(distance) : 0.0f;
        }
        return 0.0f;
    }
};

}  // namespace spatial
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_SPATIAL_ATTENUATION_H
