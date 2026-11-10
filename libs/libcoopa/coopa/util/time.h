/**
 * @file time.h
 * @brief Frame timing utility providing delta time and elapsed time.
 *
 * The single frame-clock implementation shared by every consumer. It lives
 * here rather than in gfxcoopa because it is pure std::chrono with zero
 * graphics content -- putting it in gfxcoopa would make every pure-logic
 * translation unit that just wants a stopwatch pull in the whole graphics
 * tree for no reason.
 */

#ifndef COOPA_UTIL_TIME_H
#define COOPA_UTIL_TIME_H

#include <chrono>
#include <cstdint>

namespace coopa {
namespace util {

/**
 * @class Time
 * @brief Tracks frame timing using a high-resolution clock.
 *
 * Call update() once per frame. Then query delta_time() for frame delta
 * and elapsed() for total time since construction.
 *
 * Usage:
 * @code
 * coopa::util::Time time;
 * while (running) {
 *     time.update();
 *     float dt = time.delta_time();
 * }
 * @endcode
 */
class Time {
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;
public:
    /**
     * @brief Constructs the timer, recording the start time.
     */
    Time()
        : start_(Clock::now()), last_(Clock::now()),
          delta_time_(0.0f), frame_count_(0)
    {}

    /**
     * @brief Updates the timer. Call once per frame at the start of the frame loop.
     */
    void update() {
        auto now = Clock::now();
        delta_time_ = std::chrono::duration<float>(now - last_).count();
        last_ = now;
        ++frame_count_;
    }

    /**
     * @brief Returns the time in seconds since the last update() call.
     * @return Delta time in seconds.
     */
    float delta_time() const { return delta_time_; }

    /**
     * @brief Returns total time in seconds since construction.
     * @return Elapsed time in seconds.
     */
    float elapsed() const {
        return std::chrono::duration<float>(Clock::now() - start_).count();
    }

    /**
     * @brief Returns the number of frames processed (incremented each update()).
     * @return Frame count.
     */
    uint64_t frame_count() const { return frame_count_; }

private:
    TimePoint start_;        /**< Absolute start time. */
    TimePoint last_;         /**< Time of the last update(). */
    float     delta_time_;   /**< Seconds between the last two update() calls. */
    uint64_t  frame_count_;  /**< Total frames processed. */
};

} // namespace util
} // namespace coopa

#endif // COOPA_UTIL_TIME_H
