/**
 * @file island.h
 * @brief Union-find over the contact graph, grouping dynamic bodies that must sleep as a unit.
 */

#ifndef PHYSXCOOPA_DYNAMICS_ISLAND_H
#define PHYSXCOOPA_DYNAMICS_ISLAND_H

#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace dynamics {

/**
 * @class IslandUnionFind
 * @brief Disjoint-set structure over body array indices, built fresh each substep from the
 *        current step's dynamic-dynamic contacts.
 *
 * Static and kinematic bodies never merge islands (callers must only ever call unite() for a
 * pair that is dynamic on both sides) -- so two dynamic bodies resting on the same static
 * floor are correctly treated as separate islands, each free to sleep independently.
 */
class IslandUnionFind {
public:
    explicit IslandUnionFind(size_t capacity) : parent_(capacity) {
        for (size_t i = 0; i < capacity; ++i) parent_[i] = static_cast<uint32_t>(i);
    }

    /** @brief Finds `i`'s island root, path-compressing along the way. */
    uint32_t find(uint32_t i) {
        while (parent_[i] != i) {
            parent_[i] = parent_[parent_[i]];
            i = parent_[i];
        }
        return i;
    }

    /** @brief Merges the islands containing `a` and `b`. Both must be dynamic bodies. */
    void unite(uint32_t a, uint32_t b) {
        uint32_t ra = find(a);
        uint32_t rb = find(b);
        if (ra != rb) parent_[ra] = rb;
    }

private:
    std::vector<uint32_t> parent_;
};

} // namespace dynamics
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DYNAMICS_ISLAND_H
