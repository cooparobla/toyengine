/**
 * @file cave_field.h
 * @brief The open spans a cave leaves at any point of a generated map -- the field builder
 *        `cave.h` names, and the one thing between a cave system and geometry.
 *
 * `PassCaves` grows a branching tube network and stores it as a graph: stations with a floor, a
 * roof and a radius, sliced into unbranched runs whose smoothed polylines are what the shape is
 * actually true of. Deliberately never rasterised -- a stack of floor/roof images is a lossy,
 * far larger copy of the document (`docs/caves.md`'s *Storage*).
 *
 * A consumer placing geometry asks the opposite question, though: not "where does this cave go"
 * but "is there a cave HERE, and between which two heights". That is this class. It is built once
 * per map beside `WaterField` and `BiomeField`, and `at()` is const and safe to call from many
 * threads, exactly as theirs are.
 *
 * ### It returns every span, not one
 *
 * A system is cut at several water tables, one above the next, and two storeys routinely cross
 * in plan. Collapsing them would lose the lower one at exactly the crossings that make a system
 * read as multi-level, so `at()` returns all of them, deepest first, and a consumer that can
 * only hold one takes the last.
 *
 * ### It cuts the mouth, because the pass will not
 *
 * Every station mapcoopa grows -- including the first -- is clamped a `roof_clearance_m` below
 * the ground over it, twice, and that invariant is load-bearing for the pass's own tests. So a
 * cave as grown is sealed everywhere, `MapCave::mouth` included: it is where the system
 * *started*, not a hole. An opening therefore has to be cut here, by running an extra passage
 * downhill from the mouth until the hillside above it thins to nothing. See `build_adits_()`.
 */

#ifndef COOPA_MAPS_CAVE_FIELD_H
#define COOPA_MAPS_CAVE_FIELD_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <coopa/maps/cave.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>

namespace coopa {
namespace maps {

/**
 * @brief Most open spans one column of rock may be reported to have.
 *
 * `CaveConfig::max_levels` caps a system's storeys at four, and two systems overlapping in plan
 * is rare but not impossible, so eight is a ceiling no real map reaches rather than a budget to
 * spend. A point that somehow exceeds it keeps its SHALLOWEST spans -- the ones a cave is
 * entered through, and therefore the ones whose loss would be visible.
 */
inline constexpr std::size_t k_max_cave_spans = 8;

/**
 * @struct CaveSpanSample
 * @brief One open span at a point: the air between a floor and a ceiling.
 *
 * Heights are in the same normalised `[0, 1]` field as `CaveNode::floor` and
 * `MapCenter::elevation`, so they are directly comparable with the surface overhead -- which is
 * the comparison a consumer turning this into geometry has to make.
 */
struct CaveSpanSample {
    double floor = 0.0;      /**< @brief The span's floor. */
    double roof = 0.0;       /**< @brief Its ceiling; always above `floor`. */
    double water = 0.0;      /**< @brief The table standing in it; meaningless unless `flooded`. */
    bool flooded = false;    /**< @brief Whether water stands in it, and how high `water` says. */
    std::int32_t level = 0;  /**< @brief Which of `MapCave::levels` cut it; 0 is the shallowest. */
    CaveZone zone = CaveZone::Vadose;
    CaveFeature feature = CaveFeature::Passage;
};

/**
 * @struct CaveSample
 * @brief Every open span at a point, and whether the point is a cave MOUTH.
 *
 * `open` is the one piece of information that is not a span: it says the hillside here has been
 * cut away, so a consumer must leave the column's head open to the sky rather than roofing it.
 * Without it a cave is a sealed pocket and the whole feature is invisible.
 */
struct CaveSample {
    std::size_t count = 0;                                /**< @brief Spans written, deepest first. */
    std::array<CaveSpanSample, k_max_cave_spans> spans{};
    std::int32_t cave = -1;    /**< @brief Index into `MapGraph::caves`, or -1. */
    bool open = false;         /**< @brief This column is in a mouth's daylit cut. */

