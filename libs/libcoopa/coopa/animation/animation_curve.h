/**
 * @file animation_curve.h
 * @brief A sorted list of Keyframes, sampled by time.
 */

#ifndef COOPA_ANIMATION_ANIMATION_CURVE_H
#define COOPA_ANIMATION_ANIMATION_CURVE_H

#include <coopa/animation/keyframe.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace coopa {
namespace anim {

/**
 * @class AnimationCurve
 * @brief Holds a time-sorted list of Keyframes and samples between them.
 *
 * sample() is STATELESS by design — no mutable "last key" cursor. An
 * AnimationCurve is owned by a shared, immutable AnimationClip payload
 * (coopa::asset::AssetHandle<AnimationClip>::get() returns const), and one
 * clip can be played by many Animators simultaneously, evaluated from worker
 * threads. A mutable cursor here would be a data race; std::upper_bound's
 * O(log n) search costs nothing a game-sized clip (a handful of keys) would
 * ever notice.
 */
class AnimationCurve {
public:
    /** @brief Appends a keyframe. Does not sort — call sort_keys() once all keys are added. */
    void add_key(const Keyframe& key) { keys_.push_back(key); }

    /** @brief Sorts keys by time ascending. Call once after all add_key() calls (e.g. at load time). */
    void sort_keys() {
        std::sort(keys_.begin(), keys_.end(),
            [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }

    /** @brief Returns the keys, in whatever order they were last sorted/added. */
    const std::vector<Keyframe>& keys() const { return keys_; }

    /** @brief True if this curve has no keys. */
    bool empty() const { return keys_.empty(); }

    /** @brief The time of the last key, or 0 if empty. Keys must be sorted. */
    float length() const { return keys_.empty() ? 0.0f : keys_.back().time; }

    /**
     * @brief Samples the curve at the given time into `out`.
     *
     * Clamps before the first key and after the last key. Keys must already
     * be sorted (via sort_keys()) — this performs a binary search assuming
     * ascending time order.
     *
     * @param time Time to sample at.
     * @param component_count How many of value[0..3] to write (1..4).
     * @param out Destination buffer; must have room for component_count floats.
     */
    void sample(float time, uint8_t component_count, float* out) const {
        if (keys_.empty()) {
            for (uint8_t c = 0; c < component_count; ++c) out[c] = 0.0f;
            return;
        }
        if (keys_.size() == 1 || time <= keys_.front().time) {
            const Keyframe& k = keys_.front();
            for (uint8_t c = 0; c < component_count; ++c) out[c] = k.value[c];
            return;
        }
        if (time >= keys_.back().time) {
            const Keyframe& k = keys_.back();
            for (uint8_t c = 0; c < component_count; ++c) out[c] = k.value[c];
            return;
        }

        // First key strictly after `time` — upper_bound on time.
        auto it = std::upper_bound(keys_.begin(), keys_.end(), time,
            [](float t, const Keyframe& k) { return t < k.time; });
        const Keyframe& next = *it;
        const Keyframe& prev = *(it - 1);

        float span = next.time - prev.time;
        float raw_t = (span > 0.0f) ? (time - prev.time) / span : 0.0f;
        float t = apply_easing(prev.easing, raw_t);

        for (uint8_t c = 0; c < component_count; ++c) {
            out[c] = prev.value[c] + (next.value[c] - prev.value[c]) * t;
        }
    }

private:
    std::vector<Keyframe> keys_;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ANIMATION_CURVE_H
