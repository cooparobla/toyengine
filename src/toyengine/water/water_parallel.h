/**
 * @file water_parallel.h
 * @brief The hook toyengine/water/ uses to spread CPU work over threads, without depending on
 *        a job system.
 *
 * WaterSystem builds a ParallelFor over the frame's coopa::job::JobEngine; the free functions
 * (tile builders, the surface query) take one optionally and run inline without it, so they stay
 * unit-testable. Every use writes only to its own indices: the output is identical however the
 * range is split.
 */

#ifndef TOYENGINE_WATER_WATER_PARALLEL_H
#define TOYENGINE_WATER_WATER_PARALLEL_H

#include <cstddef>
#include <functional>

namespace toy {
namespace water {

/// One range body: handles indices [begin, end).
using RangeFn = std::function<void(std::size_t begin, std::size_t end)>;

/// Runs `fn` over [0, n), each index exactly once, possibly split across threads, and returns
/// when all of it is done.
using ParallelFor = std::function<void(std::size_t n, const RangeFn& fn)>;

/** @brief `fn` over [0, n): through `par` when given, else inline on this thread. */
inline void run_range(const ParallelFor* par, std::size_t n, const RangeFn& fn) {
    if (n == 0) return;
    if (par && *par) (*par)(n, fn);
    else fn(0, n);
}

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_PARALLEL_H
