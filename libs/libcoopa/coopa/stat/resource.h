/**
 * @file resource.h
 * @brief A clamped, regenerating current/max quantity -- health, stamina, mana, etc.
 */

#ifndef COOPA_STAT_RESOURCE_H
#define COOPA_STAT_RESOURCE_H

#include <coopa/event/signal.h>
#include <algorithm>
#include <cmath>

namespace coopa {
namespace stat {

/**
 * @class Resource
 * @brief A single current/max gameplay quantity with optional delayed regen.
 *
 * Deliberately generic -- "health", "stamina", and "mana" are all just a
 * Resource with different regen_per_second/regen_delay, looked up by name
 * through StatBlock. A UI bar (uicoopa's ProgressBar) binds to one via
 * ProgressBar::bind(), reading on_changed to stay in sync without polling.
 *
 * `on_changed` only fires when the value actually moves past a small
 * epsilon -- a full, undamaged resource ticking every frame stays silent,
 * so a bound UI widget doesn't churn work for a value that never visibly
 * changes. `on_depleted` is edge-triggered: it fires once when current
 * crosses from > 0 to <= 0, not on every frame it stays at zero.
 *
 * Move-only (holds coopa::event::Signal members).
 */
class Resource {
public:
    float current          = 100.0f;
    float max               = 100.0f;
    float regen_per_second  = 0.0f;
    /** @brief Seconds after the last damage() before tick() resumes regenerating --
     *         the "wait a beat before stamina climbs back" pattern. 0 means regen
     *         resumes immediately. */
    float regen_delay       = 0.0f;

    Resource() = default;
    Resource(float max_value, float regen = 0.0f, float delay = 0.0f)
        : current(max_value), max(max_value), regen_per_second(regen), regen_delay(delay) {}

    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    Resource(Resource&&) = default;
    Resource& operator=(Resource&&) = default;

    float normalized() const { return max > 0.0f ? std::clamp(current / max, 0.0f, 1.0f) : 0.0f; }
    bool  is_depleted() const { return current <= 0.0f; }
    bool  is_full() const { return current >= max; }

    /** @brief Reduces current by `amount` (clamped at 0), resets the regen-delay
     *         countdown, and notifies. */
    void damage(float amount) {
        if (amount <= 0.0f) return;
        set_current_(current - amount);
        time_since_damage_ = 0.0f;
    }

    /** @brief Increases current by `amount` (clamped at max) and notifies. Does
     *         not affect the regen-delay countdown. */
    void heal(float amount) {
        if (amount <= 0.0f) return;
        set_current_(current + amount);
    }

    /** @brief Alias for damage() -- reads better for a non-combat drain (stamina
     *         spent on a sprint, not incoming harm). */
    void drain(float amount) { damage(amount); }

    /** @brief Sets a new max. `keep_ratio` rescales `current` to preserve
     *         normalized() rather than clamping it against the new max outright. */
    void set_max(float new_max, bool keep_ratio = false) {
        float ratio = normalized();
        max = std::max(0.0f, new_max);
        if (keep_ratio) {
            set_current_(max * ratio);
        } else {
            set_current_(current);  // re-clamp against the new max
        }
    }

    void set_current(float value) { set_current_(value); }

    /**
     * @brief Advances the regen-delay countdown and, once elapsed, applies
     *        regen_per_second * dt, clamped at max. No-op once already full.
     */
    void tick(float dt) {
        if (dt <= 0.0f) return;
        time_since_damage_ += dt;
        if (regen_per_second <= 0.0f) return;
        if (time_since_damage_ < regen_delay) return;
        if (is_full()) return;
        set_current_(current + regen_per_second * dt);
    }

    coopa::event::Signal<float, float> on_changed;   /**< (current, max) */
    coopa::event::Signal<>             on_depleted;

private:
    static constexpr float kEpsilon = 1e-5f;
    float time_since_damage_ = 0.0f;

    void set_current_(float value) {
        float clamped = std::clamp(value, 0.0f, max);
        bool changed = std::abs(clamped - current) > kEpsilon;
        bool crossed_zero = current > 0.0f && clamped <= 0.0f;
        current = clamped;
        if (changed) on_changed.emit(current, max);
        if (crossed_zero) on_depleted.emit();
    }
};

} // namespace stat
} // namespace coopa

#endif // COOPA_STAT_RESOURCE_H
