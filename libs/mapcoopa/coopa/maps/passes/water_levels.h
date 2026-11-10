/**
 * @file water_levels.h
 * @brief Gives every body of water one flat surface, at the height it would really stand, and a
 *        basin under it.
 *
 * Shared by the two passes that finish moving the ground: `PassElevation` sets the levels once
 * the height field exists, and `PassValleys` settles them again after carving, because a valley
 * cut through a lake's shore lowers the very rim the lake was levelled against.
 */

#ifndef COOPA_MAPS_PASSES_WATER_LEVELS_H
#define COOPA_MAPS_PASSES_WATER_LEVELS_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @brief How far a lake stands under the lowest point of its shore, in metres.
 *
 * Exactly at the rim, the lowest shore cell's site would sit *on* the water and the triangles
 * running away from it, down the outflow side, would read as wet ground outside the lake. A
 * metre under it keeps the whole shoreline strictly above the surface.
 */
inline constexpr double k_lake_spill_margin_m = 1.0;

/**
 * @brief How far each cell of one body of water lies from its shoreline, in grid units.
 *
 * A Dijkstra relaxation over site-to-site distances *inside* the body. Real distance rather than
 * a count of hops, so an elongated lake gets an elongated trough instead of a staircase of
 * terraces, and so the answer does not change meaning when `grid_size` changes the spacing of
 * the cells.
 *
 * Seeded from the **shoreline**, not from the shore cells: a cell touching dry land starts at
 * half the distance to that land's site, which is where the boundary between the two runs. Seeded
 * at the sites instead, every cell of a small lake -- where every cell touches land -- would read
 * zero and the body would come out flat however wide it was.
 *
 * A body entirely ringed by other water has no shoreline to seed from. Every cell then reads
 * zero, which is the only honest answer when there is no shore to slope away from.
 *
 * @param graph The graph; reads `centers`.
 * @param body The cells of one body of water, as a flood fill collected them.
 * @return One distance per entry of `body`, in the same order.
 */
inline std::vector<double> shoreline_distances(const MapGraph& graph, const std::vector<CenterId>& body) {
    const double infinity = std::numeric_limits<double>::max();
    std::vector<double> distance(body.size(), infinity);
    // The body is a handful of cells out of thousands, so it is indexed rather than walked: a
    // per-cell array over the whole graph would be allocated once per lake.
    std::unordered_map<CenterId, std::size_t> slot;
    slot.reserve(body.size() * 2);
    for (std::size_t i = 0; i < body.size(); ++i) {
        slot.emplace(body[i], i);
    }

    // Smallest distance first, so a cell is settled the first time it is popped.
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const MapCenter& cell = graph.centers[static_cast<std::size_t>(body[i])];
        double shore = infinity;
        for (const CenterId neighbor_id : cell.neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            if (!neighbor.water) {
                shore = std::min(shore, 0.5 * cell.point.distance_to(neighbor.point));
            }
        }
        if (shore < infinity) {
            distance[i] = shore;
            pending.emplace(shore, i);
        }
    }

    while (!pending.empty()) {
        const Entry entry = pending.top();
        pending.pop();
        if (entry.first > distance[entry.second]) {
            continue;   // already settled by a shorter path
        }
        const MapCenter& cell = graph.centers[static_cast<std::size_t>(body[entry.second])];
        for (const CenterId neighbor_id : cell.neighbors) {
            const auto found = slot.find(neighbor_id);
            if (found == slot.end()) {
                continue;   // outside this body
            }
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            const double reached = entry.first + cell.point.distance_to(neighbor.point);
            if (reached < distance[found->second]) {
                distance[found->second] = reached;
                pending.emplace(reached, found->second);
            }
        }
    }

    for (double& d : distance) {
        if (d == infinity) {
            d = 0.0;   // no dry shore anywhere in the body
        }
    }
    return distance;
}

