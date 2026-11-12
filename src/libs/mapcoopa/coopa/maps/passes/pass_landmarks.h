/**
 * @file pass_landmarks.h
 * @brief Finds the places on a map worth naming: terrain features, abandoned
 *        works, and one wonder for each region.
 */

#ifndef COOPA_MAPS_PASSES_PASS_LANDMARKS_H
#define COOPA_MAPS_PASSES_PASS_LANDMARKS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/landmark.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/name_generator.h>
#include <coopa/maps/portable_random.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassLandmarks
 * @brief Fills `MapGraph::landmarks`.
 *
 * Natural landmarks are *read off* the terrain rather than sprinkled onto it:
 * a peak is a cell higher than everything around it, a waterfall is a river
 * edge with a real drop across it, a cape is land reaching out into the sea.
 * Because they are discovered, they always agree with the map they sit on.
 *
 * Abandoned works go where people could live but do not, which gives the world
 * a past. Every kind is checked against `landmark_suits_biome()` before it is
 * placed, so the vocabulary follows the terrain.
 */
class PassLandmarks {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to populate; requires `PassTowns` to have run.
     * @param config Supplies the seed and the `LandmarkConfig` block.
     * @param logger Receives progress and the count placed.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: landmarks");

        graph.landmarks.clear();
        seed_ = config.seed;
        const LandmarkConfig& settings = config.landmarks;
        if (graph.centers.empty()) {
            return;
        }

        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_seed_offset);
        std::vector<bool> taken(graph.centers.size(), false);
        for (const MapTown& town : graph.towns) {
            if (town.center != k_invalid_id) {
                taken[static_cast<std::size_t>(town.center)] = true;
            }
        }

        find_natural_(graph, config, taken, rng);
        find_abandoned_(graph, config, taken, rng);
        find_wonders_(graph, taken, rng);

        logger.info("map pass: landmarks placed " + std::to_string(graph.landmarks.size()));
    }

