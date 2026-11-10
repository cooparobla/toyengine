/**
 * @file pass_towns.h
 * @brief Eighth pass: scores every land cell for habitability, places
 *        settlements on the best sites, and packs buildings inside each one.
 */

#ifndef COOPA_MAPS_PASSES_PASS_TOWNS_H
#define COOPA_MAPS_PASSES_PASS_TOWNS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/name_generator.h>
#include <coopa/maps/portable_random.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassTowns
 * @brief Fills `MapGraph::towns`.
 *
 * Sites are scored on where people actually settle: a habitable biome, low
 * ground, and access to the sea, a river or a road. The best-scoring cells are
 * then accepted greedily subject to a minimum separation, so settlements
 * spread across a continent instead of clustering on the single best river
 * mouth. Buildings are packed into the accepted cell by scanning a grid and
 * keeping any footprint that lies wholly inside the polygon.
 *
 * The upstream generator this ports has no working town pass. The scoring and
 * placement here are mapcoopa's own; the packing uses a ray-cast containment
 * test.
 */
class PassTowns {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to populate; requires `PassBiomes` and `PassRoads` to have run.
     * @param config Supplies the seed and the `TownConfig` block.
     * @param logger Receives progress and the settlement count placed.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: towns");

        graph.towns.clear();
        dialects_.clear();
        taken_names_.clear();
        const TownConfig& towns = config.towns;
        if (towns.town_count <= 0 || graph.centers.empty()) {
            return;
        }

        // Offset from the master seed so town placement varies independently
        // of the river sources, which draw from the same value.
        seed_ = config.seed;
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_seed_offset);
        coopa::maps::UniformRealDistribution<double> jitter(-towns.score_jitter, towns.score_jitter);

        const double mean_area = mean_land_area_(graph);
        std::vector<CenterId> candidates;
        std::vector<double> scores(graph.centers.size(), 0.0);
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.ocean || center.border) {
                continue;
            }
            const double score = score_site_(graph, center, config, mean_area) + jitter(rng);
            if (score <= 0.0) {
                continue;
            }
            scores[static_cast<std::size_t>(center.index)] = score;
            candidates.push_back(center.index);
        }

        coopa::maps::sort(candidates.begin(), candidates.end(), [&scores](CenterId a, CenterId b) {
            return scores[static_cast<std::size_t>(a)] > scores[static_cast<std::size_t>(b)];
        });

        const double min_spacing = meters_to_grid(config, towns.min_spacing_m);
        const double min_spacing_squared = min_spacing * min_spacing;
        // A cell belongs to at most one settlement, so two neighbouring towns
        // cannot both build on the same ground.
        std::vector<bool> claimed(graph.centers.size(), false);
        for (const CenterId candidate : candidates) {
            if (static_cast<int>(graph.towns.size()) >= towns.town_count) {
                break;
            }
            const MapCenter& center = graph.centers[static_cast<std::size_t>(candidate)];
            if (!is_clear_of_existing_(graph, center.point, min_spacing_squared)) {
                continue;
            }

            MapTown town;
            town.center = center.index;
            town.point = center.point;
            town.region = center.region;
            town.score = scores[static_cast<std::size_t>(candidate)];
            town.tier = tier_for_rank_(static_cast<int>(graph.towns.size()), towns);
            town.cells = claim_cells_(graph, center, towns, town.tier, claimed);
            // Streets and square before the packer: both are things the buildings
            // have to be laid out *around*, so they cannot be decided afterwards.
            if (graph.is_sphere()) {
                lay_out_on_sphere_(graph, town, config, rng);
            } else {
                town.streets = gather_streets_(graph, town, config);
                town.plaza = site_plaza_(graph, town, config);
                town.buildings = pack_settlement_(graph, town, config, rng);
            }
            assign_roles_(town, config.towns);
            assign_statistics_(town, center, towns, rng);
            town.name = name_for_(graph, town, rng);
            graph.towns.push_back(std::move(town));
        }

        roll_up_populations_(graph);

        int total_population = 0;
        for (const MapTown& town : graph.towns) {
            total_population += town.population;
        }
        logger.info("map pass: towns placed " + std::to_string(graph.towns.size())
                    + " of " + std::to_string(towns.town_count) + " requested, population "
                    + std::to_string(total_population));
    }

