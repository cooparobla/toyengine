/**
 * @file cloth_mesh_builder.h
 * @brief Constructs a cloth from an arbitrary welded triangle mesh -- the character-clothing
 *        path, where the sheet is a garment mesh rather than a procedural grid.
 *
 * Like cloth_builder.h this runs once at setup and is free to allocate; the solver never sees
 * any of it. The output is the same Cloth the grid builder emits -- the solver is topology-
 * agnostic -- with three differences:
 *
 *   - Constraints come from the mesh: stretch along every unique triangle edge, bend between
 *     the two apex vertices across every interior (two-triangle) edge. Bend is the same
 *     two-apart DISTANCE constraint the grid uses, for the same reasons (see cloth_builder.h's
 *     file doc on dihedral constraints).
 *   - Batches come from greedy graph colouring rather than index parity, since an arbitrary
 *     mesh has no parity to exploit. The colouring is deterministic (constraints visited in a
 *     sorted canonical order, smallest free colour wins), so the same mesh always yields the
 *     same batches and the serial/parallel bit-identity contract of ConstraintBatch holds.
 *   - Mass is distributed by area (each triangle donates a third of area * mass_per_area to
 *     each corner), so a garment with large and small triangles hangs correctly.
 *
 * Particles whose painted ClothSkinChannels::max_distance is at or below epsilon are built with
 * inv_mass == 0 -- they are fully skin-driven and the solver writes them from their skinned
 * targets (stage 3b). Tethers are seeded from exactly those particles via
 * build_tethers_from_pinned().
 */

#ifndef PHYSXCOOPA_CLOTH_CLOTH_MESH_BUILDER_H
#define PHYSXCOOPA_CLOTH_CLOTH_MESH_BUILDER_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/cloth/cloth_builder.h>
#include <physxcoopa/util/math.h>

namespace coopa {
namespace physx {
namespace cloth {

/**
 * @struct MeshClothDesc
 * @brief Authoring description of a cloth built from a welded triangle mesh.
 *
 * `positions` must be WELDED (one entry per topological vertex, shared across the triangles
 * that meet there) -- an unwelded render-vertex soup would build a cloth of disconnected
 * triangles. Positions are taken as the rest pose in whatever space the caller sims in
 * (toyengine passes world space at bind).
 */
struct MeshClothDesc {
    std::vector<glm::vec3> positions; /**< Welded rest positions, one per particle. */
    std::vector<uint32_t> triangles;  /**< 3 indices per triangle, into `positions`. */

    /** @brief Painted per-particle channels; each array sized to `positions` or empty.
     *         An array of any other size is discarded. max_distance decides pinning. */
    ClothSkinChannels channels;

    float mass_per_area = 0.2f; /**< Areal density, kg/m^2. 0.2 reads as mid-weight fabric. */
    ClothParams params;         /**< Tunables copied onto the resulting Cloth. */
    bool tethers = true;        /**< Derive tethers from the skin-pinned particles. */
};

namespace detail {

/**
 * @brief Greedily colours `list` so no two same-coloured constraints share a particle, then
 *        reorders it colour-major (stable within a colour) and emits one ConstraintBatch per
 *        colour. Deterministic for a given input order.
 */
inline void colour_and_batch(std::size_t particle_count, std::vector<ClothConstraint>& list,
                             std::vector<ConstraintBatch>& batches) {
    batches.clear();
    if (list.empty()) return;

    // Per-particle bitset of colours already used by a constraint touching it. Word-granular so
    // a pathological mesh can need more than 64 colours without breaking disjointness.
    std::vector<std::vector<uint64_t>> used(particle_count);
    std::vector<uint32_t> colour_of(list.size());
    uint32_t colour_count = 0;

    auto test = [](const std::vector<uint64_t>& bits, uint32_t colour) {
        const uint32_t word = colour >> 6;
        return word < bits.size() && (bits[word] >> (colour & 63u)) & 1u;
    };
    auto set = [](std::vector<uint64_t>& bits, uint32_t colour) {
        const uint32_t word = colour >> 6;
        if (word >= bits.size()) bits.resize(word + 1, 0u);
        bits[word] |= uint64_t{1} << (colour & 63u);
    };

    for (std::size_t i = 0; i < list.size(); ++i) {
        uint32_t colour = 0;
        while (test(used[list[i].a], colour) || test(used[list[i].b], colour)) ++colour;
        colour_of[i] = colour;
        set(used[list[i].a], colour);
        set(used[list[i].b], colour);
        colour_count = std::max(colour_count, colour + 1);
    }

    // Stable counting sort into colour-major order; each colour's run becomes one batch.
    std::vector<uint32_t> counts(colour_count + 1, 0u);
    for (const uint32_t colour : colour_of) ++counts[colour];
    uint32_t running = 0;
    for (uint32_t colour = 0; colour < colour_count; ++colour) {
        const uint32_t begin = running;
        running += counts[colour];
        counts[colour] = begin;
        if (running > begin) batches.push_back(ConstraintBatch{begin, running});
    }
    std::vector<ClothConstraint> sorted(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) sorted[counts[colour_of[i]]++] = list[i];
    list = std::move(sorted);
}

} // namespace detail

/**
 * @brief Builds a cloth from a welded triangle mesh; see this file's doc for how it differs
 *        from make_grid_cloth().
 *
 * @param desc Mesh description. Triangles with an out-of-range index or near-zero area still
 *             contribute topology (edges) but no mass.
 * @return A fully populated Cloth with columns == rows == 0 (no grid layout). Particles whose
 *         painted max_distance is <= epsilon are pinned (inv_mass == 0); if `desc.tethers` is
 *         set, every free particle is tethered to its geodesically nearest pinned one.
 *         Cloth::skin_targets is pre-seeded with the rest positions so a solve before the first
 *         target write holds the bind pose rather than reading unsized arrays.
 */
inline Cloth make_mesh_cloth(const MeshClothDesc& desc) {
    Cloth c;
    const std::size_t n = desc.positions.size();
    if (n == 0) return c;

    c.params = desc.params;
    c.valid = true;
    c.columns = 0;
    c.rows = 0;

    // --- Particles, with per-vertex mass accumulated from adjacent triangle areas ---
    c.particles.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        ClothParticle& p = c.particles[i];
        p.position = desc.positions[i];
        p.prev_position = p.position;
        p.velocity = glm::vec3(0.0f);
        p.inv_mass = 0.0f; // becomes positive below once a triangle donates mass
    }

