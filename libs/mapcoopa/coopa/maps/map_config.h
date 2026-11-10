/**
 * @file map_config.h
 * @brief Every knob that shapes a generated map: grid density, the island
 *        noise field, per-pass parameters, town placement, and the render palette.
 */

#ifndef COOPA_MAPS_MAP_CONFIG_H
#define COOPA_MAPS_MAP_CONFIG_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <noise/FastNoiseLite.h>

#include <coopa/maps/biome.h>
#include <coopa/maps/portable_random.h>

namespace coopa {
namespace maps {

/**
 * @struct NoiseConfig
 * @brief Parameters for the fractal noise field that decides land from water.
 *
 * The fields map one-to-one onto FastNoiseLite's setters. They are typed
 * against FastNoiseLite's own enums rather than raw `int`s, which would be
 * `static_cast` at the point of use, so `type = 0` would silently mean
 * OpenSimplex2 and nothing in the file would say so.
 */
struct NoiseConfig {
    /** @brief Seed for the noise field; independent of `MapConfig::seed`. */
    int seed = 251;
    /** @brief Base frequency -- larger values give smaller, more numerous islands. */
    double frequency = 0.08;
    /** @brief Which noise algorithm generates the field. */
    FastNoiseLite::NoiseType type = FastNoiseLite::NoiseType_OpenSimplex2;
    /** @brief How octaves are combined. */
    FastNoiseLite::FractalType fractal_type = FastNoiseLite::FractalType_FBm;
    /** @brief Number of octaves summed into the field. */
    int octaves = 5;
    /** @brief Frequency multiplier between successive octaves. */
    double lacunarity = 2.0;
    /** @brief Amplitude multiplier between successive octaves. */
    double gain = 0.5;
    /** @brief How strongly an octave's amplitude is weighted by the previous one. */
    double weighted_strength = 0.45;
};

/**
 * @brief Applies a noise configuration to a FastNoiseLite generator.
 *
 * The eight setters live here rather than in `Noise`'s constructor because two
 * different objects need them -- the `Noise` wrapper and `ShapeField`, which
 * warps the landmass outline. Duplicating the block is how a field ends up
 * configured in one place and silently ignored in the other.
 *
 * @param generator The generator to configure.
 * @param config The field parameters to apply.
 */
inline void configure_noise(FastNoiseLite& generator, const NoiseConfig& config) {
    generator.SetSeed(config.seed);
    generator.SetFrequency(static_cast<float>(config.frequency));
    generator.SetNoiseType(config.type);
    generator.SetFractalType(config.fractal_type);
    generator.SetFractalOctaves(config.octaves);
    generator.SetFractalLacunarity(static_cast<float>(config.lacunarity));
    generator.SetFractalGain(static_cast<float>(config.gain));
    generator.SetFractalWeightedStrength(static_cast<float>(config.weighted_strength));
}

/**
 * @enum TownTier
 * @brief The size class awarded to a settlement, best site first.
 */
enum class TownTier {
    Capital,
    Town,
    Village
};

/** @brief Maps a town tier to its serialisation name. */
inline std::string_view town_tier_name(TownTier tier) {
    switch (tier) {
        case TownTier::Capital: return "capital";
        case TownTier::Town:    return "town";
        case TownTier::Village: return "village";
    }
    return "village";
}

/** @brief Resolves a serialisation name back to a town tier, defaulting to `Village`. */
inline TownTier town_tier_from_name(std::string_view name) {
    if (name == "capital") return TownTier::Capital;
    if (name == "town")    return TownTier::Town;
    return TownTier::Village;
}

/**
 * @enum RoadClass
 * @brief How much traffic a road carries, and therefore what kind of road it is.
 *
 * Assigned from the number of routes the network pass pushed along an edge, not
 * chosen: a highway is a highway because everything goes that way. Ordered
 * least to most travelled so a comparison against a tier means what it reads
 * like, and `None` is first so a default-constructed edge carries no road.
 */
enum class RoadClass {
    None,
    Trail,
    Road,
    Highway
};

/** @brief Number of distinct `RoadClass` values, `None` included. */
inline constexpr std::size_t k_road_class_count = 4;

/**
 * @brief Maps a road class to its serialisation name.
 *
 * These `snake_case` names are the stable on-disk identity of a class, exactly
 * as `biome_name()` is for a biome.
 *
 * @param road_class The class to name.
 * @return A `snake_case` identifier, e.g. `"highway"`.
 */
inline std::string_view road_class_name(RoadClass road_class) {
    switch (road_class) {
        case RoadClass::None:    return "none";
        case RoadClass::Trail:   return "trail";
        case RoadClass::Road:    return "road";
        case RoadClass::Highway: return "highway";
    }
    return "none";
}

/**
 * @brief Resolves a serialisation name back to a road class.
 *
 * Unknown names become `Trail` rather than `None`: the name was written because
 * a road was there, and downgrading it to the least travelled class loses less
 * than erasing it.
 *
 * @param name A name previously produced by `road_class_name()`.
 * @return The matching class, or `RoadClass::Trail` if the name is unknown.
 */
inline RoadClass road_class_from_name(std::string_view name) {
    if (name == "none")    return RoadClass::None;
    if (name == "road")    return RoadClass::Road;
    if (name == "highway") return RoadClass::Highway;
    return RoadClass::Trail;
}

/**
 * @enum CompositeShading
 * @brief How the composite layer lights the biome colours.
 *
 * Both are *drawing* choices applied to the biome fill; neither touches the
 * height data, and the elevation layer on disk is unaffected either way.
 */
enum class CompositeShading {
    /**
     * @brief Brightness follows height: the higher the ground, the paler it is.
     *
     * A function of elevation alone, so the same height reads the same
     * everywhere on the map and a colour can be compared against the legend
     * plus a known offset. It shows *where the high ground is*.
     */
    Elevation,
    /**
     * @brief Brightness follows slope, lit from the north-west.
     *
     * A function of the height *gradient*, so it shows the shape of the
     * land -- ridgelines, valley walls, which way a face turns -- but says
     * nothing about altitude: a slope at sea level and the same slope on a
     * summit are drawn identically.
     */
    Hillshade
};

/** @brief Number of distinct `CompositeShading` values. */
inline constexpr std::size_t k_composite_shading_count = 2;

/**
 * @brief Maps a composite shading mode to its serialisation name.
 * @param shading The mode to name.
 * @return A `snake_case` identifier, e.g. `"hillshade"`.
 */
inline std::string_view composite_shading_name(CompositeShading shading) {
    switch (shading) {
        case CompositeShading::Elevation: return "elevation";
        case CompositeShading::Hillshade: return "hillshade";
    }
    return "elevation";
}

/**
 * @brief Resolves a serialisation name back to a shading mode.
 * @param name A name previously produced by `composite_shading_name()`.
 * @return The matching mode, or `CompositeShading::Elevation` if the name is unknown.
 */
inline CompositeShading composite_shading_from_name(std::string_view name) {
    if (name == "hillshade") return CompositeShading::Hillshade;
    return CompositeShading::Elevation;
}

/**
 * @enum ElevationSurface
 * @brief How the height field is drawn between the points it is stored at.
 *
 * The graph holds one height per cell and one per corner. Everything between
 * those is a choice, and it is the choice -- not the smoothing passes -- that
 * decides how smooth a rendered map looks: `elevation_smoothing_iterations`
 * relaxes the stored values, and no setting of it can stop the surface being
 * continuous between them.
 */
enum class ElevationSurface {
    /**
     * @brief Barycentric over the Delaunay triangle containing the sample.
     *
     * The piecewise-linear surface through the cell sites, and what a terrain
     * mesh built from this data would be. Continuous by construction, so a map
     * reads smooth however rough the underlying field is -- at one pixel to the
     * metre a 60 m cell spans 60 pixels, and a step between two cells is spread
     * across all of them.
     */
    Interpolated,
    /**
     * @brief One height per cell, hard-edged at every boundary.
     *
     * Draws `MapCenter::elevation` flat across the cell, which is the value the
     * graph actually stores -- so this is the height field shown without any
     * interpolation over it, Voronoi tessellation and all. What was wanted when
     * turning the smoothing off did not produce facets.
     */
    Flat,
    /**
     * @brief `Flat`, then blurred: a smoothing pass over the rasterised height field.
     *
     * The other two modes are sampling rules -- given a point, what height is
     * there. This one is not, and that is the whole idea. The cells are drawn flat
     * exactly as `Flat` draws them, and the *image* is then smoothed by a box blur
     * `elevation_blend` cells across, with the river channels cut back in
     * afterwards so a watercourse keeps its full contrast on the smoothed ground.
     *
     * Blending per cell instead was tried first and cannot work, however it is
     * tuned: a cell that ramps its own rim is a raised tile with a bevel, so a map
     * of them traces every Voronoi outline in a soft halo and reads *more*
     * tessellated than the hard edges it set out to hide. A blend is a property of
     * the image, not of a cell.
     *
     * A blend of 0 is `Flat` byte for byte -- there is no blur to run. It does not
     * converge on `Interpolated` at the top end and is not meant to: this is a
     * smoothed picture of the stored field, where `Interpolated` is a different
     * surface through the same points.
     */
    Blended
};

/** @brief Number of distinct `ElevationSurface` values. */
inline constexpr std::size_t k_elevation_surface_count = 3;

/**
 * @brief Maps an elevation surface style to its serialisation name.
 * @param surface The style to name.
 * @return A `snake_case` identifier, e.g. `"flat"`.
 */
inline std::string_view elevation_surface_name(ElevationSurface surface) {
    switch (surface) {
        case ElevationSurface::Interpolated: return "interpolated";
        case ElevationSurface::Flat:         return "flat";
        case ElevationSurface::Blended:      return "blended";
    }
    return "interpolated";
}

/**
 * @brief Resolves a serialisation name back to an elevation surface style.
 * @param name A name previously produced by `elevation_surface_name()`.
 * @return The matching style, or `ElevationSurface::Interpolated` if unknown.
 */
inline ElevationSurface elevation_surface_from_name(std::string_view name) {
    if (name == "flat") return ElevationSurface::Flat;
    if (name == "blended") return ElevationSurface::Blended;
    return ElevationSurface::Interpolated;
}

/**
 * @enum MapTopology
 * @brief What surface the map's cells cover.
 *
 * - **Plane** -- the square `[0, grid_size]^2`, ringed by sea (`border_length`). Every map made
 *   before spheres existed, and still the default: nothing about it changes with this setting.
 * - **Sphere** -- the whole surface of a globe: no edges, no border, latitude climate from the
 *   poles. Points lie at `sphere_radius()` grid units from the centre, so a cell is still about
 *   one grid unit across and every distance, width and noise frequency keeps its meaning.
 *   `MapPoint::z` is used; on a plane it is always 0.
 */
enum class MapTopology { Plane, Sphere };

/** @brief Maps a topology to its serialisation name. */
inline std::string_view map_topology_name(MapTopology topology) {
    return topology == MapTopology::Sphere ? "sphere" : "plane";
}

/** @brief Resolves a serialisation name back to a topology; unknown names are `Plane`. */
inline MapTopology map_topology_from_name(std::string_view name) {
    return name == "sphere" ? MapTopology::Sphere : MapTopology::Plane;
}

/**
 * @enum TerrainModel
 * @brief How the sampled ground surface is shaped between the cells' heights.
 *
 * Both models pass through every cell's own height, so the passes (rivers, biomes, water levels)
 * and everything derived from them are the same; they differ in the ground a consumer samples
 * (`MapGraph::elevation_at()`) -- what an elevation render draws and a 3D terrain stands on.
 */
enum class TerrainModel {
    /**
     * @brief Flat triangles between the cell sites, plus the `terrain_roughness` detail field.
     *
     * Faithful to the stored heights, and plain: sampled finely (a world thousands of samples
     * across), every slope is a plane and every contour a straight line.
     */
    Classic,
    /**
     * @brief A landscape: the cells' heights joined smoothly, then shaped by landform --
     *        ridged mountain ranges where the land is high, rolling hills on the lowland,
     *        flat-topped plateaus with cliff edges, all through a warped field so nothing
     *        lines up. See `NaturalTerrainConfig`.
     *
     * Kept consistent with the water: near every sea and lake the shaping may only raise
     * the ground, so no shore is ever pushed under the water beside it; rivers are fitted to
     * whatever ground this makes (their banks and channels are measured from it).
     */
    Natural
};

/** @brief Maps a terrain model to its serialisation name. */
inline std::string_view terrain_model_name(TerrainModel model) {
    return model == TerrainModel::Natural ? "natural" : "classic";
}

/** @brief Resolves a serialisation name back to a terrain model; unknown names are `Classic`. */
inline TerrainModel terrain_model_from_name(std::string_view name) {
    return name == "natural" ? TerrainModel::Natural : TerrainModel::Classic;
}

/**
 * @struct NaturalTerrainConfig
 * @brief The landforms `TerrainModel::Natural` shapes the ground into.
 *
 * Amplitudes are in normalised height (multiply by `elevation_range_m` for metres). Each landform
 * has its own noise field on `MapConfig` (`noise_hills`, `noise_ridge`, `noise_plateau`,
 * `noise_warp`), seeded from the master seed and scaled with the grid like the island field.
 */
struct NaturalTerrainConfig {
    /** @brief How smoothly the cells' heights are joined: 0 flat triangles, 1 fully rounded. */
    double smoothing = 1.0;
    /** @brief Rolling hills over the lowland. */
    double hills = 0.05;
    /** @brief Ridged mountain ranges: sharp crests and spurs where the land is high. */
    double mountains = 0.45;
    /** @brief Land height (0 the shore, 1 the summit) where mountain ranges begin. */
    double mountain_start = 0.22;
    /** @brief How high a plateau stands over the land around it. */
    double plateaus = 0.07;
    /** @brief Share of the lowland raised into plateaus, 0 to 1. */
    double plateau_coverage = 0.25;
    /** @brief How far the landform fields are pushed around, in grid units: bends ridges and valleys. */
    double warp = 1.5;

