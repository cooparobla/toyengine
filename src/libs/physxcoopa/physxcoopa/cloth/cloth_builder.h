/**
 * @file cloth_builder.h
 * @brief Constructs a grid cloth sheet, pins it to rigid bodies, and derives its tethers.
 *
 * Everything here runs once at setup, never per frame, so it is free to allocate and to use
 * std::priority_queue -- unlike cloth_solver.h, which touches no allocator at all.
 *
 * The constraint layout it emits is what makes the solver both stable and parallelisable:
 *
 *   structural  (x,y)-(x+1,y), (x,y)-(x,y+1)     resists stretching
 *   shear       (x,y)-(x+1,y+1), (x+1,y)-(x,y+1) resists in-plane racking (optional)
 *   bend        (x,y)-(x+2,y), (x,y)-(x,y+2)     resists curvature
 *
 * Bending is expressed as a plain two-apart DISTANCE constraint rather than the dihedral-angle
 * constraint a textbook PBD derivation reaches for. The dihedral version needs the four vertices
 * of two adjacent triangles, a cross-product-heavy gradient, and an acos whose derivative blows
 * up as the fold approaches flat -- roughly 6x the arithmetic for a difference that is invisible
 * on a sheet draped over a convex body. The distance version reuses the exact same XPBD kernel as
 * stretch (one normalize, one division), which also means the whole constraint solve is a single
 * inlined loop body rather than two.
 */

#ifndef PHYSXCOOPA_CLOTH_CLOTH_BUILDER_H
#define PHYSXCOOPA_CLOTH_CLOTH_BUILDER_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/util/math.h>

namespace coopa {
namespace physx {
namespace cloth {

/**
 * @struct GridClothDesc
 * @brief Authoring description of a rectangular cloth sheet.
 *
 * The sheet is built in a local XY plane with its normal along +Z, then rotated by `orientation`
 * and translated to `center` -- matching the engine's Z-up convention, so an identity orientation
 * produces a horizontal sheet that will drape downward under gravity.
 */
struct GridClothDesc {
    uint32_t columns = 21;             /**< Particles along local X (>= 2). */
    uint32_t rows = 21;                /**< Particles along local Y (>= 2). */
    float width = 2.0f;                /**< Extent along local X, metres. */
    float height = 2.0f;               /**< Extent along local Y, metres. */
    glm::vec3 center{0.0f};            /**< World position of the sheet's centre. */
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; /**< World orientation of the local frame. */
    float total_mass = 1.0f;           /**< Mass of the whole sheet, split evenly per particle. */
    bool shear = true;                 /**< Emit diagonal shear constraints. Without them the grid
                                            is a hinge lattice and racks into a parallelogram
                                            under any sideways load. */
    ClothParams params;                /**< Tunables copied onto the resulting Cloth. */
};

/**
 * @brief Builds a rectangular cloth sheet with structural, shear and bend constraints.
 *
 * Particles are emitted row-major (`index = y * columns + x`), which the triangle list, the
 * constraint batching and toyengine's ClothRenderer all rely on.
 *
 * Constraints are grouped into ConstraintBatches whose members touch pairwise-disjoint particle
 * sets, by index parity: structural edges split into even/odd along each axis, shear into even/odd
 * by column, and bend (which spans two cells) into four classes by column/row modulo 4. See
 * ConstraintBatch's doc for why disjointness -- not just thread safety -- is the requirement.
 *
 * @param desc Sheet description; `columns`/`rows` are clamped to at least 2.
 * @return A fully populated Cloth with every particle unpinned (inv_mass > 0). Call
 *         pin_to_body() / pin_static() and then build_tethers() to anchor it.
 */
inline Cloth make_grid_cloth(const GridClothDesc& desc) {
    Cloth c;
    const uint32_t cols = std::max<uint32_t>(2, desc.columns);
    const uint32_t rows = std::max<uint32_t>(2, desc.rows);
    c.columns = cols;
    c.rows = rows;
    c.params = desc.params;
    c.valid = true;

    const uint32_t count = cols * rows;
    const float particle_mass = (desc.total_mass > util::k_epsilon)
                                    ? desc.total_mass / static_cast<float>(count)
                                    : 0.0f;
    const float inv_mass = (particle_mass > util::k_epsilon) ? 1.0f / particle_mass : 0.0f;

    const float dx = desc.width / static_cast<float>(cols - 1);
    const float dy = desc.height / static_cast<float>(rows - 1);

    // --- Particles ---
    c.particles.resize(count);
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = 0; x < cols; ++x) {
            glm::vec3 local(static_cast<float>(x) * dx - desc.width * 0.5f,
                            static_cast<float>(y) * dy - desc.height * 0.5f,
                            0.0f);
            ClothParticle& p = c.particles[y * cols + x];
            p.position = desc.center + desc.orientation * local;
            p.prev_position = p.position;
            p.velocity = glm::vec3(0.0f);
            p.inv_mass = inv_mass;
        }
    }