    /** @brief Whether a cave reaches this point at all. */
    bool inside() const { return count > 0; }
    /** @brief The shallowest span -- the one a one-span consumer keeps. */
    const CaveSpanSample& outermost() const { return spans[count - 1]; }
};

/**
 * @class CaveField
 * @brief Every cave on a map, indexed so a point can be asked about in a few compares.
 *
 * Built once; `at()` is const and thread-safe. Neither copyable nor movable, because it borrows
 * the graph it indexes -- the same reason `WaterField` is not.
 */
class CaveField {
public:
    /**
     * @brief Indexes every system's passages, then cuts a mouth for each.
     *
     * @param graph    The generated map; its `caves` are what is indexed, and its cells are the
     *                 buckets they are filed under.
     * @param config   Supplies the cave sizes and the vertical scale.
     * @param detail   The fractal field, as `MapGraph::elevation_at()` takes it. Needed because
     *                 a mouth is cut against the surface anyone can SEE, not the control mesh.
     * @param channels The river cut, for the same reason.
     */
    CaveField(const MapGraph& graph, const MapConfig& config, const TerrainDetail& detail,
              const RiverChannels& channels)
        : graph_(graph),
          relief_(std::clamp(config.caves.surface_relief, 0.0, 0.45)),
          min_air_(meters_to_height(config, config.caves.passage_height_m * 0.5)),
          rubble_(surface_noise(config, 7919)),
          pendants_(surface_noise(config, 104729)) {
        if (graph.caves.empty()) return;
        build_segments_(graph, config);
        build_adits_(graph, config, detail, channels);
        index_cells_(graph);
    }

    CaveField(const CaveField&) = delete;
    CaveField& operator=(const CaveField&) = delete;

    /** @brief Whether this map has any cave at all; a null answer for every query if not. */
    bool empty() const { return segments_.empty(); }

    /**
     * @brief The open spans at a point.
     *
     * @param center The cell containing the point (the nearest site), as `elevation_at()` takes
     *               it -- which is also the bucket whose segments are scanned.
     * @param p      The point, in the graph's own frame.
     * @return Its spans, deepest first; `count == 0` in solid rock.
     */
    CaveSample at(const MapCenter& center, const MapPoint& p) const {
        CaveSample out;
        if (segments_.empty()) return out;
        const std::size_t ci = static_cast<std::size_t>(center.index);
        if (ci >= cells_.size()) return out;
        const Cell& bucket = cells_[ci];
        if (bucket.count == 0) return out;
        // The box rejects nearly every query: caves cover a fraction of a percent of a map, so
        // this is four compares against a distance loop. Same bargain RiverChannels strikes.
        if (p.x < bucket.min.x || p.x > bucket.max.x || p.y < bucket.min.y || p.y > bucket.max.y ||
            (graph_.is_sphere() && (p.z < bucket.min.z || p.z > bucket.max.z))) {
            return out;
        }

        for (std::size_t i = 0; i < bucket.count; ++i) {
            const Segment& s = segments_[refs_[bucket.first + i]];
            double t = 0.0;
            const double d = distance_and_t_(s, p, t);
            const double radius = s.radius_a + (s.radius_b - s.radius_a) * t;
            if (radius <= 0.0 || d >= radius) continue;

            CaveSpanSample span;
            span.floor = s.floor_a + (s.floor_b - s.floor_a) * t;
            span.roof = s.roof_a + (s.roof_b - s.roof_a) * t;
            span.level = s.level;
            span.zone = s.zone;
            span.feature = s.feature;
            span.flooded = s.flooded;
            span.water = s.water;
            if (span.roof <= span.floor) continue;
            // The only thing in the whole feature that varies WITHIN a cross-section; everything
            // else is decided per station and interpolated. An adit is left alone: it is the cut
            // a player walks in through, and rubble across a doorway reads as a blocked cave.
            if (!s.adit) rough_(span, p);
            out.cave = s.cave;
            // An adit column is reported open whatever else covers it: the cut is what lets a
            // player in, and a roof written over it would seal the one way through.
            if (s.adit && s.daylit) out.open = true;
            insert_(out, span);
        }
        return out;
    }