    // --- Under the water ---
    // The ground does not stop at the shore. Without these the submerged ground is the bare
    // interpolation of the cell heights, a hundred times flatter than the land beside it --
    // the shaping above fades out across the shoreline instead of stopping dead at it, so a
    // depth floor (keyed on `lake_depth_m`, see `NaturalTerrain::min_protected_depth`) keeps
    // that fade-out and the relief below from together raising a point out of water deeper
    // than the generator meant it to have.

    /**
     * @brief Rolling relief over the floor of every sea and lake: sandbanks, swells and hollows.
     *
     * Read from `MapConfig::noise_seabed`, which is a finer field than `noise_hills` -- a bed is
     * seen from a few metres away by anything swimming over it, not from a mountainside.
     */
    double seabed = 0.04;
    /**
     * @brief Ridges and gullies in deep water: banks, seamounts and the troughs between them.
     *
     * Faded in *below* the shelf rather than at it, so a shore shelves gently and only the open
     * floor is broken up. Read from the same ridged field as the mountains, at an offset.
     */
    double seabed_ridges = 0.07;
    /**
     * @brief Depth over which the bed shaping fades in from the shoreline, in normalised height.
     *
     * The shoreline is where the ground crosses the water's surface, and every pass downstream is
     * derived from where that falls -- so the shaping has to vanish there, exactly as the land
     * shaping vanishes at the shore from the other side. This is how wide that band is: at the
     * default 600 m elevation range, the first 30 m of water.
     */
    double seabed_shelf = 0.05;
};

/**
 * @enum MapShape
 * @brief The outline the landmass is confined to; everything outside it is sea.
 */
enum class MapShape {
    Rectangle,   /**< @brief Axis-aligned, `width_m` by `height_m`. */
    Circle,      /**< @brief `diameter_m` across. */
    Triangle,    /**< @brief Equilateral, `edge_length_m` a side, turned by `rotation`. */
    Continent,   /**< @brief One irregular landmass, `diameter_m` across on average. */
    Archipelago  /**< @brief `continent_count` irregular landmasses of varying size. */
};

/** @brief Number of distinct `MapShape` values. */
inline constexpr std::size_t k_map_shape_count = 5;

/**
 * @brief Maps a landmass shape to its serialisation name.
 * @param shape The shape to name.
 * @return A `snake_case` identifier, e.g. `"circle"`.
 */
inline std::string_view map_shape_name(MapShape shape) {
    switch (shape) {
        case MapShape::Rectangle:   return "rectangle";
        case MapShape::Circle:      return "circle";
        case MapShape::Triangle:    return "triangle";
        case MapShape::Continent:   return "continent";
        case MapShape::Archipelago: return "archipelago";
    }
    return "rectangle";
}

/**
 * @brief Resolves a serialisation name back to a shape.
 * @param name A name previously produced by `map_shape_name()`; `"rect"` is accepted too.
 * @return The matching shape, or `MapShape::Rectangle` if the name is unknown.
 */
inline MapShape map_shape_from_name(std::string_view name) {
    if (name == "circle")      return MapShape::Circle;
    if (name == "triangle")    return MapShape::Triangle;
    if (name == "continent")   return MapShape::Continent;
    if (name == "archipelago") return MapShape::Archipelago;
    return MapShape::Rectangle;
}

/**
 * @struct ShapeConfig
 * @brief The outline land is allowed to occupy, centred on the canvas.
 *
 * The canvas itself stays `grid_size` square whatever this says -- the shape is
 * inscribed in it, and the margin left over becomes open sea. So a smaller shape
 * is a smaller world on the same size of map, not a smaller image.
 *
 * **Every dimension defaults to zero, meaning "fill the canvas".** That makes the
 * default a rectangle the size of the whole grid, which is exactly what the
 * generator did before shapes existed, down to the byte.
 */
struct ShapeConfig {
    /** @brief Which outline to use. */
    MapShape shape = MapShape::Rectangle;
    /** @brief Rectangle width in metres; 0 spans the canvas. */
    double width_m = 0.0;
    /** @brief Rectangle height in metres; 0 spans the canvas. */
    double height_m = 0.0;
    /** @brief Circle diameter in metres; 0 spans the canvas. */
    double diameter_m = 0.0;
    /** @brief Equilateral triangle side in metres; 0 spans the canvas. */
    double edge_length_m = 0.0;
    /** @brief Rotation in radians, applied about the centre. Triangle only. */
    double rotation = 0.0;
    /**
     * @brief Mean diameter of one landmass in metres; 0 sizes it to the canvas.
     *
     * Its own dimension rather than a reuse of `diameter_m`, because the two are
     * not the same measurement: a circle's diameter is the whole world, while
     * this is the *average* of several landmasses that then vary about it. Wiring
     * them together would make an archipelago inherit whatever the circle happened
     * to be set to and fuse into one lumpy continent.
     *
     * `Continent` and `Archipelago` only. 0 means "sized to the canvas", which for
     * one continent is as wide as will fit and for an archipelago is landmasses
     * covering `k_archipelago_fill` of it between them.
     */
    double continent_size_m = 0.0;
    /**
     * @brief How many landmasses `Archipelago` attempts, at least 1.
     *
     * An upper bound, not a promise. Two landmasses placed close enough fuse
     * into one larger continent, which is what stops an archipelago reading as
     * a row of evenly spaced blobs.
     */
    int continent_count = 4;
    /**
     * @brief How far the outline of a landmass wanders from a circle, 0 to 0.6.
     *
     * Clamped to that range, so the radius never falls below 40% of the mean and
     * a landmass can never pinch itself in two. Zero gives a plain disc.
     * `Continent` and `Archipelago` only.
     */
    double irregularity = 0.35;
    /**
     * @brief Spread of landmass sizes about the mean in `Archipelago`, 0 to 0.9.
     *
     * At the default a continent is between 65% and 135% of `diameter_m`, so an
     * archipelago comes out as a couple of large landmasses and a scatter of
     * smaller ones rather than clones.
     */
    double size_variance = 0.35;
    /**
     * @brief Amplitude of the coastal warp, as a fraction of the mean radius.
     *
     * The radial outline alone is star-convex -- every ray from the centre
     * crosses the coast exactly once, so there are no fjords and no headland
     * that folds back on itself. Displacing the whole boundary by a noise field
     * breaks that, and is what produces peninsulas, inlets and the occasional
     * island lying off the coast. `Continent` and `Archipelago` only.
     */
    double coast_detail = 0.12;
};

/**
 * @struct TownConfig
 * @brief Controls settlement placement and the building footprints packed into each one.
 */
struct TownConfig {
    /** @brief Upper bound on settlements placed; fewer appear if the map lacks room. */
    int town_count = 12;
    /** @brief Minimum distance in grid units between two settlements. */
    double min_spacing_m = 360.0;
    /** @brief Rank cutoff: the first `capital_count` sites become capitals. */
    int capital_count = 1;
    /** @brief Rank cutoff: the sites after the capitals, up to this many, become towns. */
    int town_tier_count = 4;
    /** @brief Elevation above which a site is penalised for being hard to reach. */
    double elevation_penalty_start = 0.6;
    /** @brief Score subtracted per unit of elevation above `elevation_penalty_start`. */
    double elevation_penalty_scale = 2.0;
    /** @brief Score added when a site borders the ocean. */
    double coast_bonus = 0.35;
    /** @brief Score added when a site borders a river. */
    double river_bonus = 0.45;
    /** @brief Score added when a site borders a road. */
    double road_bonus = 0.2;
    /**
     * @brief Score added for a roomy cell, scaled by its area against the mean.
     *
     * Without it the highest-scoring site can be a cramped sliver, and the
     * capital ends up with fewer buildings than the towns beneath it -- a
     * settlement cannot grow past what its cell will hold.
     */
    double area_bonus = 0.5;
    /** @brief Magnitude of the seeded random jitter applied to each site score. */
    double score_jitter = 0.15;
    /**
     * @brief Cells a capital may claim, itself included.
     *
     * A settlement is not one Voronoi cell. At 60 m to the grid unit a cell is
     * about 3,600 m^2, which holds ten buildings of 10 m at a believable
     * density -- so a capital confined to its own cell is a hamlet, and the
     * three tiers become indistinguishable. Claiming neighbours is what lets a
     * capital be a town-sized thing while a village stays a village.
     */
    int capital_cells = 7;
    /** @brief Cells a town may claim, itself included. */
    int town_cells = 3;
    /** @brief Cells a village may claim; one, which is what makes it a village. */
    int village_cells = 1;
    /** @brief Buildings attempted in a capital across all its cells; lesser tiers get a fraction. */
    int buildings_per_town = 70;
    /** @brief Smallest side length of a square building footprint, in grid units. */
    double building_size_min_m = 7.0;
    /** @brief Largest side length of a square building footprint, in grid units. */
    double building_size_max_m = 14.0;
    /** @brief Fraction of `buildings_per_town` a town-tier settlement receives. */
    double town_building_scale = 0.7;
    /** @brief Fraction of `buildings_per_town` a village receives. */
    double village_building_scale = 0.4;
    /**
     * @brief Clear ground kept between a building and any water, in grid units.
     *
     * Added to a river's half-width, and applied on its own along a boundary
     * shared with a lake or the sea. Without it buildings stand in the channel:
     * rivers are used as streets, so the packer marches plots straight at them.
     */
    double water_clearance_m = 3.0;
    /**
     * @brief Clear ground kept between a building and any roadway, in grid units.
     *
     * The counterpart of `water_clearance_m`, and needed for the same reason: a
     * street is a line the packer aims plots at, so without a stated clearance it
     * marches them onto the carriageway. Added to half the roadway's width -- a
     * lane's `street_width_m`, or a road's own width by class -- so a trail and a
     * highway are each given the verge they actually need.
     */
    double street_clearance_m = 1.5;
    /** @brief Fewest occupants in a household. */
    int household_size_min = 3;
    /** @brief Most occupants in a household. */
    int household_size_max = 7;
    /** @brief Population density multiplier applied to a capital. */
    double capital_density = 1.6;
    /** @brief Population density multiplier applied to a town. */
    double town_density = 1.2;
    /**
     * @brief Nominal distance from a street's centreline to a plot centre, in metres.
     *
     * A *minimum*, not the whole story. A plot is also pushed out far enough that
     * its own rotated footprint clears the carriageway and its verge, because this
     * alone cannot: at 8 m a `building_size_max_m` plot of 14 m reaches back to 1 m
     * from the centreline, and the lane is 2 m of half-width, so the largest plots
     * were laid straight onto the street by construction.
     *
     * At the default a small plot still sits exactly here and only an oversized one
     * moves, which is what keeps the rows looking as they did.
     */
    double street_offset_m = 8.0;
    /** @brief Spacing along a street between successive plots. */
    double street_spacing_m = 14.0;
    /** @brief Random offset applied to a street-front plot, on both axes. */
    double position_jitter_m = 1.5;
    /** @brief Radians of yaw wobble about a street's bearing. */
    double rotation_jitter = 0.25;
    /** @brief Rejection draws before interior infill gives up on a cramped cell. */
    int infill_attempts = 200;
    /**
     * @brief Radius of a settlement's market square, in metres.
     *
     * Open ground at the heart of a town, which the packer keeps clear and the
     * civic buildings front onto. A settlement only gets one if the ground is
     * genuinely there -- see `plaza_min_cells` -- because a square carved out of a
     * hamlet is just a hamlet with a hole in it.
     */
    double plaza_radius_m = 26.0;
    /**
     * @brief Fewest claimed cells a settlement needs before it is granted a square.
     *
     * Two, so a capital and a town have one and a single-cell village does not.
     * That is the point of a village: it is the size that has no civic centre.
     */
    int plaza_min_cells = 2;
    /**
     * @brief Civic buildings a capital is given, beyond its dwellings.
     *
     * Taken in order from the roster in `PassTowns`, so raising this adds the next
     * kind rather than reshuffling the ones already there. Lesser tiers take a
     * prefix of the same list, which is what makes a town read as a smaller
     * version of a capital rather than a different kind of place.
     */
    int capital_civic_count = 7;
    /** @brief Civic buildings a town is given. */
    int town_civic_count = 4;
    /** @brief Civic buildings a village is given; a well, and perhaps a smithy. */
    int village_civic_count = 2;
    /**
     * @brief Width of a street where it is drawn, in metres.
     *
     * A lane, narrower than the `RoadClass::Trail` that reaches the settlement, so
     * the network and the streets inside it read as different things.
     */
    double street_width_m = 4.0;
};

/**
 * @struct RegionConfig
 * @brief Controls how the land is divided into countries and regions.
 */
struct RegionConfig {
    /** @brief Nations to place, terrain permitting. */
    int country_count = 5;
    /** @brief Provinces carved out of each country. */
    int regions_per_country = 3;
    /** @brief Minimum distance in grid units between two country seeds. */
    double min_country_spacing = 8.0;
    /**
     * @brief How strongly a height difference resists expansion.
     *
     * The reason borders land on ridges rather than cutting across them: a
     * claim spreads cheaply along a valley and dearly over a mountain.
     */
    double elevation_cost = 6.0;
    /**
     * @brief Cost of claiming across water.
     *
     * High but finite. Infinite would make every island its own nation; this
     * lets a country hold both banks of a strait while still stopping at an ocean.
     */
    double water_crossing_cost = 25.0;
};

/**
 * @struct RoadConfig
 * @brief Controls where roads run, how busy they get, and how they are drawn.
 *
 * Every cost here is a multiplier on the distance actually travelled, never a
 * flat number of hops, so the network does not change character when
 * `grid_size` does -- the same reasoning that made river widths physical
 * rather than pixel counts.
 */
struct RoadConfig {
    /** @brief Anchors the network is routed between; clamped to the land available. */
    int hub_count = 32;
    /**
     * @brief Minimum distance in grid units between two hubs.
     *
     * Matches `TownConfig::min_spacing_m` on purpose. Hubs are scored the way
     * settlement sites are, so spacing them the same way is what puts a road
     * through most of the towns the next pass but one goes on to place.
     */
    double hub_min_spacing_m = 360.0;
    /**
     * @brief Cost per unit of elevation climbed between two cells.
     *
     * The reason mountain roads switchback. Climbing straight up is dear and
     * traversing a slope is not, so a route crosses the contour at a shallow
     * angle and doubles back. Switchbacks emerge from the cost rather than
     * being drawn by construction.
     */
    double slope_cost = 9.0;
    /** @brief Cost per unit of absolute height; pushes a crossing onto the saddle. */
    double elevation_cost = 1.5;
    /** @brief Scales `1 - biome_habitability()` into a penalty for rough ground. */
    double rough_ground_cost = 2.0;
    /**
     * @brief Cost of crossing a volume-zero stream; a ford, not yet a bridge.
     *
     * Deliberately small. A headwater stream is a wet crossing and little more,
     * and pricing it like a bridge sends every route detouring to a river's
     * source -- which reads as a network that is frightened of water rather
     * than one that has learned where to cross it.
     */
    double ford_cost = 0.6;
    /** @brief Added to `ford_cost` per unit of river volume, so wide water is dear to span. */
    double bridge_cost_per_volume = 0.35;
    /**
     * @brief Cost of stepping into a lake or ocean cell.
     *
     * High but finite, and bounded in extent by `max_water_span`. Infinite
     * would strand every island with its own closed network; this lets a
     * causeway hop a narrow strait while leaving the open sea uncrossable --
     * the same bargain `RegionConfig::water_crossing_cost` already strikes for
     * a country holding both banks.
     */
    double water_crossing_cost = 30.0;
    /** @brief Consecutive water cells a crossing may span; a longer run is impassable. */
    int max_water_span = 2;
    /**
     * @brief Cost multiplier on an edge some earlier route already runs along.
     *
     * The whole reason the network has a shape rather than being a fan of
     * independent optimal paths. Routes are laid one at a time and each sees the
     * ground already built on as cheap, so a later route bends to join an
     * existing road instead of cutting its own parallel line a cell away --
     * which is how a trunk road forms, and why traffic concentrates enough for
     * a hierarchy to be worth reading off.
     */
    double reuse_discount = 0.40;
    /**
     * @brief Share of all routes that must pass along an edge for it to be a highway.
     *
     * A share and not a count, because a count cannot mean anything fixed: with
     * `n` hubs every pair is routed, so a dead-end spur carries exactly `n - 1`
     * routes and a trunk carries a large fraction of all `n(n-1)/2` of them.
     * Thresholds in absolute traffic would reclassify the entire map the moment
     * `hub_count` moved -- at 24 hubs a flat cutoff of 12 made highways of
     * 308 edges out of 322.
     */
    double highway_traffic_share = 0.30;
    /** @brief Share of all routes that makes an edge a road; below it, a trail. */
    double road_traffic_share = 0.10;
    /** @brief Chaikin corner-cutting passes applied to each road run. */
    int smoothing_iterations = 2;
};

/**
 * @struct LandmarkConfig
 * @brief Controls which notable places are found and how many.
 */
struct LandmarkConfig {
    /** @brief Cap on terrain features recorded. */
    int max_natural = 40;
    /** @brief Cap on ruins and abandoned works recorded. */
    int max_abandoned = 25;
    /** @brief Elevation at or above which a local summit counts as a peak. */
    double peak_elevation = 0.62;
    /** @brief Height difference across a river edge that makes it a waterfall. */
    double waterfall_drop = 0.055;
    /** @brief Elevation above which arid ground can be cut into a gorge. */
    double canyon_elevation = 0.45;
    /** @brief Connected fresh-water cells needed before a lake is "great". */
    std::size_t great_lake_cells = 8;
    /** @brief Distance a ruin must keep from any living settlement, in grid units. */
    double town_clearance = 2.5;
    /** @brief Numerator of the share of the natural budget any one kind may take. */
    int kind_share_numerator = 1;
    /** @brief Denominator of that share; 1/5 by default. */
    int kind_share_denominator = 5;
    /** @brief Numerator of the ocean-neighbour fraction that makes a cell a cape. */
    int cape_ocean_ratio_numerator = 3;
    /** @brief Denominator of the ocean-neighbour fraction that makes a cell a cape. */
    int cape_ocean_ratio_denominator = 2;
};

/**
 * @struct CaveConfig
 * @brief Where caves open, how far down they reach, and what shape they take.
 *
 * Every length here is in metres and every rate is dimensionless, so the numbers
 * describe caves rather than describing this generator's units. The two that
 * decide most of what a system looks like are `descent_grade`, which sets how
 * hard the entrance series falls, and the pair of branch chances, which is what
 * makes the top of a cave a single way down and the bottom of it a network.
 */
struct CaveConfig {
    /** @brief Systems to open; clamped to the qualifying slopes the map actually has. */
    int cave_count = 18;
    /**
     * @brief Rise over run a slope must reach before a cave may open on it, dimensionless.
     *
     * The whole of the placement rule. A cave mouth is a hole in a face of rock,
     * so the map's steepest ground is where one belongs -- measured by
     * `edge_grade()`, which puts every edge on the same footing whatever the
     * vertical and horizontal scales are set to. The default is a one-in-four
     * slope, steep enough to read as a face on a render and common enough that a
     * default map has far more candidates than `cave_count` asks for.
     *
     * Raising it concentrates caves in the mountains; lowering it lets them open
     * on rolling ground, where a mouth has no visible face to sit in.
     */
    double min_grade = 0.25;
    /** @brief How far apart two mouths must be, in metres. */
    double min_spacing_m = 300.0;
    /**
     * @brief Rock left between a passage ceiling and the ground above it, in metres.
     *
     * The margin the "a cave stays underground" invariant is enforced with, and
     * the reason it is an invariant rather than a tendency: every station is
     * clamped to this clearance as it is grown, and clamped again after the
     * passage is smoothed, because corner-cutting moves points.
     *
     * Measured against the surface `MapGraph::elevation_at()` returns *with*
     * terrain detail and river channels applied -- the one anyone can see -- not
     * against the control mesh. `river_channel_depth_m` alone cuts 18 m out of
     * that surface, so a clearance measured against the mesh is not the clearance
     * that exists.
     *
     * Against `elevation_at()` and not against the rendered elevation *layer*,
     * which is a slightly different surface for reasons that predate caves: the
     * sampler's answer depends on the cell it is given as a hint, and the layer
     * draws a pixel with whichever cell's subdivided outline contains it. With
     * caves off, 5.7% of drawn land pixels already differ from the sampler by more
     * than this clearance. The pass takes the lowest reading of the nearest cell
     * and its neighbours to cover the ambiguity, which leaves a handful of pixels
     * per map where a rounded stroke cap reaches between two clamped stations.
     */
    double roof_clearance_m = 25.0;
    /**
     * @brief Floor to ceiling in an ordinary passage, in metres.
     *
     * Worth choosing against `level_spacing_m` rather than by eye. That is the
     * drop between one storey and the next, so a passage taller than it reaches
     * into the level below and the two interpenetrate -- which is a cave that
     * cannot be walked and cannot be meshed. `chamber_height_m` is the binding
     * one of the pair, being the larger.
     */
    double passage_height_m = 8.0;
    /** @brief Floor to ceiling in a chamber, in metres; a room has headroom a passage does not. */
    double chamber_height_m = 20.0;
    /**
     * @brief Hard cap on how far below its own mouth a system may reach, in metres.
     *
     * A bound, not the usual constraint: `vadose_share` is what normally decides
     * where a cave levels out. This is here so an unusually high mouth over an
     * unusually deep base level cannot grow a system that falls further than the
     * configuration ever meant to allow.
     */
    double max_depth_m = 260.0;
    /**
     * @brief Share of the relief beneath a mouth that is entrance series, 0 to 1.
     *
     * Where the water table goes, and therefore where a system stops descending
     * and starts branching. A water table is a *subdued replica of the surface*,
     * not a flat sheet at sea level -- rain falls on the hill and drains toward
     * the valleys either side, so it stands well above base level under high
     * ground and meets it at the coast. Taking a share of the relief between the
     * mouth and the sea says exactly that, and says it without needing a second
     * field to be computed and stored.
     *
     * It also decides what a cave *looks* like, which is why it is not simply
     * `max_depth_m`. Measured against the sea alone a cave 400 m up needs some
     * 250 m of descent before it can level out -- further than the length budget
     * reaches -- so every system came out as pure entrance series and the
     * phreatic half of the model never appeared on a map at all. At the default
     * share both halves show on every cave that has relief to spare.
     *
     * Zero puts the water table at the mouth, so a system is level throughout;
     * one puts it at sea level and restores the pure-descent behaviour.
     */
    double vadose_share = 0.6;
    /**
     * @brief Drop from one abandoned water table to the next, in metres.
     *
     * What makes a cave multi-level, and it is not a stylistic choice: a valley
     * downcuts, the water table follows it down, and the phreatic network cut at
     * the higher level is left behind as dry passage above the new one. Do that
     * twice and the system has three storeys with rock between them, which is
     * exactly the structure the exported floor and roof layers exist to carry.
     *
     * The constraint that matters is against `chamber_height_m`, not against
     * anything on the surface. Two tables closer together than the tallest space
     * cut at either of them produce storeys that intersect where they cross, which
     * is not a multi-level cave but one malformed one -- there is no rock left
     * between the levels to stand on. The default leaves most of a chamber's
     * height again in rock between one storey's ceiling and the one above it.
     *
     * It is also what decides how many storeys a map's caves actually get, and the
     * relationship is not gentle: a system needs `level_spacing_m` of relief to
     * spend per extra table, so doubling this roughly halves the storey count. At
     * 55 m against the default terrain only six systems in seventeen reached a
     * second table; at 38 m most do.
     */
    double level_spacing_m = 38.0;
    /**
     * @brief Cap on abandoned levels per system.
     *
     * A bound rather than the usual constraint, in the same spirit as
     * `max_depth_m`: how many levels a cave actually grows comes from the relief
     * beneath its mouth divided by `level_spacing_m`, so a mouth high above base
     * level grows several and one near the coast grows one. This stops an
     * exceptionally high mouth from spending the whole node budget on storeys and
     * leaving each of them a stub.
     */
    int max_levels = 4;
    /**
     * @brief Share of the trunk length budget a newly-opened level inherits, 0 to 1.
     *
     * The counterpart of `branch_budget`, and deliberately larger. A branch is a
     * side passage off a network and should be shorter than what it leaves; a new
     * level is a whole network in its own right and wants most of a trunk's worth
     * of length, or the lower storeys come out as stubs hanging off the shafts
     * that reach them.
     *
     * This buys *network*, not descent. The climb down to the next table is added
     * on top of it, because it is overhead rather than passage anyone explores:
     * charged against the same budget, a descent at the default grade spends
     * `level_spacing_m / descent_grade` -- some 250 m of the 630 m it was given --
     * before it reaches the level it was sent to dig.
     */
    double level_budget = 0.7;
    /** @brief Distance advanced per station, in metres. */
    double step_m = 30.0;
    /** @brief Length budget of the trunk passage, in metres; branches spend from it. */
    double passage_length_m = 900.0;
    /**
     * @brief How steeply the passage falls above the water table, dimensionless.
     *
     * Water above the water table is falling under gravity and cuts downward, so
     * this is what makes an entrance series an entrance series. Below it the
     * passage runs level and this has no effect.
     */
    double descent_grade = 0.15;
    /**
     * @brief How hard a passage steers toward thicker rock, 0 to 1.
     *
     * The single thing that makes a system reach anywhere. A passage ignoring the
     * ground above it wanders out from under its own hill within a few hundred
     * metres, at which point the roof clamp has nowhere left to put it and the
     * head ends -- so caves came out as stubs, which is the one shape a cave
     * system is not.
     *
     * Real caves are not shy of the hillside by accident: a passage only exists
     * where there was rock to dissolve and a head of water to do it, and both are
     * greatest under the mass of the massif. So each station probes the surface a
     * step ahead to either side and leans toward the higher one, which is the same
     * thing said in one line of arithmetic.
     *
     * Zero bores on regardless and produces the stubs. One turns as hard as
     * `meander` does, which pins a passage to the ridge line above it.
     */
    double massif_bias = 0.6;
    /**
     * @brief How far a passage wanders from a straight line, 0 to 1.
     *
     * Applied through a coherent noise field rather than a fresh draw per step,
     * for the same reason `noise_relief` is coherent: a random turn at every
     * station is a drunkard's walk, and no passage ever cut looks like one. Zero
     * bores dead straight.
     */
    double meander = 0.5;
    /** @brief Chance a station above the water table also starts a branch. */
    double branch_chance_vadose = 0.06;
    /**
     * @brief Chance a station at or below the water table also starts a branch.
     *
     * Deliberately several times `branch_chance_vadose`. Water moving sideways
     * through rock already saturated attacks every joint it meets rather than one
     * line of them, which is why maze cave is a thing that happens at the water
     * table and not above it. Equal chances give a cave that is the same shape
     * top to bottom, which is the shape no cave is.
     */
    double branch_chance_phreatic = 0.26;
    /** @brief Share of the parent's remaining length budget a branch inherits, 0 to 1. */
    double branch_budget = 0.55;
    /** @brief Cap on branches per system, so one cave cannot swallow the node budget. */
    int max_branches = 6;
    /**
     * @brief Cap on stations per system; the hard bound on how long growth can run.
     *
     * Shared across every level a system grows, so it is divided rather than
     * spent once. At 400 a three-level cave gets some 130 stations a storey and
     * reads as three stubs stacked up, which is why this is generous.
     */
    int max_nodes = 900;
    /** @brief Width of an ordinary passage, in metres. */
    double passage_width_m = 6.0;
    /** @brief Half-width of a chamber, in metres. */
    double chamber_radius_m = 18.0;
    /**
     * @brief Chance a station above the water table is a vertical pitch instead.
     *
     * What puts a drop in the middle of an otherwise walkable cave. A pitch
     * advances almost nothing in plan and falls `shaft_drop_m`, so it is the one
     * thing that can take a system deep without taking it far.
     */
    double shaft_chance = 0.10;
    /** @brief How far a pitch falls, in metres. */
    double shaft_drop_m = 45.0;
    /** @brief Chaikin corner-cutting passes applied to each passage, matching rivers and roads. */
    int smoothing_iterations = 2;
};

/**
 * @struct MapConfig
 * @brief The complete description of a map to generate.
 *
 * A `MapConfig` plus its `seed` fully determines the output: every pass draws
 * from a generator seeded from this struct, so two runs with equal configs
 * produce byte-identical maps.
 */
struct MapConfig {
    // --- Sampling ---

