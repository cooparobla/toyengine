/**
 * @file pass_volcanoes.h
 * @brief Records where the volcanic vents are, so every consumer of them agrees.
 */

#ifndef COOPA_MAPS_PASSES_PASS_VOLCANOES_H
#define COOPA_MAPS_PASSES_PASS_VOLCANOES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/volcano.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassVolcanoes
 * @brief Writes `MapGraph::volcanoes`: one record per vent the sampled surface raises.
 *
 * ### Why a pass at all, when the cones are analytic
 *
 * The cone shape lives in `MapGraph::volcano_field_()`, sampled per point, for the same reason
 * the crater field does: a vent has to be crisp at whatever resolution a consumer bakes at, and
 * a landform interpolated between cell heights comes out the size of a Voronoi cell. Nothing
 * about the *shape* needs a pass.
 *
 * What needs a pass is the **list**. Four things have to agree on where the vents are, and none
 * of them can re-derive it: a plume effect stands at one, an ice crust is thinned from beneath
 * by one, a probe scene has to be pointed at one, and a test has to find one to measure it.
 * Searching a baked heightfield for local maxima would find mountains too, and the landmark
 * pass's `LandmarkKind::Volcano` is the wrong tool twice over -- it is descriptive (a summit
 * that happens to stand on volcanic ground), and it reads the *stored* cell elevation, which
 * the analytic field never touches.
 *
 * ### How the list cannot drift from the field
 *
 * The pass does not place vents of its own. It enumerates the **same hash lattice** the field
 * reads, through the same `MapGraph::volcano_site_()`, and then *evaluates the field itself* at
 * each candidate to decide what to record. So a recorded height is the rise the surface
 * actually applies there, not a prediction of it, and the two cannot disagree even if the
 * profile is retuned later.
 *
 * Enumeration walks the cells rather than a coordinate range, which is what makes it
 * topology-agnostic: a plane has bounds and a globe has none, but both have a `centers` array,
 * and the lattice cell a point falls in is the one thing both topologies answer the same way.
 *
 * ### Determinism
 *
 * No randomness of its own. Every decision comes from `volcano_site_()`'s integer hashing, and
 * the candidate order is fixed by a `coopa::maps::sort` over the packed lattice keys -- not by
 * the order `centers` happens to visit them, which would make the spacing filter's choice of
 * survivor depend on the triangulation.
 */
class PassVolcanoes {
public:
    /**
     * @brief Records every vent on the map.
     * @param graph The map to annotate; its `volcanoes` are replaced.
     * @param config Supplies `natural.volcanoes` and the rest of the vent keys.
     * @param logger Reports how many vents were found.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        graph.volcanoes.clear();
        if (config.natural.volcanoes <= 0.0 || graph.centers.empty()) {
            return;
        }

        // Holds references into `config`, which outlives this call. The water rings are left
        // empty on purpose: they gate the land shaping in `natural_elevation_()` and the vent
        // field does not read them.
        const NaturalTerrain natural(config, std::vector<std::uint8_t>{});
        const bool sphere = graph.is_sphere();
        const double submarine = std::clamp(config.natural.volcano_submarine, 0.0, 1.0);

        std::vector<MapVolcano> found;
        double freq = MapGraph::k_volcano_frequency / std::max(config.natural.volcano_size, 1e-3);
        for (int octave = 0; octave < MapGraph::k_volcano_octaves; ++octave) {
            const std::uint32_t salt =
                natural.volcano_salt + static_cast<std::uint32_t>(octave) * 131u;
            for (const Key& key : occupied_cells_(graph, freq, sphere)) {
                const MapGraph::VolcanoSite site =
                    MapGraph::volcano_site_(key.x, key.y, key.z, salt, config.natural.volcano_density,
                                            config.natural.volcano_caldera,
                                            config.natural.volcano_active,
                                            config.natural.volcano_kind, sphere);
                if (!site.present) {
                    continue;
                }
                MapVolcano vent;
                if (!describe_(graph, natural, site, freq, sphere, submarine, vent)) {
                    continue;
                }
                found.push_back(vent);
            }
            freq *= MapGraph::k_volcano_lacunarity;
        }

        // Tallest first, so where two cones overlap the one that survives is the one that
        // dominates the ground -- and the choice does not depend on octave order.
        coopa::maps::sort(found.begin(), found.end(), [](const MapVolcano& a, const MapVolcano& b) {
            if (a.height != b.height) return a.height > b.height;
            return a.point.x != b.point.x ? a.point.x < b.point.x : a.point.y < b.point.y;
        });
        for (const MapVolcano& vent : found) {
            if (!crowded_(graph, vent)) {
                graph.volcanoes.push_back(vent);
            }
        }

        std::size_t submerged = 0;
        std::size_t molten = 0;
        std::size_t alive = 0;
        for (const MapVolcano& vent : graph.volcanoes) {
            if (vent.submarine) ++submerged;
            if (vent.lava_rise > 0.0) ++molten;
            if (vent.alive()) ++alive;
        }
        logger.info("volcanoes: " + std::to_string(graph.volcanoes.size()) + " vents ("
                    + std::to_string(alive) + " alive, " + std::to_string(submerged)
                    + " submarine, " + std::to_string(molten) + " with lava)");
    }

private:
    /** @brief One lattice cell, as an orderable key. */
    struct Key {
        int x = 0;
        int y = 0;
        int z = 0;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };

