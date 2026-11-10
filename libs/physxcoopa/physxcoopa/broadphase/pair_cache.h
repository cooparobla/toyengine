/**
 * @file pair_cache.h
 * @brief Persistent broadphase pair set -- a sorted vector, never a pointer-keyed hash map
 *        (see the plan's "Determinism and parallelism": iterating a pointer-keyed
 *        unordered_map's ASLR-dependent order inside the step is the actual determinism
 *        hazard a naive implementation would hit here).
 */

#ifndef PHYSXCOOPA_BROADPHASE_PAIR_CACHE_H
#define PHYSXCOOPA_BROADPHASE_PAIR_CACHE_H

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace coopa {
namespace physx {
namespace broadphase {

/** @brief One broadphase pair, always stored with `a < b`. */
struct ProxyPair {
    uint32_t a;
    uint32_t b;

    bool operator==(const ProxyPair& other) const { return a == other.a && b == other.b; }
    bool operator<(const ProxyPair& other) const {
        if (a != other.a) return a < other.a;
        return b < other.b;
    }
};

/**
 * @class PairCache
 * @brief Sorted, deduplicated set of proxy pairs for one step, plus a diff against the
 *        previous step's set -- the mechanism trigger enter/stay/exit (Phase 9) and manifold
 *        persistence build on.
 */
class PairCache {
public:
    /** @brief Records a pair. Order of `x`/`y` doesn't matter -- stored canonically as a<b. */
    void add(uint32_t x, uint32_t y) {
        if (x == y) return;
        pending_.push_back(ProxyPair{std::min(x, y), std::max(x, y)});
    }

    /** @brief Sorts and deduplicates this step's added pairs. Call once after all add() calls. */
    void finalize() {
        std::sort(pending_.begin(), pending_.end());
        pending_.erase(std::unique(pending_.begin(), pending_.end()), pending_.end());
    }

    /** @brief This step's finalized pair set. */
    const std::vector<ProxyPair>& current() const { return pending_; }

    /**
     * @brief Computes which pairs appeared/disappeared relative to the last call to advance().
     *
     * @param added   Output: pairs present now but not in the previous set.
     * @param removed Output: pairs present in the previous set but not now.
     */
    void diff(std::vector<ProxyPair>& added, std::vector<ProxyPair>& removed) const {
        added.clear();
        removed.clear();
        std::set_difference(pending_.begin(), pending_.end(), previous_.begin(), previous_.end(),
                             std::back_inserter(added));
        std::set_difference(previous_.begin(), previous_.end(), pending_.begin(), pending_.end(),
                             std::back_inserter(removed));
    }

    /**
     * @brief Like diff(), but also reports `stayed` -- pairs present in both sets -- for
     *        callers that need enter/stay/exit rather than just enter/exit (Phase 9's
     *        trigger/collision event diffing).
     */
    void classify(std::vector<ProxyPair>& added, std::vector<ProxyPair>& stayed, std::vector<ProxyPair>& removed) const {
        diff(added, removed);
        stayed.clear();
        std::set_difference(pending_.begin(), pending_.end(), added.begin(), added.end(),
                             std::back_inserter(stayed));
    }

    /** @brief Moves this step's set into "previous" and clears the working set for next step. */
    void advance() {
        previous_ = pending_;
        pending_.clear();
    }

private:
    std::vector<ProxyPair> pending_;
    std::vector<ProxyPair> previous_;
};

} // namespace broadphase
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_BROADPHASE_PAIR_CACHE_H