private:
    /** @brief Added to `MapConfig::seed` so towns and rivers do not share a stream. */
    static constexpr unsigned int k_seed_offset = 7919u;

    /** @brief Scores one cell on biome, elevation and its access to water and roads. */
    double score_site_(const MapGraph& graph, const MapCenter& center, const MapConfig& config,
                       double mean_area) const {
        const TownConfig& towns = config.towns;
        double score = biome_habitability(center.biome);
        if (score <= 0.0) {
            return 0.0;
        }

        if (center.coast) {
            score += towns.coast_bonus;
        }

        bool has_river = false;
        bool has_road = false;
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.river > 0) has_river = true;
            if (edge.road) has_road = true;
        }
        if (has_river) score += towns.river_bonus;
        if (has_road) score += towns.road_bonus;

        // Land-relative: "too high to reach easily" is a statement about how far up
        // the hills a site is, not about its height above the sea floor.
        const double height = land_height(config, center.elevation);
        if (height > towns.elevation_penalty_start) {
            score -= (height - towns.elevation_penalty_start) * towns.elevation_penalty_scale;
        }

        // Room to grow. A settlement can hold no more buildings than its cell
        // has space for, so ranking sites without regard to area produces
        // capitals smaller than the towns below them.
        if (mean_area > 0.0) {
            const double relative = std::clamp(graph.cell_area(center) / mean_area, 0.0, 2.0);
            score += towns.area_bonus * (relative - 1.0);
        }
        return score;
    }

    /** @brief Mean cell area across dry land, used to normalise the area bonus. */
    static double mean_land_area_(const MapGraph& graph) {
        double total = 0.0;
        std::size_t count = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.border) {
                continue;
            }
            total += graph.cell_area(center);
            ++count;
        }
        return count == 0 ? 0.0 : total / static_cast<double>(count);
    }

    /** @brief True when `point` is at least the minimum spacing from every placed town. */
    bool is_clear_of_existing_(const MapGraph& graph, const MapPoint& point,
                               double min_spacing_squared) const {
        for (const MapTown& town : graph.towns) {
            const double dx = town.point.x - point.x;
            const double dy = town.point.y - point.y;
            const double dz = town.point.z - point.z;   // 0 on a plane map
            if (dx * dx + dy * dy + dz * dz < min_spacing_squared) {
                return false;
            }
        }
        return true;
    }

    /** @brief Awards a size class by placement rank -- capitals first, villages last. */
    TownTier tier_for_rank_(int rank, const TownConfig& towns) const {
        if (rank < towns.capital_count) {
            return TownTier::Capital;
        }
        if (rank < towns.capital_count + towns.town_tier_count) {
            return TownTier::Town;
        }
        return TownTier::Village;
    }

    /**
     * @brief Claims the cells a settlement of this tier covers, nearest first.
     *
     * Breadth-first from the site over `MapCenter::neighbors`, taking dry,
     * habitable land no other settlement holds. Breadth-first and not a radius,
     * so a coastal town grows along its shore rather than reaching across the
     * water, and so the claimed patch is always contiguous.
     *
     * A settlement that cannot find its full allowance -- on a headland, or
     * hemmed in by a neighbour -- simply gets fewer cells and correspondingly
     * fewer buildings. That is the right answer: the ground really is not there.
     *
     * @param graph The map being generated.
     * @param center The settlement's primary cell.
     * @param towns Supplies the per-tier cell allowance.
     * @param tier The settlement's size class.
     * @param claimed Updated in place; marks every cell this settlement takes.
     * @return The claimed cells, the primary one first.
     */
    static std::vector<CenterId> claim_cells_(const MapGraph& graph, const MapCenter& center,
                                              const TownConfig& towns, TownTier tier,
                                              std::vector<bool>& claimed) {
        const int allowance = cells_for_tier_(tier, towns);
        std::vector<CenterId> cells;
        if (allowance <= 0) {
            return cells;
        }

        std::queue<CenterId> pending;
        pending.push(center.index);
        claimed[static_cast<std::size_t>(center.index)] = true;
        cells.push_back(center.index);

        while (!pending.empty() && static_cast<int>(cells.size()) < allowance) {
            const CenterId current = pending.front();
            pending.pop();
            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
                if (static_cast<int>(cells.size()) >= allowance) {
                    break;
                }
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                if (claimed[index]) {
                    continue;
                }
                const MapCenter& neighbor = graph.centers[index];
                if (neighbor.water || neighbor.ocean || neighbor.border
                    || biome_habitability(neighbor.biome) <= 0.0) {
                    continue;
                }
                claimed[index] = true;
                cells.push_back(neighbor_id);
                pending.push(neighbor_id);
            }
        }
        return cells;
    }

    /** @brief How many cells a settlement of this tier may claim. */
    static int cells_for_tier_(TownTier tier, const TownConfig& towns) {
        switch (tier) {
            case TownTier::Capital: return towns.capital_cells;
            case TownTier::Town:    return towns.town_cells;
            case TownTier::Village: return towns.village_cells;
        }
        return towns.village_cells;
    }

    /**
     * @brief Packs buildings across every cell a settlement claims.
     *
     * The per-cell packer runs once per claimed cell, and the growing building
     * list is carried from one to the next so `buildings_overlap()` still
     * rejects a footprint that would cross a cell boundary into one already
     * built on. Packing each cell independently would let two plots meet exactly
     * on the shared edge.
     *
     * @param graph The map being generated.
     * @param town The settlement, with its cells already claimed.
     * @param config Supplies the layout sizes and the budget.
     * @param rng The pass's seeded generator.
     * @return Every building placed, across all of the settlement's cells.
     */
    /**
     * @brief A town on a globe: laid out exactly as on a plane, in the plane touching the globe at
     *        the town (`SphereFrame`), then lifted back. Bearings and building yaws stay in that
     *        plane's frame -- east along the equator, north toward +z.
     */
    void lay_out_on_sphere_(const MapGraph& graph, MapTown& town, const MapConfig& config,
                            std::mt19937& rng) const {
        const SphereFrame frame(graph, town.point);
        const MapGraph flat = frame.flatten(graph);
        MapTown local = town;
        local.point = frame.to_plane(town.point);
        local.streets = gather_streets_(flat, local, config);
        local.plaza = site_plaza_(flat, local, config);
        local.buildings = pack_settlement_(flat, local, config, rng);
        town.streets = std::move(local.streets);
        for (MapStreet& street : town.streets) {
            street.from = frame.to_sphere(street.from);
            street.to = frame.to_sphere(street.to);
        }
        town.plaza = local.plaza;
        town.plaza.centre = frame.to_sphere(local.plaza.centre);
        town.buildings = std::move(local.buildings);
        for (MapBuilding& building : town.buildings) building.point = frame.to_sphere(building.point);
    }

    std::vector<MapBuilding> pack_settlement_(const MapGraph& graph, const MapTown& town,
                                              const MapConfig& config, std::mt19937& rng) const {
        std::vector<MapBuilding> buildings;
        const int budget = building_budget_(town.tier, config.towns);
        for (const CenterId cell_id : town.cells) {
            if (static_cast<int>(buildings.size()) >= budget) {
                break;
            }
            pack_cell_(graph, graph.centers[static_cast<std::size_t>(cell_id)], config,
                       town.plaza, budget, rng, buildings);
        }
        return buildings;
    }

    /**
     * @brief Derives the streets running through a settlement's cell.
     *
     * A road is drawn along the Delaunay edge -- from one cell's site to its
     * neighbour's -- so an edge flagged `road` means a road enters this cell and
     * heads for its centre. The same goes for a river bank. Those approaches are
     * the streets, and buildings line them.
     *
     * A cell with neither still needs structure, or its layout degenerates into
     * scattered noise, so it falls back to lanes running from the site toward
     * its farthest corners.
     *
     * @param graph The map being generated.
     * @param center The settlement's cell.
     * @return The streets, innermost end first.
     */
    std::vector<MapStreet> derive_streets_(const MapGraph& graph, const MapCenter& center,
                                        const MapConfig& config) const {
        std::vector<MapStreet> streets;

        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (!edge.road && edge.river <= 0) {
                continue;
            }
            MapStreet street = make_street(center.point, edge.midpoint);
            if (edge.river > 0) {
                // Set the frontage back past the water's edge. Without this the
                // packer aims plots at the channel and every one is rejected,
                // losing the riverside frontage that made the site desirable.
                street.clearance = river_width(config, edge.river) * 0.5
                                 + meters_to_grid(config, config.towns.water_clearance_m);
            }
            streets.push_back(street);
        }

        if (streets.empty()) {
            std::vector<CornerId> by_distance = center.corners;
            coopa::maps::sort(by_distance.begin(), by_distance.end(),
                      [&graph, &center](CornerId a, CornerId b) {
                          return center.point.distance_to(graph.corners[static_cast<std::size_t>(a)].point)
                               > center.point.distance_to(graph.corners[static_cast<std::size_t>(b)].point);
                      });
            const std::size_t wanted = std::min<std::size_t>(k_fallback_streets, by_distance.size());
            for (std::size_t i = 0; i < wanted; ++i) {
                streets.push_back(make_street(
                    center.point, graph.corners[static_cast<std::size_t>(by_distance[i])].point));
            }
        }

        return streets;
    }

    /**
     * @brief Every street of a settlement, one fan per cell it claims.
     *
     * A capital spans seven cells and each has its own approaches, so its streets
     * are the union rather than the primary cell's alone. That was already how the
     * packer worked; this is what makes it visible.
     *
     * @param graph The map being generated.
     * @param town The settlement, with its cells already claimed.
     * @param config Supplies the river setbacks.
     * @return Every street, grouped by cell in claim order.
     */
    std::vector<MapStreet> gather_streets_(const MapGraph& graph, const MapTown& town,
                                           const MapConfig& config) const {
        std::vector<MapStreet> streets;
        for (const CenterId cell_id : town.cells) {
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            const std::vector<MapStreet> fan = derive_streets_(graph, cell, config);
            streets.insert(streets.end(), fan.begin(), fan.end());
        }
        return streets;
    }

    /**
     * @brief Places a settlement's market square, when it has the ground for one.
     *
     * At the primary cell's site, because that is where the street fan already
     * converges -- so the lanes radiate from the square without either being made
     * to agree with the other.
     *
     * Granted on two conditions, and both matter. The settlement must claim at
     * least `plaza_min_cells`, which is what keeps a one-cell village a village
     * rather than a hamlet with a hole in it. And the square must actually fit:
     * the radius is trimmed against the distance to the nearest cell corner, and
     * abandoned if what is left could not hold even one building's frontage
     * around it.
     *
     * @param graph The map being generated.
     * @param town The settlement, with its cells already claimed.
     * @param config Supplies the plaza radius and the tier thresholds.
     * @return The square, or a zero radius when the settlement gets none.
     */
    MapPlaza site_plaza_(const MapGraph& graph, const MapTown& town,
                         const MapConfig& config) const {
        MapPlaza plaza;
        const TownConfig& towns = config.towns;
        if (static_cast<int>(town.cells.size()) < towns.plaza_min_cells
            || towns.plaza_radius_m <= 0.0) {
            return plaza;
        }
        const MapCenter& primary = graph.centers[static_cast<std::size_t>(town.center)];
        if (primary.corners.empty()) {
            return plaza;
        }

        double nearest_corner = std::numeric_limits<double>::max();
        for (const CornerId corner_id : primary.corners) {
            nearest_corner = std::min(
                nearest_corner,
                primary.point.distance_to(graph.corners[static_cast<std::size_t>(corner_id)].point));
        }

        const double wanted = meters_to_grid(config, towns.plaza_radius_m);
        const double frontage = meters_to_grid(config, towns.building_size_max_m);
        // Leave a building's depth of ground between the square and the cell edge,
        // or the square swallows the cell and there is nowhere left to build.
        const double room = nearest_corner - frontage;
        if (room <= 0.0) {
            return plaza;
        }
        plaza.centre = primary.point;
        plaza.radius = std::min(wanted, room);
        return plaza;
    }

    /**
     * @brief Hands the civic roles to the buildings nearest the heart of the town.
     *
     * Nearest the square when there is one, nearest the site when there is not, so
     * the hall and the market end up at the centre rather than scattered through
     * the outskirts. Taken in order from one roster shared by every tier -- a town
     * is a prefix of a capital, which is what makes it read as a smaller version
     * of the same thing rather than a different kind of place.
     *
     * @param town The settlement; its buildings must already be packed.
     * @param towns Supplies how many civic buildings each tier is given.
     */
    void assign_roles_(MapTown& town, const TownConfig& towns) const {
        static constexpr std::array<BuildingRole, 9> k_roster = {
            BuildingRole::Well,     BuildingRole::Hall,      BuildingRole::Market,
            BuildingRole::Smithy,   BuildingRole::Temple,    BuildingRole::Inn,
            BuildingRole::Granary,  BuildingRole::Barracks,  BuildingRole::Warehouse};

        int wanted = 0;
        switch (town.tier) {
            case TownTier::Capital: wanted = towns.capital_civic_count; break;
            case TownTier::Town:    wanted = towns.town_civic_count; break;
            case TownTier::Village: wanted = towns.village_civic_count; break;
        }
        wanted = std::clamp(wanted, 0, static_cast<int>(k_roster.size()));
        // And never more than half the settlement. A hamlet that came out with
        // three buildings does not have a hall, a market and one house -- the tier
        // says what it may have, the ground says what it did.
        wanted = std::min(wanted, static_cast<int>(town.buildings.size() / 2));
        if (wanted <= 0 || town.buildings.empty()) {
            return;
        }

        const MapPoint heart = town.plaza.radius > 0.0 ? town.plaza.centre : town.point;
        std::vector<std::size_t> order(town.buildings.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        coopa::maps::sort(order.begin(), order.end(), [&town, &heart](std::size_t a, std::size_t b) {
            return heart.distance_to(town.buildings[a].point)
                 < heart.distance_to(town.buildings[b].point);
        });

        const std::size_t granted =
            std::min(static_cast<std::size_t>(wanted), order.size());
        for (std::size_t i = 0; i < granted; ++i) {
            town.buildings[order[i]].role = k_roster[i];
        }
    }

    /**
     * @brief Lays a settlement out: buildings fronting its streets, then infill.
     *
     * Plots are placed in pairs flanking each street, walking outward from the
     * cell's site, each jittered in position and yaw so the rows read as built
     * over time rather than surveyed. Whatever budget survives that is spent on
     * rejection sampling across the cell, which fills the interior without
     * falling back into a grid.
     *
     * Every candidate is accepted only if all four of its *rotated* corners lie
     * inside the cell and it clears every building already placed. Testing the
     * rotated footprint is what makes `MapBuilding::rotation` a pose a consumer
     * can trust; testing an axis-aligned box and assigning the yaw afterwards
     * would let a building drawn at its stated angle poke outside the cell or
     * into its neighbour.
     *
     * The polygon comes from `MapCenter::corners` rather than the edges' noisy
     * paths: this pass runs before the noisy-edge pass, so those paths do not
     * exist yet. A footprint that fits the straight-edged cell also fits the
     * wobbled one, since subdivision only pushes boundaries outward from the
     * midpoint by a bounded fraction.
     *
     * @param graph The map being generated.
     * @param center The settlement's cell.
     * @param towns Layout parameters.
     * @param rng The pass's seeded generator; the sole source of randomness here.
     * @return The placed buildings.
     */
    void pack_cell_(const MapGraph& graph, const MapCenter& center, const MapConfig& config,
                    const MapPlaza& plaza, int budget, std::mt19937& rng,
                    std::vector<MapBuilding>& buildings) const {
        if (center.corners.size() < 3 || budget <= 0) {
            return;
        }

        std::vector<MapPoint> polygon;
        polygon.reserve(center.corners.size());
        for (const CornerId corner_id : center.corners) {
            polygon.push_back(graph.corners[static_cast<std::size_t>(corner_id)].point);
        }

        const Layout layout = layout_for_(config);
        // Streets first: they are one of the things the buildings must keep clear
        // of, as well as the lines they are ranged along.
        const std::vector<MapStreet> streets = derive_streets_(graph, center, config);
        const KeepOut keep_out = collect_keep_out_(graph, center, streets, config);
        place_street_frontage_(polygon, streets, layout, keep_out, plaza, budget, rng, buildings);
        place_infill_(polygon, streets, layout, keep_out, plaza, budget, rng, buildings);
    }

    /**
     * @brief Names a settlement in the dialect of the region it stands in.
     *
     * Each region's language is rebuilt here from the same seed the region pass
     * used, rather than carried on `MapRegion`: a language is generator state,
     * not map data, and serialising one would put phoneme tables in every saved
     * world for no gain -- the names themselves are what a consumer needs.
     *
     * @param graph The map, for the region list.
     * @param town The settlement being named.
     * @param rng The pass's seeded generator.
     * @return A generated place name, or a fallback for an unclaimed site.
     */
    std::string name_for_(const MapGraph& graph, const MapTown& town, std::mt19937& rng) const {
        if (town.region == k_invalid_id
            || static_cast<std::size_t>(town.region) >= graph.regions.size()) {
            // Land outside any nation still gets a name, just not a local one.
            Language stateless = make_language(rng);
            return claim_name_(generate_name(stateless, rng), stateless, rng);
        }

        auto found = dialects_.find(town.region);
        if (found == dialects_.end()) {
            const MapRegion& region = graph.regions[static_cast<std::size_t>(town.region)];
            found = dialects_.emplace(
                town.region,
                dialect_for(country_language_seed(seed_, region.country),
                            region_dialect_seed(seed_, region.index))).first;
        }
        return claim_name_(generate_name(found->second, rng), found->second, rng);
    }

    /**
     * @brief Takes a drawn name if it is free, otherwise redraws until one is.
     *
     * Two settlements of one region draw from the same small phoneme table, so a
     * collision is not rare -- it is a matter of how many towns that region got.
     * Two places on the same map sharing a name is worse than a slightly duller
     * name: a consumer keying on a place name would silently conflate them.
     *
     * The last resort is a numeric suffix rather than another draw, because only
     * that terminates: a dialect has a finite vocabulary and a region crowded
     * enough can exhaust it, and a loop that draws until it gets lucky would not
     * come back.
     *
     * @param drawn The first candidate, already generated.
     * @param dialect The language to redraw from.
     * @param rng The pass's seeded generator.
     * @return A name no other settlement on this map holds.
     */
    std::string claim_name_(std::string drawn, const Language& dialect, std::mt19937& rng) const {
        for (int attempt = 0; attempt < k_name_attempts; ++attempt) {
            if (taken_names_.insert(drawn).second) {
                return drawn;
            }
            drawn = generate_name(dialect, rng);
        }
        for (int ordinal = 2;; ++ordinal) {
            std::string suffixed = drawn + " " + std::to_string(ordinal);
            if (taken_names_.insert(suffixed).second) {
                return suffixed;
            }
        }
    }

    /** @brief Sums settlement populations into their regions and countries. */
    void roll_up_populations_(MapGraph& graph) const {
        for (MapRegion& region : graph.regions) {
            region.population = 0;
            region.capital = k_invalid_id;
        }
        for (MapCountry& country : graph.countries) {
            country.population = 0;
            country.capital = k_invalid_id;
        }

        for (const MapTown& town : graph.towns) {
            if (town.region == k_invalid_id
                || static_cast<std::size_t>(town.region) >= graph.regions.size()) {
                continue;
            }
            MapRegion& region = graph.regions[static_cast<std::size_t>(town.region)];
            region.population += town.population;

            // The region's seat is its largest settlement, not its first.
            if (region.capital == k_invalid_id
                || town.population > population_at_(graph, region.capital)) {
                region.capital = town.center;
            }
        }

        for (const MapRegion& region : graph.regions) {
            if (region.country == k_invalid_id
                || static_cast<std::size_t>(region.country) >= graph.countries.size()) {
                continue;
            }
            MapCountry& country = graph.countries[static_cast<std::size_t>(region.country)];
            country.population += region.population;
            if (region.capital != k_invalid_id
                && (country.capital == k_invalid_id
                    || population_at_(graph, region.capital)
                           > population_at_(graph, country.capital))) {
                country.capital = region.capital;
            }
        }
    }

    /** @brief Population of the settlement occupying a cell, or 0 if none does. */
    static int population_at_(const MapGraph& graph, CenterId cell) {
        for (const MapTown& town : graph.towns) {
            if (town.center == cell) {
                return town.population;
            }
        }
        return 0;
    }

    /** @brief Redraws allowed before a name is disambiguated by ordinal instead. */
    static constexpr int k_name_attempts = 64;

    /** @brief Cached dialect per region, built on first use. */
    mutable std::unordered_map<RegionId, Language> dialects_;
    /** @brief Every settlement name already handed out, so no two places share one. */
    mutable std::unordered_set<std::string> taken_names_;
    /** @brief The map seed, so a region's dialect can be rebuilt from its id. */
    mutable int seed_ = 0;

    /**
     * @brief Derives a settlement's population and prosperity from what it is made of.
     *
     * Population is counted, not invented: every dwelling houses a drawn number
     * of occupants, scaled by how densely the tier builds and by how well the
     * surrounding land feeds them. A settlement's headcount and its footprint
     * therefore cannot drift apart -- add buildings and the population follows.
     *
     * @param town The settlement to annotate; its buildings must already be packed.
     * @param center The cell it occupies, for biome habitability.
     * @param towns Household size and density parameters.
     * @param rng The pass's seeded generator.
     */
    void assign_statistics_(MapTown& town, const MapCenter& center, const TownConfig& towns,
                            std::mt19937& rng) const {
        town.households = static_cast<int>(town.buildings.size());
        if (town.households == 0) {
            town.population = 0;
            town.prosperity = 0.0;
            return;
        }

        const double density = tier_density_(town.tier, towns);
        // Fertile ground supports fuller houses; the same hamlet on scorched
        // rock holds fewer people than one on grassland.
        const double land = 0.7 + 0.3 * biome_habitability(center.biome);

        coopa::maps::UniformIntDistribution<int> occupants(towns.household_size_min,
                                                     towns.household_size_max);
        double total = 0.0;
        for (int household = 0; household < town.households; ++household) {
            total += occupants(rng) * density * land;
        }
        town.population = std::max(1, static_cast<int>(total));
        town.prosperity = std::clamp(town.score * 0.5 * density, 0.0, 1.0);
    }

    /** @brief Population density multiplier for a settlement's size class. */
    static double tier_density_(TownTier tier, const TownConfig& towns) {
        switch (tier) {
            case TownTier::Capital: return towns.capital_density;
            case TownTier::Town:    return towns.town_density;
            case TownTier::Village: return 1.0;
        }
        return 1.0;
    }

    /**
     * @struct KeepOut
     * @brief Everything a settlement's buildings must stay clear of, as segments.
     *
     * Water came first: a river runs *along* a cell boundary and a lake or sea sits
     * on the far side of one, so both are edges of the polygon the buildings sit
     * inside, and containment alone cannot catch them.
     *
     * Roadways joined it for the opposite reason. A street is a line the packer
     * deliberately aims plots *at*, and the road network runs through a
     * settlement's cells on its way between them -- so with nothing stated, 37% of
     * buildings stood on a lane and 18% on a road. One list, because the test is
     * identical: keep every rotated corner further than this clearance from this
     * segment.
     */
    struct KeepOut {
        /** @brief Segment endpoints paired with the clearance required from each. */
        std::vector<std::array<MapPoint, 2>> segments;
        std::vector<double> clearances;
    };

    /** @brief Gathers the river and shoreline edges bounding a cell, with their clearances. */
    KeepOut collect_keep_out_(const MapGraph& graph, const MapCenter& center,
                              const std::vector<MapStreet>& streets,
                              const MapConfig& config) const {
        KeepOut keep_out;
        const double verge = meters_to_grid(config, config.towns.street_clearance_m);

        // The cell's own lanes. These are the lines the frontage marches along, so
        // without them the packer walks plots straight onto the carriageway.
        const double lane = meters_to_grid(config, config.towns.street_width_m) * 0.5 + verge;
        for (const MapStreet& street : streets) {
            keep_out.segments.push_back({street.from, street.to});
            keep_out.clearances.push_back(lane);
        }

        // And the road network passing through. Measured against the *smoothed*
        // centreline the roads layer actually strokes, not the straight Delaunay
        // edge it was derived from -- a building clear of one can still stand on
        // the other. Filtered to the segments that could possibly reach this cell,
        // so the inner loop stays short.
        double cell_reach = 0.0;
        for (const CornerId corner_id : center.corners) {
            cell_reach = std::max(
                cell_reach,
                center.point.distance_to(graph.corners[static_cast<std::size_t>(corner_id)].point));
        }
        for (const MapRoad& road : graph.roads) {
            const double clearance = road_width_for(config, road.road_class) * 0.5 + verge;
            for (std::size_t i = 0; i + 1 < road.points.size(); ++i) {
                if (distance_to_segment_(center.point, road.points[i], road.points[i + 1])
                    > cell_reach + clearance) {
                    continue;
                }
                keep_out.segments.push_back({road.points[i], road.points[i + 1]});
                keep_out.clearances.push_back(clearance);
            }
        }

        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }

            double clearance = 0.0;
            if (edge.river > 0) {
                clearance = river_width(config, edge.river) * 0.5
                          + meters_to_grid(config, config.towns.water_clearance_m);
            } else {
                const CenterId other = edge.d0 == center.index ? edge.d1 : edge.d0;
                if (other != k_invalid_id
                    && graph.centers[static_cast<std::size_t>(other)].water) {
                    clearance = meters_to_grid(config, config.towns.water_clearance_m);
                }
            }
            if (clearance <= 0.0) {
                continue;
            }

            keep_out.segments.push_back({graph.corners[static_cast<std::size_t>(edge.v0)].point,
                                         graph.corners[static_cast<std::size_t>(edge.v1)].point});
            keep_out.clearances.push_back(clearance);
        }
        return keep_out;
    }

    /**
     * @struct Layout
     * @brief A settlement's layout sizes, converted from metres into grid units once.
     *
     * The packer works in grid space, because that is where the cell polygon it
     * has to fit inside lives, but every one of these is configured in metres,
     * because that is what it physically is. Converting once at the top and
     * passing this down means no line further in has to remember which system
     * its number is in -- and a field in here is, by construction, grid units.
     */
    struct Layout {
        double building_min = 0.0;   /**< @brief Smallest footprint side. */
        double building_max = 0.0;   /**< @brief Largest footprint side. */
        double street_offset = 0.0;  /**< @brief Centreline to plot centre, across the street. */
        double street_spacing = 0.0; /**< @brief Along the street, between plots. */
        double position_jitter = 0.0;/**< @brief Random offset applied to a street-front plot. */
        double water_clearance = 0.0;/**< @brief Clear ground kept between a building and water. */
        /** @brief Centreline to the nearest ground a building may stand on, across a lane. */
        double lane_clearance = 0.0;
        double rotation_jitter = 0.0;/**< @brief Radians of yaw wobble; already unit-free. */
        int infill_attempts = 0;     /**< @brief Rejection draws before infill gives up. */
    };

    /** @brief Builds the grid-space `Layout` for a configuration. */
    static Layout layout_for_(const MapConfig& config) {
        const TownConfig& towns = config.towns;
        Layout layout;
        layout.building_min = meters_to_grid(config, towns.building_size_min_m);
        layout.building_max = meters_to_grid(config, towns.building_size_max_m);
        layout.street_offset = meters_to_grid(config, towns.street_offset_m);
        layout.street_spacing = meters_to_grid(config, towns.street_spacing_m);
        layout.position_jitter = meters_to_grid(config, towns.position_jitter_m);
        layout.water_clearance = meters_to_grid(config, towns.water_clearance_m);
        layout.lane_clearance = meters_to_grid(config, towns.street_width_m) * 0.5
                              + meters_to_grid(config, towns.street_clearance_m);
        layout.rotation_jitter = towns.rotation_jitter;
        layout.infill_attempts = towns.infill_attempts;
        return layout;
    }

    /** @brief How many buildings a settlement of this tier may hold. */
    static int building_budget_(TownTier tier, const TownConfig& towns) {
        switch (tier) {
            case TownTier::Capital: return towns.buildings_per_town;
            case TownTier::Town:
                return static_cast<int>(towns.buildings_per_town * towns.town_building_scale);
            case TownTier::Village:
                return static_cast<int>(towns.buildings_per_town * towns.village_building_scale);
        }
        return towns.buildings_per_town;
    }

    /** @brief Places plots in pairs flanking each street, walking outward from the site. */
    void place_street_frontage_(const std::vector<MapPoint>& polygon,
                                const std::vector<MapStreet>& streets, const Layout& layout,
                                const KeepOut& keep_out, const MapPlaza& plaza, int budget,
                                std::mt19937& rng, std::vector<MapBuilding>& buildings) const {
        if (streets.empty() || layout.street_spacing <= 0.0) {
            return;
        }

        coopa::maps::UniformRealDistribution<double> offset_jitter(-layout.position_jitter,
                                                             layout.position_jitter);
        coopa::maps::UniformRealDistribution<double> yaw_jitter(-layout.rotation_jitter,
                                                          layout.rotation_jitter);
        coopa::maps::UniformRealDistribution<double> pick_size(layout.building_min,
                                                         layout.building_max);

        for (const MapStreet& street : streets) {
            const double along_x = std::cos(street.bearing);
            const double along_y = std::sin(street.bearing);
            const double across_x = -along_y;
            const double across_y = along_x;

            for (double t = layout.street_spacing; t < street.length;
                 t += layout.street_spacing) {
                for (const double side : {-1.0, 1.0}) {
                    if (static_cast<int>(buildings.size()) >= budget) {
                        return;
                    }
                    const double size = pick_size(rng);
                    const double yaw = yaw_jitter(rng);
                    MapBuilding candidate;
                    candidate.width = size;
                    candidate.height = size;
                    candidate.rotation = street.bearing + yaw;

                    // Set back by the plot's own reach across the lane, not by a
                    // flat distance. A square of side `s` turned by `yaw` relative
                    // to the street reaches `(s/2)(|cos| + |sin|)` across it, so a
                    // flat offset puts every plot larger than it expected onto the
                    // carriageway, the largest by about a metre. `street_offset`
                    // stays as the nominal setback, so a smaller plot still sits
                    // at it.
                    const double reach =
                        size * 0.5 * (std::abs(std::cos(yaw)) + std::abs(std::sin(yaw)));
                    const double offset =
                        std::max(layout.street_offset, layout.lane_clearance + reach)
                        + street.clearance;

                    candidate.point = {
                        street.from.x + along_x * t + across_x * side * offset
                            + offset_jitter(rng),
                        street.from.y + along_y * t + across_y * side * offset
                            + offset_jitter(rng)};

                    if (can_place_(polygon, buildings, keep_out, plaza, candidate)) {
                        buildings.push_back(candidate);
                    }
                }
            }
        }
    }

    /**
     * @brief Spends the remaining budget on jittered rejection sampling inside the cell.
     *
     * Aligned to the nearest street, not to a random bearing. A yaw drawn uniformly
     * from a full turn makes a settlement read as scatter however carefully the
     * frontage has been laid: half the buildings face nowhere, and a row is only
     * legible if its neighbours agree with it. Position stays jittered,
     * which is what keeps the layout off a lattice -- that is a property of where
     * buildings sit, not of which way they face.
     */
    void place_infill_(const std::vector<MapPoint>& polygon,
                       const std::vector<MapStreet>& streets, const Layout& layout,
                       const KeepOut& keep_out, const MapPlaza& plaza, int budget,
                       std::mt19937& rng, std::vector<MapBuilding>& buildings) const {
        if (static_cast<int>(buildings.size()) >= budget) {
            return;
        }

        double min_x = polygon.front().x, max_x = polygon.front().x;
        double min_y = polygon.front().y, max_y = polygon.front().y;
        for (const MapPoint& point : polygon) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        }

        coopa::maps::UniformRealDistribution<double> pick_x(min_x, max_x);
        coopa::maps::UniformRealDistribution<double> pick_y(min_y, max_y);
        coopa::maps::UniformRealDistribution<double> pick_yaw(0.0, k_two_pi);
        coopa::maps::UniformRealDistribution<double> yaw_jitter(-layout.rotation_jitter,
                                                          layout.rotation_jitter);
        coopa::maps::UniformRealDistribution<double> pick_size(layout.building_min,
                                                         layout.building_max);

        // Bounded: a cell too cramped to hold another building would otherwise
        // sample forever.
        for (int attempt = 0; attempt < layout.infill_attempts; ++attempt) {
            if (static_cast<int>(buildings.size()) >= budget) {
                return;
            }
            const double size = pick_size(rng);
            MapBuilding candidate;
            candidate.width = size;
            candidate.height = size;
            candidate.point = {pick_x(rng), pick_y(rng)};
            candidate.rotation = streets.empty()
                                     ? pick_yaw(rng)
                                     : nearest_bearing_(streets, candidate.point) + yaw_jitter(rng);

            if (can_place_(polygon, buildings, keep_out, plaza, candidate)) {
                buildings.push_back(candidate);
            }
        }
    }

    /**
     * @brief True when a footprint fits: inside the cell, clear of water, clear of neighbours.
     * @param polygon The cell outline.
     * @param buildings Already-placed footprints.
     * @param keep_out River and shoreline segments with their required clearances.
     * @param candidate The footprint to test.
     */
    bool can_place_(const std::vector<MapPoint>& polygon,
                    const std::vector<MapBuilding>& buildings,
                    const KeepOut& keep_out, const MapPlaza& plaza,
                    const MapBuilding& candidate) const {
        const std::array<MapPoint, 4> corners = building_corners(candidate);

        for (const MapPoint& corner : corners) {
            if (!point_in_polygon(polygon, corner)) {
                return false;
            }
        }
        if (plaza.radius > 0.0) {
            // The square is open ground. Testing every rotated corner rather than
            // the centre is what stops a building from leaning into it.
            for (const MapPoint& corner : corners) {
                if (plaza.centre.distance_to(corner) < plaza.radius) {
                    return false;
                }
            }
        }
        for (std::size_t i = 0; i < keep_out.segments.size(); ++i) {
            // Against the whole footprint, not its corners. A corner test misses a
            // lane that crosses the *middle* of a large plot -- all four corners
            // are then further from the centreline than the clearance while the
            // street runs straight through the building -- and misses a short road
            // segment lying wholly inside one. The corridor is a rotated box, and
            // box-against-box is exactly what `buildings_overlap()` already
            // decides, by separating axis.
            if (buildings_overlap(corridor_(keep_out.segments[i][0], keep_out.segments[i][1],
                                            keep_out.clearances[i]),
                                  candidate)) {
                return false;
            }
        }
        for (const MapBuilding& placed : buildings) {
            if (buildings_overlap(placed, candidate)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Bearing of the street a point sits nearest to.
     *
     * Nearest by distance to the *segment*, not to its ends: a plot beside the
     * middle of a lane fronts that lane, however far its junctions are.
     *
     * @param streets The cell's streets; must not be empty.
     * @param point The plot centre being oriented.
     * @return The bearing to face, in radians.
     */
    double nearest_bearing_(const std::vector<MapStreet>& streets, const MapPoint& point) const {
        double best = streets.front().bearing;
        double nearest = std::numeric_limits<double>::max();
        for (const MapStreet& street : streets) {
            const double distance = distance_to_segment_(point, street.from, street.to);
            if (distance < nearest) {
                nearest = distance;
                best = street.bearing;
            }
        }
        return best;
    }

    /**
     * @brief A keep-out segment as the rotated box its clearance sweeps out.
     *
     * Lets a corridor be tested against a footprint with the same separating-axis
     * routine two footprints use, rather than a second near-miss geometry written
     * specially for it.
     *
     * @param from One end of the segment.
     * @param to The other end.
     * @param clearance Half the corridor's width, in grid units.
     * @return A box covering the corridor; square ends, which under-covers the caps
     *         by less than a clearance and never over-covers.
     */
    static MapBuilding corridor_(const MapPoint& from, const MapPoint& to, double clearance) {
        MapBuilding box;
        box.point = {(from.x + to.x) * 0.5, (from.y + to.y) * 0.5};
        box.width = std::max(from.distance_to(to), 1e-9);
        box.height = std::max(clearance * 2.0, 1e-9);
        box.rotation = std::atan2(to.y - from.y, to.x - from.x);
        return box;
    }

    /** @brief Shortest distance from a point to a line segment. */
    static double distance_to_segment_(const MapPoint& point, const MapPoint& a,
                                       const MapPoint& b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length_squared = dx * dx + dy * dy;
        if (length_squared == 0.0) {
            return point.distance_to(a);
        }
        double t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared;
        t = std::clamp(t, 0.0, 1.0);
        return std::hypot(point.x - (a.x + t * dx), point.y - (a.y + t * dy));
    }

    /** @brief Lanes given to a cell with no road or river to build along. */
    static constexpr std::size_t k_fallback_streets = 2;

    /** @brief Full turn in radians, for the building yaw distribution. */
    static constexpr double k_two_pi = 6.283185307179586;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_TOWNS_H