    auto index_of = [cols](uint32_t x, uint32_t y) { return y * cols + x; };
    auto emit = [&c](std::vector<ClothConstraint>& out, uint32_t a, uint32_t b) {
        ClothConstraint k;
        k.a = a;
        k.b = b;
        k.rest_length = glm::length(c.particles[a].position - c.particles[b].position);
        out.push_back(k);
    };
    auto close_batch = [](std::vector<ClothConstraint>& list, std::vector<ConstraintBatch>& batches,
                          uint32_t begin) {
        const uint32_t end = static_cast<uint32_t>(list.size());
        if (end > begin) batches.push_back(ConstraintBatch{begin, end});
    };

    // --- Structural: horizontal, split by column parity ---
    for (uint32_t parity = 0; parity < 2; ++parity) {
        const uint32_t begin = static_cast<uint32_t>(c.stretch.size());
        for (uint32_t y = 0; y < rows; ++y) {
            for (uint32_t x = parity; x + 1 < cols; x += 2) {
                emit(c.stretch, index_of(x, y), index_of(x + 1, y));
            }
        }
        close_batch(c.stretch, c.stretch_batches, begin);
    }
    // --- Structural: vertical, split by row parity ---
    for (uint32_t parity = 0; parity < 2; ++parity) {
        const uint32_t begin = static_cast<uint32_t>(c.stretch.size());
        for (uint32_t y = parity; y + 1 < rows; y += 2) {
            for (uint32_t x = 0; x < cols; ++x) {
                emit(c.stretch, index_of(x, y), index_of(x, y + 1));
            }
        }
        close_batch(c.stretch, c.stretch_batches, begin);
    }

    // --- Shear: both diagonals, each split by column parity.
    // Column parity alone is sufficient for disjointness here: a "\" constraint at (x,y) touches
    // (x,y) and (x+1,y+1), so two constraints of the same column parity can only share a particle
    // if x1 == x2 + 1 -- impossible when x1 and x2 have the same parity. Same argument for "/".
    if (desc.shear) {
        for (uint32_t parity = 0; parity < 2; ++parity) {
            const uint32_t begin = static_cast<uint32_t>(c.stretch.size());
            for (uint32_t y = 0; y + 1 < rows; ++y) {
                for (uint32_t x = parity; x + 1 < cols; x += 2) {
                    emit(c.stretch, index_of(x, y), index_of(x + 1, y + 1));
                }
            }
            close_batch(c.stretch, c.stretch_batches, begin);
        }
        for (uint32_t parity = 0; parity < 2; ++parity) {
            const uint32_t begin = static_cast<uint32_t>(c.stretch.size());
            for (uint32_t y = 0; y + 1 < rows; ++y) {
                for (uint32_t x = parity; x + 1 < cols; x += 2) {
                    emit(c.stretch, index_of(x + 1, y), index_of(x, y + 1));
                }
            }
            close_batch(c.stretch, c.stretch_batches, begin);
        }
    }

