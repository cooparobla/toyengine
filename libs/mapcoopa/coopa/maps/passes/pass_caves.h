/**
 * @file pass_caves.h
 * @brief Opens cave systems on the map's steepest slopes and grows them down
 *        into the rock beneath it.
 */

#ifndef COOPA_MAPS_PASSES_PASS_CAVES_H
#define COOPA_MAPS_PASSES_PASS_CAVES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/cave.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/name_generator.h>
#include <coopa/maps/noise.h>
#include <coopa/maps/portable_random.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassCaves
 * @brief Finds the slopes a cave would open on and grows a system down from each.
 *
 * The first feature in the module that exists *below* the ground rather than on
 * it, and the three decisions that make it work are all about staying honest
 * about that.
 *
 * **Where a cave opens.** A cave mouth is a hole in a face of rock, so it belongs
 * on the map's steepest ground -- which in a Voronoi map is an edge with a large
 * height difference across it, and an edge is already the thing that sits
 * *between* two cells. `edge_grade()` ranks them, and the rest of placement is the
 * greedy accept-with-spacing every other placement pass here uses. The mouth sits
 * at the edge's midpoint and the first passage heads from the lower cell toward
 * the higher one, because a cave mouth is something you walk *into* the hill
 * through.
 *
 * **What shape it grows into.** Two regimes, which is what limestone actually
 * does. Above the water table water is falling under gravity: the passage cuts
 * steeply down, stays narrow, branches rarely, and now and then drops a vertical
 * pitch. At and below the water table the water is moving sideways through rock
 * already full of it and attacks every joint it meets, so the passage runs level,
 * opens out and branches freely. One system therefore reads as an entrance series
 * leading to a level network rather than as a uniform tree, and every knob in
 * `CaveConfig` means something physical instead of being a tuning number.
 *
 * **Why it has storeys.** A water table is not fixed. The valley a system drains
 * to cuts down over time and the table follows it, which abandons the network cut
 * at the higher level -- leaving it as dry passage -- and starts a new one below.
 * `level_tables_()` works out how many stages a mouth has the relief to support,
 * and growth then runs the same two-regime model once per stage, joining each to
 * the next with the vadose descent that is what a shaft between levels *is*.
 *
 * That is the whole reason the exported floor and roof layers come in pairs. A
 * system cut at one table is a sheet: it can be drawn on a single heightmap and
 * a second adds nothing. Stack three tables `level_spacing_m` apart and the
 * structure stops being expressible in two dimensions, which is the point. The
 * spacing has to stay clear of `chamber_height_m` or the levels touch where they
 * cross and the export merges them back into the sheet they came from.
 *
 * **That it stays underground.** Every station is clamped to
 * `CaveConfig::roof_clearance_m` beneath the terrain, and the clamp is applied
 * *twice*: once as the station is grown, and again on every point of the smoothed
 * passage, because corner-cutting moves points and the smoothed path is the one
 * that gets drawn. The surface it clamps against is the one
 * `MapGraph::elevation_at()` returns with terrain detail and river channels
 * applied -- not the bare control mesh. That distinction is not pedantry:
 * `river_channel_depth_m` alone cuts 18 m out of the drawn surface, so a cave
 * given 25 m of clearance against the mesh has 7 m of rock over it where it
 * crosses under a river, and less than none at a higher `--channel`.
 *
 * Nothing downstream reads caves, so this runs late -- it only needs the
 * *finished* height field, which means after the valley pass, and regions, so it
 * can name itself in the local dialect.
 */