    /** @brief Cells per axis; the point set is `(grid_size + 1)^2` plus a boundary ring. */
    int grid_size = 50;
    /** @brief Plane (the square map) or sphere (a globe); see `MapTopology`. */
    MapTopology topology = MapTopology::Plane;
    /** @brief Sphere only: how many cells cover the globe; 0 means `(grid_size + 1)^2`. */
    int sphere_cells = 0;
    /** @brief How far a grid point may wander from its lattice position, in grid units. */
    double jitter = 0.5;
    /** @brief Master seed; every pass derives its generator from this. */
    int seed = 251;
    /** @brief Width of the band around the map edge whose cells are forced to border. */
    double border_length = 1.0;

    // --- World scale ---

    /**
     * @brief Side length in metres of one grid unit.
     *
     * The number that makes every other size in this struct mean something. A
     * cell is roughly one grid unit across, so this is also about how big a cell
     * is on the ground: at 60 m a cell holds a cluster of ten or so buildings,
     * which is what a village is, and `grid_size = 80` gives a 4.8 km world.
     *
     * Without it grid units would be abstract and every physical size would be
     * tuned by eye against a render resolution, with nothing in the generator
     * able to say how wide a road is.
     */
    double meters_per_grid_unit = 60.0;

    /**
     * @brief Metres of height the normalised `[0, 1]` elevation field spans.
     *
     * The vertical scale. `meters_per_grid_unit` fixes how far a grid unit
     * reaches *across* the ground; without this height would be a bare fraction,
     * and "a river one metre deep" would have nowhere to land. With this, the
     * sea floor is at 0 m, the waterline at `sea_level * elevation_range_m`, and
     * the highest peak at the full range.
     *
     * 600 m over a 4.8 km world puts the steepest ground at roughly a 1-in-8
     * grade, which is hilly without being alpine.
     */
    double elevation_range_m = 600.0;