    // --- Bend: two-apart, split modulo 4.
    // A horizontal bend constraint at column x touches x and x+2, so two constraints collide when
    // |x1 - x2| is 0 or 2. Grouping by x % 4 makes every intra-group difference a multiple of 4.
    for (uint32_t phase = 0; phase < 4; ++phase) {
        const uint32_t begin = static_cast<uint32_t>(c.bend.size());
        for (uint32_t y = 0; y < rows; ++y) {
            for (uint32_t x = phase; x + 2 < cols; x += 4) {
                emit(c.bend, index_of(x, y), index_of(x + 2, y));
            }
        }
        close_batch(c.bend, c.bend_batches, begin);
    }
    for (uint32_t phase = 0; phase < 4; ++phase) {
        const uint32_t begin = static_cast<uint32_t>(c.bend.size());
        for (uint32_t y = phase; y + 2 < rows; y += 4) {
            for (uint32_t x = 0; x < cols; ++x) {
                emit(c.bend, index_of(x, y), index_of(x, y + 2));
            }
        }
        close_batch(c.bend, c.bend_batches, begin);
    }

    // --- Triangles. Winding is counter-clockwise seen from local +Z, so the sheet's front face
    // is its +Z side at rest -- cloth materials should still disable backface culling, since a
    // draped sheet shows both sides by definition.
    c.triangles.reserve(static_cast<std::size_t>(cols - 1) * (rows - 1) * 6);
    for (uint32_t y = 0; y + 1 < rows; ++y) {
        for (uint32_t x = 0; x + 1 < cols; ++x) {
            const uint32_t i00 = index_of(x, y), i10 = index_of(x + 1, y);
            const uint32_t i01 = index_of(x, y + 1), i11 = index_of(x + 1, y + 1);
            c.triangles.insert(c.triangles.end(), {i00, i10, i11});
            c.triangles.insert(c.triangles.end(), {i00, i11, i01});
        }
    }

    // Self-collision distance defaults to a fraction of the rest spacing. It must stay BELOW the
    // structural rest length, or self-collision and the structural constraints fight each other
    // every substep (one pushing neighbours apart to `self_distance`, the other pulling them back
    // to `rest_length`) and the sheet buzzes. 0.6 leaves comfortable headroom on both axes.
    if (c.params.self_distance <= 0.0f) {
        c.params.self_distance = 0.6f * std::min(dx, dy);
    }

