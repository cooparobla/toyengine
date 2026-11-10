/**
 * @file panner.h
 * @brief Analytic binaural panner: Woodworth ITD + per-ear head-shadow
 * lowpass/ILD + distance/cone/doppler, no HRIR dataset or FFT.
 */

#ifndef SFXCOOPA_SPATIAL_PANNER_H
#define SFXCOOPA_SPATIAL_PANNER_H

#include <sfxcoopa/spatial/listener.h>
#include <sfxcoopa/spatial/attenuation.h>
#include <sfxcoopa/dsp/delay_line.h>
#include <sfxcoopa/dsp/one_pole.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coopa {
namespace sfx {
namespace spatial {

/** @brief Speed of sound in air, m/s -- shared by the ITD and doppler calculations. */
inline constexpr float k_speed_of_sound = 343.0f;

/** @brief Average human head radius, m -- the Woodworth ITD model's free parameter. */
inline constexpr float k_head_radius = 0.0875f;

/** @brief Head-shadow lowpass cutoff when an ear is maximally shadowed (source at the opposite ear). */
inline constexpr float k_shadow_min_hz = 1500.0f;

/** @brief Head-shadow lowpass cutoff when an ear faces the source directly (no shadowing). */
inline constexpr float k_shadow_max_hz = 18000.0f;

/** @brief Broadband ILD gain floor applied to the fully-shadowed ear (on top of the lowpass). */
inline constexpr float k_ild_min_gain = 0.3f;

/**
 * @struct SpatialParams
 * @brief Per-source 3D audio parameters: where it is, how it attenuates,
 * and which spatialization effects apply.
 */
struct SpatialParams {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    float spatial_blend = 1.0f; /**< 0 = pure 2D (centered stereo), 1 = fully 3D. */
    float spread = 0.0f;        /**< Widens the image (reduces ITD/ILD) as distance approaches min_distance. */
    AttenuationCurve attenuation{};
    float inner_cone_angle = 360.0f; /**< Full angle, degrees, within which the cone applies no attenuation. */
    float outer_cone_angle = 360.0f; /**< Full angle, degrees, beyond which gain is outer_cone_gain. */
    float outer_cone_gain = 1.0f;
    float doppler_factor = 1.0f;
    bool enable_itd = true;
    bool enable_head_shadow = true;
};

/** @brief Per-ear rendering targets computed by BinauralPanner::compute(). */
struct EarParams {
    float gain_l = 1.0f, gain_r = 1.0f;
    float delay_l = 0.0f, delay_r = 0.0f;   /**< In samples, at the sample rate compute() was called with. */
    float cutoff_l = k_shadow_max_hz, cutoff_r = k_shadow_max_hz; /**< Head-shadow lowpass cutoff, Hz. */
    float pitch_scale = 1.0f;               /**< Doppler-driven playback rate multiplier; Voice ramps toward it and scales its pitch by it. */
};

/**
 * @class BinauralPanner
 * @brief Computes and applies analytic binaural panning for a mono source.
 *
 * compute() is a pure function of (SpatialParams, Listener, sample_rate) --
 * intended to run on the main thread once per update(dt), with its result
 * pushed to the voice as a SetVoiceSpatial command (see Voice in
 * mixer/voice.h). process() applies whatever target was last set via
 * set_target(), holding it constant until the next set_target() -- the
 * caller controls the update cadence by how often it calls set_target().
 *
 * There is no virtual IPanner base: BinauralPanner is the only panner. Voice
 * uses it only through compute()/set_target()/process(), so an HRTF panner
 * could be swapped in without touching the Voice/Mixer call sites.
 *
 * @code
 * BinauralPanner panner(48000);
 * EarParams target = panner.compute(spatial_params, listener, 48000);
 * panner.set_target(target);
 * panner.process(mono_in, stereo_out, frames);
 * @endcode
 */
class BinauralPanner {
public:
    explicit BinauralPanner(uint32_t sample_rate, uint32_t max_delay_samples = 128)
        : sample_rate_(sample_rate), delay_l_(max_delay_samples), delay_r_(max_delay_samples) {
        lp_l_.set_cutoff(k_shadow_max_hz, sample_rate_);
        lp_r_.set_cutoff(k_shadow_max_hz, sample_rate_);
    }

