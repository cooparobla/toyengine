/**
 * @file settlements_test.cpp
 * @brief The town pass at settlement scale: siting, the cells each tier claims, the streets it
 *        emits, its civic core, names and population bookkeeping.
 *
 * Individual building footprints (containment, overlap, clearances, sizes) are buildings_test.
 * Not tested: layout-quality percentages (how many buildings face a street, how irregular the
 * plot pattern looks) -- those are tuned measurements, not contract.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("settlements");

COOPA_TEST(towns_sit_on_habitable_land_and_stay_apart) {
    const MapGenerator& generator = shared_small_world();
    const MapGraph& graph = generator.graph();
    const MapConfig& config = generator.config();
    const TownConfig& towns = config.towns;

    ASSERT_TRUE(!graph.towns.empty());
    ASSERT_TRUE(static_cast<int>(graph.towns.size()) <= towns.town_count);

    for (std::size_t i = 0; i < graph.towns.size(); ++i) {
        const MapTown& town = graph.towns[i];
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        ASSERT_TRUE(!center.water);
        ASSERT_TRUE(!center.ocean);
        ASSERT_TRUE(!center.border);

        for (std::size_t j = i + 1; j < graph.towns.size(); ++j) {
            const double dx = graph.towns[j].point.x - town.point.x;
            const double dy = graph.towns[j].point.y - town.point.y;
            // Configured in metres, compared in grid units -- the two systems meet at
            // meters_to_grid(), and nowhere else.
            const double spacing = meters_to_grid(config, towns.min_spacing_m);
            ASSERT_TRUE(dx * dx + dy * dy >= spacing * spacing);
        }
    }
}

COOPA_TEST(settlements_claim_cells_by_tier) {
    MapConfig config = world_config(77);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const TownConfig& towns = config.towns;
    ASSERT_TRUE(!graph.towns.empty());

    std::map<CenterId, int> owner_count;
    int capital_buildings = 0;
    int village_buildings = 0;
    int capitals = 0;
    int villages = 0;

    for (const MapTown& town : graph.towns) {
        ASSERT_TRUE(!town.cells.empty());
        ASSERT_EQ(town.cells.front(), town.center);

        const int allowance = town.tier == TownTier::Capital ? towns.capital_cells
                            : town.tier == TownTier::Town    ? towns.town_cells
                                                             : towns.village_cells;
        ASSERT_TRUE(static_cast<int>(town.cells.size()) <= allowance);

        for (const CenterId cell_id : town.cells) {
            // A cell belongs to at most one settlement, so two neighbours never build on the
            // same ground.
            ++owner_count[cell_id];
            ASSERT_EQ(owner_count[cell_id], 1);
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            ASSERT_TRUE(!cell.water && !cell.ocean && !cell.border);
        }

        if (town.tier == TownTier::Capital) {
            ++capitals;
            capital_buildings += static_cast<int>(town.buildings.size());
        } else if (town.tier == TownTier::Village) {
            ++villages;
            village_buildings += static_cast<int>(town.buildings.size());
        }
    }

    // The tiers exist to be distinguishable. Confined to one cell they were not: a capital and a
    // village both filled the same ~3,600 m2 and looked alike.
    if (capitals > 0 && villages > 0) {
        const double capital_mean = static_cast<double>(capital_buildings) / capitals;
        const double village_mean = static_cast<double>(village_buildings) / villages;
        ASSERT_TRUE(capital_mean > village_mean * 1.5);
    }
}

/**
 * @brief A settlement emits the streets its buildings were laid out along.
 *
 * Streets kept private to the pass would be real enough to place plots against and invisible to
 * everyone else. A settlement whose streets nobody can see reads as a scatter no matter how
 * carefully it was arranged, so emitting them is part of the layout, not a side effect of it.
 */