    std::vector<float> mass(n, 0.0f);
    c.triangles.reserve(desc.triangles.size());
    for (std::size_t t = 0; t + 2 < desc.triangles.size(); t += 3) {
        const uint32_t i0 = desc.triangles[t], i1 = desc.triangles[t + 1], i2 = desc.triangles[t + 2];
        if (i0 >= n || i1 >= n || i2 >= n) continue;
        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
        c.triangles.insert(c.triangles.end(), {i0, i1, i2});
        const glm::vec3 e1 = desc.positions[i1] - desc.positions[i0];
        const glm::vec3 e2 = desc.positions[i2] - desc.positions[i0];
        const float area = 0.5f * glm::length(glm::cross(e1, e2));
        const float share = area * desc.mass_per_area * (1.0f / 3.0f);
        mass[i0] += share;
        mass[i1] += share;
        mass[i2] += share;
    }
    for (std::size_t i = 0; i < n; ++i) {
        c.particles[i].inv_mass = (mass[i] > util::k_epsilon) ? 1.0f / mass[i] : 0.0f;
    }

    // --- Painted channels: adopt correctly sized arrays, discard the rest ---
    auto adopt = [n](const std::vector<float>& src) {
        return src.size() == n ? src : std::vector<float>{};
    };
    c.skin.max_distance = adopt(desc.channels.max_distance);
    c.skin.backstop_distance = adopt(desc.channels.backstop_distance);
    c.skin.backstop_radius = adopt(desc.channels.backstop_radius);
    c.skin.anim_drive = adopt(desc.channels.anim_drive);
    if (!c.skin.max_distance.empty()) {
        for (std::size_t i = 0; i < n; ++i) {
            if (c.skin.max_distance[i] <= util::k_epsilon) {
                c.particles[i].inv_mass = 0.0f;
                c.particles[i].velocity = glm::vec3(0.0f);
            }
        }
        // Rest positions double as the first frame's targets -- see this function's doc.
        c.skin_targets.position = desc.positions;
        c.skin_targets.prev_position = desc.positions;
    }

    // --- Stretch: every unique undirected triangle edge, in sorted canonical order ---
    struct Edge {
        uint32_t a, b;    // a < b
        uint32_t opposite; // the triangle's third vertex, for the bend pass
        bool operator<(const Edge& o) const {
            if (a != o.a) return a < o.a;
            if (b != o.b) return b < o.b;
            return opposite < o.opposite;
        }
    };
    std::vector<Edge> edges;
    edges.reserve(c.triangles.size());
    for (std::size_t t = 0; t + 2 < c.triangles.size(); t += 3) {
        const uint32_t v[3] = {c.triangles[t], c.triangles[t + 1], c.triangles[t + 2]};
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = v[e], b = v[(e + 1) % 3], opp = v[(e + 2) % 3];
            edges.push_back(Edge{std::min(a, b), std::max(a, b), opp});
        }
    }
    std::sort(edges.begin(), edges.end());

    auto emit = [&c, &desc](std::vector<ClothConstraint>& out, uint32_t a, uint32_t b) {
        ClothConstraint k;
        k.a = a;
        k.b = b;
        k.rest_length = glm::length(desc.positions[a] - desc.positions[b]);
        out.push_back(k);
    };

    float min_edge = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t run = i + 1;
        while (run < edges.size() && edges[run].a == edges[i].a && edges[run].b == edges[i].b) ++run;

        emit(c.stretch, edges[i].a, edges[i].b);
        min_edge = std::min(min_edge, c.stretch.back().rest_length);

        // --- Bend: apex pairs across this edge. A manifold interior edge has exactly two
        // incident triangles (one constraint); a non-manifold fan chains consecutive apexes,
        // which keeps the output deterministic without special-casing. Boundary edges (run of
        // one) bend nothing.
        for (std::size_t j = i + 1; j < run; ++j) {
            if (edges[j].opposite != edges[j - 1].opposite) {
                emit(c.bend, edges[j - 1].opposite, edges[j].opposite);
            }
        }
        i = run;
    }

    detail::colour_and_batch(n, c.stretch, c.stretch_batches);
    detail::colour_and_batch(n, c.bend, c.bend_batches);

    // Same fight-avoidance rule as the grid builder, against the SHORTEST edge: an irregular
    // mesh's mean edge length could exceed its shortest, and a self_distance above a structural
    // rest length makes the two constraints fight every substep (see make_grid_cloth()).
    if (c.params.self_distance <= 0.0f && min_edge < std::numeric_limits<float>::max()) {
        c.params.self_distance = 0.6f * min_edge;
    }

    if (desc.tethers && !c.skin.max_distance.empty()) build_tethers_from_pinned(c);
    if (c.params.collide_surface) build_surface_samples(c);

    c.bounds = compute_bounds(c);
    return c;
}

} // namespace cloth
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_CLOTH_CLOTH_MESH_BUILDER_H
