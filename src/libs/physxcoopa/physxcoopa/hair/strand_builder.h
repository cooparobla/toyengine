/**
 * @file strand_builder.h
 * @brief Building a StrandSet from polylines, and the constraint batching that keeps it
 *        deterministic.
 *
 * The engine grows a groom (toyengine/hair/) and hands the guide strands here as world-space
 * polylines; nothing in this file knows what hair is for or how it will be drawn.
 */

#ifndef PHYSXCOOPA_HAIR_STRAND_BUILDER_H
#define PHYSXCOOPA_HAIR_STRAND_BUILDER_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

#include <physxcoopa/hair/strand.h>

namespace coopa {
namespace physx {
namespace hair {

/**
 * @struct StrandDesc
 * @brief One guide handed to the builder: its points, and the frame its root sits in.
 */
struct StrandDesc {
    /// @brief World-space points, root first. Fewer than two is skipped.
    std::vector<glm::vec3> points;
    /// @brief The root's orientation at build time. The rest pose is recorded relative to it, so
    ///        turning the head later carries the whole groom with it.
    glm::quat root_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    /// @brief Mass of one particle, kg. Hair is light; this mostly sets how much wind moves it.
    float particle_mass = 0.001f;
};

/**
 * @brief Builds a set: particles pinned at each root, stretch constraints along each chain, the
 *        rest pose both shape constraints read, and two disjoint constraint batches.
 *
 * Root particles get `inv_mass == 0`, so every constraint treats them as infinitely massive and
 * the solve never moves them -- their position is written outright from the strand's root each
 * substep. That is what makes the coupling one-way by construction rather than by convention.
 *
 * **The batching.** A chain's stretch constraints split by index parity: constraint k touches
 * particles k and k+1, so the even-k set and the odd-k set are each pairwise particle-disjoint,
 * and separate strands never share a particle at all. Two batches therefore cover any number of
 * strands, and a batch can be solved in any order on any thread with a bit-identical result --
 * the disjointness, not merely thread safety, is the requirement, because
 * `PhysicsWorld::world_state_hash()` has to stay reproducible.
 *
 * @param strands One descriptor per guide.
 * @param params  Tunables, copied onto the set.
 * @return The set, ready for solve_strands(). Empty when every descriptor was unusable.
 */
inline StrandSet make_strands(const std::vector<StrandDesc>& strands, const StrandParams& params) {
    StrandSet set;
    set.params = params;

    std::size_t total = 0;
    for (const StrandDesc& d : strands) {
        if (d.points.size() >= 2) total += d.points.size();
    }
    if (total == 0) return set;

    set.particles.reserve(total);
    set.rest_local.reserve(total);
    set.rest_offset.reserve(total);
    set.strands.reserve(strands.size());

    // Gathered separately so the two parities come out as two contiguous ranges rather than
    // needing a sort afterwards.
    std::vector<cloth::ClothConstraint> even;
    std::vector<cloth::ClothConstraint> odd;

    for (const StrandDesc& d : strands) {
        if (d.points.size() < 2) continue;

        Strand strand;
        strand.first = static_cast<uint32_t>(set.particles.size());
        strand.count = static_cast<uint32_t>(d.points.size());
        strand.root_position = d.points.front();
        strand.root_orientation = d.root_orientation;
        strand.prev_root_position = strand.root_position;
        strand.prev_root_orientation = strand.root_orientation;

        const glm::quat to_root = glm::inverse(d.root_orientation);
        const float inv_mass = d.particle_mass > 0.0f ? 1.0f / d.particle_mass : 1.0f;

        for (std::size_t k = 0; k < d.points.size(); ++k) {
            cloth::ClothParticle p;
            p.position = d.points[k];
            p.prev_position = p.position;
            // The root is pinned: it is driven, never solved.
            p.inv_mass = (k == 0) ? 0.0f : inv_mass;
            set.particles.push_back(p);

            set.rest_local.push_back(to_root * (d.points[k] - strand.root_position));
            set.rest_offset.push_back(k == 0 ? glm::vec3(0.0f) : d.points[k] - d.points[k - 1]);

            if (k > 0) {
                cloth::ClothConstraint c;
                c.a = strand.first + static_cast<uint32_t>(k - 1);
                c.b = strand.first + static_cast<uint32_t>(k);
                c.rest_length = glm::length(d.points[k] - d.points[k - 1]);
                ((k - 1) % 2 == 0 ? even : odd).push_back(c);
            }
        }
        set.strands.push_back(strand);
    }

    set.stretch.reserve(even.size() + odd.size());
    set.stretch.insert(set.stretch.end(), even.begin(), even.end());
    set.stretch.insert(set.stretch.end(), odd.begin(), odd.end());
    if (!even.empty()) {
        set.stretch_batches.push_back({0u, static_cast<uint32_t>(even.size())});
    }
    if (!odd.empty()) {
        set.stretch_batches.push_back({static_cast<uint32_t>(even.size()),
                                       static_cast<uint32_t>(even.size() + odd.size())});
    }

    set.bounds = compute_bounds(set);
    return set;
}

/**
 * @brief Moves every strand's root, for a scalp that has moved since the set was built.
 *
 * `prev_root_*` keeps the previous target, which the solver lerps FROM across its substeps -- so
 * call this once per frame, not once per substep.
 *
 * @param set          The set to drive.
 * @param positions    One world position per strand, in `set.strands` order.
 * @param orientations One world orientation per strand, same order. Empty leaves them unchanged.
 */
inline void set_strand_roots(StrandSet& set, const std::vector<glm::vec3>& positions,
                             const std::vector<glm::quat>& orientations = {}) {
    const std::size_t n = std::min(set.strands.size(), positions.size());
    for (std::size_t i = 0; i < n; ++i) {
        Strand& strand = set.strands[i];
        strand.prev_root_position = strand.root_position;
        strand.prev_root_orientation = strand.root_orientation;
        strand.root_position = positions[i];
        if (i < orientations.size()) strand.root_orientation = orientations[i];
    }
}

} // namespace hair
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_HAIR_STRAND_BUILDER_H
