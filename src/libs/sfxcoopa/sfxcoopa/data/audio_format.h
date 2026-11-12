/**
 * @file audio_format.h
 * @brief Sample format enumeration and the engine's ambient audio constants.
 */

#ifndef SFXCOOPA_DATA_AUDIO_FORMAT_H
#define SFXCOOPA_DATA_AUDIO_FORMAT_H

#include <cstdint>

namespace coopa {
namespace sfx {
namespace data {

/** @brief PCM sample storage format, mirroring miniaudio's ma_format. */
enum class SampleFormat {
    U8,
    S16,
    S24,
    S32,
    F32
};

/** @brief Sample rate + channel count pair describing a PCM stream. */
struct AudioFormat {
    uint32_t sample_rate = 48000; /**< Frames per second. */
    uint32_t channels = 2;        /**< Interleaved channel count. */
};

/** @brief Default sample rate (DeviceConfig's default). */
inline constexpr uint32_t k_default_sample_rate = 48000;

/** @brief Sub-block granularity (in frames) at which voice parameters ramp toward their targets. */
inline constexpr uint32_t k_control_block_frames = 64;

/** @brief Default voice pool size; playing beyond the pool size steals the lowest-priority/oldest voice. */
inline constexpr uint32_t k_max_voices = 128;

}  // namespace data
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DATA_AUDIO_FORMAT_H
