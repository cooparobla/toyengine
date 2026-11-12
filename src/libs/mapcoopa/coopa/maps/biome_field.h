/**
 * @file biome_field.h
 * @brief The biome at any point of a generated map, classified from continuous fields.
 *
 * The counterpart of `WaterField` for biomes. `MapCenter::biome` is one enum per Voronoi
 * cell -- the right grain for roads, towns and regions, which reason about cells -- but a
 * consumer meshing the ground samples far finer than a cell, and copying the nearest cell's
 * enum stamps every boundary of the diagram onto the terrain as a hard Voronoi edge.
 *
 * This query classifies **per point** instead, from the same `classify_biome()` diagram the
 * biome pass uses, fed with continuous inputs:
 *
 * - **Elevation** is the sampled ground itself (`WaterSample::ground` -- detail and river
 *   channels included), so the alpine and montane bands trace the actual ridges and the
 *   basin band traces the actual valley floors, not the cell mosaic underneath them.
 * - **Moisture and temperature** are blended barycentrically over the Delaunay triangle,
 *   the same interpolation the ground gets, with a low-amplitude dither so a threshold
 *   crossing wanders organically instead of following cell seams.
 * - **The shore is graded, not a flag.** Proximity to water fades over
 *   `TerrainSurface::water_rings()`, and a shore is a beach only while the ground stays
 *   within `beach_height_m` of the water surface -- so a flat shelf grows a wide beach, a
 *   cliff coast none, and the band ends where the ground climbs away, wherever that falls.
 * - **Rivers are boundaries.** A river runs along a Voronoi edge, which is the boundary
 *   between the two cells it separates -- so the bank a point stands on is exactly the
 *   cell nearest to it, which the caller already had to find. Blending across such an
 *   edge is suppressed (`biome_river_boundary`), which makes the climate fields
 *   discontinuous at the watercourse: a transition that would have landed anywhere within
 *   a cell of the river instead lands ON it, and the two banks can read as different
 *   biomes. Away from rivers nothing changes -- those boundaries stay where the climate
 *   puts them.
 *
 * Build one per map beside the `WaterField` and hand each sample's `WaterSample` in;
 * `at()` is const and safe to call from many threads.
 */

#ifndef COOPA_MAPS_BIOME_FIELD_H
#define COOPA_MAPS_BIOME_FIELD_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>
#include <coopa/maps/water_field.h>

namespace coopa {
namespace maps {

/**
 * @class BiomeField
 * @brief The per-point biome query; see the file doc for the rule it applies.
 */
class BiomeField {
public:
    /**
     * @param graph The generated map. Must outlive the field.
     * @param config The config it was generated with.
     */
    BiomeField(const MapGraph& graph, const MapConfig& config)
        : graph_(graph), config_(config), rings_(TerrainSurface::water_rings(graph)),
          dither_(config.noise_biome), banks_(graph.centers.size()) {
        // Every edge a river runs along separates two banks. Indexed per cell once, because
        // `at()` asks the question per sample and a map has far fewer river edges than a
        // bake has tiles.
        for (const MapEdge& edge : graph.edges) {
            if (edge.river <= 0 || edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
                continue;
            }
            const std::size_t a = static_cast<std::size_t>(edge.d0);
            const std::size_t b = static_cast<std::size_t>(edge.d1);
            if (a >= banks_.size() || b >= banks_.size()) continue;
            banks_[a].push_back(edge.d1);
            banks_[b].push_back(edge.d0);
        }
    }

    BiomeField(const BiomeField&) = delete;
    BiomeField& operator=(const BiomeField&) = delete;

    /**
     * @brief The biome at a point.
     *
     * @param center The cell containing the point (the nearest site), as `elevation_at()`
     *        takes.
     * @param p The point, in grid units (on the globe for a sphere map).
     * @param water The water at the same point, from `WaterField::at()` -- supplies the
     *        continuous ground height and whether the point is under a sea or lake.
     * @return The classified biome.
     */
    Biome at(const MapCenter& center, const MapPoint& p, const WaterSample& water) const {
        return classify_biome(sample_at(center, p, water));
    }