    /**
     * @struct Mouth
     * @brief Where a system was opened, and whether the cut ever reached daylight.
     *
     * Kept so a caller can say which systems are actually enterable. A system on ground too
     * gentle for its own roof thickness stays sealed, and a locator reporting that is worth more
     * than a cave nobody can find.
     */
    struct Mouth {
        MapPoint point;              /**< @brief The mouth the adit was cut from. */
        double floor = 0.0;          /**< @brief The adit's floor; the system's own at its root. */
        double roof = 0.0;           /**< @brief Its ceiling. */
        bool daylit = false;         /**< @brief Whether the hillside thinned enough to open. */
        std::int32_t cave = -1;      /**< @brief Index into `MapGraph::caves`. */
        std::int32_t stations = 0;   /**< @brief How many steps the cut took. */
    };

    /** @brief Every system's mouth, in `MapGraph::caves` order. */
    const std::vector<Mouth>& mouths() const { return mouths_; }

private:
    /**
     * @brief The field the floor and the ceiling are roughened with, at one of two seeds.
     *
     * Its wavelength is a multiple of a passage's WIDTH rather than a fixed number of grid
     * units, so the roughness is the same shape on a body whose tiles are four metres across as
     * on one whose tiles are one -- a fixed frequency would be boulders on one world and gravel
     * on the next.
     *
     * Two things are overridden rather than inherited from `noise_cave`, and both decide whether
     * this reads as rock or as static:
     *
     * - **The octave count: ONE.** `noise_cave` is a five-octave fBm field, and octaves are what
     *   put detail below tile size -- every tile then draws an independent value and a floor
     *   comes out as salt and pepper. Even two octaves leave a half-wavelength chop riding on
     *   the mounds, which quantises into isolated cubes rather than into terraces. A single
     *   octave is smooth, and the step quantisation is what turns smooth into rock here.
     * - **The base wavelength.** Two and a half passage widths, so a feature is around twenty
     *   tiles across on the authored globes: a mound to walk over and a dome to walk under,
     *   rather than a kerb at every step.
     *
     * @param config Supplies `noise_cave`, the passage width and the horizontal scale.
     * @param salt   Added to the seed, so the floor and the ceiling are independent draws.
     * @return The configuration to build a sampler from.
     */
    static NoiseConfig surface_noise(const MapConfig& config, int salt) {
        NoiseConfig noise = config.noise_cave;
        noise.seed += salt;
        noise.octaves = 1;
        const double feature = meters_to_grid(config, config.caves.passage_width_m * 2.5);
        noise.frequency = feature > 1e-9 ? 1.0 / feature : config.noise_cave.frequency;
        return noise;
    }

    /**
     * @brief Piles breakdown on a span's floor and hangs pendants from its ceiling.
     *
     * Both faces move INWARD and only inward -- see `CaveConfig::surface_relief` for why the
     * ceiling may never rise. The amplitude is a share of this span's own height and is capped
     * again so `min_air_` survives, so the same key gives boulders in a hall and gravel in a
     * crawl and pinches neither shut.
     *
     * @param span The sample to roughen, already interpolated.
     * @param p    Where it was asked about, which is what makes the result coherent tile to tile.
     */
    /** @brief What share of the floor's roughness the ceiling gets; see `rough_()`. */
    static constexpr double k_roof_relief_share = 0.6;

    void rough_(CaveSpanSample& span, const MapPoint& p) const {
        if (relief_ <= 0.0) return;
        const double height = span.roof - span.floor;
        const double amplitude = std::min(relief_ * height, std::max(0.0, (height - min_air_) * 0.5));
        if (amplitude <= 0.0) return;
        const auto draw = [&](const Noise& field) {
            const double n = graph_.is_sphere() ? field.sample(p.x, p.y, p.z)
                                                : field.sample(p.x, p.y);
            return amplitude * std::clamp(0.5 + 0.5 * n, 0.0, 1.0);
        };
        span.floor += draw(rubble_);
        // The ceiling gets less than the floor, and not for a rendering reason: rock that falls
        // off a roof lands on the floor, so the two surfaces are not symmetric. Visually it is
        // also the one seen edge-on from under it, where every lip reads twice.
        span.roof -= draw(pendants_) * k_roof_relief_share;
        // The water surface is INSIDE the span, so both faces moving in can catch it: a pile
        // standing out of the pool is dry, and a pendant hanging into one is still under water
        // rather than above it -- a drowned passage is full, not overfull, which is the rule
        // `set_flood_()` already states for a segment and this restates for a sample.
        if (span.flooded) {
            span.water = std::min(span.water, span.roof);
            if (span.water <= span.floor) span.flooded = false;
        }
    }