    /** @brief A cone closer than this to the ground it stands on is not worth recording. */
    static constexpr double k_min_height = 0.005;
    /** @brief How much of a recorded cone's radius must be clear of any taller one. */
    static constexpr double k_spacing = 0.6;

    /**
     * @brief Every lattice cell any cell site falls in, sorted and deduplicated.
     *
     * The lattice is unbounded and mostly empty, so it is enumerated from the map rather than
     * swept: a vent can only matter where there is ground to raise. One cell per site
     * under-counts in principle -- a lattice cell smaller than a Voronoi cell could fall
     * entirely between two sites -- but the lattice is deliberately coarser than the graph
     * (a vent spans many cells), so in practice every occupied cell is named several times over.
     */
    std::vector<Key> occupied_cells_(const MapGraph& graph, double freq, bool sphere) const {
        std::vector<Key> keys;
        keys.reserve(graph.centers.size());
        for (const MapCenter& center : graph.centers) {
            double dx = center.point.x, dy = center.point.y, dz = center.point.z;
            if (sphere) {
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len > 1e-9) { dx /= len; dy /= len; dz /= len; }
            } else {
                dx *= MapGraph::k_volcano_plane_scale;
                dy *= MapGraph::k_volcano_plane_scale;
                dz = 0.0;
            }
            keys.push_back(Key{static_cast<int>(std::floor(dx * freq)),
                               static_cast<int>(std::floor(dy * freq)),
                               sphere ? static_cast<int>(std::floor(dz * freq)) : 0});
        }
        coopa::maps::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        });
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        return keys;
    }

    /**
     * @brief Turns a lattice site into a record, or rejects it.
     *
     * The height is read back out of `volcano_field_()` at the vent instead of being predicted
     * from the site, which is what makes a record honest: a site whose jitter carried it off the
     * globe's shell, or whose cone is damped because it stands under water on a world with
     * `volcano_submarine` turned down, reports the rise that is really there -- and is dropped
     * when that is nothing worth standing on.
     *
     * @return False when the site raises too little ground to record.
     */
    bool describe_(const MapGraph& graph, const NaturalTerrain& natural,
                   const MapGraph::VolcanoSite& site, double freq, bool sphere, double submarine,
                   MapVolcano& out) const {
        MapPoint point;
        if (sphere) {
            // The site is jittered inside a lattice cell over the unit direction, so it does not
            // land exactly on the shell; project it back out to the globe.
            const double ux = site.x / freq, uy = site.y / freq, uz = site.z / freq;
            const double len = std::sqrt(ux * ux + uy * uy + uz * uz);
            if (len <= 1e-9) return false;
            const double r = graph.sphere_radius;
            point = MapPoint{ux / len * r, uy / len * r, uz / len * r};
        } else {
            const double scale = freq * MapGraph::k_volcano_plane_scale;
            point = MapPoint{site.x / scale, site.y / scale};
        }

        const CenterId nearest = nearest_center_(graph, point);
        if (nearest == k_invalid_id) return false;
        const MapCenter& center = graph.centers[static_cast<std::size_t>(nearest)];

        // OFF THE MAP IS NOT A PLACE. A lattice cell overlapping the edge of a plane puts its
        // jittered vent outside the triangulation, where `natural_elevation_()` gives up and
        // returns the bare control-mesh height -- so the cone the field reports there is never
        // applied, and a record of it would send a plume, a probe scene and a test to a vent
        // that does not exist. A globe has no hull and always passes this.
        std::array<CenterId, 3> cells{};
        std::array<double, 3> weights{};
        if (!graph.triangle_at(center, point, cells, weights)) {
            return false;
        }

        const double ground = graph.elevation_at(center, point);
        const double water = center.water_level;
        const double depth = water - ground;
        const bool submerged = depth > 0.0;
        // The same gate `natural_elevation_()` applies, so the recorded height is the applied one.
        const double wet = submerged ? smoothstep_(0.0, 0.02, depth) : 0.0;
        const double gate = (1.0 - wet) + wet * submarine;
        // THE PROMINENCE, NOT THE HEIGHT AT THE MIDDLE. For most kinds those are the same
        // thing, and for a caldera they are opposites: its centre is the floor, so a collapsed
        // summit measured there reports nothing and the vent is thrown away as too small --
        // which silently emptied the record on exactly the kind the rim was built for. Sampling
        // the crater ring as well costs four field evaluations per candidate, once per map.
        const VolcanoShape shape = volcano_shape(site.kind);
        const double middle = graph.volcano_field_(point, natural);
        double rise = middle;
        if (shape.crater > 0.0) {
            const double ring = radius_of(site, freq, sphere, graph) * shape.crater;
            for (int i = 0; i < 4; ++i) {
                const double bearing = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / 4.0;
                rise = std::max(rise, graph.volcano_field_(graph.step_from(point, bearing, ring), natural));
            }
        }
        rise *= natural.config.volcanoes * gate;
        if (rise < k_min_height) return false;

        out.center = nearest;
        out.point = point;
        out.height = rise;
        out.caldera = site.caldera;
        out.kind = site.kind;
        out.activity = site.activity;
        out.submarine = submerged;
        out.region = center.region;
        // The cone's base, carried out of lattice units into the grid units every other length
        // on the map is measured in.
        out.radius = radius_of(site, freq, sphere, graph);
        if (site.half_length > 0.0) {
            out.line = MapPoint{site.dx, site.dy, site.dz};
            out.half_length = to_grid_(site.half_length, freq, sphere, graph);
        }

        // THE POOL FILLS THE CRATER, SO THE CRATER'S OWN DEPTH IS WHAT SETS IT. That depth is
        // exactly what the prominence measurement above already found: the rim stands at
        // `rise`, the floor at the middle, and the difference is the hole. A kind with no
        // crater measures the same height both times and so pools nothing without a special
        // case -- which is the honest answer for a fissure, a dome and a cinder cone alike.
        // A DEAD VOLCANO HOLDS NO LAVA, and NEITHER DOES A DROWNED ONE. The first is the whole
        // of what activity costs the ground: the landform is identical either way, and an
        // extinct caldera is still a caldera with a rim and a floor -- it is simply cold. The
        // second is what a vent under water is: the sea stands over its crater and quenches
        // whatever reaches it, so the basin holds the sea and not a lake of its own. Both are
        // recorded vents either way -- the cone is there, and something has to stand at it.
        const double hole = rise - middle * natural.config.volcanoes * gate;
        if (shape.lava > 0.0 && hole > 0.0 && out.activity > 0.0 && !submerged) {
            // A sleepy vent keeps a pond and a vigorous one brims, so the fill reads the vigour
            // rather than only the on/off.
            out.lava_rise = shape.lava * hole * (0.4 + 0.6 * out.activity);
            // Out to the crater wall and a little into it, so the shoreline is decided by the
            // ground crossing the surface rather than by this circle -- the same rule a sea's
            // shore follows. Past the rim the ground stands above the pool and nothing is wet.
            out.lava_radius = out.radius * std::min(1.0, shape.crater + 0.15);
        }
        return true;
    }

    /** @brief A vent's base radius carried out of lattice units into the map's own grid units. */
    static double radius_of(const MapGraph::VolcanoSite& site, double freq, bool sphere,
                            const MapGraph& graph) {
        return to_grid_(site.radius, freq, sphere, graph);
    }

    /** @brief Any lattice length in the map's own grid units. */
    static double to_grid_(double lattice, double freq, bool sphere, const MapGraph& graph) {
        return sphere ? lattice / freq * graph.sphere_radius
                      : lattice / (freq * MapGraph::k_volcano_plane_scale);
    }

    /** @brief Whether an already-recorded vent stands too close to this one. */
    bool crowded_(const MapGraph& graph, const MapVolcano& vent) const {
        for (const MapVolcano& kept : graph.volcanoes) {
            if (kept.point.distance_to(vent.point) < k_spacing * kept.radius) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief The cell whose site is nearest a point, by brute force.
     *
     * A walk over neighbours would be faster and is what the samplers do, but there are tens of
     * vents against thousands of cells and this runs once per map -- and a downhill walk can
     * stop at a local minimum, which here would file a vent under the wrong region.
     */
    static CenterId nearest_center_(const MapGraph& graph, const MapPoint& p) {
        CenterId best = k_invalid_id;
        double best_distance = 0.0;
        for (const MapCenter& center : graph.centers) {
            const double distance = center.point.distance_to(p);
            if (best == k_invalid_id || distance < best_distance) {
                best = center.index;
                best_distance = distance;
            }
        }
        return best;
    }

    /** @brief The same easing `natural_elevation_()` gates the vent field with. */
    static double smoothstep_(double e0, double e1, double v) {
        const double t = std::clamp((v - e0) / (e1 - e0), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_VOLCANOES_H
