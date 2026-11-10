/**
 * @file biquad.h
 * @brief RBJ Audio EQ Cookbook biquad filter (Direct Form I).
 */

#ifndef SFXCOOPA_DSP_BIQUAD_H
#define SFXCOOPA_DSP_BIQUAD_H

#include <cmath>
#include <cstdint>

namespace coopa {
namespace sfx {
namespace dsp {

/**
 * @class Biquad
 * @brief A single second-order IIR section, coefficients from the RBJ
 * "Audio EQ Cookbook", evaluated in Direct Form I.
 *
 * @code
 * coopa::sfx::dsp::Biquad lp = coopa::sfx::dsp::Biquad::lowpass(1000.0f, 44100, 0.707f);
 * float y = lp.process(x);
 * @endcode
 */
class Biquad {
public:
    /** @brief Constructs an identity (pass-through) biquad. */
    Biquad() = default;

    /** @brief RBJ cookbook lowpass, -3dB at `freq_hz`, resonance `q`. */
    static Biquad lowpass(float freq_hz, uint32_t sample_rate, float q = 0.70710678f) {
        const Coeffs c = rbj_(freq_hz, sample_rate, q);
        const float b0 = (1.0f - c.cos_w0) * 0.5f;
        const float b1 = 1.0f - c.cos_w0;
        const float b2 = (1.0f - c.cos_w0) * 0.5f;
        const float a0 = 1.0f + c.alpha;
        const float a1 = -2.0f * c.cos_w0;
        const float a2 = 1.0f - c.alpha;
        return Biquad(b0, b1, b2, a0, a1, a2);
    }

    /** @brief RBJ cookbook highpass, -3dB at `freq_hz`, resonance `q`. */
    static Biquad highpass(float freq_hz, uint32_t sample_rate, float q = 0.70710678f) {
        const Coeffs c = rbj_(freq_hz, sample_rate, q);
        const float b0 = (1.0f + c.cos_w0) * 0.5f;
        const float b1 = -(1.0f + c.cos_w0);
        const float b2 = (1.0f + c.cos_w0) * 0.5f;
        const float a0 = 1.0f + c.alpha;
        const float a1 = -2.0f * c.cos_w0;
        const float a2 = 1.0f - c.alpha;
        return Biquad(b0, b1, b2, a0, a1, a2);
    }

    /** @brief RBJ cookbook peaking EQ: `gain_db` boost/cut centred at `freq_hz`. */
    static Biquad peaking(float freq_hz, uint32_t sample_rate, float q, float gain_db) {
        const Coeffs c = rbj_(freq_hz, sample_rate, q);
        const float a_amp = std::pow(10.0f, gain_db / 40.0f);
        const float b0 = 1.0f + c.alpha * a_amp;
        const float b1 = -2.0f * c.cos_w0;
        const float b2 = 1.0f - c.alpha * a_amp;
        const float a0 = 1.0f + c.alpha / a_amp;
        const float a1 = -2.0f * c.cos_w0;
        const float a2 = 1.0f - c.alpha / a_amp;
        return Biquad(b0, b1, b2, a0, a1, a2);
    }

    /** @brief RBJ cookbook low shelf: `gain_db` boost/cut below `freq_hz`. */
    static Biquad lowshelf(float freq_hz, uint32_t sample_rate, float gain_db, float shelf_slope = 1.0f) {
        const Coeffs c = rbj_shelf_(freq_hz, sample_rate, gain_db, shelf_slope);
        const float b0 = c.a * ((c.a + 1.0f) - (c.a - 1.0f) * c.cos_w0 + 2.0f * c.sqrt_a * c.alpha);
        const float b1 = 2.0f * c.a * ((c.a - 1.0f) - (c.a + 1.0f) * c.cos_w0);
        const float b2 = c.a * ((c.a + 1.0f) - (c.a - 1.0f) * c.cos_w0 - 2.0f * c.sqrt_a * c.alpha);
        const float a0 = (c.a + 1.0f) + (c.a - 1.0f) * c.cos_w0 + 2.0f * c.sqrt_a * c.alpha;
        const float a1 = -2.0f * ((c.a - 1.0f) + (c.a + 1.0f) * c.cos_w0);
        const float a2 = (c.a + 1.0f) + (c.a - 1.0f) * c.cos_w0 - 2.0f * c.sqrt_a * c.alpha;
        return Biquad(b0, b1, b2, a0, a1, a2);
    }