    /**
     * @brief One run of a passage between two smoothed points, with both cross-sections.
     *
     * Both ends are carried rather than one, so `at()` interpolates instead of stepping: a
     * passage read as a chain of constant-radius cylinders is visibly beaded at every station.
     */
    struct Segment {
        MapPoint a, b;
        double floor_a = 0.0, floor_b = 0.0;
        double roof_a = 0.0, roof_b = 0.0;
        double radius_a = 0.0, radius_b = 0.0;
        double water = 0.0;
        std::int32_t cave = -1;
        std::int32_t level = 0;
        CaveZone zone = CaveZone::Vadose;
        CaveFeature feature = CaveFeature::Passage;
        bool flooded = false;
        bool adit = false;    /**< @brief Part of a mouth's cut rather than of the grown cave. */
        bool daylit = false;  /**< @brief An adit segment the hillside no longer covers. */
    };

    /** @brief One cell's slice of `refs_`, and the box that bounds the segments in it. */
    struct Cell {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        MapPoint min, max;
    };

    /**
     * @brief Distance from `p` to a segment, and where along it the nearest point fell.
     *
     * `t` is what every cross-section is interpolated at, so it has to come out of the same
     * projection the distance did -- computing it twice is how a floor and a roof end up
     * described at different points of the same passage.
     */
    double distance_and_t_(const Segment& s, const MapPoint& p, double& t) const {
        const double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
        const double dz = graph_.is_sphere() ? s.b.z - s.a.z : 0.0;
        const double len2 = dx * dx + dy * dy + dz * dz;
        t = 0.0;
        if (len2 > 0.0) {
            const double pz = graph_.is_sphere() ? (p.z - s.a.z) * dz : 0.0;
            t = std::clamp(((p.x - s.a.x) * dx + (p.y - s.a.y) * dy + pz) / len2, 0.0, 1.0);
        }
        const double qx = p.x - (s.a.x + t * dx), qy = p.y - (s.a.y + t * dy);
        const double qz = graph_.is_sphere() ? p.z - (s.a.z + t * dz) : 0.0;
        return std::sqrt(qx * qx + qy * qy + qz * qz);
    }

    /**
     * @brief Files a span into a sample, deepest first, merging anything it overlaps.
     *
     * Merging rather than appending is what keeps a junction from reading as two spans: where a
     * branch leaves a passage both cover the point, and two overlapping spans would put a
     * ceiling inside the air below it. Keeping them sorted is what lets a one-span consumer take
     * the last and get the reachable one.
     */
    static void insert_(CaveSample& out, const CaveSpanSample& span) {
        for (std::size_t i = 0; i < out.count; ++i) {
            CaveSpanSample& have = out.spans[i];
            if (span.floor > have.roof || span.roof < have.floor) continue;
            // The union, and the shallower storey's identity: a merged span is entered from
            // whichever of the two is nearer the surface.
            if (span.roof > have.roof) {
                have.roof = span.roof;
                have.level = span.level;
                have.zone = span.zone;
                have.feature = span.feature;
            }
            have.floor = std::min(have.floor, span.floor);
            if (span.flooded && span.water > have.water) {
                have.flooded = true;
                have.water = span.water;
            }
            return;
        }
        if (out.count == k_max_cave_spans) {
            // Full. Drop the deepest, which is the one furthest from any way in.
            if (span.roof <= out.spans[0].roof) return;
            for (std::size_t i = 1; i < k_max_cave_spans; ++i) out.spans[i - 1] = out.spans[i];
            out.spans[k_max_cave_spans - 1] = span;
            return;
        }
        std::size_t at = out.count;
        while (at > 0 && out.spans[at - 1].roof > span.roof) {
            out.spans[at] = out.spans[at - 1];
            --at;
        }
        out.spans[at] = span;
        ++out.count;
    }

