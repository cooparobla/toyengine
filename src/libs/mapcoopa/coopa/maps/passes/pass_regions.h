/**
 * @file pass_regions.h
 * @brief Divides the land into countries and the regions inside them, and gives
 *        each one a language and a name.
 */

#ifndef COOPA_MAPS_PASSES_PASS_REGIONS_H
#define COOPA_MAPS_PASSES_PASS_REGIONS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/name_generator.h>
#include <coopa/maps/portable_random.h>
#include <coopa/maps/region.h>

namespace coopa {
namespace maps {

/**
 * @class PassRegions
 * @brief Fills `MapGraph::countries` and `MapGraph::regions`, and stamps every
 *        land cell with the pair it belongs to.
 *
 * Territory is claimed by a cost-weighted flood fill run from all seeds at once
 * -- a multi-source Dijkstra, so every cell ends up with the nearest claim by
 * travel difficulty rather than by straight-line distance. Climbing is
 * expensive and crossing water more so, which is why the borders that emerge
 * follow ridgelines and coasts instead of cutting across them.
 *
 * Runs after roads and before towns: a settlement needs to know its region to
 * be named in the right dialect.
 */
class PassRegions {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassBiomes` to have run.
     * @param config Supplies the `RegionConfig` block and the seed.
     * @param logger Receives progress and the counts placed.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: regions");

        graph.regions.clear();
        graph.countries.clear();
        for (MapCenter& center : graph.centers) {
            center.region = k_invalid_id;
            center.country = k_invalid_id;
        }

        const RegionConfig& settings = config.regions;
        if (settings.country_count <= 0) {
            return;
        }

        seed_ = config.seed;
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_seed_offset);

        const std::vector<CenterId> country_seeds =
            scatter_seeds_(graph, settings.country_count, settings.min_country_spacing, rng);
        if (country_seeds.empty()) {
            logger.warn("map pass: regions found no habitable land to claim");
            return;
        }

        claim_(graph, settings, country_seeds, ClaimLevel::Country);
        adopt_stragglers_(graph);
        build_countries_(graph, country_seeds, rng);
        subdivide_(graph, settings, rng);
        summarise_(graph);

        logger.info("map pass: regions placed " + std::to_string(graph.countries.size())
                    + " countries and " + std::to_string(graph.regions.size()) + " regions");
    }

private:
    /** @brief Offset from `MapConfig::seed` so regions do not share a stream with other passes. */
    static constexpr unsigned int k_seed_offset = 15485863u;

    /** @brief Which political level a claim is filling. */
    enum class ClaimLevel { Country, Region };

    /**
     * @brief Picks seed cells spread across dry land.
     *
     * Rejection sampling against a minimum spacing, relaxed if the land cannot
     * accommodate the requested count -- a small archipelago should still get
     * nations rather than none at all.
     */
    std::vector<CenterId> scatter_seeds_(const MapGraph& graph, int count, double min_spacing,
                                         std::mt19937& rng) const {
        std::vector<CenterId> land;
        for (const MapCenter& center : graph.centers) {
            if (!center.water && !center.ocean && !center.border) {
                land.push_back(center.index);
            }
        }
        if (land.empty() || count <= 0) {
            return {};
        }

        coopa::maps::shuffle(land.begin(), land.end(), rng);
        std::vector<CenterId> seeds;
        double spacing = min_spacing;

        // Two rounds: the second drops the spacing requirement so a cramped map
        // still gets the nations it asked for.
        for (int round = 0; round < 2 && static_cast<int>(seeds.size()) < count; ++round) {
            const double spacing_squared = spacing * spacing;
            for (const CenterId candidate : land) {
                if (static_cast<int>(seeds.size()) >= count) {
                    break;
                }
                const MapPoint& point = graph.centers[static_cast<std::size_t>(candidate)].point;
                const bool clear = std::none_of(seeds.begin(), seeds.end(),
                    [&graph, &point, spacing_squared](CenterId placed) {
                        const MapPoint& other = graph.centers[static_cast<std::size_t>(placed)].point;
                        const double dx = other.x - point.x;
                        const double dy = other.y - point.y;
                        const double dz = other.z - point.z;   // 0 on a plane map
                        return dx * dx + dy * dy + dz * dz < spacing_squared;
                    });
                if (clear) {
                    seeds.push_back(candidate);
                }
            }
            spacing = 0.0;
        }
        return seeds;
    }

    /**
     * @brief Multi-source Dijkstra assigning every reachable cell to its cheapest seed.
     *
     * @param graph The graph to stamp.
     * @param settings Supplies the terrain costs.
     * @param seeds One seed cell per claimant, indexed by claim id.
     * @param level Whether to write `country` or `region` ids.
     * @param restrict_to When filling regions, the country whose cells may be claimed.
     */
    void claim_(MapGraph& graph, const RegionConfig& settings, const std::vector<CenterId>& seeds,
                ClaimLevel level, CountryId restrict_to = k_invalid_id) const {
        const double infinity = std::numeric_limits<double>::infinity();
        std::vector<double> best(graph.centers.size(), infinity);
        std::vector<std::int32_t> owner(graph.centers.size(), k_invalid_id);

        using Entry = std::pair<double, CenterId>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;

        for (std::size_t i = 0; i < seeds.size(); ++i) {
            const std::size_t seed = static_cast<std::size_t>(seeds[i]);
            best[seed] = 0.0;
            owner[seed] = static_cast<std::int32_t>(i);
            frontier.emplace(0.0, seeds[i]);
        }

        while (!frontier.empty()) {
            const auto [cost, current_id] = frontier.top();
            frontier.pop();
            const std::size_t current = static_cast<std::size_t>(current_id);
            if (cost > best[current]) {
                continue; // A cheaper route to this cell was already settled.
            }

            for (const CenterId neighbor_id : graph.centers[current].neighbors) {
                const std::size_t neighbor = static_cast<std::size_t>(neighbor_id);
                const MapCenter& target = graph.centers[neighbor];
                if (target.border) {
                    continue;
                }
                if (level == ClaimLevel::Region && target.country != restrict_to) {
                    continue;
                }

                double step = 1.0;
                step += settings.elevation_cost
                      * std::abs(target.elevation - graph.centers[current].elevation);
                if (target.water) {
                    step += settings.water_crossing_cost;
                }

                const double candidate = cost + step;
                if (candidate < best[neighbor]) {
                    best[neighbor] = candidate;
                    owner[neighbor] = owner[current];
                    frontier.emplace(candidate, neighbor_id);
                }
            }
        }

        for (std::size_t i = 0; i < graph.centers.size(); ++i) {
            MapCenter& center = graph.centers[i];
            // Open water belongs to nobody; territory is land.
            if (owner[i] == k_invalid_id || center.water || center.ocean || center.border) {
                continue;
            }
            if (level == ClaimLevel::Country) {
                center.country = static_cast<CountryId>(owner[i]);
            } else {
                center.region = static_cast<RegionId>(owner[i]);
            }
        }
    }

    /**
     * @brief Hands any land the flood fill could not reach to its nearest claimant.
     *
     * Small islands can sit behind more water than the crossing cost will pay
     * for, and would otherwise stay stateless. A nation claiming an outlying
     * island is more plausible than land belonging to nobody.
     */
    void adopt_stragglers_(MapGraph& graph) const {
        std::vector<CenterId> unclaimed;
        for (const MapCenter& center : graph.centers) {
            if (!center.water && !center.ocean && !center.border
                && center.country == k_invalid_id) {
                unclaimed.push_back(center.index);
            }
        }
        if (unclaimed.empty()) {
            return;
        }

        for (const CenterId orphan : unclaimed) {
            const MapPoint& point = graph.centers[static_cast<std::size_t>(orphan)].point;
            double best = std::numeric_limits<double>::infinity();
            CountryId nearest = k_invalid_id;
            for (const MapCenter& center : graph.centers) {
                if (center.country == k_invalid_id) {
                    continue;
                }
                const double dx = center.point.x - point.x;
                const double dy = center.point.y - point.y;
                const double dz = center.point.z - point.z;   // 0 on a plane map
                const double distance = dx * dx + dy * dy + dz * dz;
                if (distance < best) {
                    best = distance;
                    nearest = center.country;
                }
            }
            graph.centers[static_cast<std::size_t>(orphan)].country = nearest;
        }
    }

    /** @brief Creates a country per seed, with its language and name. */
    void build_countries_(MapGraph& graph, const std::vector<CenterId>& seeds,
                          std::mt19937& rng) const {
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            MapCountry country;
            country.index = static_cast<CountryId>(i);
            country.color = distinct_color_(i, seeds.size(), 0.45f);
            const Language language = language_for(country_language_seed(seed_, country.index));
            country.name = generate_name(languages_[country.index] = language, rng);
            graph.countries.push_back(std::move(country));
        }
    }

    /** @brief Carves each country into regions and names them in its dialect. */
    void subdivide_(MapGraph& graph, const RegionConfig& settings, std::mt19937& rng) const {
        for (MapCountry& country : graph.countries) {
            std::vector<CenterId> owned;
            for (const MapCenter& center : graph.centers) {
                if (center.country == country.index) {
                    owned.push_back(center.index);
                }
            }
            if (owned.empty()) {
                continue;
            }

            const int wanted = std::max(1, std::min(settings.regions_per_country,
                                                    static_cast<int>(owned.size())));
            coopa::maps::shuffle(owned.begin(), owned.end(), rng);
            const std::vector<CenterId> seeds(owned.begin(), owned.begin() + wanted);

            // Region ids are per-country during the fill, then remapped to the
            // global region array below.
            claim_(graph, settings, seeds, ClaimLevel::Region, country.index);

            std::unordered_map<RegionId, RegionId> local_to_global;
            for (int local = 0; local < wanted; ++local) {
                MapRegion region;
                region.index = static_cast<RegionId>(graph.regions.size());
                region.country = country.index;
                region.seed = seeds[static_cast<std::size_t>(local)];
                region.color = distinct_color_(static_cast<std::size_t>(region.index),
                                               static_cast<std::size_t>(wanted)
                                                   * graph.countries.size(), 0.6f);

                const Language dialect = dialect_for(country_language_seed(seed_, country.index),
                                                    region_dialect_seed(seed_, region.index));
                region.name = generate_name(dialect, rng);

                local_to_global[static_cast<RegionId>(local)] = region.index;
                country.regions.push_back(region.index);
                graph.regions.push_back(std::move(region));
            }

            for (MapCenter& center : graph.centers) {
                if (center.country != country.index || center.region == k_invalid_id) {
                    continue;
                }
                const auto found = local_to_global.find(center.region);
                center.region = found == local_to_global.end() ? k_invalid_id : found->second;
            }
        }
    }

    /** @brief Fills in each region's and country's cell list, area and dominant biome. */
    void summarise_(MapGraph& graph) const {
        for (const MapCenter& center : graph.centers) {
            if (center.region == k_invalid_id) {
                continue;
            }
            graph.regions[static_cast<std::size_t>(center.region)].cells.push_back(center.index);
        }

        for (MapRegion& region : graph.regions) {
            std::vector<int> tally(k_biome_count, 0);
            for (const CenterId cell : region.cells) {
                const MapCenter& center = graph.centers[static_cast<std::size_t>(cell)];
                ++tally[static_cast<std::size_t>(center.biome)];
                region.area += graph.cell_area(center);
            }
            const auto peak = std::max_element(tally.begin(), tally.end());
            if (peak != tally.end() && *peak > 0) {
                region.dominant_biome =
                    static_cast<Biome>(std::distance(tally.begin(), peak));
            }
            if (region.country != k_invalid_id) {
                graph.countries[static_cast<std::size_t>(region.country)].area += region.area;
            }
        }
    }

    /**
     * @brief Spreads `count` tints evenly around the hue circle.
     *
     * Golden-ratio spacing rather than even division, so adjacent ids land far
     * apart in hue and two neighbouring territories are never near-identical.
     */
    static glm::vec3 distinct_color_(std::size_t index, std::size_t count, float saturation) {
        (void)count;
        const float hue = std::fmod(static_cast<float>(index) * 0.618033988f, 1.0f) * 6.0f;
        const int sector = static_cast<int>(hue);
        const float fraction = hue - static_cast<float>(sector);
        const float v = 255.0f;
        const float p = v * (1.0f - saturation);
        const float q = v * (1.0f - saturation * fraction);
        const float t = v * (1.0f - saturation * (1.0f - fraction));

        switch (sector % 6) {
            case 0: return whole_(v, t, p);
            case 1: return whole_(q, v, p);
            case 2: return whole_(p, v, t);
            case 3: return whole_(p, q, v);
            case 4: return whole_(t, p, v);
            default: return whole_(v, p, q);
        }
    }

    /**
     * @brief Rounds a colour to whole 0-255 components.
     *
     * Because these colours are *written to disk*. Every other colour in the
     * library is an integer triple, but a hue-wheel conversion lands on values
     * like 231.478, and the YAML keeps six significant digits -- so a region
     * reloaded from a saved map came back a step off, and the regions layer
     * rendered a pixel different across every province it filled. Rounding here
     * makes the colours exactly representable rather than making the round-trip
     * test tolerate the loss.
     */
    static glm::vec3 whole_(float r, float g, float b) {
        return glm::vec3(std::round(r), std::round(g), std::round(b));
    }

    /** @brief One language per country, kept between the build and subdivide steps. */
    mutable std::unordered_map<CountryId, Language> languages_;
    /** @brief The map seed, so language seeds can be derived from ids. */
    mutable int seed_ = 0;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_REGIONS_H
