/**
 * @file one_pole.h
 * @brief One-pole (single-order) lowpass filter for smoothing and head-shadow shaping.
 */

#ifndef SFXCOOPA_DSP_ONE_POLE_H
#define SFXCOOPA_DSP_ONE_POLE_H

#include <cmath>
#include <cstdint>

namespace coopa {
namespace sfx {
namespace dsp {

/**
 * @class OnePoleLowpass
 * @brief A single-pole exponential-moving-average lowpass filter.
 *
 * Used both as a DSP building block (per-ear head-shadow shaping in
 * BinauralPanner) and as a general-purpose parameter smoother. Unity DC
 * gain: a constant input converges exactly to that value.
 */
class OnePoleLowpass {
public:
    /**
     * @brief Sets the -3dB cutoff frequency.
     * @param hz Cutoff frequency in Hz.
     * @param sample_rate Sample rate in Hz.
     */
    void set_cutoff(float hz, uint32_t sample_rate) {
        const float omega = 2.0f * 3.14159265358979323846f * hz / static_cast<float>(sample_rate);
        alpha_ = 1.0f - std::exp(-omega);
    }

    /** @brief Processes one sample, returning the filtered output. */
    float process(float x) {
        state_ += alpha_ * (x - state_);
        return state_;
    }

    /** @brief Resets internal state to zero. */
    void reset() { state_ = 0.0f; }

private:
    float alpha_ = 1.0f; /**< Smoothing coefficient in [0,1]; 1 = no filtering. */
    float state_ = 0.0f; /**< Last output sample. */
};

}  // namespace dsp
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DSP_ONE_POLE_H