    /**
     * @brief Every smoothed passage of every system, as segments.
     *
     * The smoothed arrays rather than the station chain: `points`, `floors`, `roofs` and `radii`
     * are the same length by construction and index *i* across all four is one coherent
     * cross-section, and they are what `finish_passage_()` re-clamped under the terrain. The
     * stations are what the cave was grown as; these are what it IS.
     *
     * Chambers are added as their own stubby segments, because `is_open_feature()` nodes are
     * widened where passages meet and the smoothed centreline does not carry that width.
     */
    void build_segments_(const MapGraph& graph, const MapConfig& config) {
        for (std::size_t ci = 0; ci < graph.caves.size(); ++ci) {
            const MapCave& cave = graph.caves[ci];
            for (const CavePassage& run : cave.passages) {
                const std::size_t n = run.points.size();
                if (n < 2 || run.floors.size() != n || run.roofs.size() != n ||
                    run.radii.size() != n) {
                    continue;
                }
                for (std::size_t i = 1; i < n; ++i) {
                    Segment s;
                    s.a = run.points[i - 1];
                    s.b = run.points[i];
                    s.floor_a = run.floors[i - 1];
                    s.floor_b = run.floors[i];
                    s.roof_a = run.roofs[i - 1];
                    s.roof_b = run.roofs[i];
                    s.radius_a = run.radii[i - 1];
                    s.radius_b = run.radii[i];
                    s.cave = static_cast<std::int32_t>(ci);
                    s.level = run.level;
                    // The zone and the feature belong to the stations, not to the smoothed
                    // copy, so they come off the run's own tail station -- the one its heads
                    // were cutting when it ended.
                    const std::int32_t tail = run.nodes.empty() ? -1 : run.nodes.back();
                    if (tail >= 0 && static_cast<std::size_t>(tail) < cave.nodes.size()) {
                        s.zone = cave.nodes[static_cast<std::size_t>(tail)].zone;
                        s.feature = cave.nodes[static_cast<std::size_t>(tail)].feature;
                    }
                    set_flood_(s, cave);
                    segments_.push_back(s);
                }
            }
            // A chamber is a room, and a room is where a system is worth standing in. Its width
            // is on the station; the smoothed line through it is only a passage's width.
            //
            // The station's own radius, not the configured one: growth varies a room's section
            // the way it varies a passage's (`CaveConfig::section_variation`), and taking the
            // larger of the two would flatten every small room back to one size -- which is the
            // thing the section model exists to stop.
            for (const CaveNode& node : cave.nodes) {
                if (!is_open_feature(node.feature)) continue;
                Segment s;
                s.a = node.point;
                s.b = node.point;
                s.floor_a = s.floor_b = node.floor;
                s.roof_a = s.roof_b = node.roof;
                s.radius_a = s.radius_b = node.radius;
                s.cave = static_cast<std::int32_t>(ci);
                s.level = node.level;
                s.zone = node.zone;
                s.feature = node.feature;
                set_flood_(s, cave);
                segments_.push_back(s);
            }
        }
    }

    /**
     * @brief Whether water stands in a segment, and at what height.
     *
     * A phreatic passage was cut AT its storey's water table and a sump ends against the one
     * constraint it could not pass, so both are flooded by construction -- that is what the
     * zone records. The level is the SYSTEM's own table, never sea level: a cave high in the
     * hills drains to a table well above the coast, which is the whole point of
     * `CaveConfig::vadose_share`.
     */
    static void set_flood_(Segment& s, const MapCave& cave) {
        const bool sump = s.feature == CaveFeature::Sump;
        if (s.zone != CaveZone::Phreatic && !sump) return;
        const std::size_t l = static_cast<std::size_t>(std::max(0, s.level));
        // ABANDONED, and the test for it is which storey this is rather than where the floor
        // sits. A system has storeys because its table FELL: every level above the deepest is
        // dry passage left behind, whatever height it was cut at. Reading it off the floor
        // instead -- which is what a comparison against the level's own table amounts to -- puts
        // water back in every storey the moment a passage is allowed to loop below its table
        // (`CaveConfig::phreatic_relief_m`), and a cave with a lake on every floor is a cave with
        // no history.
        if (l + 1 < cave.levels.size()) return;
        const double table = l < cave.levels.size() ? cave.levels[l] : cave.phreatic_level;
        const double floor = std::min(s.floor_a, s.floor_b);
        if (table < floor) return;
        // A SUMP is water by definition -- it is the low point a run ended in because it could
        // not pass the table -- so it floods wherever the table still reaches its floor.
        //
        // A phreatic PASSAGE only floods where the table stands clear ABOVE its floor. `grow_()`
        // holds a descent at the table, so a passage cut at it sits exactly on the waterline:
        // that is a walkable passage with water at its feet, not a drowned one, and flooding
        // every one would put four roofed columns in five underwater on the shipped shape. What
        // this leaves wet is the dips of the active level, which is where water in a cave is.
        if (!sump && table <= floor) return;
        s.flooded = true;
        // The surface INSIDE the span: the table where it stands between floor and ceiling, and
        // the ceiling where the table is over it -- a drowned passage is full, not overfull.
        s.water = std::min(std::max(table, floor), std::min(s.roof_a, s.roof_b));
    }