private:
    /** @brief Offset from `MapConfig::seed` so landmarks get their own stream. */
    static constexpr unsigned int k_seed_offset = 32452843u;

    /**
     * @brief Reads every natural feature off the terrain, keeping the best.
     *
     * Candidates are gathered first and ranked by how distinctive their kind is,
     * rather than taken in cell order until the cap fills. A map has hundreds of
     * gorges and a handful of summits; first-come-first-served spends the whole
     * budget on gorges and silently drops every peak.
     */
    void find_natural_(MapGraph& graph, const MapConfig& config, std::vector<bool>& taken,
                       std::mt19937& rng) const {
        const LandmarkConfig& settings = config.landmarks;
        struct Candidate {
            CenterId center;
            LandmarkKind kind;
            int priority;
            double elevation;
        };
        std::vector<Candidate> candidates;

        for (const MapCenter& center : graph.centers) {
            if (taken[static_cast<std::size_t>(center.index)] || center.border) {
                continue;
            }
            const LandmarkKind kind = natural_kind_(graph, center, config);
            if (kind == k_no_kind || !landmark_suits_biome(kind, center.biome)) {
                continue;
            }
            candidates.push_back({center.index, kind, kind_priority_(kind), center.elevation});
        }

        coopa::maps::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.priority != b.priority) {
                          return a.priority < b.priority;
                      }
                      return a.elevation > b.elevation;
                  });

        // Cap any one kind's share of the budget. Ranking alone is not enough:
        // whichever signature happens to be commonest on a given map takes every
        // remaining slot, and the result is forty hot springs and no coastline.
        const int per_kind_cap = std::max(1, static_cast<int>(settings.max_natural
                                                              * settings.kind_share_numerator
                                                              / settings.kind_share_denominator));
        std::vector<int> per_kind(k_landmark_kind_count, 0);

        int placed = 0;
        for (const Candidate& candidate : candidates) {
            if (placed >= settings.max_natural) {
                return;
            }
            if (taken[static_cast<std::size_t>(candidate.center)]) {
                continue;
            }
            int& count = per_kind[static_cast<std::size_t>(candidate.kind)];
            if (count >= per_kind_cap) {
                continue;
            }
            place_(graph, graph.centers[static_cast<std::size_t>(candidate.center)], candidate.kind,
                   taken, rng);
            ++count;
            ++placed;
        }
    }

    /**
     * @brief How strongly a kind deserves a place in the budget; lower wins.
     *
     * Ordered by rarity, so the one volcano on a map outranks the two hundredth
     * gorge.
     */
    static int kind_priority_(LandmarkKind kind) {
        switch (kind) {
            case LandmarkKind::Volcano:   return 0;
            case LandmarkKind::Peak:      return 1;
            case LandmarkKind::Glacier:   return 2;
            case LandmarkKind::Crater:    return 3;
            case LandmarkKind::Oasis:     return 4;
            case LandmarkKind::HotSpring: return 5;
            case LandmarkKind::Waterfall: return 6;
            case LandmarkKind::GreatLake: return 7;
            case LandmarkKind::Cape:      return 8;
            default:                      return 9;
        }
    }

    /** @brief Sentinel for "this cell is not a natural landmark". */
    static constexpr LandmarkKind k_no_kind = static_cast<LandmarkKind>(-1);

    /**
     * @brief Identifies what natural feature, if any, a cell represents.
     *
     * Ordered by how distinctive each signature is, so a volcanic summit is
     * reported as a volcano rather than merely a peak.
     */
    LandmarkKind natural_kind_(const MapGraph& graph, const MapCenter& center,
                               const MapConfig& config) const {
        const LandmarkConfig& settings = config.landmarks;
        if (center.water && !center.ocean) {
            return lake_cluster_size_(graph, center) >= settings.great_lake_cells
                       ? LandmarkKind::GreatLake
                       : k_no_kind;
        }
        if (center.water) {
            return k_no_kind;
        }

        // A summit: strictly higher than every neighbour, and high in absolute terms.
        // Land-relative against the threshold; the neighbour comparisons below stay
        // raw, because a monotonic rescale cannot change which of two cells is
        // higher.
        bool is_summit = land_height(config, center.elevation) >= settings.peak_elevation;
        for (const CenterId neighbor_id : center.neighbors) {
            if (graph.centers[static_cast<std::size_t>(neighbor_id)].elevation >= center.elevation) {
                is_summit = false;
                break;
            }
        }
        if (is_summit) {
            if (center.biome == Biome::VolcanicField || center.biome == Biome::Scorched) {
                return LandmarkKind::Volcano;
            }
            if (landmark_suits_biome(LandmarkKind::Glacier, center.biome)) {
                return LandmarkKind::Glacier;
            }
            return LandmarkKind::Peak;
        }

        // A basin: dry land lower than everything around it. A rainless hollow
        // like this is what a crater looks like from above.
        bool is_basin = !center.neighbors.empty();
        for (const CenterId neighbor_id : center.neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            if (neighbor.water || neighbor.elevation <= center.elevation) {
                is_basin = false;
                break;
            }
        }
        if (is_basin && land_height(config, center.elevation) >= settings.canyon_elevation) {
            return LandmarkKind::Crater;
        }

        // A hot spring: geologically restless or high-latitude ground with fresh
        // water running through it.
        if (landmark_suits_biome(LandmarkKind::HotSpring, center.biome)) {
            for (const EdgeId edge_id : center.borders) {
                if (graph.edges[static_cast<std::size_t>(edge_id)].river > 0) {
                    return LandmarkKind::HotSpring;
                }
            }
        }

        // A waterfall: a river crossing a real drop between its two banks.
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.river <= 0 || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }
            const double drop =
                std::abs(graph.corners[static_cast<std::size_t>(edge.v0)].elevation
                       - graph.corners[static_cast<std::size_t>(edge.v1)].elevation);
            if (drop >= settings.waterfall_drop) {
                return LandmarkKind::Waterfall;
            }
        }

        // An oasis: fresh water in a desert.
        if (landmark_suits_biome(LandmarkKind::Oasis, center.biome)) {
            for (const CenterId neighbor_id : center.neighbors) {
                const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
                if (neighbor.water && !neighbor.ocean) {
                    return LandmarkKind::Oasis;
                }
            }
        }

        // A cape: land reaching out, with most of its neighbours open sea.
        std::size_t ocean_neighbors = 0;
        for (const CenterId neighbor_id : center.neighbors) {
            if (graph.centers[static_cast<std::size_t>(neighbor_id)].ocean) {
                ++ocean_neighbors;
            }
        }
        if (!center.neighbors.empty()
            && ocean_neighbors * 2 >= center.neighbors.size() * settings.cape_ocean_ratio_numerator
                                          / settings.cape_ocean_ratio_denominator) {
            return LandmarkKind::Cape;
        }

        if (landmark_suits_biome(LandmarkKind::Canyon, center.biome)
            && land_height(config, center.elevation) > settings.canyon_elevation) {
            return LandmarkKind::Canyon;
        }
        return k_no_kind;
    }

    /** @brief Size of the connected body of fresh water a cell belongs to. */
    std::size_t lake_cluster_size_(const MapGraph& graph, const MapCenter& start) const {
        std::vector<CenterId> stack{start.index};
        std::vector<bool> seen(graph.centers.size(), false);
        seen[static_cast<std::size_t>(start.index)] = true;
        std::size_t size = 0;

        while (!stack.empty()) {
            const CenterId current = stack.back();
            stack.pop_back();
            ++size;
            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
                const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
                if (!seen[static_cast<std::size_t>(neighbor_id)] && neighbor.water && !neighbor.ocean) {
                    seen[static_cast<std::size_t>(neighbor_id)] = true;
                    stack.push_back(neighbor_id);
                }
            }
        }
        return size;
    }

    /**
     * @brief Scatters ruins and works on habitable land nobody settled.
     *
     * Sited away from living towns, so the world reads as having been more
     * populated once than it is now.
     */
    void find_abandoned_(MapGraph& graph, const MapConfig& config, std::vector<bool>& taken,
                         std::mt19937& rng) const {
        const LandmarkConfig& settings = config.landmarks;
        std::vector<CenterId> candidates;
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.ocean || center.border
                || taken[static_cast<std::size_t>(center.index)]) {
                continue;
            }
            if (far_from_towns_(graph, center.point, settings.town_clearance)) {
                candidates.push_back(center.index);
            }
        }
        coopa::maps::shuffle(candidates.begin(), candidates.end(), rng);

        static const LandmarkKind kinds[] = {LandmarkKind::Ruins, LandmarkKind::StandingStones,
                                             LandmarkKind::Monolith, LandmarkKind::Wreck,
                                             LandmarkKind::Tower, LandmarkKind::Shrine};

        int placed = 0;
        for (const CenterId candidate : candidates) {
            if (placed >= settings.max_abandoned) {
                return;
            }
            const MapCenter& center = graph.centers[static_cast<std::size_t>(candidate)];

            // Draw only from the kinds this terrain allows, rather than guessing
            // and retrying. Guessing gives a terrain-specific kind -- a wreck
            // needs a shore -- a vanishing chance against the four that fit
            // anywhere, so those kinds never appear at all.
            std::vector<LandmarkKind> allowed;
            for (const LandmarkKind kind : kinds) {
                if (landmark_suits_biome(kind, center.biome)) {
                    allowed.push_back(kind);
                }
            }
            if (allowed.empty()) {
                continue;
            }
            coopa::maps::UniformIntDistribution<std::size_t> pick(0, allowed.size() - 1);
            place_(graph, center, allowed[pick(rng)], taken, rng);
            ++placed;
        }
    }

    /** @brief Gives every region one signature wonder, at its most extreme cell. */
    void find_wonders_(MapGraph& graph, std::vector<bool>& taken, std::mt19937& rng) const {
        for (const MapRegion& region : graph.regions) {
            CenterId best = k_invalid_id;
            double best_elevation = -1.0;
            for (const CenterId cell : region.cells) {
                const MapCenter& center = graph.centers[static_cast<std::size_t>(cell)];
                if (center.water || taken[static_cast<std::size_t>(cell)]) {
                    continue;
                }
                if (center.elevation > best_elevation) {
                    best_elevation = center.elevation;
                    best = cell;
                }
            }
            if (best != k_invalid_id) {
                place_(graph, graph.centers[static_cast<std::size_t>(best)], LandmarkKind::Wonder,
                       taken, rng);
            }
        }
    }

    /** @brief True when a point is at least `clearance` from every settlement. */
    static bool far_from_towns_(const MapGraph& graph, const MapPoint& point, double clearance) {
        const double clearance_squared = clearance * clearance;
        for (const MapTown& town : graph.towns) {
            const double dx = town.point.x - point.x;
            const double dy = town.point.y - point.y;
            const double dz = town.point.z - point.z;   // 0 on a plane map
            if (dx * dx + dy * dy + dz * dz < clearance_squared) {
                return false;
            }
        }
        return true;
    }

    /** @brief Records a landmark on a cell and names it in the local dialect. */
    void place_(MapGraph& graph, const MapCenter& center, LandmarkKind kind,
                std::vector<bool>& taken, std::mt19937& rng) const {
        MapLandmark landmark;
        landmark.center = center.index;
        landmark.point = center.point;
        landmark.kind = kind;
        landmark.region = center.region;
        landmark.name = std::string(name_stem_(graph, center.region, rng)) + " "
                      + std::string(landmark_noun(kind));

        taken[static_cast<std::size_t>(center.index)] = true;
        graph.landmarks.push_back(std::move(landmark));
    }

    /** @brief A place-name stem in the dialect of a region, or a neutral one outside any. */
    std::string name_stem_(const MapGraph& graph, RegionId region, std::mt19937& rng) const {
        if (region == k_invalid_id || static_cast<std::size_t>(region) >= graph.regions.size()) {
            Language stateless = make_language(rng);
            return generate_name(stateless, rng);
        }
        auto found = dialects_.find(region);
        if (found == dialects_.end()) {
            const MapRegion& owner = graph.regions[static_cast<std::size_t>(region)];
            found = dialects_.emplace(region,
                                      dialect_for(country_language_seed(seed_, owner.country),
                                                  region_dialect_seed(seed_, owner.index))).first;
        }
        return generate_name(found->second, rng);
    }

    /** @brief Cached dialect per region, built on first use. */
    mutable std::unordered_map<RegionId, Language> dialects_;
    /** @brief The map seed, so a region's dialect can be rebuilt from its id. */
    mutable int seed_ = 0;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_LANDMARKS_H