class PassCaves {
public:
    /**
     * @brief Opens and grows every cave system.
     * @param graph The graph to annotate; reads `edges` and `centers`, writes `caves`.
     * @param config Supplies `CaveConfig`, the noise field and both world scales.
     * @param logger Receives one line naming the pass and one summarising the result.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: caves");

        graph.caves.clear();
        dialects_.clear();
        taken_names_.clear();
        seed_ = config.seed;

        const CaveConfig& caves = config.caves;
        if (caves.cave_count <= 0 || graph.edges.empty() || graph.centers.empty()) {
            return;
        }

        // The surface a cave is held beneath is the one a reader sees, so it is
        // sampled with the same detail and channels the renderer applies. Built
        // once for the whole pass: `Noise` wraps a FastNoiseLite and the channel
        // index walks every river.
        const TerrainSurface surface(graph, config);
        const TerrainDetail& detail = surface.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const Noise wander(config.noise_cave);

        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_seed_offset);

        std::size_t stations = 0;
        std::size_t deepest_stack = 0;
        double total_length = 0.0;
        for (const EdgeId mouth : choose_mouths_(graph, config)) {
            MapCave cave = grow_(graph, config, mouth, detail, channels, wander, rng);
            if (static_cast<int>(cave.nodes.size()) < k_min_nodes) {
                // Too little rock under the slope to hold anything worth calling a
                // cave. Discarded rather than emitted as a two-station stub, the
                // same way the river pass rejects a trickle.
                continue;
            }
            cave.name = name_for_(graph, cave, rng);
            stations += cave.nodes.size();
            deepest_stack = std::max(deepest_stack, cave.levels.size());
            total_length += cave.length_m;
            graph.caves.push_back(std::move(cave));
        }

        const double mean_depth =
            graph.caves.empty() ? 0.0 : mean_depth_metres_(graph.caves, config);
        logger.info("map pass: caves opened " + std::to_string(graph.caves.size()) + " of "
                    + std::to_string(caves.cave_count) + " requested, " + std::to_string(stations)
                    + " stations, " + std::to_string(static_cast<int>(total_length))
                    + " m of passage, mean depth " + std::to_string(static_cast<int>(mean_depth))
                    + " m, up to " + std::to_string(deepest_stack) + " levels");
    }

private:
    /** @brief Offset from `MapConfig::seed` so this pass does not share a stream with the others. */
    static constexpr unsigned int k_seed_offset = 49979687u;
    /** @brief Stations a system needs before it is worth recording at all. */
    static constexpr int k_min_nodes = 4;
    /** @brief Largest heading change one station may make, in radians, at `meander` 1. */
    static constexpr double k_max_turn = 0.55;
    /** @brief How far to either side a station probes the surface, in radians. */
    static constexpr double k_probe_angle = 0.90;
    /** @brief Multiple of the needed headroom below which a passage starts steering. */
    static constexpr double k_lean_margin = 2.0;
    /** @brief Fraction of a normal step a vertical pitch advances in plan. */
    static constexpr double k_shaft_advance = 0.15;
    /** @brief Grade a passage keeps below the water table; not level, but nearly. */
    static constexpr double k_phreatic_grade = 0.02;
    /** @brief Smallest angle a branch leaves its parent at, in radians. */
    static constexpr double k_branch_min_angle = 0.70;
    /** @brief Extra angle a branch may take beyond `k_branch_min_angle`, in radians. */
    static constexpr double k_branch_spread = 0.70;
    /** @brief How close a floor must be to the water table to count as having reached it. */
    static constexpr double k_zone_epsilon = 1e-6;
    /** @brief Hops the cell walk may take before giving up; a step is under one cell. */
    static constexpr int k_locate_hops = 8;
    /** @brief Probes taken around a passage's rim to find the lowest ground over it. */
    static constexpr int k_rim_probes = 8;
    /** @brief One full turn in radians, for walking those probes around. */
    static constexpr double k_turn = 6.283185307179586;
    /** @brief Redraws a colliding name gets before an ordinal is appended. */
    static constexpr int k_name_attempts = 8;
    /**
     * @brief Ways down a system opens to each of its lower levels.
     *
     * Two rather than one, so a lower storey is a network reached from two places
     * instead of a cul-de-sac hanging off a single shaft; and two rather than
     * many, because every descent spends `level_budget` of a trunk and the node
     * cap is shared across the whole system.
     */
    static constexpr int k_descents_per_level = 2;

    /** @brief A dialect per region, built on demand; a cave is named where it opens. */
    mutable std::unordered_map<RegionId, Language> dialects_;
    /** @brief Names already given out, so two caves are never confused for one. */
    mutable std::unordered_set<std::string> taken_names_;
    /** @brief The map seed, for deriving a region's dialect. */
    mutable int seed_ = 0;

    /** @brief One growing end of a system, and everything needed to carry it forward. */
    struct Head {
        MapPoint point;              /**< @brief Where the next station will be measured from. */
        double floor = 0.0;          /**< @brief Floor height it continues from. */
        double bearing = 0.0;        /**< @brief Current heading, in radians. */
        double budget = 0.0;         /**< @brief Length left to spend, in grid units. */
        double phase = 0.0;          /**< @brief Offset into the wander field, so two heads differ. */
        std::int32_t parent = -1;    /**< @brief Station this head continues from. */
        /** @brief Index into `MapCave::levels` of the table this head works to. */
        std::int32_t level = 0;
    };

    /**
     * @brief Ranks the map's slopes and takes the steepest, spaced apart.
     *
     * Both cells have to be dry, unclaimed land: a mouth on a water cell is a hole
     * in a lake bed, and one on the border band is a hole in the frame of the map.
     * Ties break on `EdgeId` so the same configuration always chooses the same
     * slopes, which is the discipline every ranked pass here keeps.
     *
     * @param graph The generated graph.
     * @param config Supplies the grade threshold, the spacing and the count.
     * @return The chosen edges, steepest first.
     */
    std::vector<EdgeId> choose_mouths_(const MapGraph& graph, const MapConfig& config) const {
        const CaveConfig& caves = config.caves;

        std::vector<double> grades(graph.edges.size(), 0.0);
        std::vector<EdgeId> candidates;
        for (const MapEdge& edge : graph.edges) {
            if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
                continue;
            }
            const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
            const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
            if (a.water || a.ocean || a.border || b.water || b.ocean || b.border) {
                continue;
            }
            const double grade = edge_grade(graph, edge, config);
            if (grade < caves.min_grade) {
                continue;
            }
            grades[static_cast<std::size_t>(edge.index)] = grade;
            candidates.push_back(edge.index);
        }

