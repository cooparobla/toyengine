/**
 * @file db.h
 * @brief Decibel <-> linear amplitude conversion helpers.
 */

#ifndef SFXCOOPA_UTIL_DB_H
#define SFXCOOPA_UTIL_DB_H

#include <cmath>

namespace coopa {
namespace sfx {
namespace util {

/** @brief Amplitude floor below which a gain is treated as silence, in dB. */
inline constexpr float k_silence_db = -80.0f;

/**
 * @brief Converts a decibel value to a linear amplitude multiplier.
 * @param db Gain in decibels (0 dB = unity, -inf = silence).
 * @return Linear amplitude: 10^(db/20).
 */
inline float db_to_linear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

/**
 * @brief Converts a linear amplitude multiplier to decibels.
 * @param linear Linear amplitude (1.0 = unity).
 * @return Gain in decibels; clamped to k_silence_db for linear <= 0.
 */
inline float linear_to_db(float linear) {
    if (linear <= 0.0f) return k_silence_db;
    float db = 20.0f * std::log10(linear);
    return db < k_silence_db ? k_silence_db : db;
}

}  // namespace util
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_UTIL_DB_H
