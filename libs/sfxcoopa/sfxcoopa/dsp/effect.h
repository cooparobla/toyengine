/**
 * @file effect.h
 * @brief Interface for per-bus DSP effect chain links.
 */

#ifndef SFXCOOPA_DSP_EFFECT_H
#define SFXCOOPA_DSP_EFFECT_H

#include <sfxcoopa/data/audio_format.h>

#include <cstdint>

namespace coopa {
namespace sfx {
namespace dsp {

/**
 * @class IEffect
 * @brief In-place audio effect processed as part of a MixerBus's effect chain.
 *
 * All allocation must happen in prepare(); process() must be allocation-free
 * and lock-free so it is safe to call from the audio thread.
 */
class IEffect {
public:
    virtual ~IEffect() = default;

    /**
     * @brief Allocates any internal buffers needed to process up to `max_frames` frames.
     * @param format Sample rate / channel count the effect will run at.
     * @param max_frames Largest frame count process() will ever be called with.
     */
    virtual void prepare(const coopa::sfx::data::AudioFormat& format, uint32_t max_frames) = 0;

    /**
     * @brief Processes `frames` interleaved frames of `channels` channels, in place.
     */
    virtual void process(float* buffer, uint32_t frames, uint32_t channels) = 0;

    /** @brief Clears any internal filter/delay state (not buffer allocations). */
    virtual void reset() {}
};

}  // namespace dsp
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DSP_EFFECT_H
