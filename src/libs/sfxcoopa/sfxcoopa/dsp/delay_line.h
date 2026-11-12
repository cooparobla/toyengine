/**
 * @file delay_line.h
 * @brief Circular buffer with fractional (linearly interpolated) read, used
 * for the sub-sample ITD delay in BinauralPanner.
 */

#ifndef SFXCOOPA_DSP_DELAY_LINE_H
#define SFXCOOPA_DSP_DELAY_LINE_H

#include <cstdint>
#include <vector>

namespace coopa {
namespace sfx {
namespace dsp {

/**
 * @class DelayLine
 * @brief Fixed-capacity circular f32 buffer supporting fractional-sample delay reads.
 *
 * Linear interpolation between the two nearest integer-delay taps is what
 * makes sub-sample ITD -- and therefore smooth head rotation with no
 * zipper noise -- possible at all: without it, delay could only move in
 * whole-sample steps, audible as discrete clicks as a source sweeps past
 * the listener.
 */
class DelayLine {
public:
    /**
     * @brief Constructs a delay line able to hold delays up to `max_delay_samples`.
     * @param max_delay_samples Maximum delay, in samples, that set_delay_samples() may request.
     */
    explicit DelayLine(uint32_t max_delay_samples)
        : buffer_(static_cast<size_t>(max_delay_samples) + 4, 0.0f) {}

    /** @brief Sets the read delay, in fractional samples behind the most recently pushed sample. */
    void set_delay_samples(float delay_samples) { delay_samples_ = delay_samples; }

    /** @brief Pushes a new sample, advancing the write cursor. */
    void push(float sample) {
        buffer_[write_pos_] = sample;
        write_pos_ = (write_pos_ + 1) % buffer_.size();
    }

    /** @brief Reads the currently configured delay via linear interpolation. */
    float read() const {
        const size_t capacity = buffer_.size();
        double pos = static_cast<double>(write_pos_) - 1.0 - static_cast<double>(delay_samples_);
        while (pos < 0.0) pos += static_cast<double>(capacity);
        const auto i0 = static_cast<size_t>(pos) % capacity;
        const size_t i1 = (i0 + 1) % capacity;
        const float frac = static_cast<float>(pos - static_cast<double>(static_cast<int64_t>(pos)));
        return buffer_[i0] * (1.0f - frac) + buffer_[i1] * frac;
    }

    /** @brief Clears all buffered samples and resets the write cursor. */
    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_pos_ = 0;
    }

private:
    std::vector<float> buffer_;
    size_t write_pos_ = 0;
    float delay_samples_ = 0.0f;
};

}  // namespace dsp
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DSP_DELAY_LINE_H