    /**
     * @brief Cuts each system a way in: an extra passage running downhill until it daylights.
     *
     * The pass leaves no opening anywhere (see the file comment), so this is the only thing that
     * makes a cave enterable. The run is held level at the root station's floor and ceiling and
     * advanced downhill in `step_m` increments; the hillside over it thins as it goes, and the
     * first station where the surface drops to the ceiling is where the cut breaks out.
     *
     * The length is PROBED rather than configured, because what it takes is
     * `(roof_clearance + passage_height) / grade` and the grade is a property of the slope the
     * mouth happened to open on -- a tile or two on the steep ground a cube-sphere world is full
     * of, and tens of grid units on the gentle ground of a small body.
     *
     * What bounds it is `adit_reach_()`: the run that same expression gives at `min_grade`, which
     * is the shallowest slope `choose_mouths_()` would open on. So a mouth that passed the filter
     * can always break out, and one on ground flatter than the filter allows does not exist to
     * trench across. A cut that still fails to daylight inside it is left sealed and reported as
     * such.
     */
    void build_adits_(const MapGraph& graph, const MapConfig& config, const TerrainDetail& detail,
                      const RiverChannels& channels) {
        // A quarter of the passage's own width, NOT `step_m`. The stations a cave was grown with
        // are tens of tiles apart, which is the right spacing for a passage and far too coarse
        // for this: the cut has to stop within a tile or two of where the hillside thins out, and
        // a coarse walk overshoots into a trench across the slope.
        const double step =
            std::max(1e-3, meters_to_grid(config, config.caves.passage_width_m * 0.25));
        // How far the ceiling may rise above the ground before the cut counts as open. One
        // passage height, so the break-out is a whole opening rather than a crack.
        const double margin = meters_to_height(config, config.caves.passage_height_m);
        // How many of those steps the cut gets, from the run `min_grade` promises. One extra, so
        // a mouth standing exactly at the threshold still arrives rather than stopping a step
        // short of its own break-out.
        const std::int32_t stations = std::clamp(
            static_cast<std::int32_t>(std::ceil(adit_reach_(config) / step)) + 1,
            k_adit_min_stations, k_adit_max_stations);

        for (std::size_t ci = 0; ci < graph.caves.size(); ++ci) {
            const MapCave& cave = graph.caves[ci];
            if (cave.nodes.empty()) continue;
            const CaveNode& root = cave.nodes.front();

            Mouth mouth;
            mouth.point = cave.mouth;
            mouth.floor = root.floor;
            mouth.roof = root.roof;
            mouth.cave = static_cast<std::int32_t>(ci);

            // Downhill: the reverse of the bearing the system was grown on, which ran from the
            // mouth edge's lower cell toward its higher one.
            const double bearing = graph.bearing_between(root.point, cave.mouth);
            CenterId hint = root.center;
            MapPoint from = cave.mouth;
            bool daylit = false;
            // Staged, and committed only if the cut reaches daylight. A cut that does not is a
            // trench across a hillside leading nowhere -- worse than the sealed cave it was
            // trying to open, because it is visible.
            std::vector<Segment> cut;

            for (std::int32_t i = 0; i < stations && !daylit; ++i) {
                const MapPoint to = graph.step_from(from, bearing, step);
                hint = locate_(graph, to, hint);
                if (hint == k_invalid_id) break;
                const MapCenter& cell = graph.centers[static_cast<std::size_t>(hint)];
                const double surface = graph.elevation_at(cell, to, detail, channels);
                // Broken out: the ground here is at or below the passage's own ceiling, so there
                // is no hill left to hold a roof up.
                daylit = surface <= root.roof + margin;

                Segment s;
                s.a = from;
                s.b = to;
                s.floor_a = s.floor_b = root.floor;
                s.roof_a = s.roof_b = root.roof;
                s.radius_a = s.radius_b = root.radius;
                s.cave = static_cast<std::int32_t>(ci);
                s.level = root.level;
                s.zone = CaveZone::Vadose;
                s.feature = CaveFeature::Passage;
                s.adit = true;
                s.daylit = daylit;
                // An adit that daylights is above the local table by construction -- or it is a
                // spring, which is not what this is. Dry, which also keeps a water surface off
                // the one plane where air and rock meet.
                cut.push_back(s);

                from = to;
                ++mouth.stations;
            }

            if (daylit) {
                // Every segment of the cut is marked daylit, not only the last: the whole run
                // from the break-out back to the first roofed column is what a player walks
                // down, so all of it must be left open to the sky.
                for (Segment& s : cut) s.daylit = true;
                segments_.insert(segments_.end(), cut.begin(), cut.end());
            } else {
                mouth.stations = 0;
            }

            mouth.daylit = daylit;
            mouths_.push_back(mouth);
        }
    }