    // --- Rendering ---

    /**
     * @brief Ground covered by one rendered pixel, in metres.
     *
     * **Always 1.0 by default**, and deliberately so: a PNG is then a literal
     * one-pixel-per-metre map, and a pixel count read off a render *is* a
     * measurement -- a 6 m road is 6 px wide because it is 6 m wide. Every
     * physical size in this struct is denominated in metres on that
     * understanding, so 1.0 is the setting under which a render can be measured
     * rather than merely looked at.
     *
     * Raising it renders the same world smaller and faster. Setting
     * `image_size` instead back-computes this, which is the same knob from the
     * other end -- see `image_size` for which wins.
     */
    double meters_per_pixel = 1.0;

    /**
     * @brief How the composite layer lights the biome colours.
     *
     * Elevation by default. Hillshading draws a more sculptural picture, but it
     * is a function of slope rather than height, so it answers "which way does
     * this face turn" and not "how high is this" -- and on a map whose point is
     * the terrain underneath it, the second question is usually the one being
     * asked.
     */
    CompositeShading composite_shading = CompositeShading::Elevation;
    /**
     * @brief How the height field is drawn between the cells it is stored at.
     *
     * `Flat` gives every cell one height and a hard edge at every boundary -- the
     * stored field shown without interpolation -- with the river channels still cut
     * through it. The cut is a function of position rather than of the
     * interpolation, so it subtracts from a constant as readily as from a gradient:
     * measured, 11.6 grey levels of contrast half a cell from a centreline, against
     * 12.3 for the interpolated surface. River valleys are there too, carved into
     * the cell heights themselves, and come out as stepped cells rather than a
     * smooth trough.
     *
     * Terrain roughness is the one thing `Flat` drops, and deliberately: that is
     * surface texture, which a fill constant across a cell has no business
     * carrying, where a river is a feature of the ground.
     */
    ElevationSurface elevation_surface = ElevationSurface::Interpolated;
    /**
     * @brief How wide `ElevationSurface::Blended` blurs, as a fraction of a cell.
     *
     * The radius of the box blur that mode runs over the finished raster, in cells
     * -- so 0.5 is half a cell at any render size, and one number means the same
     * thing at 512 px as at 4800. At 0 there is no blur and the mode is `Flat` byte
     * for byte. One cell is the ceiling, because past there the blur is wider than
     * the tessellation and starts erasing the landforms along with the facets.
     *
     * Measured at 512 px: the 99th-percentile adjacent-pixel step falls from 34 grey
     * levels flat to 12, 8 and 6 at a quarter, half and a full cell. The worst step
     * stays near 75 throughout and is meant to -- that is a river bank, cut back in
     * after the blur.
     *
     * **Not one of the `elevation_smoothing_*` knobs, and not a substitute for
     * them.** Those relax the heights the graph *stores*, which is the shape of the
     * landform everything else is classified from. This changes only how the drawn
     * surface gets from one stored height to the next, and nothing downstream of the
     * renderer can tell it was set.
     */
    double elevation_blend = 0.5;
    /**
     * @brief How much `elevation_blend` varies from cell to cell, 0 to 1.
     *
     * A single blur radius smooths everywhere equally, which is the one thing real
     * ground never does. This gives **every cell its own radius**, drawn from
     * `noise_blend` at the cell's own site, so one cell keeps hard edges while the
     * cell beside it is fully smoothed. `noise_blend.frequency` controls how those
     * cells are distributed -- per-cell random through to broad patches.
     *
     * The factor is `1 + variation * n` for a field value `n` in `[-1, 1]`, so the
     * variation is **symmetric about `elevation_blend`** -- the measured mean factor
     * is 0.999, so the average radius is where `elevation_blend` put it and turning
     * this up makes a map more varied rather than uniformly softer or sharper.
     *
     * At 0 the field is not consulted at all -- move its seed or its frequency and
     * the render does not shift by a byte. At 1 the factor is nominally `[0, 2]`,
     * though a noise field seldom reaches its extremes.
     *
     * Note the top of that range can exceed the one-cell ceiling `elevation_blend`
     * is capped at, and deliberately: asking for full variation is asking for some
     * cells smoothed past a cell width, and clamping would quietly make the knob
     * one-sided.
     *
     * What it buys, measured at 900 px with `elevation_blend = 0.5` as the spread
     * of each cell's own edge sharpness against an unvaried render: standard
     * deviation 0.154 at variation 0.5 and 0.294 at 1, and adjacent cells landing
     * on opposite sides of the uniform blur across 3.5% of shared edges -- against
     * 0.3% for a low-frequency field (0.045, last row of the table below), which is the difference
     * between variation you can see and variation you cannot.
     *
     * **It scales `elevation_blend`, so a small blend leaves little to vary.** At
     * `elevation_blend = 0.1` the radius is 3 to 4 px on a 60 px cell and
     * neighbouring cells differ correspondingly little.
     *
     * Costs two extra blurs of the raster and one polygon pass, no matter how high
     * it is set: the elevation layer goes from 1.20 s to 2.35 s at 4800 px.
     */
    double elevation_blend_variation = 0.0;

    /**
     * @brief Deflate effort used when encoding a PNG; higher is smaller and slower.
     *
     * stb's own default is 8. Encoding is the dominant cost of an export -- more
     * than half the runtime of a default map -- so this is worth reaching for
     * when a run is a preview rather than an artefact. It changes file size only;
     * the pixels are identical at every level.
     */
    int png_compression_level = 8;

    /**
     * @brief Side length in pixels of the square PNG renders.
     *
     * This and `meters_per_pixel` are the two ends of one knob, and they must
     * never disagree: every feature is stroked in *metres* and converted through
     * the scale, so a stale pairing would draw a "10 m highway" at whatever width
     * the wrong scale implied. The invariant is
     * `image_size * meters_per_pixel == grid_size * meters_per_grid_unit`.
     *
     * Either end may be the one you set. Naming a resolution is the more concrete
     * statement of intent, so `image_size` wins where both are given and
     * `meters_per_pixel` is back-computed from it -- `apply_config_node()` does
     * that on load and `--image-size` does it on the command line. Say nothing
     * and `derive_image_size()` fills this in from the scale.
     *
     * The default is `derive_image_size()` of the other defaults rather than a
     * round number, so a default-constructed config already satisfies the
     * invariant instead of starting out contradicting itself.
     */
    int image_size = 3000;
    /** @brief Tint cells by the region that claims them, so borders are visible. */
    bool show_regions = true;
    /** @brief How strongly the region tint is mixed over the biome colour, in `[0, 1]`. */
    float region_tint = 0.13f;
    /**
     * @brief Whether landmark marks are painted into the `Landmarks` and `Composite` layers.
     *
     * Render-only, like `show_regions` beside it: no pass reads it, and nothing downstream of
     * the renderer can tell it was set. It does **not** affect generation -- `graph.landmarks`
     * is populated either way. `enable_landmarks` is the setting that decides whether a map has
     * landmarks at all, and the two are not interchangeable: turning that one off leaves nothing
     * to draw *or* to look up.
     *
     * Off by default because a landmark is a label, not terrain. Baking a diamond into the
     * picture fixes its size to whatever the render happened to be and gives a viewer no way to
     * ask what it is; an interactive overlay can place a marker that stays legible at any zoom
     * and carries the name. The map viewer (examples/map_viewer.cpp) draws them that way.
     *
     * Setting it true restores the previous output byte for byte. Town marks and cave-mouth
     * rings are unaffected either way -- they are still painted, so the `Landmarks` layer keeps
     * its meaning rather than becoming an empty image.
     */
    bool draw_landmark_marks = false;

    // --- Pass parameters ---