/**
 * @brief The half-width, in grid units, a water connection to the open sea needs to read as open
 *        rather than enclosed -- see `ocean_enclosure()`.
 *
 * Grid units, like every other distance in this file: a finer `grid_size` resolves a finer
 * strait, which is a property of the graph's own resolution rather than something that needs
 * retuning per map. Scale-free in the one sense that matters here -- it depends on nothing about
 * where the strait sits on the map, only on how wide it is.
 */
inline constexpr double k_strait_half_width = 5.0;

/**
 * @brief How many more strait-widths past the open-sea core the enclosure strength takes to ramp
 *        from 0 to 1 -- see `ocean_enclosure()`. Keeps a bay's own mouth a smooth taper into its
 *        carved basin rather than a cliff where the two meet.
 */
inline constexpr double k_enclosure_fade = 2.0;

/**
 * @brief Distance from the nearest dry cell, over every water cell of the graph at once.
 *
 * The global form of `shoreline_distances()`, which only ever covers one flood-filled body: a
 * cell reads how far it stands into open water without first needing to know which lagoon or sea
 * it belongs to. Seeding and relaxation are otherwise identical -- see that function's doc for
 * why the seed is half the distance to the nearest dry neighbour rather than zero at the water's
 * own site.
 *
 * @param graph The graph; reads `centers`.
 * @return One distance per cell of `graph.centers`, 0 on dry land.
 */
inline std::vector<double> water_shore_distances(const MapGraph& graph) {
    const std::size_t n = graph.centers.size();
    const double infinity = std::numeric_limits<double>::max();
    std::vector<double> distance(n, infinity);
    using Entry = std::pair<double, CenterId>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
    for (const MapCenter& cell : graph.centers) {
        if (!cell.water) continue;
        double shore = infinity;
        for (const CenterId neighbor_id : cell.neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            if (!neighbor.water) shore = std::min(shore, 0.5 * cell.point.distance_to(neighbor.point));
        }
        if (shore < infinity) {
            distance[static_cast<std::size_t>(cell.index)] = shore;
            pending.emplace(shore, cell.index);
        }
    }
    while (!pending.empty()) {
        const Entry entry = pending.top();
        pending.pop();
        const std::size_t ci = static_cast<std::size_t>(entry.second);
        if (entry.first > distance[ci]) continue;   // already settled shorter
        const MapCenter& cell = graph.centers[ci];
        for (const CenterId neighbor_id : cell.neighbors) {
            const std::size_t ni = static_cast<std::size_t>(neighbor_id);
            if (!graph.centers[ni].water) continue;
            const double reached = entry.first + cell.point.distance_to(graph.centers[ni].point);
            if (reached < distance[ni]) {
                distance[ni] = reached;
                pending.emplace(reached, neighbor_id);
            }
        }
    }
    for (double& d : distance) {
        if (d == infinity) d = 0.0;   // dry land
    }
    return distance;
}

