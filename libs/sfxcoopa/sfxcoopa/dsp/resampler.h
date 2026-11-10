/**
 * @file resampler.h
 * @brief Cubic-Hermite fractional-position sample rate / pitch converter.
 */

#ifndef SFXCOOPA_DSP_RESAMPLER_H
#define SFXCOOPA_DSP_RESAMPLER_H

#include <cstdint>
#include <vector>

namespace coopa {
namespace sfx {
namespace dsp {

/**
 * @class Resampler
 * @brief Converts between sample rates (and applies pitch shift) via a
 * 4-point Catmull-Rom cubic Hermite interpolator.
 *
 * Cubic Hermite needs samples at fractional-position offsets -1, 0, +1, +2
 * relative to its read cursor, so it must retain history/lookahead state
 * across process() calls rather than treating each call's input in
 * isolation -- otherwise the very first and last few output samples of
 * every chunk would be wrong, and chunking the same input differently
 * would produce different output. Internally this is a single buffer of
 * not-yet-fully-consumed input, front-compacted (never reallocated once its
 * initial capacity is reserved) as the read cursor advances past samples no
 * future tap will need again.
 *
 * @code
 * coopa::sfx::dsp::Resampler r;
 * r.set_ratio(48000.0 / 44100.0);  // output_rate / input_rate
 * uint32_t in_consumed = 0, out_written = 0;
 * r.process(in, in_frames, in_consumed, out, out_frames, out_written);
 * @endcode
 */
class Resampler {
public:
    /** @brief Constructs a resampler at unity ratio (pass-through, with cubic-Hermite smoothing). */
    explicit Resampler(uint32_t reserve_frames = 4096) {
        // 3 leading zero-history samples so the earliest taps (before any
        // real input has arrived) resolve to silence instead of reading
        // uninitialized data; pos_ starts at 3.0 so the first tap set
        // (indices 2,3,4,5) lines up with [history, first, second, third].
        pending_.reserve(static_cast<size_t>(reserve_frames) + 8);
        pending_.assign(3, 0.0f);
        pos_ = 3.0;
    }

    /** @brief Sets the conversion ratio: output_rate / input_rate. 2.0 doubles the frame count. */
    void set_ratio(double ratio) { step_ = 1.0 / ratio; }

    /**
     * @brief Consumes input and produces output, both up to the given counts.
     *
     * Always accepts (buffers) the entirety of `in` -- `in_consumed` is
     * always `in_frames`. `out_written` may be less than `out_frames` if
     * not enough input has been supplied yet (across this and prior calls)
     * to compute further output samples; call again with more input to
     * continue. For a single whole-buffer conversion (the entire input already
     * in memory), pad the input with a couple of
     * trailing repeated samples before the final call so the last output
     * samples -- which need lookahead past the true end of the file -- can
     * still be produced.
     */
    void process(const float* in, uint32_t in_frames, uint32_t& in_consumed,
                 float* out, uint32_t out_frames, uint32_t& out_written) {
        pending_.insert(pending_.end(), in, in + in_frames);
        in_consumed = in_frames;

        uint32_t written = 0;
        const int64_t count = static_cast<int64_t>(pending_.size());
        while (written < out_frames) {
            const auto i1 = static_cast<int64_t>(pos_);
            const int64_t i3 = i1 + 2;
            if (i3 >= count) break;  // not enough lookahead yet
            const int64_t i0 = i1 - 1;
            const int64_t i2 = i1 + 1;
            const float frac = static_cast<float>(pos_ - static_cast<double>(i1));
            const float y0 = pending_[static_cast<size_t>(i0)];
            const float y1 = pending_[static_cast<size_t>(i1)];
            const float y2 = pending_[static_cast<size_t>(i2)];
            const float y3 = pending_[static_cast<size_t>(i3)];
            out[written++] = cubic_hermite_(y0, y1, y2, y3, frac);
            pos_ += step_;
        }
        out_written = written;

        // Compact: drop everything before the earliest index any future tap
        // could still reference (one sample of slack beyond i0's minimum).
        const int64_t safe_drop = static_cast<int64_t>(pos_) - 2;
        if (safe_drop > 0) {
            const auto drop = static_cast<size_t>(safe_drop);
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(drop));
            pos_ -= static_cast<double>(drop);
        }
    }

    /** @brief Resets the resampler to its just-constructed state. */
    void reset() {
        pending_.clear();
        pending_.assign(3, 0.0f);
        pos_ = 3.0;
    }

private:
    static float cubic_hermite_(float y0, float y1, float y2, float y3, float frac) {
        // Catmull-Rom cubic Hermite spline: exact passthrough at frac == 0.
        const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float a2 = -0.5f * y0 + 0.5f * y2;
        const float a3 = y1;
        return ((a0 * frac + a1) * frac + a2) * frac + a3;
    }

    std::vector<float> pending_; /**< Not-yet-fully-consumed input, front-compacted each call. */
    double pos_ = 3.0;           /**< Absolute fractional read cursor into pending_. */
    double step_ = 1.0;          /**< Input-frame advance per output sample (1/ratio). */
};

}  // namespace dsp
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DSP_RESAMPLER_H