    /** @brief The island noise field sampled by the water pass. */
    NoiseConfig noise_island;
    /** @brief The variation field that keeps isotherms from running straight. */
    NoiseConfig noise_temperature{1733, 0.035};
    /**
     * @brief The large-scale field that decides where the high ground goes.
     *
     * Low frequency, because this shapes massifs rather than texture: a feature
     * spans many cells. Compare `noise_terrain`, which is deliberately finer
     * than a single cell.
     */
    NoiseConfig noise_relief{7717, 0.055};
    /**
     * @brief The field that warps the coastline of the organic landmass shapes.
     *
     * Only read by `MapShape::Continent` and `MapShape::Archipelago`. Low
     * frequency on purpose: this bends the *outline* of a continent, so a
     * feature is a bay or a headland spanning many cells. The island field
     * carves the detail inside whatever silhouette this leaves.
     */
    NoiseConfig noise_shape{5233, 0.03};
    /**
     * @brief How much fractal relief reshapes the coast-distance height field, 0 to 1.
     *
     * Height is breadth-first *distance from the coast*, which is what keeps
     * coastlines at sea level and puts mountains inland -- but taken alone it
     * makes the high ground the literal medial axis of the landmass, so peaks
     * trace thin winding ridges equidistant from the bays either side. Real
     * terrain does not do that.
     *
     * This multiplies the distance field by a noise factor before the heights are
     * rank-remapped, which reorders them: some of the skeleton drops into
     * valleys, some of the flanks rise, and what comes out reads as massifs and
     * basins. It is applied to *land* corners only, so the ordering of water
     * against land is untouched and rivers still run downhill to a coast that is
     * still the lowest ground there is.
     *
     * Defaults to 0.65, where the skeleton is gone but the coast-distance trend
     * still reads -- land still broadly rises inland. Zero restores the pure
     * distance field, which is what Amit Patel's original generator produced and
     * what this looked like before.
     */
    double terrain_relief = 0.65;
    /**
     * @brief The detail field displacing the sampled ground surface.
     *
     * A higher frequency than the island or temperature fields on purpose: this
     * is texture within a cell, not a landform. Cells are 60 m across at the
     * default scale, so the detail has to be finer than that to show at all.
     */
    NoiseConfig noise_terrain{4919, 0.35};
    /**
     * @brief The field that varies the blur radius under `ElevationSurface::Blended`.
     *
     * Only read when `elevation_blend_variation` is above 0, and sampled **once per
     * cell, at the cell's site** -- so `frequency` here means something unusually
     * concrete. Sites sit one grid unit apart, and a grid unit is a cell, so the
     * field's wavelength `1 / frequency` *is* the patch size in cells:
     *
     * This is the knob for how the variation is *distributed*, where
     * `elevation_blend_variation` is how strong it is. Measured as the correlation
     * between the factors of two cells that share an edge:
     *
     * | `frequency` | wavelength | neighbour correlation | reads as |
     * |---|---|---|---|
     * | 0.5 *(default)* | 2 cells | -0.01 | every cell independent |
     * | 0.33 | 3 cells | 0.14 | mostly independent |
     * | 0.25 | 4 cells | 0.35 | loose clumps |
     * | 0.2 | 5 cells | 0.51 | clear patches |
     * | 0.125 | 8 cells | 0.78 | broad regions |
     * | 0.045 | 22 cells | **0.97** | no visible variation |
     *
     * Note the knee: anything at or above 0.5 is already fully decorrelated, so
     * raising it further buys nothing. The interesting range is 0.5 down to 0.125.
     *
     * That last row is the trap. A frequency like 0.045 follows the theory that
     * smoothness should vary the way bedrock hardness does -- a defensible
     * idea that produces nothing anyone can see, because at a correlation of 0.97
     * every cell in any neighbourhood gets the same factor and the map comes out
     * uniformly blurred. What varies has to vary at the scale of the thing it
     * varies.
     *
     * One octave, not the usual five, and that matters twice. Octaves above the
     * first sit below cell size, so point-sampling one value per cell picks up
     * near-white noise from them whatever the base frequency says -- which blurs
     * the distinction the table draws. And FBm's gain leaves the base octave only
     * about two thirds of the amplitude, so dropping the rest measurably widens the
     * effect: the spread of per-cell edge sharpness goes from 0.213 to 0.294, and
     * adjacent cells land on opposite sides of the uniform blur half again as often
     * (2.3% of shared edges to 3.5%).
     *
     * Not rescaled by `grid_size`, unlike `noise_island`: a patch measured in cells
     * should be the same number of cells on any map.
     */
    NoiseConfig noise_blend{3571, 0.5, FastNoiseLite::NoiseType_OpenSimplex2,
                            FastNoiseLite::FractalType_FBm, 1};
    /**
     * @brief The field a cave passage takes its wandering from.
     *
     * The frequency is set against `CaveConfig::step_m`, not against the landforms
     * the other fields shape, and that is the whole of why it is this high. A
     * station advances half a grid unit; sampled at a landform frequency the field
     * barely changes over that, so every station turns by very nearly the same
     * amount as the last -- which is the definition of a circle, and is what the
     * passages came out as. At this frequency a step crosses a fifth of a period,
     * which wanders smoothly over a handful of stations and never closes a loop.
     *
     * Sampled at the station's own position, so two passages of one system that
     * pass near each other bend alike -- which is what a shared joint in the rock
     * would do.
     */
    NoiseConfig noise_cave{2011, 0.45};
    /**
     * @brief Which ground surface `MapGraph::elevation_at()` samples; see `TerrainModel`.
     */
    TerrainModel terrain_model = TerrainModel::Classic;
    /** @brief The landforms of `TerrainModel::Natural`. */
    NaturalTerrainConfig natural;
    /** @brief Natural terrain's rolling hills (frequency at 40 cells, scaled with the grid). */
    NoiseConfig noise_hills{6151, 0.3};
    /** @brief Natural terrain's mountain ranges: ridged, so crests come out sharp. */
    NoiseConfig noise_ridge{6871, 0.16, FastNoiseLite::NoiseType_OpenSimplex2, FastNoiseLite::FractalType_Ridged, 5,
                            2.0, 0.5, 0.6};
    /** @brief Natural terrain's plateau mask: broad, low-frequency patches. */
    NoiseConfig noise_plateau{7297, 0.09, FastNoiseLite::NoiseType_OpenSimplex2, FastNoiseLite::FractalType_FBm, 3};
    /** @brief Natural terrain's domain warp: what bends the other fields. */
    NoiseConfig noise_warp{8461, 0.2, FastNoiseLite::NoiseType_OpenSimplex2, FastNoiseLite::FractalType_FBm, 3};
    /**
     * @brief Natural terrain's bed relief, under every sea and lake.
     *
     * Three times the frequency of `noise_hills` and the same five octaves, so the floor carries
     * detail down to a few metres. A bed is read from just above it -- a swimmer's distance --
     * where a hillside is read from across a valley, so the two want different scales.
     */
    NoiseConfig noise_seabed{9173, 0.9};
    /**
     * @brief How strongly the detail field displaces the sampled surface, 0 to 1.
     *
     * Zero -- the default -- leaves the surface exactly as the passes computed
     * it. Raising it adds jaggedness *without* touching the cell and corner
     * heights, so biomes, rivers and roads are classified on the same smooth
     * control field either way and only the sampled surface roughens. The
     * displacement is scaled by the local height, so coastlines stay at sea level
     * however high this goes.
     *
     * Pair it with `elevation_smoothing_iterations`: that controls how smooth the
     * underlying landform is, this controls how rough the skin over it is.
     */
    double terrain_roughness = 0.0;
    /**
     * @brief How much a full unit of elevation cools the air.
     *
     * At the default a mountain top is roughly half a climate band colder than
     * the lowland on the same latitude, which is what puts snow on equatorial
     * peaks.
     */
    double temperature_lapse_rate = 0.40;
    /**
     * @brief Shapes the latitude curve between the polar cap and the equator.
     *
     * A straight ramp (1.0) makes most of the map cold once the lapse rate is
     * subtracted -- the mean latitude term is only 0.5 before altitude takes its
     * cut. Values above 1 hold the temperate band wide across the middle and
     * push the drop out toward the caps, which is how a real world is
     * distributed.
     *
     * It shapes the *temperate* half of the curve only. Where the frozen ground
     * begins is set outright by `polar_extent_north` / `polar_extent_south`, not
     * left to emerge from this exponent.
     */
    double temperature_falloff = 1.7;
    /**
     * @brief Shifts the whole world warmer or colder, before the range clamp.
     *
     * The knob the climate had no way of expressing: latitude said where the cold
     * went and the lapse rate said how much height cost, but nothing said how warm
     * the world *is*. Added to every sample, so an ice age and a hothouse are one
     * number apart on the same map.
     *
     * Roughly -0.5 to +0.5 is useful; beyond that the clamp flattens whole
     * hemispheres to one value. Zero changes nothing.
     */
    double temperature_offset = 0.0;
    /**
     * @brief Fraction of the map at the `y = 0` edge that is frozen, 0 to 0.5.
     *
     * The polar cap, stated as a share of the map rather than inferred from an
     * exponent. Inside it the curve falls from freezing at the cap's inner edge to
     * nothing at the pole; outside it the curve climbs from freezing to full warmth
     * at the equator, shaped by `temperature_falloff`.
     *
     * **Zero means no polar region on this side.** The cap vanishes and the curve
     * spans freezing to equatorial across the whole hemisphere, so latitude alone
     * never selects ice, glacier or cold desert. Altitude still can, which is
     * right: a mountain is cold at any latitude.
     *
     * The default matches the cap a single power curve would produce -- at a
     * falloff of 1.7 the band falls below freezing beyond `0.8^(1/1.7)` of the way
     * to the pole, which is the outer 6.5% of the map.
     */
    double polar_extent_north = 0.065;
    /** @brief Fraction of the map at the `y = grid_size` edge that is frozen, 0 to 0.5. */
    double polar_extent_south = 0.065;
    /**
     * @brief Laplacian relaxation passes applied to the height field.
     *
     * Distance-from-coast elevation is terraced: a quarter of the full height
     * range could fall between two adjacent cells. Each pass pulls a corner toward
     * the mean of its neighbours, and then a cell toward the mean of its
     * neighbours, so what this relaxes is the *stored* field -- the shape of the
     * landform, and what biomes and routing are classified from.
     *
     * It is not what decides how smooth a rendered map looks, and setting it to 0
     * does not produce facets. Two things smooth the picture regardless:
     * `assign_center_elevations_()` always gives a cell the mean of its corners --
     * which span some 70 m -- and `MapGraph::elevation_at()` interpolates between
     * cell sites, so the drawn surface is continuous by construction. At one pixel
     * to the metre a 60 m cell is 60 pixels across, and a step between two cells is
     * spread over all of them.
     *
     * `MapConfig::elevation_surface` is the setting that turns the interpolation
     * off.
     */
    int elevation_smoothing_iterations = 6;
    /** @brief How far toward the neighbour mean each smoothing pass moves a corner. */
    double elevation_smoothing_strength = 0.5;
    /**
     * @brief The waterline, in the normalised `[0, 1]` height field.
     *
     * A real height, not a convention. The sea bed occupies everything below it
     * and land everything above, so "the ground here is under water" is an
     * honest comparison rather than a vacuous one, as it would be if the whole
     * field started at zero and the sea were pinned to the bottom of it.
     *
     * Two consequences worth knowing. The elevation layer carries genuine
     * bathymetry, dark where the sea is deep. And the sea's surface is a visible
     * mid-grey on the same scale as everything else, rather than a black that
     * would be indistinguishable from dry land.
     *
     * Land therefore spans `[sea_level, 1]`, so anything comparing against a
     * *land* height must go through `land_height()` first.
     */
    double sea_level = 0.25;
    /** @brief Noise value above which a corner is water. */
    double threshold_water = 0.3;
    /** @brief A cell becomes water once more than this many of its corners are. */
    int threshold_water_count = 2;
    /** @brief Number of river sources attempted. */
    int river_count = 25;
    /**
     * @brief Corners a watercourse must run through before it counts as a river.
     *
     * Most land is close to a coast, so a source drawn uniformly usually sits a
     * couple of cells from the sea and produces a trickle. Rejecting the short
     * ones and drawing again is what leaves rivers that actually cross the map.
     */
    int river_min_length = 6;
    /**
     * @brief Lowest elevation a river may start from.
     *
     * The other half of keeping rivers long: a low floor (0.3, say) lets a source
     * appear on the coastal plain it is meant to run down to.
     */
    double river_source_min_elevation = 0.45;
    /** @brief Highest elevation a river may start from. */
    double river_source_max_elevation = 0.9;
    /** @brief Chaikin corner-cutting passes applied to each traced watercourse. */
    int river_smoothing_iterations = 2;