COOPA_TEST(towns_emit_their_streets) {
    const MapGraph& graph = shared_world().graph();
    ASSERT_TRUE(!graph.towns.empty());

    std::size_t checked = 0;
    for (const MapTown& town : graph.towns) {
        // Every settlement gets streets: the pass falls back to lanes toward the cell's farthest
        // corners when no road or river reaches it.
        ASSERT_TRUE(!town.streets.empty());
        for (const MapStreet& street : town.streets) {
            ASSERT_TRUE(street.length > 0.0);
            ASSERT_TRUE(std::abs(street.length - street.from.distance_to(street.to)) < 1e-9);
            ASSERT_TRUE(std::abs(street.bearing
                                 - std::atan2(street.to.y - street.from.y,
                                              street.to.x - street.from.x)) < 1e-9);
            // The inner end is a claimed cell's own site, which is what makes the fans of a
            // multi-cell settlement meet rather than merely overlap.
            bool from_a_claimed_site = false;
            for (const CenterId cell_id : town.cells) {
                const MapPoint& site = graph.centers[static_cast<std::size_t>(cell_id)].point;
                from_a_claimed_site = from_a_claimed_site || street.from.distance_to(site) < 1e-9;
            }
            ASSERT_TRUE(from_a_claimed_site);
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief A settlement has a civic core, sized to what it is.
 *
 * Without roles every building is the same object, so a capital is a village with more squares
 * in it. Roles are what make the tiers different in kind rather than only in count -- and the
 * core sits at the heart, not scattered through the outskirts.
 */
COOPA_TEST(settlements_have_a_civic_core) {
    const MapGraph& graph = shared_world().graph();
    ASSERT_TRUE(!graph.towns.empty());

    std::size_t capitals = 0;
    for (const MapTown& town : graph.towns) {
        std::vector<std::size_t> per_role(k_building_role_count, 0);
        for (const MapBuilding& building : town.buildings) {
            ASSERT_TRUE(static_cast<std::size_t>(building.role) < k_building_role_count);
            ++per_role[static_cast<std::size_t>(building.role)];
        }
        // A settlement has one hall and one market, never two.
        for (const BuildingRole role : {BuildingRole::Hall, BuildingRole::Market,
                                        BuildingRole::Temple, BuildingRole::Well}) {
            ASSERT_TRUE(per_role[static_cast<std::size_t>(role)] <= 1);
        }
        if (town.tier == TownTier::Capital && !town.buildings.empty()) {
            ++capitals;
            ASSERT_TRUE(per_role[static_cast<std::size_t>(BuildingRole::Hall)] == 1);
            ASSERT_TRUE(per_role[static_cast<std::size_t>(BuildingRole::Market)] == 1);
        }
        // Most of a settlement is homes, whatever else it has.
        ASSERT_TRUE(per_role[static_cast<std::size_t>(BuildingRole::Dwelling)] * 2
                    >= town.buildings.size());

        // The core is central: every civic building is nearer the heart than the furthest
        // dwelling is.
        const MapPoint heart = town.plaza.radius > 0.0 ? town.plaza.centre : town.point;
        double furthest_dwelling = 0.0;
        for (const MapBuilding& building : town.buildings) {
            if (!is_civic_role(building.role)) {
                furthest_dwelling = std::max(furthest_dwelling, heart.distance_to(building.point));
            }
        }
        for (const MapBuilding& building : town.buildings) {
            if (is_civic_role(building.role)) {
                ASSERT_TRUE(heart.distance_to(building.point) <= furthest_dwelling);
            }
        }
    }
    ASSERT_TRUE(capitals > 0);
}

/** @brief Every role names itself, and an unknown name falls back to a dwelling. */
COOPA_TEST(building_role_names_round_trip) {
    for (std::size_t i = 0; i < k_building_role_count; ++i) {
        const BuildingRole role = static_cast<BuildingRole>(i);
        ASSERT_TRUE(building_role_from_name(building_role_name(role)) == role);
    }
    ASSERT_TRUE(building_role_from_name("dwelling") == BuildingRole::Dwelling);
    ASSERT_TRUE(building_role_from_name("not_a_role") == BuildingRole::Dwelling);
    // Only a dwelling is not part of the civic core.
    ASSERT_TRUE(!is_civic_role(BuildingRole::Dwelling));
    ASSERT_TRUE(is_civic_role(BuildingRole::Hall));
}

COOPA_TEST(names_are_unique_and_reproducible) {
    // Regenerated rather than shared: the point is that a second, independent run agrees.
    const MapGraph& first = shared_world().graph();
    MapGenerator second(world_config(), maps_logger());
    second.generate();

    ASSERT_EQ(first.towns.size(), second.graph().towns.size());
    std::vector<std::string> names;
    for (std::size_t i = 0; i < first.towns.size(); ++i) {
        const std::string& name = first.towns[i].name;
        ASSERT_TRUE(!name.empty());
        ASSERT_TRUE(name == second.graph().towns[i].name);
        names.push_back(name);
    }
    ASSERT_TRUE(!names.empty());

    std::sort(names.begin(), names.end());
    ASSERT_TRUE(std::adjacent_find(names.begin(), names.end()) == names.end());

    ASSERT_EQ(first.regions.size(), second.graph().regions.size());
    for (std::size_t i = 0; i < first.regions.size(); ++i) {
        ASSERT_TRUE(first.regions[i].name == second.graph().regions[i].name);
    }
}

COOPA_TEST(population_scales_with_buildings) {
    const MapGenerator& generator = shared_world();
    const MapGraph& graph = generator.graph();

    int capital_population = 0;
    int village_population = 0;
    std::size_t villages = 0;
    for (const MapTown& town : graph.towns) {
        ASSERT_EQ(town.households, static_cast<int>(town.buildings.size()));
        if (town.households > 0) {
            ASSERT_TRUE(town.population > 0);
            // Population is counted from dwellings, so it cannot exceed the most each could
            // possibly hold.
            const double ceiling = town.households * generator.config().towns.household_size_max
                                 * generator.config().towns.capital_density;
            ASSERT_TRUE(town.population <= static_cast<int>(ceiling) + 1);
        }
        if (town.tier == TownTier::Capital) {
            capital_population = std::max(capital_population, town.population);
        } else if (town.tier == TownTier::Village) {
            village_population += town.population;
            ++villages;
        }
    }
    ASSERT_TRUE(villages > 0);
    // A capital that a village outgrows means the tier hierarchy is decorative.
    ASSERT_TRUE(capital_population > static_cast<int>(village_population / static_cast<int>(villages)));

    // Region totals are the sum of their towns: nobody is counted twice or dropped.
    int region_total = 0;
    for (const MapRegion& region : graph.regions) {
        ASSERT_TRUE(region.population >= 0);
        region_total += region.population;
    }
    int town_total = 0;
    for (const MapTown& town : graph.towns) {
        if (town.region != k_invalid_id) {
            town_total += town.population;
        }
    }
    ASSERT_EQ(region_total, town_total);
}