/**
 * @brief How enclosed each `ocean` cell is: 0 in the open sea, ramping to 1 behind a strait.
 *
 * `PassWater`'s flood fill gives the sea and every inlet off it the same `ocean` flag the instant
 * they touch through even one strait cell, so a lagoon a few cells across, a stone's throw from
 * the coast, is exactly as "ocean" as the open water a hundred cells out -- and
 * `assign_water_levels()` carves a basin under everything EXCEPT ocean, on the reasoning that the
 * open sea's bed is already the elevation pass's job. That reasoning fails for the lagoon: it has
 * no real relief of its own, just whatever rank the elevation remap happened to leave the cells
 * at, which near a shore is barely under `sea_level`.
 *
 * This is a morphological OPENING of the water mask -- erode, then reconstruct by dilating the
 * survivors back out within the original mask -- scale-free by construction, since every distance
 * here is measured in strait-widths and none of it cares where the strait sits on the map. (An
 * earlier version compared a cell's water-only path to the map's BORDER against the straight-line
 * distance to it, which could only ever flag a bay close to the edge: for a body tens of cells
 * inland, the detour through its strait is a small fraction of an already-long common journey, so
 * the ratio never climbed far enough to fire. A version after that used a cell's own local width,
 * `shore[]`, directly as its "enclosure" signal -- but a cell one unit from shore reads a small
 * `shore[]` whether it sits in a one-cell strait or hugs the edge of the open ocean, so that
 * punished the mere act of being near any coast. Distance from an ERODED core, not raw `shore[]`,
 * is what actually tells the two apart.)
 *
 *  1. `shore[]`: distance from the nearest dry cell, over water only (`water_shore_distances()`).
 *  2. ERODE: the open-sea CORE is every water cell `shore >= k_strait_half_width` -- a strip
 *     `k_strait_half_width` wide along every shore, narrow strait included, does not survive.
 *  3. RECONSTRUCT: flood the core back out, unrestricted, over every water cell (not just the
 *     wide ones) -- `core_dist[]`, true water-path distance to the nearest surviving core cell.
 *     This is what brings the open sea's own narrow shoreline band back (it sits within
 *     `k_strait_half_width` of a core cell just offshore), while a strait narrower than twice
 *     that half-width has no core cell near enough on the far side to reach back through it.
 *  4. The strength eases from 0 at the core to 1 over `k_enclosure_fade` more strait-widths, so a
 *     bay's own mouth ramps into its carved basin instead of snapping from flat to carved.
 *
 * A globe has no border and so no core to measure against; every ocean cell there reads 0, same
 * as before this existed -- a planet's seas keep the elevation pass's own bed.
 *
 * @param graph The graph; reads `centers`.
 * @return One strength in `[0, 1]` per cell of `graph.centers`, 0 off `ocean` water.
 */
inline std::vector<double> ocean_enclosure(const MapGraph& graph) {
    const std::size_t n = graph.centers.size();
    std::vector<double> strength(n, 0.0);

    bool any_border_water = false;
    for (const MapCenter& cell : graph.centers) {
        if (cell.border && cell.water) {
            any_border_water = true;
            break;
        }
    }
    if (!any_border_water) return strength;   // no border water at all -- a globe

    const std::vector<double> shore = water_shore_distances(graph);

    // Step 2 (erode): the open-sea core is simply every cell wide enough on its own.
    std::vector<bool> core(n, false);
    bool any_core = false;
    for (const MapCenter& cell : graph.centers) {
        if (!cell.water) continue;
        const std::size_t ci = static_cast<std::size_t>(cell.index);
        if (shore[ci] >= k_strait_half_width) {
            core[ci] = true;
            any_core = true;
        }
    }
    if (!any_core) return strength;   // no open water wide enough to anchor a core at all

    // Step 3 (reconstruct): flood from every core cell, unrestricted, over every water cell.
    const double infinity = std::numeric_limits<double>::max();
    std::vector<double> core_dist(n, infinity);
    using Entry = std::pair<double, CenterId>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
    for (std::size_t i = 0; i < n; ++i) {
        if (!core[i]) continue;
        core_dist[i] = 0.0;
        pending.emplace(0.0, graph.centers[i].index);
    }
    while (!pending.empty()) {
        const Entry entry = pending.top();
        pending.pop();
        const std::size_t ci = static_cast<std::size_t>(entry.second);
        if (entry.first > core_dist[ci]) continue;
        const MapCenter& cell = graph.centers[ci];
        for (const CenterId neighbor_id : cell.neighbors) {
            const std::size_t ni = static_cast<std::size_t>(neighbor_id);
            if (!graph.centers[ni].water) continue;
            const double reached = entry.first + cell.point.distance_to(graph.centers[ni].point);
            if (reached < core_dist[ni]) {
                core_dist[ni] = reached;
                pending.emplace(reached, neighbor_id);
            }
        }
    }

    // Step 4: ease out from the core. Never on a lake (ocean only) or the border itself, nor on a
    // cell with no path to any core cell at all -- every ocean cell is, by construction, water
    // connected to the border, but a border-less map (a globe) never reaches here because `core`
    // is empty and the early return above already fired; this is a guard against a malformed
    // graph, not a path this function's own callers can hit.
    const double fade = std::max(1e-9, k_strait_half_width * k_enclosure_fade);
    for (const MapCenter& cell : graph.centers) {
        const std::size_t ci = static_cast<std::size_t>(cell.index);
        if (!cell.water || !cell.ocean || cell.border || core_dist[ci] == infinity) continue;
        const double t = std::clamp((core_dist[ci] - k_strait_half_width) / fade, 0.0, 1.0);
        strength[ci] = t * t * (3.0 - 2.0 * t);   // smoothstep
    }
    return strength;
}