    /**
     * @brief Width of a volume-zero stream, in metres.
     *
     * Widths are physical, not pixel counts. A pixel width would make a river's
     * width depend on the render resolution -- the same map at 2048 would have
     * rivers half as wide in world terms as at 1024 -- and would leave the town
     * packer no resolution-independent number to keep buildings out of the
     * channel.
     */
    double river_width_base_m = 5.0;
    /** @brief Additional width per unit of river volume, in metres. */
    double river_width_per_volume_m = 2.0;
    /**
     * @brief How far a river's surface sits above the ground it runs over, in metres.
     *
     * A river is not a line painted on the terrain, it is water standing in a
     * channel -- so its surface has to be *above* the ground, or a mesh built
     * from the two fights with itself along every watercourse. Deepens with
     * volume: a stream is ankle-deep and a trunk river is not.
     *
     * Above the ground **as the elevation layer draws it** -- `river_surface_at()`
     * measures from `MapGraph::elevation_at()`, not from `MapCorner::elevation`.
     * The two are different surfaces, by some 22 m at a river corner, and while
     * this was measured from the corner heights the sheet was drawn below the
     * terrain along nearly half of every watercourse.
     *
     * Complementary to `river_incision_m` and `river_channel_depth_m`, not a
     * duplicate of either: incision cuts the valley, the channel depth cuts the
     * bed within it, and this floats the sheet of water above that bed. A metre
     * of freeboard, twenty of channel and sixty of valley are all true of the
     * same river.
     */
    double river_depth_m = 1.0;
    /** @brief Additional depth per unit of river volume, in metres. */
    double river_depth_per_volume_m = 0.35;
    /**
     * @brief How deep the channel is cut into the sampled ground, in metres.
     *
     * The third and last of the river depths, and the only one that puts a
     * watercourse in the *picture* of the terrain. Worth stating what each does,
     * because three depths on one river invites the wrong guess:
     *
     * - `river_incision_m` carves the **valley** into the control mesh -- corner
     *   and cell heights. A landform, hundreds of metres across.
     * - This cuts the **channel** into the surface sampled *between* those
     *   heights. A few metres across, and the only one fine enough to read as a
     *   river rather than as a dip in the ground.
     * - `river_depth_m` floats the **water sheet** above the control mesh, which
     *   is what stops a mesh built from ground and water fighting itself.
     *
     * It has to live at sample time rather than in the control mesh because the
     * mesh cannot hold it: `elevation_at()` interpolates between cell sites some
     * 60 m apart, and a river is 5 to 20 m wide. Carved into cell heights the
     * best achievable was a 500 m depression with no edge -- measurably deep,
     * invisible to look at. Same reason `terrain_roughness` lives here.
     *
     * Zero leaves the sampled surface exactly as the control mesh describes it.
     */
    double river_channel_depth_m = 18.0;
    /**
     * @brief Additional channel depth per unit of river volume, in metres.
     *
     * The channel widens with volume through `river_width()` whether this is set
     * or not; this is what also makes it deepen, so a trunk river reads as a
     * trench and a headwater stream as a scratch.
     */
    double river_channel_depth_per_volume_m = 4.0;
    /**
     * @brief How far upstream a river's surface transitions to meet its mouth, in metres.
     *
     * Over this last stretch two things change together: the sheet settles from the
     * rim of its channel down into the bed, and it is drawn toward the level of the
     * water it empties into. Both are needed to arrive flush -- sampling the rim
     * holds a river 10 m above the sea at the median mouth, and without the pull
     * toward the body it lands wherever the ground happened to leave it.
     *
     * The pull is downward only, so a river entering a lake standing above it keeps
     * its own height rather than climbing to meet it.
     *
     * About four cells at the default scale: long enough to read as a descent rather
     * than a kink, short enough that the upland profile is untouched.
     */
    double river_mouth_blend_m = 250.0;
    /**
     * @brief How deep a river cuts the valley it runs in, in metres.
     *
     * Rivers erode. Without this the elevation field has no idea a river pass
     * ever ran -- elevation is computed before rivers are routed, so the height
     * field came out of the generator with a drainage network drawn on a surface
     * that has nowhere for the water to go, and the elevation layer showed no
     * trace of the rivers the water layer is full of.
     *
     * This is a *valley*, not a channel, and the difference is forced by the
     * geometry. The rendered surface interpolates cell-site heights over Delaunay
     * triangles, while rivers run along Voronoi edges -- cell *boundaries*. So the
     * narrowest thing the surface can express is about a cell across, 60 m at the
     * default scale, against a river 5 to 20 m wide. Which is the right answer
     * anyway: a river sits in a valley far wider than itself.
     *
     * Zero restores the uncarved height field exactly, which is what the
     * generator produced before valleys existed.
     */
    double river_incision_m = 60.0;
    /**
     * @brief Additional valley depth per unit of river volume, in metres.
     *
     * A trunk river has had far longer to cut than the stream feeding it, so the
     * valley deepens downstream on its own rather than needing to be authored.
     */
    double river_incision_per_volume_m = 12.0;
    /**
     * @brief How many corners out from the watercourse the valley opens.
     *
     * Rings of `MapCorner::adjacent`, so 1 is a trench with the river at the
     * bottom of it and nothing either side, and 2 gives it banks. Raising it
     * widens and gentles the valley rather than deepening it.
     */
    int river_valley_width = 2;
    /**
     * @brief Depth multiplier per ring outward from the watercourse, 0 to 1.
     *
     * What turns a step into a slope. At the default each ring is carved half as
     * deeply as the one inside it, so the valley wall grades away instead of
     * dropping vertically -- which matters for more than looks, since a cliff at
     * every watercourse would push the mean elevation step between neighbouring
     * cells past what `elevation_smoothing_iterations` is there to hold down.
     */
    double river_valley_falloff = 0.5;
    /**
     * @brief How far every water surface is extended past its own edge, in metres.
     *
     * So the water clips *into* the terrain rather than meeting it exactly. Two
     * surfaces that share an edge exactly will show a seam wherever the meshes
     * disagree by a rounding error, and along a coastline they always do.
     */
    double water_edge_overlap_m = 1.0;
    /**
     * @brief How deep a lake's bed lies under its surface at the shore, in metres.
     *
     * Lake cells are ranked with the land, so a cell the water pass flooded can carry a "bed"
     * well up a hillside. `assign_water_levels()` lowers every lake cell to at least this far
     * under the lake's spill level, which is what turns a flooded patch into a basin.
     *
     * The depth at the *rim*, not everywhere: the bed falls away from there at `lake_bed_grade`.
     */
    double lake_depth_m = 6.0;
    /**
     * @brief How steeply a lake's bed falls away from its shore, as rise over run.
     *
     * What makes a lake a basin rather than a pan. `lake_depth_m` on its own is one depth for the
     * whole body: every cell of a kilometre-wide lake is lowered to the same height and its bed
     * comes out exactly flat -- measurably so, zero spread on every lake of every seed. With a
     * grade, depth grows with the distance from the shoreline, so a tarn stays ankle-deep and a
     * wide lake has a middle worth sounding.
     *
     * Dimensionless and measured in real metres both ways (through `meters_per_grid_unit`), like
     * `edge_grade()`, so the default means a one-in-eight bed whatever the world's scale. Zero
     * restores one flat depth everywhere.
     */
    double lake_bed_grade = 0.125;
    /**
     * @brief How deep a lake's bed may reach under its surface, in metres.
     *
     * A ceiling on `lake_bed_grade`, so an inland sea does not have its middle pushed down to
     * the floor of the height field -- where it would be clamped flat again, which is the very
     * thing the grade exists to avoid.
     */
    double lake_depth_max_m = 90.0;
    /**
     * @brief Carriageway width of a `RoadClass::Trail`, in metres.
     *
     * A cart track. These three are real widths and not render tuning: at the
     * default `meters_per_pixel` they come out 3, 6 and 10 pixels, because that
     * is what 3, 6 and 10 metres of ground are. Widths picked so the three land
     * on different *pixel* widths at some resolution give the wrong ground
     * widths -- a 12 m road and an 18 m highway are motorway proportions on a
     * medieval map.
     */
    double trail_width_m = 3.0;
    /** @brief Carriageway width of a `RoadClass::Road`, in metres; two carts abreast. */
    double road_width_m = 6.0;
    /** @brief Carriageway width of a `RoadClass::Highway`, in metres. */
    double highway_width_m = 10.0;
    /** @brief The outline land is confined to. */
    ShapeConfig shape;
    /** @brief Settlement placement parameters. */
    TownConfig towns;
    /** @brief Road network parameters. */
    RoadConfig roads;
    /** @brief Political geography parameters. */
    RegionConfig regions;
    /** @brief Notable-place parameters. */
    LandmarkConfig landmarks;
    /** @brief Cave system parameters. */
    CaveConfig caves;

    // --- Pass toggles ---

    bool enable_water = true;       /**< @brief Run the water pass. */
    bool enable_coast = true;       /**< @brief Run the coast pass. */
    bool enable_elevation = true;   /**< @brief Run the elevation pass. */
    bool enable_temperature = true; /**< @brief Run the temperature pass. */
    bool enable_rivers = true;      /**< @brief Run the river pass. */
    bool enable_valleys = true;     /**< @brief Run the valley pass, which cuts rivers into the terrain. */
    bool enable_moisture = true;    /**< @brief Run the moisture pass. */
    bool enable_biomes = true;      /**< @brief Run the biome pass. */
    bool enable_roads = true;       /**< @brief Run the road pass. */
    bool enable_regions = true;     /**< @brief Run the region pass. */
    bool enable_towns = true;       /**< @brief Run the town pass. */
    bool enable_landmarks = true;   /**< @brief Run the landmark pass. */
    bool enable_caves = true;       /**< @brief Run the cave pass. */
    bool enable_noisy_edges = true; /**< @brief Run the noisy-edge pass. */

    /**
     * @brief Subdivide noisy edges rather than emitting a straight two-point line.
     *
     * Off is the cheap path and reproduces the original generator's output,
     * which shipped with subdivision commented out and therefore drew every
     * cell boundary straight. On gives the ragged, natural coastlines the
     * algorithm was written for, at the cost of a few extra points per edge.
     */
    bool subdivide_noisy_edges = true;
};

/** @brief How many angular harmonics wander the outline of one landmass. */
inline constexpr std::size_t k_shape_harmonics = 4;

/**
 * @brief The angular frequencies summed into a landmass outline.
 *
 * Whole numbers, so the outline closes on itself with no seam at `theta = pi`,
 * and pairwise coprime, so the sum has no shorter period than a full turn --
 * a set like {2, 4, 6} would come out twofold symmetric and read as a manufactured
 * shape rather than a coastline.
 */
inline constexpr std::array<double, k_shape_harmonics> k_shape_modes{2.0, 3.0, 5.0, 7.0};

/** @brief Amplitude multiplier between successive outline harmonics. */
inline constexpr double k_shape_harmonic_gain = 0.55;

/** @brief Seed offset for landmass placement; distinct from every pass's own. */
inline constexpr unsigned int k_shape_seed_offset = 6291469u;

/**
 * @brief How much of the canvas an `Archipelago` fills when `diameter_m` is 0.
 *
 * The fraction of the largest disc that fits the canvas which the landmasses,
 * counting the full reach of their outlines, cover between them. Well under 1,
 * and that is the point: the slack is the room they have to scatter into. Size
 * them to fill the canvas and every centre gets pinned near the middle, and the
 * archipelago fuses into one continent with a lumpy edge.
 */
inline constexpr double k_archipelago_fill = 0.55;

/** @brief Candidate positions considered per landmass when placing an archipelago. */
inline constexpr int k_shape_placement_darts = 32;

/**
 * @brief How far out of the middle candidate landmass positions are pushed, 0 to 1.
 *
 * A landmass is large next to the disc its centre is allowed to fall in, so a
 * centre sampled near the middle of that disc overlaps everything else there is
 * -- area-uniform sampling puts a landmass in the one place it cannot help but
 * fuse. Holding the candidates out to an annulus instead keeps roughly one more
 * landmass distinct at the same size and the same coverage, and it is what puts
 * open sea *between* the landmasses rather than only around the outside of them.
 */
inline constexpr double k_shape_placement_bias = 0.55;

/**
 * @brief Grid size the coastal warp frequency is quoted against.
 *
 * `noise_shape.frequency` is in grid units, so holding it fixed while raising
 * `grid_size` would not resolve the coast more finely -- it would crumble a
 * continent into a hundred little wobbles. Scaling by this keeps the silhouette
 * of a map the same shape at every resolution, which is what `--grid-size` is
 * supposed to control. The island field is scaled the same way, for the same
 * reason.
 */
inline constexpr double k_shape_reference_grid = 40.0;

/**
 * @struct ShapeBlob
 * @brief One landmass: where it sits, how big it is, and how its outline wanders.
 */
struct ShapeBlob {
    /** @brief Centre, in grid units. */
    double centre_x = 0.0;
    /** @brief Centre, in grid units. */
    double centre_y = 0.0;
    /** @brief Mean radius in grid units; the outline varies about it. */
    double radius = 0.0;
    /** @brief Phase of each outline harmonic, in radians. */
    std::array<double, k_shape_harmonics> phase{};
};

/**
 * @class ShapeField
 * @brief The outline the landmass is confined to, resolved once and sampled cheaply.
 *
 * The shape of the world comes from one predicate: `inset()` returns how far
 * inside the outline a point lies. `border_check_()` flags any corner whose inset
 * falls below `border_length`; the water pass forces those cells to sea, and the
 * elevation pass measures height as distance from them -- so this class decides
 * the coastline, the mountains, the regions and the roads, all without any of
 * them knowing shapes exist.
 *
 * It is an object rather than a free function because the organic shapes carry
 * state: a landmass layout drawn from the seed, and a noise generator that warps
 * the coast. Rebuilding those per sample is the exact mistake `Noise` was
 * introduced to undo -- `inset()` is called once per cell *and* once per corner,
 * so it runs on the order of a hundred thousand times per map. Build one field
 * and reuse it; `shape_inset()` is the convenience path for one-off queries.
 *
 * `Rectangle`, `Circle` and `Triangle` are pure geometry and touch neither the
 * layout nor the generator, so constructing a field for them costs nothing.
 *
 * ### Sizing
 *
 * Every dimension treats 0 as "as big as the canvas allows", which is what makes
 * the default rectangle span the grid. `continent_size_m` is the *mean* diameter
 * of a single landmass, so 0 has to mean something per-shape there: a `Continent`
 * grows until its widest lobe just fits the canvas, and an `Archipelago` sizes its
 * landmasses so they cover `k_archipelago_fill` of it between them.
 */
class ShapeField {
public:
    /**
     * @brief Resolves the outline described by a configuration.
     * @param config Supplies the shape, its dimensions, the grid size, the world
     *        scale, the master seed and the coastal warp field.
     */
    explicit ShapeField(const MapConfig& config)
        : shape_(config.shape.shape),
          grid_(static_cast<double>(config.grid_size)),
          centre_(static_cast<double>(config.grid_size) * 0.5),
          rotation_(config.shape.rotation) {
        // A dimension of 0 means "as big as the canvas allows".
        const auto extent = [&config, this](double meters) {
            return meters > 0.0 ? meters / config.meters_per_grid_unit : grid_;
        };

        switch (shape_) {
            case MapShape::Circle:
                radius_ = extent(config.shape.diameter_m) * 0.5;
                return;
            case MapShape::Triangle:
                half_side_ = extent(config.shape.edge_length_m) * 0.5;
                return;
            case MapShape::Continent:
            case MapShape::Archipelago:
                build_landmasses_(config);
                return;
            case MapShape::Rectangle:
                break;
        }
        half_width_ = extent(config.shape.width_m) * 0.5;
        half_height_ = extent(config.shape.height_m) * 0.5;
    }

    /**
     * @brief How far inside the outline a point lies, in grid units.
     *
     * Positive inside, negative outside, zero on the boundary. The three
     * geometric shapes return an exact signed distance; the organic ones return a
     * signed pseudo-distance, whose sign is exact and whose magnitude near the
     * boundary is unit-scaled -- which is all `border_length` compares against.
     *
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return Positive inside the shape, negative outside, zero on its boundary.
     */
    double inset(double x, double y) const {
        const double dx = x - centre_;
        const double dy = y - centre_;

        switch (shape_) {
            case MapShape::Circle:
                return radius_ - std::sqrt(dx * dx + dy * dy);
            case MapShape::Triangle: {
                // Rotate into the triangle's own frame, then measure. Rotating the
                // query rather than the triangle keeps the shape description to one
                // number.
                const double c = std::cos(-rotation_);
                const double sn = std::sin(-rotation_);
                const double lx = dx * c - dy * sn;
                const double ly = dx * sn + dy * c;

                // Signed distance to an equilateral triangle centred on its
                // centroid, with `half_side_` as the base half-width and k = sqrt(3).
                const double k = 1.7320508075688772;
                if (half_side_ <= 0.0) {
                    return -1.0;
                }
                // The formula is written for maths axes, where y climbs; image rows
                // descend. Flipping here is what puts the apex at the top of the
                // picture rather than the bottom.
                double px = std::abs(lx) - half_side_;
                double py = -ly + half_side_ / k;
                if (px + k * py > 0.0) {
                    const double folded_x = (px - k * py) * 0.5;
                    const double folded_y = (-k * px - py) * 0.5;
                    px = folded_x;
                    py = folded_y;
                }
                px -= std::clamp(px, -2.0 * half_side_, 0.0);
                // Distance, signed by which side of the folded edge the point landed
                // on. Positive inside, to match the other two shapes.
                return std::sqrt(px * px + py * py) * (py > 0.0 ? 1.0 : -1.0);
            }
            case MapShape::Continent:
            case MapShape::Archipelago: {
                // Union of the landmasses: a point is inside the shape if it is
                // inside any one of them, so two that overlap fuse into a single
                // larger continent instead of drawing a coast through each other.
                double best = blob_inset_(blobs_.front(), x, y);
                for (std::size_t i = 1; i < blobs_.size(); ++i) {
                    best = std::max(best, blob_inset_(blobs_[i], x, y));
                }
                // Warping after the union rather than per landmass keeps the coastal
                // texture continuous across a fused pair, and is what lets a coast
                // fold back on itself -- the radial outline alone cannot, so without
                // this there are no fjords and no offshore islands.
                return best + warp_amplitude_ *
                                  static_cast<double>(warp_.GetNoise(static_cast<float>(x),
                                                                    static_cast<float>(y)));
            }
            case MapShape::Rectangle:
                break;
        }

        return std::min(half_width_ - std::abs(dx), half_height_ - std::abs(dy));
    }

