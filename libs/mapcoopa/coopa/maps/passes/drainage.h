/**
 * @file drainage.h
 * @brief The two invariants a height field must satisfy for water to run off it:
 *        no closed basin on dry land, and every corner pointing downhill.
 */

#ifndef COOPA_MAPS_PASSES_DRAINAGE_H
#define COOPA_MAPS_PASSES_DRAINAGE_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @brief How far above its outlet a filled corner is raised.
 *
 * What buys strictness rather than a flat spillway, and deliberately tiny:
 * chains run a few hundred corners at most, so the total rise is on the order of
 * 1e-4 of the height range -- well under a tenth of a millimetre at the default
 * 600 m.
 */
inline constexpr double k_fill_epsilon = 1e-7;

/**
 * @brief Raises closed basins until every land corner drains to water.
 *
 * The distance-from-coast field is monotone, but nothing after it is: relief
 * noise, the rank remap and the smoothing passes all move corners
 * independently, and any of them can leave a corner lower than every
 * neighbour. Such a corner is a pit, and a downhill walk that reaches one
 * stops on dry land. Unfilled, a default map has 80 of 11 438 land corners as
 * pits, enough to end 23 of 55 rivers in the middle of a field.
 *
 * This is the priority-flood fill (Barnes, Lehman & Mulla 2014), which is
 * what DEM processing uses for the same problem. Every corner already at or
 * on water seeds a min-heap; popping the lowest unresolved corner and
 * raising each of its dry neighbours to just above it walks the terrain
 * outward from the sea in ascending order of the height water would have to
 * reach to get there. Each corner is therefore resolved *from* a strictly
 * lower one, and following that chain backwards is a descending path to
 * water. Since steepest descent from any corner also strictly descends, and
 * only a seed can have no lower neighbour, every land corner now drains.
 *
 * The epsilon is what buys strictness rather than a flat spillway, and it is
 * deliberately tiny: chains run a few hundred corners at most, so the total
 * rise is on the order of 1e-4 of the height range -- well under a tenth of
 * a millimetre at the default 600 m. Filling a pit is not a visible change
 * to the terrain; it is the difference between a puddle and a river mouth.
 *
 * Lake corners seed the queue alongside the sea, so an inflow that reaches a
 * lake has arrived and the lake bed is never filled in to force the water
 * onward. That is also what keeps an endorheic basin a lake instead of a
 * river running over its rim.
 *
 * @param graph The graph to fill; reads `adjacent` and rewrites `elevation`.
 */
inline void fill_depressions(MapGraph& graph) {
    if (graph.corners.empty()) {
        return;
    }

    // (elevation, corner), so the heap orders on height and ties break on a
    // stable index rather than on whatever the allocator handed back.
    using Entry = std::pair<double, CornerId>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
    std::vector<bool> resolved(graph.corners.size(), false);

    for (const MapCorner& corner : graph.corners) {
        if (!corner.water && !corner.coast) {
            continue;
        }
        resolved[static_cast<std::size_t>(corner.index)] = true;
        pending.emplace(corner.elevation, corner.index);
    }

    while (!pending.empty()) {
        const Entry entry = pending.top();
        pending.pop();
        const MapCorner& corner = graph.corners[static_cast<std::size_t>(entry.second)];
        for (const CornerId neighbor_id : corner.adjacent) {
            const std::size_t index = static_cast<std::size_t>(neighbor_id);
            if (resolved[index]) {
                continue;
            }
            MapCorner& neighbor = graph.corners[index];
            neighbor.elevation = std::max(neighbor.elevation, entry.first + k_fill_epsilon);
            resolved[index] = true;
            pending.emplace(neighbor.elevation, neighbor.index);
        }
    }
}

/**
 * @brief Points each corner at its lowest neighbour, or at itself in a basin.
 *
 * The comparison is strictly `<`, not `<=`. An equal-height neighbour is not
 * downhill, and accepting one lets two corners at exactly the same elevation
 * name each other as their downslope -- a two-cycle that a flow walk follows
 * until it hits its step guard. After `fill_depressions()` no such tie exists
 * along a flow path anyway, but the strict test is what makes "points at
 * itself" mean "has nowhere lower to go" rather than "happens to have been
 * scanned last".
 *
 * @param graph The graph to point; reads `elevation` and rewrites `downslope`.
 */
inline void assign_downslopes(MapGraph& graph) {
    for (MapCorner& corner : graph.corners) {
        CornerId lowest = corner.index;
        double lowest_elevation = corner.elevation;
        for (const CornerId neighbor_id : corner.adjacent) {
            const MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
            if (neighbor.elevation < lowest_elevation) {
                lowest = neighbor.index;
                lowest_elevation = neighbor.elevation;
            }
        }
        corner.downslope = lowest;
    }
}

/**
 * @brief Restores both drainage invariants after something has moved the terrain.
 *
 * Anything that rewrites corner heights owes the map these two: the height field
 * is where "water has somewhere to go" is stated, so a pass that lowers the
 * ground and stops there leaves a `downslope` graph describing terrain that no
 * longer exists, and basins that water would sit in forever. `PassElevation`
 * runs them as the last thing it does to the corner field; `PassValleys` runs
 * them again because carving the valleys moved it.
 *
 * @param graph The graph to repair.
 */
inline void restore_drainage(MapGraph& graph) {
    fill_depressions(graph);
    assign_downslopes(graph);
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_DRAINAGE_H