/**
 * @brief Which `ocean` cells are really a bay or lagoon rather than open sea -- see
 *        `ocean_enclosure()`, which this is the boolean reading of.
 *
 * @return One flag per cell of `graph.centers`, true wherever `ocean_enclosure()` reads nonzero.
 */
inline std::vector<bool> enclosed_ocean_cells(const MapGraph& graph) {
    const std::vector<double> strength = ocean_enclosure(graph);
    std::vector<bool> enclosed(strength.size(), false);
    for (std::size_t i = 0; i < strength.size(); ++i) enclosed[i] = strength[i] > 0.0;
    return enclosed;
}

/**
 * @brief Gives every body of water one flat surface height, and every lake a basin under it.
 *
 * `elevation` is the height of the *ground*, which under water is the bed. `water_level` is the
 * height of the *surface*, and a surface is flat -- which a bed is not. Stating them separately
 * is what lets a consumer flood a terrain mesh, and what lets the water layer draw a sheet
 * instead of a mottled field.
 *
 * - **The sea** takes `MapConfig::sea_level` everywhere, which after the elevation pass's split
 *   rank remap is exactly the height its bed rises to meet.
 * - **A lake** takes its **spill level**: the lowest cell on its shore, less
 *   `k_lake_spill_margin_m`. That is the height water poured into the basin would stop at, and
 *   the only one that keeps every shore above the water. (It used to be the highest bed in the
 *   body, which put a lake on a slope up to a hundred metres over the low side of its shore --
 *   water standing in the air.) Never below the sea it would otherwise drain into.
 * - **A lake's bed** becomes a basin: at least `MapConfig::lake_depth_m` under that surface
 *   anywhere, and deeper by `MapConfig::lake_bed_grade` for every metre in from the shoreline,
 *   to a floor of `MapConfig::lake_depth_max_m`. Lake cells are ranked with the land, so
 *   a cell the water pass flooded can carry a "bed" well up a hillside; without the lowering the
 *   ground would rise through its own lake, and with one flat depth for the whole body a
 *   kilometre-wide lake is a pan of exactly zero bed spread.
 *
 * Only cell heights move. Corners keep the heights the drainage was read from, and the surface
 * a consumer samples (`MapGraph::elevation_at()`) interpolates cells, so the basin shows there.
 * The finer relief over that basin is `NaturalTerrainConfig::seabed`, applied at sample time.
 *
 * @param graph The graph to annotate; requires cell elevations to be final.
 * @param config Supplies the waterline, the lake depths and the bed grade.
 */