        coopa::maps::sort(candidates.begin(), candidates.end(), [&grades](EdgeId a, EdgeId b) {
            const double ga = grades[static_cast<std::size_t>(a)];
            const double gb = grades[static_cast<std::size_t>(b)];
            if (ga != gb) {
                return ga > gb;
            }
            return a < b; // Stable against ties, so a run is reproducible.
        });

        const double spacing = meters_to_grid(config, caves.min_spacing_m);
        const double spacing_squared = spacing * spacing;
        std::vector<EdgeId> chosen;
        for (const EdgeId candidate : candidates) {
            if (static_cast<int>(chosen.size()) >= caves.cave_count) {
                break;
            }
            const MapPoint& here = graph.edges[static_cast<std::size_t>(candidate)].midpoint;
            bool clear = true;
            for (const EdgeId taken : chosen) {
                const MapPoint& other = graph.edges[static_cast<std::size_t>(taken)].midpoint;
                const double dx = here.x - other.x;
                const double dy = here.y - other.y;
                const double dz = here.z - other.z;   // 0 on a plane map
                if (dx * dx + dy * dy + dz * dz < spacing_squared) {
                    clear = false;
                    break;
                }
            }
            if (clear) {
                chosen.push_back(candidate);
            }
        }
        return chosen;
    }

    /**
     * @brief Grows one system from one slope.
     *
     * Heads are processed in the order they were created and may push more as they
     * go, so the work list is walked by index rather than popped -- breadth-first
     * over the branches, which spends the station budget across the whole system
     * instead of exhausting it down the first passage.
     *
     * @param graph The generated graph.
     * @param config Supplies every cave parameter and both scales.
     * @param mouth_edge The slope to open on.
     * @param detail Terrain displacement, as the renderer applies it.
     * @param channels River channels, as the renderer cuts them.
     * @param wander The field passages take their meander from.
     * @param rng The pass's stream, for branch rolls and pitches.
     * @return The system, which may be too small to keep.
     */
    MapCave grow_(const MapGraph& graph, const MapConfig& config, EdgeId mouth_edge,
                  const TerrainDetail& detail, const RiverChannels& channels, const Noise& wander,
                  std::mt19937& rng) const {
        const CaveConfig& caves = config.caves;
        const MapEdge& edge = graph.edges[static_cast<std::size_t>(mouth_edge)];
        const MapCenter& d0 = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& d1 = graph.centers[static_cast<std::size_t>(edge.d1)];
        const MapCenter& low = d0.elevation <= d1.elevation ? d0 : d1;
        const MapCenter& high = d0.elevation <= d1.elevation ? d1 : d0;

        const double clearance = meters_to_height(config, caves.roof_clearance_m);
        const double passage_height = meters_to_height(config, caves.passage_height_m);
        const double chamber_height = meters_to_height(config, caves.chamber_height_m);
        const double shaft_drop = meters_to_height(config, caves.shaft_drop_m);
        const double step = meters_to_grid(config, caves.step_m);
        // A grade is rise over run in *metres*, so the drop it implies has to be
        // measured in metres and then converted -- multiplying a grade by a length
        // already in grid units mixes the horizontal scale into the vertical one,
        // and at the defaults that is a 3.5-in-1 descent rather than a 0.35 one.
        const double descent_drop = meters_to_height(config, caves.step_m * caves.descent_grade);
        const double phreatic_drop = meters_to_height(config, caves.step_m * k_phreatic_grade);
        const double passage_radius = meters_to_grid(config, caves.passage_width_m) * 0.5;
        const double chamber_radius = meters_to_grid(config, caves.chamber_radius_m);

        MapCave cave;
        cave.mouth_edge = edge.index;
        cave.mouth = edge.midpoint;
        cave.mouth_grade = edge_grade(graph, edge, config);
        cave.surface_at_mouth = graph.elevation_at(low, cave.mouth, detail, channels);
        cave.region = low.region;

        const double mouth_floor = cave.surface_at_mouth - clearance - passage_height;
        cave.levels = level_tables_(config, mouth_floor);
        // The shallowest table is where the entrance series stops falling, which is
        // what `phreatic_level` has always meant; the rest of the sequence is the
        // storeys beneath it.
        cave.phreatic_level = cave.levels.front();

        CaveNode root;
        root.point = cave.mouth;
        root.floor = mouth_floor;
        root.roof = mouth_floor + passage_height;
        root.radius = passage_radius;
        root.center = low.index;
        root.zone = zone_of_(mouth_floor, cave.phreatic_level);
        root.feature = CaveFeature::Passage;
        root.level = 0;
        root.parent = -1;
        cave.nodes.push_back(root);
        cave.deepest = mouth_floor;

        coopa::maps::UniformRealDistribution<double> unit(0.0, 1.0);
        coopa::maps::UniformRealDistribution<double> phase(-64.0, 64.0);

        const double trunk_budget = meters_to_grid(config, caves.passage_length_m);
        // What it costs to climb from one table to the next at the descent grade.
        // Handed to a descent on top of its network budget rather than taken out of
        // it -- see `CaveConfig::level_budget`.
        const double descent_run =
            caves.descent_grade > 0.0
                ? meters_to_grid(config, caves.level_spacing_m / caves.descent_grade)
                : 0.0;

        std::vector<Head> heads;
        heads.push_back(Head{cave.mouth, mouth_floor,
                             graph.bearing_between(low.point, high.point),
                             trunk_budget, phase(rng), 0, 0});

        const double limit = static_cast<double>(config.grid_size);
        int branches = 0;
        // Ways down opened to each level so far, so one storey cannot be reached
        // by every run that happens to finish at the table above it.
        std::vector<int> descents(cave.levels.size(), 0);
        std::vector<std::vector<std::int32_t>> runs;

        for (std::size_t h = 0; h < heads.size(); ++h) {
            Head head = heads[h];
            // The table this head works to. Everything the loop below branches on
            // is measured against it rather than against the system, which is what
            // lets one storey run level while the next is still being descended to.
            const double table = cave.levels[static_cast<std::size_t>(head.level)];
            std::vector<std::int32_t> chain{head.parent};
            // Whether the head was stopped by the rock rather than by its own
            // budget. The two end in different places and deserve different names:
            // a run that simply finished is a chamber, one that was cut short by
            // the water table or by ground falling away is a sump.
            bool cut_short = false;

            while (head.budget > 0.0 && static_cast<int>(cave.nodes.size()) < caves.max_nodes) {
                const bool phreatic = head.floor <= table + k_zone_epsilon;

                head.bearing += caves.meander * k_max_turn
                                * static_cast<double>(graph.is_sphere()
                                                          ? wander.sample(head.point.x + head.phase,
                                                                          head.point.y - head.phase,
                                                                          head.point.z + head.phase)
                                                          : wander.sample(head.point.x + head.phase,
                                                                          head.point.y - head.phase));
                head.bearing += massif_lean_(
                    graph, config, head,
                    cave.nodes[static_cast<std::size_t>(head.parent)].center, step,
                    clearance + passage_height, detail, channels);

                // A pitch is water falling rather than flowing, so it belongs only
                // above the water table; below it there is nothing to fall through.
                const bool shaft = !phreatic && unit(rng) < caves.shaft_chance;
                const double advance = shaft ? step * k_shaft_advance : step;
                const double drop = shaft      ? shaft_drop
                                    : phreatic ? phreatic_drop * unit(rng)
                                               : descent_drop;

                const MapPoint next = graph.step_from(head.point, head.bearing, advance);
                // A globe has no edge to run off.
                if (!graph.is_sphere() && (next.x < 0.0 || next.y < 0.0 || next.x > limit || next.y > limit)) {
                    cut_short = true;
                    break;
                }
                const CenterId cell = locate_(graph, next, cave.nodes[static_cast<std::size_t>(
                                                               head.parent)].center);
                const MapCenter& center = graph.centers[static_cast<std::size_t>(cell)];
                if (center.water || center.ocean || center.border) {
                    // Under a lake bed or off the edge of the world. Both are places
                    // a passage stops rather than places it may pass through.
                    cut_short = true;
                    break;
                }

                // The branch roll happens *before* the station is settled, because a
                // junction is a chamber and a chamber has headroom a passage does
                // not -- deciding afterwards would settle the roof at the wrong
                // height and then widen the room into it.
                const bool junction =
                    branches < caves.max_branches && head.budget > advance * 2.0
                    && static_cast<int>(cave.nodes.size()) + 1 < caves.max_nodes
                    && unit(rng) < (phreatic ? caves.branch_chance_phreatic
                                             : caves.branch_chance_vadose);

                const double height = junction ? chamber_height : passage_height;
                const double radius = junction ? chamber_radius : passage_radius;
                double floor = std::max(head.floor - drop, table);
                double roof = floor + height;
                const double surface =
                    lowest_surface_(graph, next, radius, cell, detail, channels);
                if (!settle_(surface, clearance, height, table, floor, roof)) {
                    // The rock ran out: either the ground above dropped faster than
                    // the passage could, or the passage has arrived at the water it
                    // drains to. Both end the head here rather than being allowed to
                    // surface.
                    cut_short = true;
                    break;
                }

                CaveNode node;
                node.point = next;
                node.floor = floor;
                node.roof = roof;
                node.radius = radius;
                node.center = cell;
                node.zone = zone_of_(floor, table);
                node.feature = shaft      ? CaveFeature::Shaft
                               : junction ? CaveFeature::Chamber
                                          : CaveFeature::Passage;
                node.level = head.level;
                node.parent = head.parent;

                const std::int32_t index = static_cast<std::int32_t>(cave.nodes.size());
                cave.nodes.push_back(node);
                chain.push_back(index);
                cave.deepest = std::min(cave.deepest, floor);

                if (junction) {
                    ++branches;
                    Head branch;
                    branch.point = next;
                    branch.floor = floor;
                    branch.bearing = head.bearing
                                     + (unit(rng) < 0.5 ? 1.0 : -1.0)
                                           * (k_branch_min_angle + k_branch_spread * unit(rng));
                    branch.budget = head.budget * caves.branch_budget;
                    branch.phase = phase(rng);
                    branch.parent = index;
                    branch.level = head.level; // A branch stays on its own storey.
                    heads.push_back(branch);
                }

                head.point = next;
                head.floor = floor;
                head.parent = index;
                head.budget -= advance;
            }

            if (chain.size() > 1) {
                // A passage ends in a space rather than at a point: the rock has been
                // worked at from one direction for as long as the water ran, and
                // there is nowhere for it to go on to.
                const std::int32_t tail = chain.back();
                CaveNode& last = cave.nodes[static_cast<std::size_t>(tail)];
                if (last.feature == CaveFeature::Passage) {
                    last.feature = cut_short ? CaveFeature::Sump : CaveFeature::Chamber;
                    last.radius = chamber_radius;
                }
                runs.push_back(std::move(chain));
                open_descent_(cave, caves, heads, descents, tail, head,
                              trunk_budget * caves.level_budget + descent_run, rng);
            }
        }

        for (const std::vector<std::int32_t>& run : runs) {
            cave.passages.push_back(
                finish_passage_(graph, config, cave, run, detail, channels, cave.levels.back()));
        }
        cave.length_m = measure_(cave, config);
        return cave;
    }

    /**
     * @brief Every water table a mouth has the relief to have been cut at.
     *
     * A cave is multi-level because its base level fell. The valley it drains to
     * cut down, the water table followed, and the phreatic network standing at the
     * old level was abandoned and left dry above the new one. Each entry returned
     * here is one of those stages, shallowest first.
     *
     * The sequence is anchored at the *bottom* rather than the top, which keeps
     * `vadose_share` and `max_depth_m` meaning the same thing whatever the number
     * of levels: the deepest table sits where a single table would, so a system
     * bottoms out where the configuration says it may. The abandoned levels
     * are then stacked upward from it at `level_spacing_m`, as many as fit while
     * leaving at least one spacing of entrance series between the mouth and the
     * shallowest of them -- a cave whose first table is immediately under its own
     * mouth has no way in.
     *
     * Anchoring at the top instead would have split the same relief between the
     * entrance series and the storeys, so raising `level_spacing_m` would have made
     * caves shallower and the two knobs would not have been independent.
     *
     * Always at least one table, so a mouth with no relief to spend -- one at the
     * coast -- still grows the single level network it would have grown before.
     *
     * @param config Supplies the spacing, the depth cap, the share and the sea.
     * @param mouth_floor Floor height of the first station, which the drop is from.
     * @return The tables, shallowest first; never empty.
     */
    static std::vector<double> level_tables_(const MapConfig& config, double mouth_floor) {
        const CaveConfig& caves = config.caves;
        // A share of the relief between the mouth and the sea, capped by the
        // configured depth and never below the sea itself -- a subdued replica of
        // the surface rather than a flat sheet, which is what a water table is.
        const double relief = std::max(0.0, mouth_floor - config.sea_level);
        const double reach = std::min(meters_to_height(config, caves.max_depth_m),
                                      relief * std::max(0.0, caves.vadose_share));
        const double deepest = std::max(config.sea_level, mouth_floor - reach);
        const double spacing = meters_to_height(config, caves.level_spacing_m);

        int count = 1;
        if (spacing > 0.0) {
            const double span = (mouth_floor - deepest) - spacing;
            if (span > 0.0) {
                count += static_cast<int>(span / spacing);
            }
        }
        count = std::clamp(count, 1, std::max(1, caves.max_levels));

        std::vector<double> tables(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            tables[static_cast<std::size_t>(i)] = deepest + spacing * (count - 1 - i);
        }
        return tables;
    }

    /**
     * @brief Opens a way down from a finished run to the storey beneath it.
     *
     * Called once per run and declines far more often than it accepts. A descent
     * only makes sense from a station that actually reached its own table: a run
     * that died halfway down the entrance series is not the floor of anything, and
     * hanging the next level off it would leave a shaft starting in mid-rock.
     *
     * The new head is given a fresh budget rather than whatever its parent had left,
     * because a storey is a network in its own right and the run it hangs off is by
     * definition exhausted. It starts at the tail's floor and at level `L + 1`, so
     * the vadose model carries it down the whole `level_spacing_m` to the next table
     * before the phreatic model takes over and spreads it out. The shaft between two
     * levels is not a special case; it is the descent model running where it has
     * always run.
     *
     * Pushing onto the same work list `grow_()` is walking is deliberate -- that
     * loop advances by index and already tolerates growth, so a level opened here
     * is picked up in turn and the whole system stays breadth-first.
     *
     * @param cave The system being grown; reads `levels` and `nodes`.
     * @param caves Supplies the descent budget share and the node cap.
     * @param heads The work list to append to.
     * @param descents Ways down opened per level so far; updated on acceptance.
     * @param tail The station the run ended on.
     * @param head The run's head, for its heading and its level.
     * @param budget Length for the new level, descent included, in grid units.
     * @param rng The pass's stream, for the turn and the wander phase.
     */
    static void open_descent_(const MapCave& cave, const CaveConfig& caves,
                              std::vector<Head>& heads, std::vector<int>& descents,
                              std::int32_t tail, const Head& head, double budget,
                              std::mt19937& rng) {
        const std::size_t next = static_cast<std::size_t>(head.level) + 1;
        if (next >= cave.levels.size() || descents[next] >= k_descents_per_level
            || static_cast<int>(cave.nodes.size()) >= caves.max_nodes) {
            return;
        }
        const CaveNode& from = cave.nodes[static_cast<std::size_t>(tail)];
        if (from.zone != CaveZone::Phreatic) {
            // The run never reached its own table, so there is no level here to
            // leave from.
            return;
        }
        ++descents[next];

        coopa::maps::UniformRealDistribution<double> unit(0.0, 1.0);
        coopa::maps::UniformRealDistribution<double> phase(-64.0, 64.0);

        Head down;
        down.point = from.point;
        down.floor = from.floor;
        // Away from the way the run came in, for the same reason a branch turns: a
        // descent doubling back along its parent would be drawn over it.
        down.bearing = head.bearing
                       + (unit(rng) < 0.5 ? 1.0 : -1.0)
                             * (k_branch_min_angle + k_branch_spread * unit(rng));
        down.budget = budget;
        down.phase = phase(rng);
        down.parent = tail;
        down.level = static_cast<std::int32_t>(next);
        heads.push_back(down);
    }

    /**
     * @brief Settles a station's floor and ceiling under the ground above it.
     *
     * The one place the "a cave stays underground" invariant is enforced, called
     * once as a station is grown and again for every point of the smoothed
     * passage. The ceiling is the hard constraint and the floor gives way to it:
     * a passage crossing under ground that drops away is pushed down, not thinned
     * from the top, because thinning is what eventually opens it to daylight.
     *
     * Returns false when the result had to break the system's own floor, which
     * growth treats as the end of a head. The smoothed pass ignores it and keeps
     * the clamped values, because dropping a point out of a smoothed path
     * disconnects it -- and staying under the terrain matters more than staying
     * above a preferred depth.
     *
     * @param surface Ground height overhead, sampled as the renderer draws it.
     * @param clearance Rock to leave between the ceiling and that ground.
     * @param height Floor to ceiling wanted, in height units.
     * @param bottom Lowest floor this system should reach.
     * @param floor Proposed floor; settled in place.
     * @param roof Receives the settled ceiling.
     * @return True when the station fits without breaking `bottom` or the bedrock.
     */
    static bool settle_(double surface, double clearance, double height, double bottom,
                        double& floor, double& roof) {
        roof = std::min(floor + height, surface - clearance);
        floor = roof - height;
        bool within = true;
        if (floor < bottom) {
            within = false;
        }
        if (floor < 0.0) {
            floor = 0.0;
            within = false;
        }
        if (roof < floor) {
            roof = floor;
            within = false;
        }
        return within;
    }

    /**
     * @brief How far to turn toward the thicker rock either side of a heading.
     *
     * Probes the ground one step ahead to the left and to the right and leans
     * toward whichever is higher. That is the whole of why a system reaches
     * anywhere: a passage held only by the roof clamp walks out from under its own
     * hill in a few hundred metres and then has to end, because there is no longer
     * anything above it to be under.
     *
     * The difference is measured in units of the headroom a station needs, so the
     * lean means the same thing whatever the vertical scale is set to, and it
     * saturates rather than growing without bound over a cliff.
     *
     * @param graph The generated graph.
     * @param config Supplies `massif_bias`.
     * @param head The head being advanced.
     * @param from The head's current cell, to start each probe's walk from.
     * @param step How far ahead to probe, in grid units.
     * @param needed Rock a station needs above its floor; both the trigger and the scale.
     * @param detail Terrain displacement, as the renderer applies it.
     * @param channels River channels, as the renderer cuts them.
     * @return A heading change in radians; positive turns right.
     */
    static double massif_lean_(const MapGraph& graph, const MapConfig& config, const Head& head,
                               CenterId from, double step, double needed,
                               const TerrainDetail& detail, const RiverChannels& channels) {
        const double bias = config.caves.massif_bias;
        if (bias <= 0.0 || needed <= 0.0) {
            return 0.0;
        }
        const auto probe = [&](double bearing) {
            const MapPoint at = graph.step_from(head.point, bearing, step);
            const CenterId cell = locate_(graph, at, from);
            return graph.elevation_at(graph.centers[static_cast<std::size_t>(cell)], at, detail, channels);
        };

        // Only steer when the rock ahead is actually thinning. Under a massif every
        // direction has depth to spare, so there is nothing to avoid and the
        // passage is left to go where the meander takes it. Leaning toward the
        // higher ground unconditionally -- which is what this did -- makes a head
        // orbit the nearest summit, and a cave drawn as a spiral is not a cave.
        const double headroom = probe(head.bearing) - head.floor;
        if (headroom > needed * k_lean_margin) {
            return 0.0;
        }

        const double left = probe(head.bearing - k_probe_angle);
        const double right = probe(head.bearing + k_probe_angle);
        // Positive when the left probe stands higher, and a positive lean has to
        // turn the heading *down* -- bearings increase clockwise in this space, so
        // subtracting is what steers left.
        const double lean = std::clamp((left - right) / needed, -1.0, 1.0);
        return -bias * k_max_turn * lean;
    }

    /**
     * @brief The lowest ground anywhere over a passage's cross-section.
     *
     * A passage is a tube, not a line, and the rock has to hold over the whole of
     * it. Clamped against the centreline alone the invariant is true of a curve
     * nobody draws: the rim sits `radius` away, and on the steep ground a cave
     * opens on by construction the surface there can be tens of metres lower. It
     * showed as passages poking out of cliff faces -- 48 pixels of a default map,
     * every one of them on a break in slope.
     *
     * Eight probes around the rim rather than four, because four axis-aligned ones
     * can straddle a cliff whose aspect falls between them, which is the case that
     * matters.
     *
     * @param graph The generated graph.
     * @param point Centre of the cross-section, in grid units.
     * @param radius Half-width of the passage there, in grid units.
     * @param from A cell to start each walk from; the previous station's.
     * @param detail Terrain displacement, as the renderer applies it.
     * @param channels River channels, as the renderer cuts them.
     * @return The lowest surface height over the footprint.
     */
    static double lowest_surface_(const MapGraph& graph, const MapPoint& point, double radius,
                                  CenterId from, const TerrainDetail& detail,
                                  const RiverChannels& channels) {
        const auto sample = [&](const MapPoint& at) {
            // The lowest reading any cell that could *draw* this point would give,
            // not the reading its nearest site gives. `elevation_at()` takes a cell
            // as a hint and falls back to inverse-distance over that cell's corners
            // when the point lies outside every triangle incident to it -- so two
            // cells can answer differently for one position, by as much as a
            // hundred metres. The renderer picks the cell whose *subdivided*
            // outline contains the pixel, and a wobbled outline bulges past the
            // straight Voronoi boundary toward the neighbouring site, so the cell
            // that draws a point near a boundary is often not the nearest one.
            //
            // Taking the minimum over the nearest cell and its neighbours is what
            // makes the clamp hold against whichever of them the renderer happens
            // to choose. Without it a passage reads as poking out of the hillside
            // on 0.85% of its pixels -- all of them at cell boundaries, which is
            // where a wobbled outline differs from a straight one.
            const CenterId cell = locate_(graph, at, from);
            const MapCenter& center = graph.centers[static_cast<std::size_t>(cell)];
            double height = graph.elevation_at(center, at, detail, channels);
            for (const CenterId neighbor : center.neighbors) {
                height = std::min(height,
                                  graph.elevation_at(graph.centers[static_cast<std::size_t>(neighbor)],
                                                     at, detail, channels));
            }
            return height;
        };
        double lowest = sample(point);
        if (radius <= 0.0) {
            return lowest;
        }
        for (int i = 0; i < k_rim_probes; ++i) {
            const double angle = k_turn * static_cast<double>(i) / static_cast<double>(k_rim_probes);
            lowest = std::min(lowest, sample(graph.step_from(point, angle, radius)));
        }
        return lowest;
    }

    /** @brief Which regime a floor at this height was cut in. */
    static CaveZone zone_of_(double floor, double phreatic) {
        return floor <= phreatic + k_zone_epsilon ? CaveZone::Phreatic : CaveZone::Vadose;
    }

    /**
     * @brief Finds the cell a point falls in by walking downhill in distance to a site.
     *
     * A Voronoi cell *is* the set of points nearest its site, so stepping to
     * whichever neighbour is nearer and repeating arrives at the right cell and
     * stops there. Starting from the previous station's cell makes that one or two
     * hops: a step is `step_m` long against cells `meters_per_grid_unit` across, so
     * consecutive stations are almost always neighbours.
     *
     * @param graph The generated graph.
     * @param point The position to locate.
     * @param from A cell to start the walk from; the previous station's.
     * @return The cell containing the point.
     */
    static CenterId locate_(const MapGraph& graph, const MapPoint& point, CenterId from) {
        CenterId current = from;
        for (int hop = 0; hop < k_locate_hops; ++hop) {
            const MapCenter& center = graph.centers[static_cast<std::size_t>(current)];
            double best = center.point.distance_to(point);
            CenterId nearest = current;
            for (const CenterId neighbor : center.neighbors) {
                const double distance =
                    graph.centers[static_cast<std::size_t>(neighbor)].point.distance_to(point);
                if (distance < best) {
                    best = distance;
                    nearest = neighbor;
                }
            }
            if (nearest == current) {
                break;
            }
            current = nearest;
        }
        return current;
    }

    /**
     * @brief Traces one grown chain into the smoothed passage that gets drawn.
     *
     * The four output arrays are smoothed with `chaikin_smooth()` -- the same
     * corner-cutter rivers and roads use -- by packing the two heights into one
     * `MapPoint` sequence and the radius into another. The smoother is a
     * per-coordinate convex combination, so running it on a packed pair smooths
     * each member independently, and running it on all three sequences with the
     * same iteration count leaves them the same length by construction. That is
     * what lets `CavePassage` promise a consumer can read one index across all
     * four and get a coherent cross-section.
     *
     * Every smoothed point is then settled again. Corner-cutting moves points, so
     * a midpoint over concave ground can rise above a surface both its neighbours
     * were safely beneath -- and the smoothed path is the one that is drawn and
     * exported, so it is the one the invariant has to hold on.
     *
     * @param graph The generated graph.
     * @param config Supplies the smoothing count and the clearance.
     * @param cave The system being traced; reads `nodes`.
     * @param chain Station indices, in order along the run.
     * @param detail Terrain displacement, as the renderer applies it.
     * @param channels River channels, as the renderer cuts them.
     * @param bottom The system's preferred lowest floor.
     * @return The traced passage, chain and smoothed arrays both.
     */
    CavePassage finish_passage_(const MapGraph& graph, const MapConfig& config, const MapCave& cave,
                                const std::vector<std::int32_t>& chain, const TerrainDetail& detail,
                                const RiverChannels& channels, double bottom) const {
        CavePassage passage;
        passage.nodes = chain;
        // The run's own storey, which is the level of every station on it bar the
        // first -- that one is the station it was grown from, and for a descent
        // that station is on the level above.
        passage.level = cave.nodes[static_cast<std::size_t>(chain.back())].level;

        std::vector<MapPoint> plan;
        std::vector<MapPoint> heights; // x carries the floor, y the ceiling.
        std::vector<MapPoint> widths;  // x carries the radius; y is unused.
        plan.reserve(chain.size());
        heights.reserve(chain.size());
        widths.reserve(chain.size());
        for (const std::int32_t index : chain) {
            const CaveNode& node = cave.nodes[static_cast<std::size_t>(index)];
            plan.push_back(node.point);
            heights.push_back(MapPoint{node.floor, node.roof});
            widths.push_back(MapPoint{node.radius, 0.0});
        }

        const int iterations = config.caves.smoothing_iterations;
        chaikin_smooth(plan, iterations);
        chaikin_smooth(heights, iterations);
        chaikin_smooth(widths, iterations);

        const double clearance = meters_to_height(config, config.caves.roof_clearance_m);
        passage.points = std::move(plan);
        passage.floors.reserve(passage.points.size());
        passage.roofs.reserve(passage.points.size());
        passage.radii.reserve(passage.points.size());

        CenterId cell = cave.nodes[static_cast<std::size_t>(chain.front())].center;
        for (std::size_t i = 0; i < passage.points.size(); ++i) {
            const MapPoint& point = passage.points[i];
            cell = locate_(graph, point, cell);
            const double radius = std::max(0.0, widths[i].x);
            const double surface = lowest_surface_(graph, point, radius, cell, detail, channels);

            double floor = heights[i].x;
            double roof = heights[i].y;
            settle_(surface, clearance, std::max(0.0, roof - floor), bottom, floor, roof);

            passage.floors.push_back(floor);
            passage.roofs.push_back(roof);
            passage.radii.push_back(radius);
        }
        return passage;
    }

    /** @brief Total length of every smoothed passage in a system, in metres. */
    static double measure_(const MapCave& cave, const MapConfig& config) {
        double grid = 0.0;
        for (const CavePassage& passage : cave.passages) {
            for (std::size_t i = 0; i + 1 < passage.points.size(); ++i) {
                grid += passage.points[i].distance_to(passage.points[i + 1]);
            }
        }
        return grid_to_meters(config, grid);
    }

    /** @brief Mean drop from mouth to deepest floor across every system, in metres. */
    static double mean_depth_metres_(const std::vector<MapCave>& caves, const MapConfig& config) {
        double total = 0.0;
        for (const MapCave& cave : caves) {
            total += height_to_meters(config, cave.surface_at_mouth - cave.deepest);
        }
        return total / static_cast<double>(caves.size());
    }

    /**
     * @brief Names a cave in the dialect of the region it opens in.
     *
     * The same machinery `PassTowns` names settlements with, and for the same
     * reason: a cave and the village below it should sound like they are in the
     * same country. Land outside any nation still gets a name, just not a local
     * one.
     *
     * @param graph The generated graph; reads `regions`.
     * @param cave The system to name.
     * @param rng The pass's stream.
     * @return A name no other cave on this map holds.
     */
    std::string name_for_(const MapGraph& graph, const MapCave& cave, std::mt19937& rng) const {
        if (cave.region == k_invalid_id
            || static_cast<std::size_t>(cave.region) >= graph.regions.size()) {
            Language stateless = make_language(rng);
            return claim_name_(generate_name(stateless, rng), stateless, rng);
        }

        auto found = dialects_.find(cave.region);
        if (found == dialects_.end()) {
            const MapRegion& region = graph.regions[static_cast<std::size_t>(cave.region)];
            found = dialects_
                        .emplace(cave.region,
                                 dialect_for(country_language_seed(seed_, region.country),
                                             region_dialect_seed(seed_, region.index)))
                        .first;
        }
        return claim_name_(generate_name(found->second, rng), found->second, rng);
    }

    /** @brief Redraws a name already taken, then falls back to an ordinal. */
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
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_CAVES_H