    /**
     * @brief Computes this frame's ear targets from a source's spatial
     * parameters and the listener's current pose. Pure/stateless.
     */
    EarParams compute(const SpatialParams& p, const Listener& listener, uint32_t sample_rate) const {
        const glm::vec3 offset = p.position - listener.position;
        const float distance = glm::length(offset);
        const bool has_direction = distance > 1e-6f;
        const glm::vec3 local = has_direction ? (glm::transpose(listener.basis()) * offset) : glm::vec3(0.0f);
        const float azimuth = has_direction ? std::atan2(local.x, -local.z) : 0.0f;

        EarParams out{};

        if (p.enable_itd) {
            const float theta = azimuth;
            float itd_seconds;
            if (std::fabs(theta) <= 1.57079633f) {
                itd_seconds = (k_head_radius / k_speed_of_sound) * (theta + std::sin(theta));
            } else {
                const float sign = theta >= 0.0f ? 1.0f : -1.0f;
                const float abs_theta = std::fabs(theta);
                itd_seconds = (k_head_radius / k_speed_of_sound) *
                              (3.14159265f - abs_theta + std::sin(abs_theta)) * sign;
            }
            const float itd_samples = itd_seconds * static_cast<float>(sample_rate);
            if (itd_samples >= 0.0f) {
                out.delay_l = itd_samples;
                out.delay_r = 0.0f;
            } else {
                out.delay_l = 0.0f;
                out.delay_r = -itd_samples;
            }
        }

        if (p.enable_head_shadow) {
            const float angle_r = azimuth - 1.57079633f;  // right ear sits at +90 deg azimuth
            const float angle_l = azimuth + 1.57079633f;  // left ear sits at -90 deg azimuth
            const auto shape = [](float angle) { return 0.5f + 0.5f * std::cos(angle); };
            out.cutoff_r = k_shadow_min_hz + (k_shadow_max_hz - k_shadow_min_hz) * shape(angle_r);
            out.cutoff_l = k_shadow_min_hz + (k_shadow_max_hz - k_shadow_min_hz) * shape(angle_l);
            out.gain_r *= k_ild_min_gain + (1.0f - k_ild_min_gain) * shape(angle_r);
            out.gain_l *= k_ild_min_gain + (1.0f - k_ild_min_gain) * shape(angle_l);
        }

        const float dist_gain = p.attenuation.evaluate(distance);
        out.gain_l *= dist_gain;
        out.gain_r *= dist_gain;

        // Cone: attenuates based on how far off-axis the listener is from
        // the source's own facing direction.
        {
            const glm::vec3 dir = glm::normalize(p.direction);
            const glm::vec3 to_listener = has_direction ? (-offset / distance) : dir;
            const float cos_angle = std::clamp(glm::dot(dir, to_listener), -1.0f, 1.0f);
            const float angle_deg = glm::degrees(std::acos(cos_angle));
            const float half_inner = p.inner_cone_angle * 0.5f;
            const float half_outer = p.outer_cone_angle * 0.5f;
            float cone_gain = 1.0f;
            if (angle_deg >= half_outer) {
                cone_gain = p.outer_cone_gain;
            } else if (angle_deg > half_inner && half_outer > half_inner) {
                const float t = std::clamp((angle_deg - half_inner) / (half_outer - half_inner), 0.0f, 1.0f);
                const float smooth = t * t * (3.0f - 2.0f * t);
                cone_gain = 1.0f + (p.outer_cone_gain - 1.0f) * smooth;
            }
            out.gain_l *= cone_gain;
            out.gain_r *= cone_gain;
        }

        // Doppler: dir points from listener to source, so a source moving
        // toward the listener (velocity anti-parallel to dir) raises pitch.
        {
            const glm::vec3 dir = has_direction ? (offset / distance) : glm::vec3(0.0f, 0.0f, 1.0f);
            const float denom = k_speed_of_sound + glm::dot(p.velocity, dir);
            const float numer = k_speed_of_sound + glm::dot(listener.velocity, dir);
            const float raw = std::fabs(denom) > 1e-4f ? numer / denom : 1.0f;
            const float scaled = 1.0f + (raw - 1.0f) * p.doppler_factor;
            out.pitch_scale = std::clamp(scaled, 0.5f, 2.0f);
        }

        // Blend toward centered stereo, and spread narrows the image near min_distance.
        {
            const float avg_gain = (out.gain_l + out.gain_r) * 0.5f;
            const float avg_delay = (out.delay_l + out.delay_r) * 0.5f;
            const float avg_cutoff = (out.cutoff_l + out.cutoff_r) * 0.5f;
            const float blend_t = 1.0f - std::clamp(p.spatial_blend, 0.0f, 1.0f);
            const float spread_t = p.spread * std::max(0.0f, 1.0f - distance / std::max(p.attenuation.min_distance, 1e-4f));
            const float t = std::clamp(blend_t + spread_t - blend_t * spread_t, 0.0f, 1.0f);
            out.gain_l += (avg_gain - out.gain_l) * t;
            out.gain_r += (avg_gain - out.gain_r) * t;
            out.delay_l += (avg_delay - out.delay_l) * t;
            out.delay_r += (avg_delay - out.delay_r) * t;
            out.cutoff_l += (avg_cutoff - out.cutoff_l) * t;
            out.cutoff_r += (avg_cutoff - out.cutoff_r) * t;
        }

        return out;
    }

    /** @brief Updates the held target ear parameters that process() renders. Held constant until the next call. */
    void set_target(const EarParams& target) {
        target_ = target;
        delay_l_.set_delay_samples(target.delay_l);
        delay_r_.set_delay_samples(target.delay_r);
        lp_l_.set_cutoff(target.cutoff_l, sample_rate_);
        lp_r_.set_cutoff(target.cutoff_r, sample_rate_);
    }

    /** @brief Renders `frames` mono input samples into interleaved stereo output using the current target. */
    void process(const float* mono_in, float* stereo_out, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i) {
            const float x = mono_in[i];
            delay_l_.push(x);
            delay_r_.push(x);
            const float l = lp_l_.process(delay_l_.read()) * target_.gain_l;
            const float r = lp_r_.process(delay_r_.read()) * target_.gain_r;
            stereo_out[2 * i] = l;
            stereo_out[2 * i + 1] = r;
        }
    }

    /** @brief Resets all internal filter/delay state. */
    void reset() {
        delay_l_.reset();
        delay_r_.reset();
        lp_l_.reset();
        lp_r_.reset();
    }

private:
    uint32_t sample_rate_;
    EarParams target_{};
    coopa::sfx::dsp::DelayLine delay_l_;
    coopa::sfx::dsp::DelayLine delay_r_;
    coopa::sfx::dsp::OnePoleLowpass lp_l_;
    coopa::sfx::dsp::OnePoleLowpass lp_r_;
};

}  // namespace spatial
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_SPATIAL_PANNER_H