inline void assign_water_levels(MapGraph& graph, const MapConfig& config) {
    for (MapCenter& center : graph.centers) {
        // Below the waterline the ground is sea bed whatever the water pass decided, so the
        // sea's surface is the waterline there too.
        center.water_level = (center.ocean || center.elevation < config.sea_level) ? config.sea_level : 0.0;
    }

    const double margin = meters_to_height(config, k_lake_spill_margin_m);
    const double least_depth = meters_to_height(config, std::max(0.0, config.lake_depth_m));
    const double max_depth = std::max(least_depth, meters_to_height(config, std::max(0.0, config.lake_depth_max_m)));
    // The grade is rise over run in real metres both ways; folding the horizontal scale in here
    // leaves the walk below free to work in the graph's own grid units.
    const double grade = meters_to_height(
        config, std::max(0.0, config.lake_bed_grade) * config.meters_per_grid_unit);
    std::vector<bool> visited(graph.centers.size(), false);

    for (MapCenter& seed : graph.centers) {
        const std::size_t seed_index = static_cast<std::size_t>(seed.index);
        if (!seed.water || seed.ocean || visited[seed_index]) {
            continue;
        }

        std::vector<CenterId> body;
        std::queue<CenterId> pending;
        pending.push(seed.index);
        visited[seed_index] = true;

        double rim = std::numeric_limits<double>::max();
        while (!pending.empty()) {
            const CenterId current = pending.front();
            pending.pop();
            body.push_back(current);

            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                const MapCenter& neighbor = graph.centers[index];
                if (!neighbor.water) {
                    rim = std::min(rim, neighbor.elevation);
                    continue;
                }
                if (visited[index] || neighbor.ocean) {
                    continue;
                }
                visited[index] = true;
                pending.push(neighbor_id);
            }
        }

        // A lake with no dry shore at all (a flooded pocket ringed by other water) has nothing
        // to spill over; its highest bed is the only height that keeps it out of the air.
        if (rim == std::numeric_limits<double>::max()) {
            rim = 0.0;
            for (const CenterId cell_id : body) {
                rim = std::max(rim, graph.centers[static_cast<std::size_t>(cell_id)].elevation + margin);
            }
        }
        const double surface = std::max(config.sea_level, rim - margin);
        const std::vector<double> from_shore = shoreline_distances(graph, body);
        for (std::size_t i = 0; i < body.size(); ++i) {
            MapCenter& cell = graph.centers[static_cast<std::size_t>(body[i])];
            const double depth = std::min(max_depth, least_depth + grade * from_shore[i]);
            cell.water_level = surface;
            // Never under the floor of the height field: a basin deeper than its surface stands
            // high would otherwise be clamped flat, which is what the grade is here to avoid.
            cell.elevation = std::max(0.0, std::min(cell.elevation, surface - depth));
        }
    }

    // A bay or lagoon reads `ocean` (PassWater floods that flag the instant it touches the sea
    // through even one strait cell) and so was skipped by the loop above, which trusts the open
    // sea's own bed to the elevation pass. `ocean_enclosure()` tells the two apart; give the
    // enclosed ones the same basin a lake gets, scaled by how enclosed they read, at the sea's
    // own flat `water_level` rather than a rim it has no shore of its own to spill over -- it is
    // still one surface with the open sea.
    const std::vector<double> enclosure = ocean_enclosure(graph);
    for (MapCenter& seed : graph.centers) {
        const std::size_t seed_index = static_cast<std::size_t>(seed.index);
        if (!seed.water || !seed.ocean || enclosure[seed_index] <= 0.0 || visited[seed_index]) {
            continue;
        }

        std::vector<CenterId> body;
        std::queue<CenterId> pending;
        pending.push(seed.index);
        visited[seed_index] = true;
        while (!pending.empty()) {
            const CenterId current = pending.front();
            pending.pop();
            body.push_back(current);
            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                const MapCenter& neighbor = graph.centers[index];
                if (!neighbor.water || !neighbor.ocean || enclosure[index] <= 0.0 || visited[index]) continue;
                visited[index] = true;
                pending.push(neighbor_id);
            }
        }

        const std::vector<double> from_shore = shoreline_distances(graph, body);
        for (std::size_t i = 0; i < body.size(); ++i) {
            const std::size_t index = static_cast<std::size_t>(body[i]);
            MapCenter& cell = graph.centers[index];
            const double depth = enclosure[index] * std::min(max_depth, least_depth + grade * from_shore[i]);
            // water_level is already config.sea_level from the top of this function -- the bay
            // stays one flat surface with the sea it opens onto; only its bed moves.
            cell.elevation = std::max(0.0, std::min(cell.elevation, cell.water_level - depth));
        }
    }
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_WATER_LEVELS_H