    /**
     * @brief The cell nearest a point, walked from a hint through the graph's own adjacency.
     *
     * The same greedy descent `pass_caves.h` uses: a Delaunay neighbour list leads downhill to
     * the nearest site in a few hops from anywhere nearby, which is what every step of an adit
     * is from the last.
     */
    static CenterId locate_(const MapGraph& graph, const MapPoint& p, CenterId from) {
        if (graph.centers.empty()) return k_invalid_id;
        CenterId best = from == k_invalid_id ? 0 : from;
        double best_d = graph.centers[static_cast<std::size_t>(best)].point.distance_to(p);
        for (std::int32_t hop = 0; hop < k_locate_hops; ++hop) {
            CenterId next = best;
            for (const CenterId id : graph.centers[static_cast<std::size_t>(best)].neighbors) {
                if (id == k_invalid_id) continue;
                const double d = graph.centers[static_cast<std::size_t>(id)].point.distance_to(p);
                if (d < best_d) {
                    best_d = d;
                    next = id;
                }
            }
            if (next == best) break;
            best = next;
        }
        return best;
    }

    /**
     * @brief Files every segment under the cells whose ground it could reach, with their boxes.
     *
     * Two passes, count then fill, as `WaterField::build_pools_()` does -- and with the same
     * filing test, because a cave passage is a swept disc and that test is already written for a
     * disc: a segment belongs to every cell within its radius plus that cell's own corner reach.
     */
    void index_cells_(const MapGraph& graph) {
        if (segments_.empty()) return;
        cells_.assign(graph.centers.size(), Cell{});

        std::vector<double> reach(graph.centers.size(), 0.0);
        for (std::size_t ci = 0; ci < graph.centers.size(); ++ci) {
            const MapCenter& cell = graph.centers[ci];
            double r = 0.0;
            for (const CornerId corner : cell.corners) {
                if (corner == k_invalid_id) continue;
                r = std::max(r, cell.point.distance_to(
                                    graph.corners[static_cast<std::size_t>(corner)].point));
            }
            reach[ci] = r;
        }

        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                std::uint32_t total = 0;
                for (Cell& cell : cells_) {
                    cell.first = total;
                    total += cell.count;
                    cell.count = 0;
                }
                refs_.resize(total);
            }
            for (std::size_t ci = 0; ci < graph.centers.size(); ++ci) {
                const MapCenter& cell = graph.centers[ci];
                for (std::size_t si = 0; si < segments_.size(); ++si) {
                    const Segment& s = segments_[si];
                    const double radius = std::max(s.radius_a, s.radius_b);
                    if (segment_distance_(s, cell.point) > radius + reach[ci]) continue;
                    if (pass == 0) {
                        ++cells_[ci].count;
                    } else {
                        refs_[cells_[ci].first + cells_[ci].count++] = static_cast<std::uint32_t>(si);
                    }
                }
            }
        }