    /** @brief RBJ cookbook high shelf: `gain_db` boost/cut above `freq_hz`. */
    static Biquad highshelf(float freq_hz, uint32_t sample_rate, float gain_db, float shelf_slope = 1.0f) {
        const Coeffs c = rbj_shelf_(freq_hz, sample_rate, gain_db, shelf_slope);
        const float b0 = c.a * ((c.a + 1.0f) + (c.a - 1.0f) * c.cos_w0 + 2.0f * c.sqrt_a * c.alpha);
        const float b1 = -2.0f * c.a * ((c.a - 1.0f) + (c.a + 1.0f) * c.cos_w0);
        const float b2 = c.a * ((c.a + 1.0f) + (c.a - 1.0f) * c.cos_w0 - 2.0f * c.sqrt_a * c.alpha);
        const float a0 = (c.a + 1.0f) - (c.a - 1.0f) * c.cos_w0 + 2.0f * c.sqrt_a * c.alpha;
        const float a1 = 2.0f * ((c.a - 1.0f) - (c.a + 1.0f) * c.cos_w0);
        const float a2 = (c.a + 1.0f) - (c.a - 1.0f) * c.cos_w0 - 2.0f * c.sqrt_a * c.alpha;
        return Biquad(b0, b1, b2, a0, a1, a2);
    }

    /** @brief Processes one sample through Direct Form I. */
    float process(float x) {
        const float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return y;
    }

    /** @brief Resets internal state (input/output history) to zero. */
    void reset() { x1_ = x2_ = y1_ = y2_ = 0.0f; }

private:
    struct Coeffs {
        float cos_w0;
        float alpha;
        float a = 1.0f;
        float sqrt_a = 1.0f;
    };

    static Coeffs rbj_(float freq_hz, uint32_t sample_rate, float q) {
        const float w0 = 2.0f * 3.14159265358979323846f * freq_hz / static_cast<float>(sample_rate);
        Coeffs c;
        c.cos_w0 = std::cos(w0);
        c.alpha = std::sin(w0) / (2.0f * q);
        return c;
    }

    static Coeffs rbj_shelf_(float freq_hz, uint32_t sample_rate, float gain_db, float shelf_slope) {
        const float w0 = 2.0f * 3.14159265358979323846f * freq_hz / static_cast<float>(sample_rate);
        Coeffs c;
        c.cos_w0 = std::cos(w0);
        c.a = std::pow(10.0f, gain_db / 40.0f);
        c.sqrt_a = std::sqrt(c.a);
        const float sin_w0 = std::sin(w0);
        c.alpha = sin_w0 * 0.5f * std::sqrt((c.a + 1.0f / c.a) * (1.0f / shelf_slope - 1.0f) + 2.0f);
        return c;
    }

    Biquad(float b0, float b1, float b2, float a0, float a1, float a2)
        : b0_(b0 / a0), b1_(b1 / a0), b2_(b2 / a0), a1_(a1 / a0), a2_(a2 / a0) {}

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f; /**< Normalized feed-forward coefficients. */
    float a1_ = 0.0f, a2_ = 0.0f;             /**< Normalized feedback coefficients (a0 == 1 after normalization). */
    float x1_ = 0.0f, x2_ = 0.0f;             /**< Input history. */
    float y1_ = 0.0f, y2_ = 0.0f;             /**< Output history. */
};

}  // namespace dsp
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DSP_BIQUAD_H