    /**
     * @brief The landmasses the organic shapes resolved to.
     * @return One entry per landmass; empty for the geometric shapes.
     */
    const std::vector<ShapeBlob>& landmasses() const { return blobs_; }

private:
    /**
     * @brief Draws the landmass layout from the master seed.
     *
     * Sizes come first, because a landmass has to know how far it reaches before
     * anywhere is a legal place to put it: the centre is confined to a disc small
     * enough that the widest lobe plus the coastal warp still clears the canvas
     * edge by `margin_`. Without that the ocean flood fill can find a landmass
     * touching the border and mistake the sea for a lake.
     *
     * @param config Supplies the seed, the world scale and the shape parameters.
     */
    void build_landmasses_(const MapConfig& config) {
        irregularity_ = std::clamp(config.shape.irregularity, 0.0, 0.6);
        const double detail = std::max(config.shape.coast_detail, 0.0);
        const double variance = std::clamp(config.shape.size_variance, 0.0, 0.9);
        const int count = shape_ == MapShape::Archipelago
                              ? std::max(1, config.shape.continent_count)
                              : 1;
        margin_ = std::max(1.0, grid_ * 0.03);

        // How much wider than its mean radius a landmass can get: the tallest
        // outline lobe, plus the coastal warp on top of it.
        const double reach = 1.0 + irregularity_ + detail;
        const double fitting_radius = std::max(0.0, (grid_ * 0.5 - margin_) / reach);

        double base_radius =
            config.shape.continent_size_m > 0.0
                ? config.shape.continent_size_m / config.meters_per_grid_unit * 0.5
                : 0.0;
        if (base_radius <= 0.0) {
            // Pack `count` landmasses into the canvas disc at `k_archipelago_fill`
            // coverage: N * (R * reach)^2 = fill * (grid/2 - margin)^2. A lone
            // continent skips the packing and simply grows until it fits.
            base_radius = shape_ == MapShape::Archipelago
                              ? fitting_radius * std::sqrt(k_archipelago_fill /
                                                           static_cast<double>(count))
                              : fitting_radius;
        }
        base_radius = std::min(base_radius, fitting_radius);
        warp_amplitude_ = detail * base_radius;

        configure_noise(warp_, config.noise_shape);
        warp_.SetFrequency(static_cast<float>(config.noise_shape.frequency *
                                              (k_shape_reference_grid / grid_)));

        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_shape_seed_offset);
        coopa::maps::UniformRealDistribution<double> unit(0.0, 1.0);
        coopa::maps::UniformRealDistribution<double> turn(0.0, 2.0 * 3.14159265358979323846);

        blobs_.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            ShapeBlob blob;
            blob.radius =
                count > 1 ? base_radius * (1.0 + variance * (unit(rng) * 2.0 - 1.0)) : base_radius;
            blob.radius = std::min(blob.radius, fitting_radius);
            for (std::size_t k = 0; k < k_shape_harmonics; ++k) {
                blob.phase[k] = turn(rng);
            }

            const double limit =
                std::max(0.0, grid_ * 0.5 - margin_ - blob.radius * (1.0 + irregularity_) -
                                  warp_amplitude_);
            place_(blob, limit, rng, unit, turn);
            blobs_.push_back(blob);
        }
    }

    /**
     * @brief Picks a centre for one landmass by Mitchell's best-candidate sampling.
     *
     * Throws `k_shape_placement_darts` positions into the legal disc and keeps the
     * one furthest from every landmass already placed. Rejection sampling against a
     * minimum separation would spread them more evenly but can run out of room and
     * fail to place the last landmass at all; this always places one, and still
     * lets two land close enough to fuse -- which is what stops an archipelago
     * reading as a row of evenly spaced blobs.
     *
     * @param blob The landmass to position; its radius must already be set.
     * @param limit Radius of the disc the centre may fall in, in grid units.
     * @param rng The layout random stream.
     * @param unit A `[0, 1)` distribution over `rng`.
     * @param turn A `[0, 2*pi)` distribution over `rng`.
     */
    void place_(ShapeBlob& blob, double limit, std::mt19937& rng,
                coopa::maps::UniformRealDistribution<double>& unit,
                coopa::maps::UniformRealDistribution<double>& turn) const {
        if (limit <= 0.0 || blobs_.empty()) {
            // Nowhere to go, or nothing to stay away from: sample once so the
            // stream advances identically either way.
            const double angle = turn(rng);
            const double distance = sample_distance_(limit, unit(rng));
            blob.centre_x = centre_ + std::cos(angle) * distance;
            blob.centre_y = centre_ + std::sin(angle) * distance;
            return;
        }

        double best_x = centre_;
        double best_y = centre_;
        double best_clearance = -1.0;
        for (int dart = 0; dart < k_shape_placement_darts; ++dart) {
            const double angle = turn(rng);
            const double distance = sample_distance_(limit, unit(rng));
            const double x = centre_ + std::cos(angle) * distance;
            const double y = centre_ + std::sin(angle) * distance;

            double clearance = std::numeric_limits<double>::max();
            for (const ShapeBlob& placed : blobs_) {
                const double dx = x - placed.centre_x;
                const double dy = y - placed.centre_y;
                // Measured between the rims, not the centres, so a large landmass
                // pushes its neighbours further away than a small one does.
                clearance = std::min(clearance,
                                     std::sqrt(dx * dx + dy * dy) - placed.radius - blob.radius);
            }
            if (clearance > best_clearance) {
                best_clearance = clearance;
                best_x = x;
                best_y = y;
            }
        }
        blob.centre_x = best_x;
        blob.centre_y = best_y;
    }

    /**
     * @brief Turns a uniform sample into a distance from the canvas centre.
     *
     * The square root is what makes the draw area-uniform over a disc rather than
     * bunched at the middle, since a ring has more area the further out it sits.
     * `k_shape_placement_bias` then lifts the floor off the centre entirely.
     *
     * @param limit Radius of the disc the centre may fall in, in grid units.
     * @param sample A uniform draw in `[0, 1)`.
     * @return A distance in `[bias * limit, limit]`.
     */
    static double sample_distance_(double limit, double sample) {
        return limit * (k_shape_placement_bias +
                        (1.0 - k_shape_placement_bias) * std::sqrt(sample));
    }

    /**
     * @brief Inset relative to a single landmass, before the coastal warp.
     * @param blob The landmass to measure against.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return Positive inside that landmass, negative outside.
     */
    double blob_inset_(const ShapeBlob& blob, double x, double y) const {
        const double dx = x - blob.centre_x;
        const double dy = y - blob.centre_y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        const double theta = std::atan2(dy, dx) - rotation_;

        // A fractal sum in the angle: each harmonic turns faster and counts for
        // less, which is the one-dimensional equivalent of the fbm that gives the
        // terrain its shape. Normalised by the total weight so `irregularity_`
        // means the same thing however many harmonics there are.
        double wobble = 0.0;
        double weight = 0.0;
        double gain = 1.0;
        for (std::size_t k = 0; k < k_shape_harmonics; ++k) {
            wobble += gain * std::sin(k_shape_modes[k] * theta + blob.phase[k]);
            weight += gain;
            gain *= k_shape_harmonic_gain;
        }
        return blob.radius * (1.0 + irregularity_ * (wobble / weight)) - distance;
    }

    MapShape shape_;              /**< @brief Which outline this field describes. */
    double grid_;                 /**< @brief Canvas size in grid units. */
    double centre_;               /**< @brief Canvas centre in grid units, both axes. */
    double rotation_;             /**< @brief Rotation about the centre, in radians. */
    double radius_ = 0.0;         /**< @brief Circle radius, in grid units. */
    double half_side_ = 0.0;      /**< @brief Triangle base half-width, in grid units. */
    double half_width_ = 0.0;     /**< @brief Rectangle half-width, in grid units. */
    double half_height_ = 0.0;    /**< @brief Rectangle half-height, in grid units. */
    double irregularity_ = 0.0;   /**< @brief Clamped outline wander, 0 to 0.6. */
    double margin_ = 0.0;         /**< @brief Open sea kept clear round the canvas edge. */
    double warp_amplitude_ = 0.0; /**< @brief Coastal warp amplitude, in grid units. */
    std::vector<ShapeBlob> blobs_; /**< @brief The landmasses; empty unless organic. */
    FastNoiseLite warp_;           /**< @brief The coastal warp field; unused unless organic. */
};

/**
 * @brief How far inside the landmass shape a point lies, in grid units.
 *
 * A one-off query against a freshly resolved `ShapeField`. Convenient, but it
 * rebuilds the field every call -- anything sampling the shape more than a
 * handful of times should hold a `ShapeField` and call `ShapeField::inset()`,
 * which is what `border_check_()` does.
 *
 * @param config Supplies the shape, the grid size and the world scale.
 * @param x Horizontal grid position.
 * @param y Vertical grid position.
 * @return Positive inside the shape, negative outside, zero on its boundary.
 */
inline double shape_inset(const MapConfig& config, double x, double y) {
    return ShapeField(config).inset(x, y);
}

/** @brief How many cells a sphere map has: `sphere_cells`, or `(grid_size + 1)^2`. */
inline int sphere_point_count(const MapConfig& config) {
    if (config.sphere_cells > 0) return std::max(12, config.sphere_cells);
    return std::max(12, (config.grid_size + 1) * (config.grid_size + 1));
}

/**
 * @brief A sphere map's radius in grid units: the one at which its cells average a grid unit
 *        across (`4 pi R^2 = N`), so every distance on it means what it means on a plane.
 */
inline double sphere_radius(const MapConfig& config) {
    return std::sqrt(static_cast<double>(sphere_point_count(config)) / (4.0 * 3.14159265358979323846));
}

/**
 * @class SphereShapeField
 * @brief `ShapeField` on a globe: where land may rise, as a signed distance in grid units.
 *
 * - **Rectangle** has no edge on a sphere: the whole globe is open to land, and the island noise
 *   alone decides where it is (`inset()` is always far positive).
 * - **Circle** and **Triangle** are one round landmass -- a cap -- of `diameter_m`
 *   (`edge_length_m`) across, centred on a seeded point; 0 is half the globe.
 * - **Continent** is one irregular landmass of `continent_size_m` (0: half the globe), and
 *   **Archipelago** `continent_count` of them, spread apart over the globe with their sizes
 *   varied by `size_variance`.
 *
 * A landmass's edge wanders by `irregularity` (a low-frequency field over the globe) and its coast
 * by `coast_detail` (the shape noise, sampled in 3D). Distances are along the surface.
 */
class SphereShapeField {
public:
    explicit SphereShapeField(const MapConfig& config)
        : shape_(config.shape.shape), radius_(sphere_radius(config)) {
        configure_noise(warp_, config.noise_shape);
        warp_.SetFrequency(static_cast<float>(config.noise_shape.frequency * 3.0));
        irregularity_ = std::clamp(config.shape.irregularity, 0.0, 0.6);
        coast_ = std::max(0.0, config.shape.coast_detail);
        const double half_globe = 0.5 * 3.14159265358979323846 * radius_;   // arc to the equator from a pole
        const auto arc_of = [&](double meters) {
            return meters > 0.0 ? 0.5 * meters / config.meters_per_grid_unit : half_globe;
        };
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed * 2654435761u + 97u));
        UniformRealDistribution<double> unit(0.0, 1.0);
        const auto random_dir = [&]() {
            const double z = 2.0 * unit(rng) - 1.0;
            const double a = 2.0 * 3.14159265358979323846 * unit(rng);
            const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
            return glm::dvec3(r * std::cos(a), r * std::sin(a), z);
        };
        switch (shape_) {
            case MapShape::Rectangle:
                return;
            case MapShape::Circle:
                blobs_.push_back({random_dir(), arc_of(config.shape.diameter_m)});
                return;
            case MapShape::Triangle:
                blobs_.push_back({random_dir(), arc_of(config.shape.edge_length_m)});
                return;
            case MapShape::Continent:
                blobs_.push_back({random_dir(), arc_of(config.shape.continent_size_m)});
                return;
            case MapShape::Archipelago: {
                const int count = std::max(1, config.shape.continent_count);
                // Mean size: the landmasses share about half the globe between them.
                const double mean = config.shape.continent_size_m > 0.0
                                        ? 0.5 * config.shape.continent_size_m / config.meters_per_grid_unit
                                        : half_globe / std::sqrt(static_cast<double>(count));
                const double variance = std::clamp(config.shape.size_variance, 0.0, 0.9);
                for (int i = 0; i < count; ++i) {
                    // Best of a few candidates: the one furthest from those already placed.
                    glm::dvec3 best = random_dir();
                    double best_gap = -1.0;
                    for (int c = 0; c < 12; ++c) {
                        const glm::dvec3 d = random_dir();
                        double gap = 4.0;
                        for (const Blob& b : blobs_) gap = std::min(gap, glm::length(d - b.centre));
                        if (gap > best_gap) { best_gap = gap; best = d; }
                    }
                    const double scale = 1.0 + variance * (2.0 * unit(rng) - 1.0);
                    blobs_.push_back({best, mean * scale});
                }
                return;
            }
        }
    }

    /** @brief How far inside the land's outline a point of the globe lies, in grid units. */
    double inset(double x, double y, double z) const {
        if (blobs_.empty()) return std::numeric_limits<double>::max();
        const glm::dvec3 p(x, y, z);
        const double len = glm::length(p);
        if (len <= 0.0) return 0.0;
        const glm::dvec3 dir = p / len;
        const double wobble = static_cast<double>(warp_.GetNoise(static_cast<float>(dir.x * 4.0),
                                                                  static_cast<float>(dir.y * 4.0),
                                                                  static_cast<float>(dir.z * 4.0)));
        const double coast = static_cast<double>(warp_.GetNoise(static_cast<float>(x), static_cast<float>(y),
                                                                 static_cast<float>(z)));
        double best = -std::numeric_limits<double>::max();
        for (const Blob& b : blobs_) {
            const double arc = std::acos(std::clamp(glm::dot(dir, b.centre), -1.0, 1.0)) * radius_;
            const double reach = b.radius * (1.0 + irregularity_ * wobble) + coast_ * b.radius * coast;
            best = std::max(best, reach - arc);
        }
        return best;
    }