    /**
     * @brief The classifier's inputs at a point, before the diagram is consulted.
     *
     * What `at()` classifies. Public because the blend is the interesting part and
     * `at()` hides it: two moistures a band apart give the same biome, so a caller
     * measuring how the fields behave -- a test pinning the step a river puts in
     * them, a debug view shading moisture -- has nothing to read otherwise.
     *
     * @param center The cell containing the point (the nearest site).
     * @param p The point, in grid units (on the globe for a sphere map).
     * @param water The water at the same point, from `WaterField::at()`.
     * @return The blended, dithered sample.
     */
    BiomeSample sample_at(const MapCenter& center, const MapPoint& p,
                          const WaterSample& water) const {
        double moisture = center.moisture;
        double temperature = center.temperature;
        double ring = ring_of_(center.index);
        double water_level = center.water_level;

        std::array<CenterId, 3> cells{};
        std::array<double, 3> weights{};
        if (graph_.triangle_at(center, p, cells, weights)) {
            double blended_moisture = 0.0;
            double blended_temperature = 0.0;
            double blended_ring = 0.0;
            double total = 0.0;
            for (std::size_t i = 0; i < 3; ++i) {
                double w = std::clamp(weights[i], 0.0, 1.0);
                const MapCenter& cell = graph_.centers[static_cast<std::size_t>(cells[i])];
                // `center` is the nearest site, so it is the bank this point stands on.
                // A cell on the far side of a watercourse contributes that much less.
                if (across_river_(center.index, cell.index)) {
                    w *= 1.0 - river_boundary_();
                }
                blended_moisture += w * cell.moisture;
                blended_temperature += w * cell.temperature;
                blended_ring += w * ring_of_(cell.index);
                total += w;
                // The highest surface any of the triangle's cells puts over the point --
                // the same reach `WaterField::at()` reads, so beach height is measured
                // against the body actually beside the sand. Taken over every cell,
                // suppressed or not: a river does not move the lake beside it.
                water_level = std::max(water_level, cell.water_level);
            }
            // Only when something survived the suppression. At full strength a point whose
            // triangle lies entirely across the water from its own bank has nothing left to
            // blend, and its own cell's values are the answer.
            if (total > 1.0e-9) {
                moisture = blended_moisture / total;
                temperature = blended_temperature / total;
                ring = blended_ring / total;
            }
        }

        BiomeSample sample;
        const double dither = std::clamp(config_.biome_dither, 0.0, 1.0);
        if (dither > 0.0) {
            moisture += dither * static_cast<double>(graph_.noise_at(dither_, p));
            temperature += dither * 0.5
                         * static_cast<double>(graph_.noise_at(
                               dither_, MapPoint{p.x + 157.3, p.y - 89.1, p.z + 43.7}));
        }
        sample.moisture = std::clamp(moisture, 0.0, 1.0);
        sample.temperature = std::clamp(temperature, 0.0, 1.0);
        sample.elevation = land_height(config_, water.ground);
        sample.is_water = water.kind == WaterKind::Sea || water.kind == WaterKind::Lake;
        sample.is_ocean = water.kind == WaterKind::Sea;
        // Proximity fades to nothing over the first two rings of cells around the
        // water; past that the interior diagram answers alone.
        sample.shore = std::clamp(1.0 - 0.5 * ring, 0.0, 1.0);
        const double level = water_level > 0.0 ? water_level : config_.sea_level;
        sample.relief_m =
            height_to_meters(config_, std::max(0.0, water.ground - level));
        sample.beach_height_m = config_.beach_height_m;
        return sample;
    }

private:
    double ring_of_(CenterId id) const {
        const std::size_t index = static_cast<std::size_t>(id);
        return index < rings_.size() ? static_cast<double>(rings_[index]) : 4.0;
    }

    /** @brief How strongly a watercourse cuts the blend, clamped to `[0, 1]`. */
    double river_boundary_() const { return std::clamp(config_.biome_river_boundary, 0.0, 1.0); }

    /** @brief True when a river runs along the edge between these two cells. */
    bool across_river_(CenterId home, CenterId other) const {
        if (home == other) return false;
        const std::size_t index = static_cast<std::size_t>(home);
        if (index >= banks_.size()) return false;
        for (const CenterId id : banks_[index]) {
            if (id == other) return true;
        }
        return false;
    }

    const MapGraph& graph_;
    const MapConfig& config_;
    /** @brief Per cell, rings of cells out from the nearest sea or lake cell, capped at 4. */
    std::vector<std::uint8_t> rings_;
    Noise dither_;
    /** @brief Per cell, the cells a watercourse separates it from. Short: a cell has ~6 edges. */
    std::vector<std::vector<CenterId>> banks_;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_BIOME_FIELD_H