        // The boxes, widened by each segment's own radius, so a query inside a passage is never
        // rejected by the box that is supposed to admit it.
        for (std::size_t ci = 0; ci < cells_.size(); ++ci) {
            Cell& cell = cells_[ci];
            if (cell.count == 0) continue;
            bool first = true;
            for (std::uint32_t i = 0; i < cell.count; ++i) {
                const Segment& s = segments_[refs_[cell.first + i]];
                const double r = std::max(s.radius_a, s.radius_b);
                for (const MapPoint& end : {s.a, s.b}) {
                    const MapPoint lo{end.x - r, end.y - r, end.z - r};
                    const MapPoint hi{end.x + r, end.y + r, end.z + r};
                    if (first) {
                        cell.min = lo;
                        cell.max = hi;
                        first = false;
                        continue;
                    }
                    cell.min.x = std::min(cell.min.x, lo.x);
                    cell.min.y = std::min(cell.min.y, lo.y);
                    cell.min.z = std::min(cell.min.z, lo.z);
                    cell.max.x = std::max(cell.max.x, hi.x);
                    cell.max.y = std::max(cell.max.y, hi.y);
                    cell.max.z = std::max(cell.max.z, hi.z);
                }
            }
        }
    }

    /** @brief Distance from a point to a segment, in whichever topology the graph is. */
    double segment_distance_(const Segment& s, const MapPoint& p) const {
        return graph_.is_sphere() ? distance_to_segment(p, s.a, s.b)
                                  : distance_to_segment(p.x, p.y, s.a, s.b);
    }

    /**
     * @brief How far a cut may run, in grid units: the run `min_grade` itself promises.
     *
     * A cut falls at the slope it was opened on and has to lose `roof_clearance + passage_height`
     * of cover, so the run it needs is that over the grade. `choose_mouths_()` opens on nothing
     * flatter than `min_grade`, so the run at `min_grade` bounds every cut the pass can ask for
     * -- which is why this is derived rather than authored. Authoring it separately is what lets
     * the two disagree, and they disagree in the expensive direction: a bound too short seals
     * every mouth on a gentle world, and the caves are then invisible rather than absent.
     */
    static double adit_reach_(const MapConfig& config) {
        const CaveConfig& caves = config.caves;
        const double cover = caves.roof_clearance_m + caves.passage_height_m;
        const double grade = std::max(caves.min_grade, 1e-4);
        return meters_to_grid(config, cover / grade);
    }

    /** @brief Fewest steps a cut may take, so a near-vertical mouth still gets a threshold. */
    static constexpr std::int32_t k_adit_min_stations = 4;
    /**
     * @brief Most steps a cut may take, whatever `adit_reach_()` asks for.
     *
     * A backstop against a hand-authored `min_grade` near zero, not a tuning knob: at a quarter
     * of the passage width per step this is tens of tiles, and a cut longer than that reads as a
     * cutting dug across the slope rather than as a way in. The first attempt at this walked at
     * `step_m` -- the grown passage's own station spacing, tens of tiles -- and left a 196-tile
     * trench.
     */
    static constexpr std::int32_t k_adit_max_stations = 64;
    /** @brief Hops the cell walk takes before settling, as `pass_caves.h`'s own does. */
    static constexpr std::int32_t k_locate_hops = 8;

    const MapGraph& graph_;
    double relief_ = 0.0;      /**< @brief `CaveConfig::surface_relief`, clamped. */
    double min_air_ = 0.0;     /**< @brief Air a roughened span must keep, in height units. */
    Noise rubble_;             /**< @brief The floor's field. */
    Noise pendants_;           /**< @brief The ceiling's, at a different seed. */
    std::vector<Segment> segments_;
    std::vector<std::uint32_t> refs_;
    std::vector<Cell> cells_;
    std::vector<Mouth> mouths_;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_CAVE_FIELD_H