private:
    struct Blob {
        glm::dvec3 centre; /**< Unit direction. */
        double radius;     /**< Arc length from the centre to the edge, in grid units. */
    };
    MapShape shape_;
    double radius_;
    double irregularity_ = 0.0;
    double coast_ = 0.0;
    std::vector<Blob> blobs_;
    FastNoiseLite warp_;
};

/**
 * @brief Converts a height in metres into the normalised elevation field.
 *
 * The vertical counterpart of `meters_to_grid()`. Heights are stored as a
 * fraction of `elevation_range_m`, so anything physical -- how deep a river is,
 * how far water overhangs its bank -- crosses here.
 *
 * @param config Supplies the vertical scale.
 * @param meters The height in metres.
 * @return The same height as a fraction of the elevation range.
 */
inline double meters_to_height(const MapConfig& config, double meters) {
    return config.elevation_range_m > 0.0 ? meters / config.elevation_range_m : 0.0;
}

/**
 * @brief Converts a normalised height into metres.
 * @param config Supplies the vertical scale.
 * @param height A height from the `[0, 1]` field.
 * @return The same height in metres above the sea floor.
 */
inline double height_to_meters(const MapConfig& config, double height) {
    return height * config.elevation_range_m;
}

/**
 * @brief Re-expresses a height as a fraction of the land above the waterline.
 *
 * Land occupies `[sea_level, 1]` of the height field, so a threshold that
 * describes *land* -- where a peak begins, where a town is too high to reach,
 * which row of the Whittaker diagram a cell falls in -- cannot be compared
 * against a raw elevation without first taking the sea out of the range. Every
 * such threshold is phrased against this, which is what kept them meaning the
 * same thing when the waterline moved off zero.
 *
 * @param config Supplies the waterline.
 * @param elevation A raw height from the `[0, 1]` field.
 * @return 0 at the shoreline, 1 at the highest ground; 0 for anything submerged.
 */
inline double land_height(const MapConfig& config, double elevation) {
    const double span = 1.0 - config.sea_level;
    if (span <= 0.0) {
        return 0.0;
    }
    return std::clamp((elevation - config.sea_level) / span, 0.0, 1.0);
}

/**
 * @brief Converts a length in metres to grid units.
 *
 * The bridge between the two systems the generator speaks. Sizes are configured
 * and reasoned about in metres, because that is what they physically are;
 * geometry is computed in grid units, because that is the space the Voronoi
 * graph lives in. Everything crosses here.
 *
 * @param config Supplies `meters_per_grid_unit`.
 * @param meters The length in metres.
 * @return The same length in grid units.
 */
inline double meters_to_grid(const MapConfig& config, double meters) {
    return meters / config.meters_per_grid_unit;
}

/**
 * @brief Converts a length in grid units to metres.
 * @param config Supplies `meters_per_grid_unit`.
 * @param grid The length in grid units.
 * @return The same length in metres.
 */
inline double grid_to_meters(const MapConfig& config, double grid) {
    return grid * config.meters_per_grid_unit;
}

/**
 * @brief The render resolution the world extent and `meters_per_pixel` imply.
 *
 * `grid_size * meters_per_grid_unit` metres of world, divided by the ground each
 * pixel covers. A free function rather than a member so the arithmetic has
 * exactly one home, and so `image_size` cannot drift from the scale that
 * produced it.
 *
 * @param config Supplies the grid size and both scale factors.
 * @return The side length in pixels, at least 1.
 */
inline double sphere_radius(const MapConfig& config);

inline int derive_image_size(const MapConfig& config) {
    // A sphere renders equirectangular: `image_size` is the width, the equator's length.
    const double grid_extent = config.topology == MapTopology::Sphere
                                   ? 2.0 * 3.14159265358979323846 * sphere_radius(config)
                                   : static_cast<double>(config.grid_size);
    const double meters = grid_extent * config.meters_per_grid_unit;
    const double pixels = meters / (config.meters_per_pixel > 0.0 ? config.meters_per_pixel : 1.0);
    return std::max(1, static_cast<int>(pixels + 0.5));
}

/**
 * @brief The width of a river carrying a given volume, in metres.
 * @param config Supplies the width parameters.
 * @param volume The edge's river volume.
 * @return The river's width in metres.
 */
inline double river_width_meters(const MapConfig& config, int volume) {
    return config.river_width_base_m
         + config.river_width_per_volume_m * static_cast<double>(volume);
}

/**
 * @brief The physical width of a river carrying a given volume, in grid units.
 *
 * The one definition of how wide a river is. The renderer scales it to pixels;
 * the town packer uses it directly to keep buildings on the bank.
 *
 * @param config Supplies the width parameters.
 * @param volume The edge's river volume.
 * @return The river's width in grid units.
 */
inline double river_width(const MapConfig& config, int volume) {
    return meters_to_grid(config, river_width_meters(config, volume));
}

/**
 * @brief The width of a road of a given class, in metres.
 *
 * The counterpart of `river_width_meters()`, and the one definition of how wide
 * a road is: the renderer scales it to pixels, and a consumer laying geometry
 * along a road reads the same number.
 *
 * @param config Supplies the per-class widths.
 * @param road_class The class of road.
 * @return The road's width in metres; 0 for `RoadClass::None`.
 */
inline double road_width_meters(const MapConfig& config, RoadClass road_class) {
    switch (road_class) {
        case RoadClass::Trail:   return config.trail_width_m;
        case RoadClass::Road:    return config.road_width_m;
        case RoadClass::Highway: return config.highway_width_m;
        case RoadClass::None:    break;
    }
    return 0.0;
}

/**
 * @brief The width of a road of a given class, in grid units.
 *
 * The grid-space counterpart, so a caller working in graph coordinates never has
 * to remember which system a width was configured in.
 *
 * @param config Supplies the per-class widths and the world scale.
 * @param road_class The class of road.
 * @return The road's width in grid units; 0 for `RoadClass::None`.
 */
inline double road_width_for(const MapConfig& config, RoadClass road_class) {
    return meters_to_grid(config, road_width_meters(config, road_class));
}

/**
 * @struct BiomePalette
 * @brief Render colours, in 0-255 component range, indexed by `Biome`.
 *
 * A flat array rather than one named field per biome: the renderer needs a
 * colour per cell per frame, and indexing an array by the enum avoids a
 * twenty-branch string comparison chain at that point.
 */
struct BiomePalette {
    /** @brief One colour per `Biome`, indexed by `static_cast<std::size_t>(biome)`. */
    std::array<glm::vec3, k_biome_count> biome_colors = {
        glm::vec3(94, 182, 223),   // Ocean
        glm::vec3(94, 182, 223),   // Lake
        glm::vec3(33, 94, 33),     // Marsh
        glm::vec3(146, 206, 231),  // Ice
        glm::vec3(245, 222, 179),  // Beach
        glm::vec3(255, 250, 250),  // Snow
        glm::vec3(169, 169, 169),  // Tundra
        glm::vec3(201, 180, 155),  // Bare
        glm::vec3(153, 130, 109),  // Scorched
        glm::vec3(51, 102, 0),     // Taiga
        glm::vec3(128, 128, 0),    // Shrubland
        glm::vec3(238, 214, 175),  // TemperateDesert
        glm::vec3(85, 107, 47),    // TemperateRainForest
        glm::vec3(34, 139, 34),    // TemperateDeciduousForest
        glm::vec3(124, 252, 0),    // Grassland
        glm::vec3(0, 100, 0),      // TropicalRainForest
        glm::vec3(107, 142, 35),   // TropicalSeasonalForest
        glm::vec3(250, 250, 210),  // SubtropicalDesert
        glm::vec3(142, 186, 124),  // AlpineMeadow
        glm::vec3(222, 241, 247),  // Glacier
        glm::vec3(186, 184, 160),  // ColdDesert
        glm::vec3(178, 182, 108),  // Steppe
        glm::vec3(196, 190, 90),   // Savanna
        glm::vec3(150, 160, 92),   // Chaparral
        glm::vec3(126, 116, 96),   // Moorland
        glm::vec3(72, 110, 96),    // BorealWetland
        glm::vec3(58, 92, 62),     // Swamp
        glm::vec3(46, 120, 90),    // Mangrove
        glm::vec3(96, 150, 118),   // CloudForest
        glm::vec3(178, 128, 88),   // Badlands
        glm::vec3(238, 238, 230),  // SaltFlat
        glm::vec3(232, 206, 148),  // Dunes
        glm::vec3(92, 74, 70)      // VolcanicField
    };

    /** @brief Colour of river strokes. */
    glm::vec3 river_color = glm::vec3(94, 182, 223);
    /**
     * @brief Colour of a `RoadClass::Road` stroke.
     *
     * Warm earth rather than black. A pure black line of even width across a
     * coloured map reads as an administrative border, not as a road.
     */
    glm::vec3 road_color = glm::vec3(122, 101, 82);
    /** @brief Colour of a `RoadClass::Trail` stroke. */
    glm::vec3 trail_color = glm::vec3(136, 122, 100);
    /** @brief Colour of a `RoadClass::Highway` stroke. */
    glm::vec3 highway_color = glm::vec3(146, 108, 62);
    /**
     * @brief Colour of a bridge or causeway parapet, drawn square across the road.
     *
     * Stone rather than near-black. Roads have no dark casing: at 6 m wide over a
     * hillshaded composite a road does not vanish against dark forest, and an
     * outline under every stroke would make each road read as drawn-on ink
     * rather than as ground.
     */
    glm::vec3 bridge_color = glm::vec3(112, 108, 102);
    /** @brief Colour of the settlement marker drawn at a town's centre. */
    glm::vec3 town_color = glm::vec3(120, 40, 40);
    /** @brief Colour of a natural landmark marker. */
    glm::vec3 landmark_natural_color = glm::vec3(40, 60, 130);
    /** @brief Colour of a built landmark marker. */
    glm::vec3 landmark_built_color = glm::vec3(90, 60, 130);
    /** @brief Colour of an individual packed building footprint. */
    glm::vec3 building_color = glm::vec3(70, 50, 40);
    /**
     * @brief Colour of a civic building -- a hall, market, temple and the rest.
     *
     * Distinct from `building_color` on purpose: a settlement whose key structures
     * look exactly like its houses has no legible centre.
     */
    glm::vec3 civic_color = glm::vec3(150, 92, 46);
    /** @brief Colour of a street inside a settlement; a lane, not a road. */
    glm::vec3 street_color = glm::vec3(176, 158, 134);
    /** @brief Colour of the open ground of a market square. */
    glm::vec3 plaza_color = glm::vec3(206, 192, 170);
    /**
     * @brief Colour of the shallowest cave passage on the overview layer.
     *
     * The two ends of a ramp, not two categories: a passage is coloured by
     * interpolating between them on its floor height, so the overview reads as an
     * elevation map of the caves in the same way the terrain layer does of the
     * ground. Warm at the top and cold at the bottom, which is the one ordering a
     * reader does not have to be told.
     */
    glm::vec3 cave_shallow_color = glm::vec3(236, 196, 120);
    /** @brief Colour of the deepest cave passage on the overview layer. */
    glm::vec3 cave_deep_color = glm::vec3(86, 72, 156);
    /** @brief Colour of the ringed marker drawn where a cave opens. */
    glm::vec3 cave_mouth_color = glm::vec3(230, 230, 236);
    /** @brief Colour of a chamber outline, drawn over the passage colour. */
    glm::vec3 cave_chamber_color = glm::vec3(210, 140, 96);
    /** @brief Colour the canvas is cleared to before any cell is filled. */
    glm::vec3 background_color = glm::vec3(255, 255, 255);

    /**
     * @brief Looks up the stroke colour for a road class.
     * @param road_class The class to colour; `None` returns `bridge_color`.
     * @return The palette entry, in 0-255 component range.
     */
    const glm::vec3& color_for(RoadClass road_class) const {
        switch (road_class) {
            case RoadClass::Trail:   return trail_color;
            case RoadClass::Road:    return road_color;
            case RoadClass::Highway: return highway_color;
            case RoadClass::None:    break;
        }
        return bridge_color;
    }

    /**
     * @brief Looks up the colour for a biome.
     * @param biome The biome to colour.
     * @return The palette entry, in 0-255 component range.
     */
    const glm::vec3& color_for(Biome biome) const {
        return biome_colors[static_cast<std::size_t>(biome)];
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_CONFIG_H