    c.bounds = compute_bounds(c);
    return c;
}

/**
 * @brief Pins every particle within `radius` of `world_point` to `body`.
 *
 * Pinned particles get inv_mass = 0 and a ClothAnchor recording their offset in the body's local
 * frame, so they rigidly follow it from then on -- rotation included. This is the "cloth attached
 * to a character" case; anchoring to the crown of a sphere gives a cape or a dust sheet.
 *
 * @param c           Cloth to modify.
 * @param id          Handle of the body to follow (recorded, not dereferenced later).
 * @param body        That body's current pose, used to compute the local offsets.
 * @param world_point Centre of the pinning region, in world space.
 * @param radius      Pinning radius in metres.
 * @return Number of particles newly pinned.
 */
inline uint32_t pin_to_body(Cloth& c, dynamics::BodyId id, const dynamics::Body& body,
                            const glm::vec3& world_point, float radius) {
    const float r2 = radius * radius;
    const glm::quat inv_rot = glm::inverse(body.orientation);
    uint32_t pinned = 0;
    for (uint32_t i = 0; i < c.particles.size(); ++i) {
        ClothParticle& p = c.particles[i];
        if (p.inv_mass == 0.0f) continue; // already pinned by an earlier anchor
        glm::vec3 d = p.position - world_point;
        if (glm::dot(d, d) > r2) continue;

        p.inv_mass = 0.0f;
        p.velocity = glm::vec3(0.0f);
        ClothAnchor a;
        a.particle = i;
        a.body = id;
        a.local_position = inv_rot * (p.position - body.position);
        a.world_position = p.position;
        a.prev_world_position = p.position;
        c.anchors.push_back(a);
        ++pinned;
    }
    return pinned;
}

/**
 * @brief Pins every particle within `radius` of `world_point` in place, with no body to follow.
 *
 * The fixed-point equivalent of pin_to_body() -- a banner nailed to a wall. Implemented as an
 * anchor with an invalid BodyId so the solver's single anchor path handles both cases.
 *
 * @param c           Cloth to modify.
 * @param world_point Centre of the pinning region, in world space.
 * @param radius      Pinning radius in metres.
 * @return Number of particles newly pinned.
 */
inline uint32_t pin_static(Cloth& c, const glm::vec3& world_point, float radius) {
    const float r2 = radius * radius;
    uint32_t pinned = 0;
    for (uint32_t i = 0; i < c.particles.size(); ++i) {
        ClothParticle& p = c.particles[i];
        if (p.inv_mass == 0.0f) continue;
        glm::vec3 d = p.position - world_point;
        if (glm::dot(d, d) > r2) continue;

        p.inv_mass = 0.0f;
        p.velocity = glm::vec3(0.0f);
        ClothAnchor a;
        a.particle = i;
        a.body = dynamics::BodyId{}; // invalid -> static pin at world_position
        a.local_position = glm::vec3(0.0f);
        a.world_position = p.position;
        a.prev_world_position = p.position;
        c.anchors.push_back(a);
        ++pinned;
    }
    return pinned;
}

/**
 * @brief Derives one tether per free particle from the current anchors.
 *
 * Runs Dijkstra over the stretch graph from every anchored particle at once (all anchors seeded
 * at distance 0), so each particle ends up tethered to its geodesically nearest anchor at its
 * rest-pose path length. Geodesic, not Euclidean: a particle hanging round the far side of a
 * sphere is Euclidean-close to the anchor but can legitimately be a long way from it along the
 * fabric, and a Euclidean tether would yank it through the body.
 *
 * Replaces any previously built tethers. Call after all pin_to_body()/pin_static() calls; a
 * cloth with no anchors gets no tethers.
 *
 * @param c Cloth to modify.
 */
inline void build_tethers(Cloth& c) {
    c.tethers.clear();
    if (c.anchors.empty() || c.particles.empty()) return;

    const std::size_t n = c.particles.size();

    // Adjacency over the stretch graph only -- bend edges span two cells and would report a
    // geodesic shorter than the fabric actually allows.
    std::vector<std::vector<uint32_t>> adjacency(n);
    for (const ClothConstraint& k : c.stretch) {
        adjacency[k.a].push_back(k.b);
        adjacency[k.b].push_back(k.a);
    }

    const float inf = std::numeric_limits<float>::max();
    std::vector<float> dist(n, inf);
    std::vector<uint32_t> source(n, 0);

    using Entry = std::pair<float, uint32_t>; // (distance, particle)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
    for (const ClothAnchor& a : c.anchors) {
        dist[a.particle] = 0.0f;
        source[a.particle] = a.particle;
        frontier.push({0.0f, a.particle});
    }

    while (!frontier.empty()) {
        const Entry top = frontier.top();
        frontier.pop();
        if (top.first > dist[top.second]) continue; // stale queue entry
        for (uint32_t next : adjacency[top.second]) {
            const float edge = glm::length(c.particles[next].position - c.particles[top.second].position);
            const float candidate = top.first + edge;
            if (candidate >= dist[next]) continue;
            dist[next] = candidate;
            source[next] = source[top.second];
            frontier.push({candidate, next});
        }
    }

    const float scale = std::max(1.0f, c.params.tether_scale);
    for (uint32_t i = 0; i < n; ++i) {
        if (c.particles[i].inv_mass == 0.0f) continue; // anchors tether nothing
        if (dist[i] >= inf) continue;                  // disconnected island, no reachable anchor
        ClothTether t;
        t.particle = i;
        t.anchor = source[i];
        t.max_length = dist[i] * scale;
        c.tethers.push_back(t);
    }
}

} // namespace cloth
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_CLOTH_CLOTH_BUILDER_H
