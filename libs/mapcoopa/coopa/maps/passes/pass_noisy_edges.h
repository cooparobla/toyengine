/**
 * @file pass_noisy_edges.h
 * @brief Ninth pass: replaces each straight cell boundary with a recursively
 *        wobbled path, so coastlines and biome borders read as natural.
 */

#ifndef COOPA_MAPS_PASSES_PASS_NOISY_EDGES_H
#define COOPA_MAPS_PASSES_PASS_NOISY_EDGES_H

#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/portable_random.h>

namespace coopa {
namespace maps {

/**
 * @class PassNoisyEdges
 * @brief Fills `MapEdge::noisy_points0` and `noisy_points1`.
 *
 * A Voronoi edge and the two cell sites it separates form a quadrilateral. The
 * boundary is redrawn as a path that stays inside that quadrilateral but
 * wanders within it, recursively, which perturbs the outline without ever
 * letting two neighbouring cells disagree about where their shared border runs
 * -- both read the same edge, so both get the same path.
 *
 * Subdivision cost is paid only where it shows. An edge between two ocean
 * cells is never subdivided, an edge between two cells of the same biome is
 * subdivided only if it is long, and coastlines, river banks and biome
 * boundaries always are.
 */
class PassNoisyEdges {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassBiomes` to have run.
     * @param config Supplies the seed and `subdivide_noisy_edges`.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: noisy edges");

        // Seeded from the map, not the wall clock: one clock-seeded pass would
        // make the whole generator irreproducible.
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_seed_offset);

        for (MapEdge& edge : graph.edges) {
            if (edge.noisy) {
                continue;
            }
            if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id
                || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }

            const MapPoint& v0 = graph.corners[static_cast<std::size_t>(edge.v0)].point;
            const MapPoint& v1 = graph.corners[static_cast<std::size_t>(edge.v1)].point;
            const MapPoint& d0 = graph.centers[static_cast<std::size_t>(edge.d0)].point;
            const MapPoint& d1 = graph.centers[static_cast<std::size_t>(edge.d1)].point;

            const MapPoint t = interpolate_(v0, d0, k_noise_line_tradeoff);
            const MapPoint q = interpolate_(v0, d1, k_noise_line_tradeoff);
            const MapPoint r = interpolate_(v1, d0, k_noise_line_tradeoff);
            const MapPoint s = interpolate_(v1, d1, k_noise_line_tradeoff);

            const double min_length = config.subdivide_noisy_edges
                ? min_length_for_(graph, edge)
                : k_no_subdivision_length;

            edge.noisy_points0.clear();
            edge.noisy_points1.clear();
            build_path_(edge.noisy_points0, v0, t, edge.midpoint, q, min_length, rng);
            build_path_(edge.noisy_points1, v1, s, edge.midpoint, r, min_length, rng);
            if (graph.is_sphere()) {
                // Subdivided in straight lines, the path cuts inside a globe: put it back on.
                for (MapPoint& p : edge.noisy_points0) p = graph.on_surface(p);
                for (MapPoint& p : edge.noisy_points1) p = graph.on_surface(p);
            }
            edge.noisy = true;
        }
    }

private:
    /** @brief Offset from `MapConfig::seed` so this pass does not share a stream with the others. */
    static constexpr unsigned int k_seed_offset = 104729u;
    /** @brief How far from the Voronoi edge toward the cell sites the path may wander. */
    static constexpr double k_noise_line_tradeoff = 0.5;
    /** @brief Deepest recursion allowed; each level roughly doubles the point count. */
    static constexpr int k_max_recursion = 3;
    /** @brief Minimum segment length for a boundary worth detailing. */
    static constexpr double k_detail_length = 0.15;
    /** @brief Minimum segment length for an ordinary interior boundary. */
    static constexpr double k_default_length = 10.0;
    /** @brief A length no edge can exceed, which disables subdivision entirely. */
    static constexpr double k_no_subdivision_length = 100.0;

    /**
     * @brief Chooses the subdivision threshold for one edge.
     *
     * Computed fresh per edge rather than held in a member, so a coastline's
     * detail threshold cannot leak into every edge processed after it.
     */
    double min_length_for_(const MapGraph& graph, const MapEdge& edge) const {
        const MapCenter& c0 = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& c1 = graph.centers[static_cast<std::size_t>(edge.d1)];

        if (c0.ocean && c1.ocean) {
            return k_no_subdivision_length;
        }
        if (edge.river > 0 || c0.coast || c1.coast || c0.biome != c1.biome) {
            return k_detail_length;
        }
        return k_default_length;
    }

    /** @brief Linear interpolation between two points. */
    static MapPoint interpolate_(const MapPoint& from, const MapPoint& to, double f) {
        return {from.x + f * (to.x - from.x), from.y + f * (to.y - from.y), from.z + f * (to.z - from.z)};
    }

    /** @brief Euclidean distance between two points. */
    static double distance_(const MapPoint& a, const MapPoint& b) {
        if (a.z == 0.0 && b.z == 0.0) return std::hypot(b.x - a.x, b.y - a.y);   // a plane map
        return a.distance_to(b);
    }

    /** @brief Emits the path from `a` to `c` through the quadrilateral `a b c d`. */
    void build_path_(std::vector<MapPoint>& points,
                     const MapPoint& a, const MapPoint& b,
                     const MapPoint& c, const MapPoint& d,
                     double min_length, std::mt19937& rng) const {
        points.push_back(a);
        subdivide_(points, a, b, c, d, min_length, 0, rng);
        points.push_back(c);
    }

    /**
     * @brief Recursively splits the quadrilateral `a b c d`, emitting midpoints.
     *
     * The depth counter is passed by value, so each half gets its own budget
     * (a shared counter would end the second half early), and the threshold is
     * a `double`, so the 0.15 detail length is not truncated to zero. The
     * emitted point belongs *between* the two recursive calls -- pushing it first
     * interleaves the halves and produces a self-crossing outline.
     */
    void subdivide_(std::vector<MapPoint>& points,
                    const MapPoint& a, const MapPoint& b,
                    const MapPoint& c, const MapPoint& d,
                    double min_length, int recursion_level, std::mt19937& rng) const {
        if (recursion_level >= k_max_recursion
            || distance_(a, c) < min_length
            || distance_(b, d) < min_length) {
            return;
        }

        coopa::maps::UniformRealDistribution<double> unit(0.2, 0.8);
        const double p = unit(rng);
        const double q = unit(rng);

        const MapPoint e = interpolate_(a, d, p);
        const MapPoint f = interpolate_(b, c, p);
        const MapPoint g = interpolate_(a, b, q);
        const MapPoint i = interpolate_(d, c, q);
        const MapPoint h = interpolate_(e, f, q);

        const double s = 1.0 - unit(rng) * 0.8;
        const double t = 1.0 - unit(rng) * 0.8;

        subdivide_(points, a, interpolate_(g, b, s), h, interpolate_(e, d, t),
                   min_length, recursion_level + 1, rng);
        points.push_back(h);
        subdivide_(points, h, interpolate_(f, c, s), c, interpolate_(i, d, t),
                   min_length, recursion_level + 1, rng);
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_NOISY_EDGES_H
