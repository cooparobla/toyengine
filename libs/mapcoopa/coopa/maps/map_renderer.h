/**
 * @file map_renderer.h
 * @brief Software renderers that turn a `MapGraph` into the map's image layers.
 */

#ifndef COOPA_MAPS_MAP_RENDERER_H
#define COOPA_MAPS_MAP_RENDERER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/maps/image.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/water_field.h>

namespace coopa {
namespace maps {

/**
 * @enum MapLayer
 * @brief One rendered view of a map.
 *
 * The generator writes one PNG per value. Splitting them apart is what lets a
 * consumer take the height field without the roads drawn over it, or the road
 * network without the terrain under it -- which a single composited image can
 * never give back.
 */
enum class MapLayer {
    Elevation,  /**< @brief Terrain height, greyscale. Land and lake beds; water is its own layer. */
    Water,      /**< @brief Water-surface height: sea, lakes and river channels. */
    Biomes,     /**< @brief Flat terrain colour, no overlays. */
    Roads,      /**< @brief The road network, transparent elsewhere. */
    Structures, /**< @brief Building footprints, transparent elsewhere. */
    Landmarks,  /**< @brief Settlement and landmark markers, transparent elsewhere. */
    Regions,    /**< @brief Provinces in flat colour, one fill per region and nothing over them. */
    Composite,  /**< @brief Every surface layer above, lit and blended. Cave mouths, no passage. */
    Caves       /**< @brief Readable overview of every system, over dimmed terrain. */
};

/**
 * @brief Number of distinct `MapLayer` values.
 *
 * A cave's *geometry* is deliberately not among them, and is not rastered at all.
 * A branching network of passages at several depths does not fit a stack of
 * heightmaps without being both flattened and quantised, and the saved map
 * already carries every station and every smoothed passage at full precision --
 * so the picture would be a lossy, far larger copy of the document. `Caves` is
 * the one readable overview, and `Composite` shows where systems *open*, which is
 * the only part of a cave anyone on the surface could see.
 */
inline constexpr std::size_t k_map_layer_count = 9;

/**
 * @brief Maps a layer to the suffix its file takes, after the output prefix.
 * @param layer The layer to name.
 * @return A `snake_case` identifier, e.g. `"elevation"`.
 */
inline std::string_view map_layer_name(MapLayer layer) {
    switch (layer) {
        case MapLayer::Elevation:  return "elevation";
        case MapLayer::Water:      return "water";
        case MapLayer::Biomes:     return "biomes";
        case MapLayer::Roads:      return "roads";
        case MapLayer::Structures: return "structures";
        case MapLayer::Landmarks:  return "landmarks";
        case MapLayer::Regions:    return "regions";
        case MapLayer::Composite:  return "composite";
        case MapLayer::Caves:      return "caves";
    }
    return "composite";
}

/**
 * @brief True when a layer is an overlay, drawn on transparency rather than on ground.
 * @param layer The layer to classify.
 * @return True if it should be rendered RGBA.
 */
inline bool map_layer_has_alpha(MapLayer layer) {
    return layer == MapLayer::Roads || layer == MapLayer::Structures
        || layer == MapLayer::Landmarks;
}

/**
 * @struct CellGeometry
 * @brief Every cell's outline in pixel space, and the rows it spans.
 *
 * Built once and reused, for two reasons. Each of the layers otherwise
 * rebuilds the same 6,889 outlines from scratch -- `MapGraph::cell_outline()`
 * walks a cell's edges and concatenates their subdivided paths, which is not
 * free and does not depend on which layer is being drawn.
 *
 * And it is what lets a render be split across threads by row band without the
 * split costing more than it saves: a band needs to know which cells reach into
 * its rows, and `min_row`/`max_row` answer that from the *actual* outline rather
 * than from an estimate. The subdivided boundary bulges outside the straight
 * corner polygon -- toward the neighbouring cell's site -- so a range derived
 * from the corners alone would be wrong at exactly the seams that matter.
 */
struct CellGeometry {
    /** @brief One outline per cell, in pixel coordinates, indexed by `CenterId`. */
    std::vector<std::vector<MapPoint>> outlines;
    /** @brief First row each cell's outline touches. */
    std::vector<int> min_row;
    /** @brief Last row each cell's outline touches. */
    std::vector<int> max_row;

    /** @brief Whether geometry has been built for a given cell count. */
    bool covers(std::size_t cell_count) const { return outlines.size() == cell_count; }
};

/**
 * @struct HeightField
 * @brief A single-channel copy of the height raster, for shading only.
 *
 * One byte per pixel rather than three: the shading reads height alone, and
 * at 23 megapixels the two channels it would otherwise carry are 46 MB of
 * nothing.
 */
struct HeightField {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> values;

    /** @brief Reads a sample, clamping at the edges rather than falling off them. */
    double at(int x, int y) const {
        const int cx = std::clamp(x, 0, width - 1);
        const int cy = std::clamp(y, 0, height - 1);
        return static_cast<double>(
                   values[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width)
                          + static_cast<std::size_t>(cx)])
             / 255.0;
    }
};

/**
 * @struct RenderSlice
 * @brief What part of a layer to draw, and the geometry to draw it from.
 *
 * Defaulted everywhere: a caller that wants a whole layer and does not care
 * about sharing geometry never mentions it.
 */
struct RenderSlice {
    /** @brief Rows this render may touch. */
    RowBand band;
    /** @brief Shared cell outlines, or null to build them locally. */
    const CellGeometry* geometry = nullptr;
    /**
     * @brief Shared height field for hillshading, or null to build one locally.
     *
     * Must be supplied when a hillshaded composite is drawn in bands, and the
     * reason is a factor of eighty. The field is smoothed and its gradient is
     * sampled across a whole cell, so both read well outside whatever band is
     * being drawn -- a band cannot build a correct one from its own rows. Left to
     * build its own, every band rebuilds the *entire* raster and re-blurs it,
     * which made a threaded hillshade render slower than a serial one.
     */
    const HeightField* height = nullptr;
};

/**
 * @class MapLayers
 * @brief Renders a `MapGraph` to any of its image layers.
 *
 * Every layer is drawn at the same scale, so they register pixel for pixel and
 * stack without resampling. At the default `meters_per_pixel` of 1 that scale is
 * one pixel to the metre: a 6 m road is 6 pixels wide because it is 6 metres
 * wide, and a pixel count read off a render is a measurement rather than a
 * rendering artefact.
 *
 * One layer is built at a time. At a 4.8 km world and 1 m/px a layer is 23
 * megapixels, which is ~92 MB held as RGBA; the generator renders, writes and
 * drops each in turn rather than holding them all at once.
 */
class MapLayers {
public:
    /**
     * @brief Builds the shared cell geometry for a map at a given render scale.
     *
     * Serial by design -- it is the prologue a parallel render needs before it can
     * safely divide the image up, and it allocates, so it is not worth threading.
     *
     * @param graph The map to outline.
     * @param config Supplies the image and grid size the outlines are scaled to.
     * @return Outlines and row ranges for every cell.
     */
    static CellGeometry build_cell_geometry(const MapGraph& graph, const MapConfig& config) {
        CellGeometry geometry;
        if (graph.is_sphere()) {
            return geometry;   // a globe renders per pixel (`draw_sphere_()`): no outlines
        }
        const double scale = pixels_per_grid_unit_(config);
        geometry.outlines.resize(graph.centers.size());
        geometry.min_row.assign(graph.centers.size(), 0);
        geometry.max_row.assign(graph.centers.size(), -1);

        for (const MapCenter& center : graph.centers) {
            const std::size_t index = static_cast<std::size_t>(center.index);
            std::vector<MapPoint>& outline = geometry.outlines[index];
            outline = graph.cell_outline(center);
            int min_row = std::numeric_limits<int>::max();
            int max_row = std::numeric_limits<int>::min();
            for (MapPoint& point : outline) {
                point.x *= scale;
                point.y *= scale;
                min_row = std::min(min_row, static_cast<int>(std::floor(point.y)));
                max_row = std::max(max_row, static_cast<int>(std::ceil(point.y)));
            }
            if (outline.empty()) {
                min_row = 0;
                max_row = -1;
            }
            geometry.min_row[index] = min_row;
            geometry.max_row[index] = max_row;
        }
        return geometry;
    }

    /**
     * @brief Builds the smoothed height field a hillshaded composite needs.
     *
     * Only `CompositeShading::Hillshade` uses this; under the default elevation
     * shading the composite samples `MapGraph::elevation_at()` per pixel and
     * needs nothing precomputed. Build it once per image and hand it to every
     * band through `RenderSlice::height`.
     *
     * Under `ElevationSurface::Blended` the composite needs one too, for a
     * different reason: its elevation shading would otherwise sample the *unsmoothed*
     * flat surface while the elevation layer showed the smoothed one, and the two
     * layers would disagree about the same ground. Pass `extra_blur_cells = 0` for
     * that case -- the raster it reads has already been smoothed by `finish()`, and
     * blurring it again would shade from a field no layer draws.
     *
     * @param graph The map to measure.
     * @param config Supplies the render size and scale.
     * @param palette Passed through to the elevation render it is derived from.
     * @param geometry Shared cell outlines, or null to build them locally.
     * @param extra_blur_cells Box radius to apply on top of the raster, in cells.
     * @return The smoothed field, ready to share.
     */
    static HeightField build_height_field(const MapGraph& graph, const MapConfig& config,
                                          const BiomePalette& palette = BiomePalette{},
                                          const CellGeometry* geometry = nullptr,
                                          double extra_blur_cells = k_shade_blur_cells) {
        if (graph.is_sphere()) {
            return HeightField{};   // a globe shades per pixel (`draw_sphere_()`)
        }
        const double scale = pixels_per_grid_unit_(config);
        // `elevation()` finishes the raster itself, so under `Blended` this is
        // already the smoothed field -- which is the whole point of reading it here
        // rather than sampling `surface_height_()` again.
        const Image raster = elevation(graph, config, palette, RenderSlice{RowBand{}, geometry});
        return smoothed_height_(raster, std::max(0, static_cast<int>(scale * extra_blur_cells)));
    }

    /**
     * @brief The style actually drawn, once a blend too small to see collapses to flat.
     *
     * `Blended` and `Flat` are not merely close at a radius of 0 -- they are the
     * same picture, and the knob is a continuum whose lower end has to *be* the
     * lower style. Resolving it here rather than inside the pass is what makes that
     * exact: the pass works on bytes, so a channel subtracted from an
     * already-quantised height can land a grey level away from one subtracted
     * before quantisation, and "byte-identical" would quietly become "almost".
     *
     * Resolved once per layer and handed down, not recomputed per pixel.
     *
     * @param config Supplies the style and the blend knob.
     * @return The style to draw with.
     */
    static ElevationSurface effective_surface(const MapConfig& config) {
        // Natural terrain IS a smooth surface: drawing it flat-and-blurred would draw the cells,
        // not the ground a 3D consumer stands on.
        if (config.terrain_model == TerrainModel::Natural) {
            return ElevationSurface::Interpolated;
        }
        if (config.elevation_surface == ElevationSurface::Blended
            && blend_radius_(config) <= 0) {
            return ElevationSurface::Flat;
        }
        return config.elevation_surface;
    }

    /**
     * @brief Applies the whole-image passes a layer needs once its pixels are drawn.
     *
     * The counterpart of `allocate()`: `render_into()` draws a *band*, and anything
     * that reads outside the band it is given cannot live there. Today that is the
     * `Blended` elevation surface, which is a blur over the finished raster.
     *
     * Call it exactly once per image, after every band has been drawn. Calling it
     * twice blurs twice; calling it per band would make each band's output depend
     * on which others had run. `render()` and `MapExporter` both do this for the
     * caller -- a caller driving `render_into()` itself is the one that has to.
     *
     * @param image The fully drawn image.
     * @param layer Which layer it holds.
     * @param graph The map it was drawn from.
     * @param config Supplies the surface mode and the blend radius.
     * @param palette Unused today; taken so the signature can carry a layer that needs it.
     * @param slice Supplies shared cell geometry; its band is ignored, by definition.
     */
    static void finish(Image& image, MapLayer layer, const MapGraph& graph,
                       const MapConfig& config, const BiomePalette& palette = BiomePalette{},
                       const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return;
        }
        if (layer == MapLayer::Elevation) {
            smooth_elevation_raster_(image, graph, config, palette, slice.geometry);
        }
    }

    /**
     * @brief Allocates and clears the buffer a layer draws into, drawing nothing.
     *
     * Separate from drawing so a banded render can clear once and then have every
     * band draw into the same buffer. A band that cleared its own rows would be
     * fine; a band that cleared the whole image would erase its neighbours, and
     * the two are one easy mistake apart.
     *
     * @param layer Which layer the buffer is for; decides channels and clear colour.
     * @param config Supplies the image size.
     * @param palette Supplies the background colour for the layers that have one.
     * @return A cleared buffer, RGBA for an overlay layer and RGB otherwise.
     */
    static Image allocate(MapLayer layer, const MapConfig& config,
                          const BiomePalette& palette = BiomePalette{}) {
        Image image;
        // A globe is drawn equirectangular: the full circle of longitude across, half of it down.
        const int rows = config.topology == MapTopology::Sphere ? std::max(1, config.image_size / 2)
                                                                : config.image_size;
        if (map_layer_has_alpha(layer)) {
            image.reset(config.image_size, rows, 4, glm::vec4(0.0f));
            return image;
        }
        const bool on_background = layer == MapLayer::Biomes || layer == MapLayer::Regions
                                || layer == MapLayer::Composite;
        image.reset(config.image_size, rows, 3,
                    on_background ? palette.background_color : glm::vec3(0.0f));
        return image;
    }

    /**
     * @brief Draws a slice of a layer into a buffer `allocate()` already prepared.
     *
     * The entry point a threaded render uses: several calls with disjoint bands,
     * into one buffer, add up to exactly what one whole-image call would have
     * produced.
     *
     * @param image The target, already sized and cleared by `allocate()`.
     * @param layer Which layer to draw.
     * @param graph The map to draw.
     * @param config Supplies the render size, scale and feature widths.
     * @param palette Colours for each biome and overlay.
     * @param slice The rows to draw, and the shared cell geometry to draw from.
     */
    static void render_into(Image& image, MapLayer layer, const MapGraph& graph,
                            const MapConfig& config,
                            const BiomePalette& palette = BiomePalette{},
                            const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            draw_sphere_(image, layer, graph, config, palette, slice.band);
            return;
        }
        switch (layer) {
            case MapLayer::Elevation:  draw_elevation_(image, graph, config, palette, slice); return;
            case MapLayer::Water:      draw_water_(image, graph, config, palette, slice); return;
            case MapLayer::Biomes:     draw_biomes_(image, graph, config, palette, slice); return;
            case MapLayer::Roads:      draw_roads_layer_(image, graph, config, palette, slice); return;
            case MapLayer::Structures: draw_structures_layer_(image, graph, config, palette, slice); return;
            case MapLayer::Landmarks:  draw_landmarks_layer_(image, graph, config, palette, slice); return;
            case MapLayer::Regions:    draw_regions_(image, graph, config, palette, slice); return;
            case MapLayer::Caves:      draw_caves_(image, graph, config, palette, slice); return;
            case MapLayer::Composite:  break;
        }
        draw_composite_(image, graph, config, palette, slice);
    }

    /**
     * @brief Renders one layer.
     * @param layer Which view to draw.
     * @param graph The map to draw.
     * @param config Supplies the image size, world scale and feature widths.
     * @param palette Colours for each biome and overlay.
     * @return The rendered image: RGBA for an overlay layer, RGB otherwise.
     */
    static Image render(MapLayer layer, const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette = BiomePalette{},
                        const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(layer, graph, config, palette, slice);
        }
        switch (layer) {
            case MapLayer::Elevation:  return elevation(graph, config, palette, slice);
            case MapLayer::Water:      return water(graph, config, palette, slice);
            case MapLayer::Biomes:     return biomes(graph, config, palette, slice);
            case MapLayer::Roads:      return roads(graph, config, palette, slice);
            case MapLayer::Structures: return structures(graph, config, palette, slice);
            case MapLayer::Landmarks:  return landmarks(graph, config, palette, slice);
            case MapLayer::Regions:    return regions(graph, config, palette, slice);
            case MapLayer::Caves:      return caves(graph, config, palette, slice);
            case MapLayer::Composite:  break;
        }
        return composite(graph, config, palette, slice);
    }

    /**
     * @brief Terrain height as greyscale, black at sea level and white at the summit.
     *
     * Shaded per pixel from the cell's corner heights rather than filled flat: a
     * flat fill draws the Voronoi tessellation, not the terrain.
     *
     * Rivers do show here, and nothing about that is painted. Dimming the river
     * network into the layer would be wrong twice over -- it would make the layer
     * a picture of the terrain rather than the terrain itself, and a consumer
     * flooding a mesh to those values would find channels already cut.
     *
     * What shows is ground that really is lower, at two scales. `PassValleys`
     * carves the valley into the control mesh, and `make_river_channels()` cuts
     * the channel into the surface sampled between its vertices. The second is
     * what makes a river legible: the mesh has 60 m between sites and a river is
     * 5 to 20 m wide, so carved into cell heights alone the best achievable is a
     * 500 m depression with no edge -- measurably deep and invisible to look at.
     *
     * Set `river_incision_m` and `river_channel_depth_m` to 0 and the network
     * vanishes from here, because then the ground really is flat under it.
     *
     * How the surface is drawn *between* the cells is
     * `MapConfig::elevation_surface`. The default interpolates, which is why a map
     * reads smooth whatever the smoothing passes are set to; `flat` gives one
     * height per cell and a hard edge at every boundary; `blended` draws flat and
     * then smooths the raster, which is why this function calls `finish()` rather
     * than returning the moment the last cell is filled.
     */
    static Image elevation(const MapGraph& graph, const MapConfig& config,
                           const BiomePalette& palette = BiomePalette{},
                           const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Elevation, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Elevation, config, palette);
        draw_elevation_(image, graph, config, palette, slice);
        finish(image, MapLayer::Elevation, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief Height of every water surface, on the same scale as the elevation layer.
     *
     * Read it exactly as `_elevation.png`: the same normalised height, the same
     * greyscale mapping. Where there is water this is the height of its
     * *surface*; where there is none it is black. That works only because
     * `sea_level` is a real height partway up the range, so the sea reads as a
     * visible mid-grey rather than the black that made it indistinguishable from
     * dry land -- which is what it was when sea level sat at the bottom of the
     * field.
     *
     * Three kinds of surface, drawn in an order that matters:
     *
     * - **Rivers first**, at the ground height they run over plus a depth, so the
     *   water stands *in* its channel rather than being painted on the terrain.
     *   Interpolated along the smoothed centreline rather than stepped per
     *   corner, so the fall downstream is continuous.
     * - **Then lakes and the sea**, flat, which therefore win inside their own
     *   outlines. Drawn the other way round -- which they were -- a river stroked
     *   at ground height gouges a channel across the flat lake it flows into.
     * - **Then a rim** along every body's edge, `water_edge_overlap_m` wide, so
     *   the water clips into the terrain instead of meeting it exactly.
     */
    static Image water(const MapGraph& graph, const MapConfig& config,
                       const BiomePalette& palette = BiomePalette{},
                       const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Water, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Water, config, palette);
        draw_water_(image, graph, config, palette, slice);
        return image;
    }

    /** @brief Flat biome colour per cell, tinted by region when `show_regions` is set. */
    static Image biomes(const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette = BiomePalette{},
                        const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Biomes, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Biomes, config, palette);
        draw_biomes_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief Provinces in flat colour, one fill per region and nothing else.
     *
     * The political counterpart of the biome layer, and it registers with it
     * pixel for pixel. Region colours are spread around the hue circle by the
     * region pass, so neighbouring provinces are told apart by hue.
     *
     * No ink over the fills -- no borders, no outlines, no shading. That makes
     * the layer readable as *data*: every pixel is either exactly one region's
     * colour or exactly the background, so a consumer can recover which region
     * covers a pixel by looking it up. Country borders stroked over the top would
     * break that property for every pixel they covered, and read as an artefact
     * besides: a border only exists on a land-to-land edge, so every one of them
     * would dangle into the sea instead of closing around its country.
     *
     * Countries are therefore not distinguishable here. `MapCountry` is still in
     * the graph and in `world.yaml` for anyone who needs it, and the composite
     * still tints each cell toward its region colour.
     *
     * Ocean and unclaimed land are left on the background colour rather than
     * given a colour of their own -- there is nothing political about them, and a
     * fill would imply otherwise.
     */
    static Image regions(const MapGraph& graph, const MapConfig& config,
                         const BiomePalette& palette = BiomePalette{},
                         const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Regions, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Regions, config, palette);
        draw_regions_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief The road network on transparency, stroked at its real width.
     *
     * Trails, then roads, then highways, so a highway is never interrupted by the
     * trail joining it. No casing: a 6 m road over shaded ground reads as a road
     * without an outline, and an outline would make it read as drawn-on ink.
     */
    static Image roads(const MapGraph& graph, const MapConfig& config,
                       const BiomePalette& palette = BiomePalette{},
                       const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Roads, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Roads, config, palette);
        draw_roads_layer_(image, graph, config, palette, slice);
        return image;
    }

    /** @brief Building footprints on transparency, as the rotated quads they are. */
    static Image structures(const MapGraph& graph, const MapConfig& config,
                            const BiomePalette& palette = BiomePalette{},
                            const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Structures, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Structures, config, palette);
        draw_structures_layer_(image, graph, config, palette, slice);
        return image;
    }

    /** @brief Settlement and landmark markers on transparency. */
    static Image landmarks(const MapGraph& graph, const MapConfig& config,
                           const BiomePalette& palette = BiomePalette{},
                           const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Landmarks, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Landmarks, config, palette);
        draw_landmarks_layer_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief Everything at once: lit terrain, then water, roads, buildings, markers.
     *
     * The light comes from the elevation field the map already carries rather
     * than from a second noise source, so it can never disagree with the
     * heightmap layer. Flat biome polygons read as a diagram; lit ones read as
     * ground.
     *
     * `MapConfig::composite_shading` chooses between brightness following
     * *height* -- the default, so high ground is pale -- and brightness
     * following *slope*, which sculpts the relief but is blind to altitude.
     */
    static Image composite(const MapGraph& graph, const MapConfig& config,
                           const BiomePalette& palette = BiomePalette{},
                           const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Composite, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Composite, config, palette);
        draw_composite_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief Every cave system over dimmed terrain, coloured by how deep it runs.
     *
     * The readable one, and the only cave layer meant to be looked at rather than
     * sampled. The terrain behind it is the elevation layer at a fraction of its
     * brightness, which is what lets a reader see *which hill* a system runs under
     * -- a cave drawn on black is a tangle of lines with nothing to locate it
     * against.
     *
     * Passages take their colour from `land_height()`, so the ramp means the same
     * thing here as everywhere else and two systems on opposite sides of a map are
     * directly comparable. Chambers are drawn at their real radius and mouths get a
     * ring, because where a system *opens* is the one thing a reader looks for
     * first.
     *
     * Deliberately absent from the composite. Caves are underground; drawing them
     * on the surface render would be drawing something nobody standing there could
     * see.
     */
    static Image caves(const MapGraph& graph, const MapConfig& config,
                       const BiomePalette& palette = BiomePalette{},
                       const RenderSlice& slice = RenderSlice{}) {
        if (graph.is_sphere()) {
            return sphere_layer_(MapLayer::Caves, graph, config, palette, slice);
        }
        Image image = allocate(MapLayer::Caves, config, palette);
        draw_caves_(image, graph, config, palette, slice);
        return image;
    }

private:
    /** @brief How much of its brightness the terrain keeps behind the cave overview. */
    static constexpr double k_cave_backdrop_dim = 0.22;
    /** @brief Radius of a cave mouth's ring, in metres. */
    static constexpr double k_cave_mouth_radius_m = 26.0;
    /** @brief Share of that radius the ring's bright rim takes. */
    static constexpr double k_cave_mouth_rim = 0.45;
    /** @brief Narrowest depth span the overview's ramp is ever stretched over. */
    static constexpr double k_cave_ramp_minimum = 1e-4;

    /** @brief Draws the readable cave overview into an already-allocated buffer. */
    static void draw_caves_(Image& image, const MapGraph& graph, const MapConfig& config,
                            const BiomePalette& palette, const RenderSlice& slice) {
        const double scale = pixels_per_grid_unit_(config);
        const double inverse_scale = 1.0 / scale;

        // The terrain, dimmed. Drawn from the same sampler the elevation layer uses
        // so the two register: a passage sitting on a ridge here is on that ridge
        // there.
        const TerrainSurface ground(graph, config);
        const TerrainDetail& detail = ground.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const ElevationSurface surface = effective_surface(config);
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon_shaded(
                image, outline,
                [&graph, &center, &detail, &channels, surface, inverse_scale](double px, double py) {
                    const double height =
                        surface_height_(graph, center, px * inverse_scale, py * inverse_scale,
                                        detail, channels, surface);
                    const float grey = static_cast<float>(std::clamp(height, 0.0, 1.0) * 255.0
                                                          * k_cave_backdrop_dim);
                    return glm::vec3(grey, grey, grey);
                },
                slice.band);
        }

        const CaveDepthRange range = cave_depth_range_(graph);
        for (const MapCave& cave : graph.caves) {
            for (const CavePassage& passage : cave.passages) {
                const std::size_t count =
                    std::min({passage.points.size(), passage.floors.size(), passage.radii.size()});
                for (std::size_t i = 0; i + 1 < count; ++i) {
                    const double middle = (passage.floors[i] + passage.floors[i + 1]) * 0.5;
                    const double width = (passage.radii[i] + passage.radii[i + 1]) * 0.5;
                    draw_line(image, passage.points[i].x * scale, passage.points[i].y * scale,
                              passage.points[i + 1].x * scale, passage.points[i + 1].y * scale,
                              std::max(1.0, width * scale), cave_depth_color_(range, palette, middle),
                              slice.band);
                }
            }
            // Chambers at their real radius, so a reader can tell a room from the
            // going between rooms -- which is most of what makes a survey legible.
            for (const CaveNode& node : cave.nodes) {
                if (!is_open_feature(node.feature)) {
                    continue;
                }
                fill_polygon(image, disc_outline_(node.point, node.radius, scale),
                             cave_depth_color_(range, palette, node.floor), slice.band);
            }
        }

        // Mouths last and on top: where a system opens is the first thing anyone
        // looks for, and a ring reads as an entrance where a dot reads as a marker.
        const double mouth_radius = meters_to_grid(config, k_cave_mouth_radius_m);
        for (const MapCave& cave : graph.caves) {
            fill_polygon(image, disc_outline_(cave.mouth, mouth_radius, scale),
                         palette.cave_mouth_color, slice.band);
            fill_polygon(image, disc_outline_(cave.mouth, mouth_radius * k_cave_mouth_rim, scale),
                         cave_depth_color_(range, palette, cave.surface_at_mouth), slice.band);
        }
    }

    /** @brief The span of cave floor heights on a map; what the overview's ramp covers. */
    struct CaveDepthRange {
        double low = 0.0;  /**< @brief Deepest floor anywhere on the map. */
        double high = 1.0; /**< @brief Shallowest. */
    };

    /**
     * @brief The range of floor heights every cave on a map covers.
     *
     * The overview's ramp is stretched over *this* rather than over the land
     * range, and the difference is the difference between a legible picture and a
     * uniform purple tangle. Caves occupy a narrow band low in the land range --
     * floors run from about sea level to perhaps a third of the way up -- so a ramp
     * spanning all of `land_height()` spends most of itself on ground no passage
     * ever reaches and draws every cave on the map the same colour.
     *
     * It does make the overview's colour relative to the map it came from, which is
     * the right trade for the one cave layer whose job is to be *looked at*.
     * Absolute heights are what the four exported surface layers carry, on the
     * elevation layer's own scale, and nothing about those depends on this.
     */
    static CaveDepthRange cave_depth_range_(const MapGraph& graph) {
        CaveDepthRange range;
        bool seen = false;
        for (const MapCave& cave : graph.caves) {
            for (const CavePassage& passage : cave.passages) {
                for (const double floor : passage.floors) {
                    range.low = seen ? std::min(range.low, floor) : floor;
                    range.high = seen ? std::max(range.high, floor) : floor;
                    seen = true;
                }
            }
        }
        if (!seen || range.high - range.low < k_cave_ramp_minimum) {
            // One cave, or a map whose caves all sit at one height. A ramp with no
            // span to stretch over would divide by nothing; widening it about the
            // middle draws them all at the ramp's midpoint instead, which is what
            // "these are all at the same depth" ought to look like.
            const double middle = seen ? (range.low + range.high) * 0.5 : 0.5;
            range.low = middle - k_cave_ramp_minimum * 0.5;
            range.high = middle + k_cave_ramp_minimum * 0.5;
        }
        return range;
    }

    /** @brief Colour for a cave surface at a height, on the palette's depth ramp. */
    static glm::vec3 cave_depth_color_(const CaveDepthRange& range, const BiomePalette& palette,
                                       double height) {
        const float deep = static_cast<float>(
            std::clamp(1.0 - (height - range.low) / (range.high - range.low), 0.0, 1.0));
        return palette.cave_shallow_color * (1.0f - deep) + palette.cave_deep_color * deep;
    }

    /** @brief One whole layer of a globe. */
    static Image sphere_layer_(MapLayer layer, const MapGraph& graph, const MapConfig& config,
                               const BiomePalette& palette, const RenderSlice& slice) {
        Image image = allocate(layer, config, palette);
        draw_sphere_(image, layer, graph, config, palette, slice.band);
        return image;
    }

    /**
     * @brief Draws a globe's layer, equirectangular: x is longitude (-180 to 180 degrees east),
     *        y latitude (north, +z, at the top).
     *
     * Per pixel rather than per cell: each pixel's point on the globe finds its cell by walking
     * from the last pixel's (the Delaunay neighbours lead greedily to the nearest site), then
     * reads the same queries a 3D consumer does -- the ground, the water, the cell's biome and
     * region. No outline crosses the seam or wraps a pole, so neither needs handling. The
     * overlay layers (roads, structures, landmarks) are left empty on a globe, and the cave
     * overview is its dimmed terrain alone.
     */
    static void draw_sphere_(Image& image, MapLayer layer, const MapGraph& graph, const MapConfig& config,
                             const BiomePalette& palette, const RowBand& band) {
        if (graph.centers.empty() || map_layer_has_alpha(layer)) return;
        const int width = image.width;
        const int height = image.height;
        const double radius = graph.sphere_radius;
        const TerrainSurface ground(graph, config);
        const RiverChannels channels = make_river_channels(graph, config, ground.detail());
        const bool needs_water =
            layer == MapLayer::Water || layer == MapLayer::Composite || layer == MapLayer::Caves;
        std::unique_ptr<WaterField> field;
        if (needs_water) field = std::make_unique<WaterField>(graph, config, ground.detail(), channels);
        const double pi = 3.14159265358979323846;
        const int y0 = std::max(0, band.begin);
        const int y1 = std::min(height, band.end);
        std::size_t hint = 0;
        for (int y = y0; y < y1; ++y) {
            const double lat = 0.5 * pi - (static_cast<double>(y) + 0.5) / static_cast<double>(height) * pi;
            for (int x = 0; x < width; ++x) {
                const double lon = -pi + (static_cast<double>(x) + 0.5) / static_cast<double>(width) * 2.0 * pi;
                const MapPoint p{radius * std::cos(lat) * std::cos(lon), radius * std::cos(lat) * std::sin(lon),
                                 radius * std::sin(lat)};
                hint = nearest_cell_(graph, p, hint);
                const MapCenter& center = graph.centers[hint];
                glm::vec3 color(0.0f);
                switch (layer) {
                    case MapLayer::Elevation:
                        color = height_grey_(graph.elevation_at(center, p, ground.detail(), channels));
                        break;
                    case MapLayer::Water: {
                        const WaterSample w = field->at(center, p);
                        color = w.wet() ? height_grey_(w.surface) : glm::vec3(0.0f);
                        break;
                    }
                    case MapLayer::Biomes:
                        color = palette.color_for(center.biome);
                        break;
                    case MapLayer::Regions:
                        color = center.region != k_invalid_id && static_cast<std::size_t>(center.region) < graph.regions.size()
                                    ? graph.regions[static_cast<std::size_t>(center.region)].color
                                    : palette.background_color;
                        break;
                    default: {   // Composite
                        const WaterSample w = field->at(center, p);
                        if (w.kind == WaterKind::River) {
                            color = palette.river_color;
                        } else if (w.wet()) {
                            color = palette.color_for(center.water ? center.biome : Biome::Ocean);
                        } else {
                            const double land = land_height(config, w.ground);
                            color = biome_color_(graph, center, config, palette)
                                    * static_cast<float>(0.8 + 0.4 * std::clamp(land, 0.0, 1.0));
                        }
                        break;
                    }
                }
                if (layer == MapLayer::Caves) color *= static_cast<float>(k_cave_backdrop_dim);   // the terrain, dimmed
                image.set_pixel(x, y, glm::clamp(color, glm::vec3(0.0f), glm::vec3(255.0f)));
            }
        }
    }

    /** @brief The cell whose site is nearest a point of a globe, walking from `hint`. */
    static std::size_t nearest_cell_(const MapGraph& graph, const MapPoint& p, std::size_t hint) {
        std::size_t best = hint < graph.centers.size() ? hint : 0;
        double distance = graph.centers[best].point.distance_to(p);
        for (int guard = 0; guard < 4096; ++guard) {
            std::size_t next = best;
            for (const CenterId n : graph.centers[best].neighbors) {
                const double d = graph.centers[static_cast<std::size_t>(n)].point.distance_to(p);
                if (d < distance) {
                    distance = d;
                    next = static_cast<std::size_t>(n);
                }
            }
            if (next == best) break;
            best = next;
        }
        return best;
    }

    /** @brief Draws the elevation layer into an already-allocated buffer. */
    static void draw_elevation_(Image& image, const MapGraph& graph, const MapConfig& config,
                         const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        const double scale = pixels_per_grid_unit_(config);
        const double inverse_scale = 1.0 / scale;
        // One sampler for the whole layer, not one per pixel: `Noise` wraps a
        // FastNoiseLite and is not free to build.
        const TerrainSurface ground(graph, config);
        const TerrainDetail& detail = ground.detail();
        // Likewise one channel index for the whole layer. Walking the rivers per
        // pixel would be absurd; per layer it is a few milliseconds.
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const ElevationSurface surface = effective_surface(config);
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon_shaded(image, outline,
                [&graph, &center, &detail, &channels, surface,
                 inverse_scale](double px, double py) {
                    const double height = surface_height_(graph, center, px * inverse_scale,
                                                          py * inverse_scale, detail,
                                                          channels, surface);
                    const float grey = static_cast<float>(std::clamp(height, 0.0, 1.0) * 255.0);
                    return glm::vec3(grey, grey, grey);
                }, slice.band);
        }
    }

    /** @brief Draws the water layer into an already-allocated buffer. */
    static void draw_water_(Image& image, const MapGraph& graph, const MapConfig& config,
                            const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        const double scale = pixels_per_grid_unit_(config);
        const double overlap = meters_to_grid(config, config.water_edge_overlap_m);

        // The same detail field the elevation layer uses. A river's surface is
        // measured *from the ground the other layer draws*, so this layer has to
        // sample the identical surface or the two disagree about where the bed is.
        const TerrainSurface ground(graph, config);
        const TerrainDetail& detail = ground.detail();
        // The channels too, because the sheet settles into its bed as it nears the
        // mouth rather than staying up on the rim -- which is what lets it meet the
        // sea flush instead of ending a few metres above it.
        const RiverChannels channels = make_river_channels(graph, config, detail);
        const RiverSurfaces surfaces = make_river_surfaces(graph, config, detail, channels);

        // Rivers first, so a body drawn over them keeps its flat surface.
        for (std::size_t i = 0; i < graph.rivers.size(); ++i) {
            stroke_river_surface_(image, graph, graph.rivers[i], config, surfaces, i, overlap,
                                  slice.band);
        }

        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            if (center.water_level <= 0.0) {
                continue;
            }
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            const glm::vec3 surface = height_grey_(center.water_level);
            fill_polygon(image, outline, surface, slice.band);

            // And a rim, so the sheet overhangs its own edge and clips into the
            // terrain rather than sharing a boundary with it exactly.
            if (overlap > 0.0) {
                const double half_width = half_width_pixels_(overlap * 2.0, scale);
                for (std::size_t i = 0; i < outline.size(); ++i) {
                    const MapPoint& from = outline[i];
                    const MapPoint& to = outline[(i + 1) % outline.size()];
                    draw_line(image, from.x, from.y, to.x, to.y, half_width, surface,
                              slice.band);
                }
            }
        }
    }

    /**
     * @brief Strokes a river's water surface at the heights `RiverSurfaces` settled on.
     *
     * The height is deliberately not computed here. Interpolating between
     * `MapCorner::elevation` values would follow the control mesh rather than the
     * surface the elevation layer draws, putting half of every watercourse beneath
     * the terrain; computing it per segment in isolation cannot see that a water
     * surface only falls, nor that a river has a sea to meet. Both are properties
     * of the course, so the course is where they are decided.
     *
     * Width is still stepped along the corner chain here, since that is a property
     * of the stroke rather than of the surface: depth and width both rise with
     * volume, a headwater stream being a trickle in a groove and a trunk river
     * neither.
     */
    static void stroke_river_surface_(Image& image, const MapGraph& graph,
                                      const MapRiver& river, const MapConfig& config,
                                      const RiverSurfaces& surfaces, std::size_t river_index,
                                      double overlap, const RowBand& band) {
        if (river.points.size() < 2 || river.corners.empty()) {
            return;
        }
        const double scale = pixels_per_grid_unit_(config);
        const std::size_t segments = river.points.size() - 1;
        const std::size_t last_corner = river.corners.size() - 1;

        const auto corner_at = [&graph, &river, last_corner](std::size_t index) -> const MapCorner& {
            return graph.corners[static_cast<std::size_t>(
                river.corners[std::min(index, last_corner)])];
        };

        for (std::size_t i = 0; i < segments; ++i) {
            // Where along the corner chain this segment sits, as a real number, so
            // both the height and the volume can be interpolated rather than
            // stepped.
            const double along = static_cast<double>(last_corner)
                               * static_cast<double>(i) / static_cast<double>(segments);
            const std::size_t index = static_cast<std::size_t>(along);
            const double fraction = along - static_cast<double>(index);

            const MapCorner& from = corner_at(index);
            const MapCorner& to = corner_at(index + 1);
            const double volume = static_cast<double>(from.river)
                                + (static_cast<double>(to.river)
                                   - static_cast<double>(from.river)) * fraction;

            const double surface = surfaces.at(river_index, i);
            const double width = river_width(config, static_cast<int>(volume)) + overlap * 2.0;

            draw_line(image, river.points[i].x * scale, river.points[i].y * scale,
                      river.points[i + 1].x * scale, river.points[i + 1].y * scale,
                      half_width_pixels_(width, scale), height_grey_(surface), band);
        }
    }

    /** @brief Draws the biomes layer into an already-allocated buffer. */
    static void draw_biomes_(Image& image, const MapGraph& graph, const MapConfig& config,
                             const BiomePalette& palette, const RenderSlice& slice) {
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon(image, outline, biome_color_(graph, center, config, palette),
                         slice.band);
        }
    }

    /** @brief Draws the regions layer into an already-allocated buffer. */
    static void draw_regions_(Image& image, const MapGraph& graph, const MapConfig& config,
                              const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            if (center.region == k_invalid_id
                || static_cast<std::size_t>(center.region) >= graph.regions.size()) {
                continue;
            }
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon(image, outline,
                         graph.regions[static_cast<std::size_t>(center.region)].color,
                         slice.band);
        }
    }

    /** @brief Draws the roads layer into an already-allocated buffer. */
    static void draw_roads_layer_(Image& image, const MapGraph& graph, const MapConfig& config,
                                  const BiomePalette& palette, const RenderSlice& slice) {
        draw_roads_(image, graph, config, palette, slice.band);
    }

    /** @brief Draws the structures layer into an already-allocated buffer. */
    static void draw_structures_layer_(Image& image, const MapGraph& graph, const MapConfig& config,
                                       const BiomePalette& palette, const RenderSlice& slice) {
        draw_structures_(image, graph, config, palette, slice.band);
    }

    /** @brief Draws the landmarks layer into an already-allocated buffer. */
    static void draw_landmarks_layer_(Image& image, const MapGraph& graph, const MapConfig& config,
                                      const BiomePalette& palette, const RenderSlice& slice) {
        draw_markers_(image, graph, config, palette, slice.band);
    }

    /** @brief Draws the composite layer into an already-allocated buffer. */
    static void draw_composite_(Image& image, const MapGraph& graph, const MapConfig& config,
                         const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        const double scale = pixels_per_grid_unit_(config);
        const double inverse_scale = 1.0 / scale;
        const TerrainSurface ground(graph, config);
        const TerrainDetail& detail = ground.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        Outlines outlines(graph, config, slice);

        // Only the slope mode needs the rasterised height field, and it is the
        // expensive half of this function -- a second full-resolution pass plus a
        // blur. It is always built from the *whole* image, never from the band:
        // the blur and the gradient both read outside the band, so a band-local
        // field would make one band's output depend on which others had run.
        //
        // Which is exactly why a caller drawing in bands must pass one in. Built
        // here per band instead, the whole raster is rebuilt once per band.
        //
        // The elevation mode needs one too under `ElevationSurface::Blended`, and
        // for a reason worth stating: the blend lives in the raster, not in
        // `surface_height_()`, so shading from the sampler would light the composite
        // by the *unsmoothed* flat surface while the elevation layer showed the
        // smoothed one. Two layers disagreeing about the same ground is a class of
        // bug this renderer has produced before.
        const ElevationSurface surface = effective_surface(config);
        const bool shade_from_raster = config.composite_shading == CompositeShading::Hillshade
                                    || surface == ElevationSurface::Blended;

        HeightField local;
        const HeightField* height = slice.height;
        int baseline = 1;
        if (shade_from_raster) {
            if (height == nullptr) {
                // No extra blur under the elevation mode: the raster arrives
                // smoothed already, and blurring it again would shade from a field
                // no layer draws.
                const double extra = config.composite_shading == CompositeShading::Hillshade
                                       ? k_shade_blur_cells
                                       : 0.0;
                local = build_height_field(graph, config, palette, slice.geometry, extra);
                height = &local;
            }
            baseline = std::max(1, static_cast<int>(scale * k_shade_baseline_cells));
        }

        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            const glm::vec3 base = biome_color_(graph, center, config, palette);
            // Water is filled flat under either mode: a lake surface is level,
            // and shading it from the basin beneath would ripple still water.
            if (center.water) {
                fill_polygon(image, outline, base, slice.band);
                continue;
            }
            if (config.composite_shading == CompositeShading::Hillshade) {
                fill_polygon_shaded(image, outline,
                    [height, base, baseline](double px, double py) {
                        return base * hillshade_(*height, static_cast<int>(px),
                                                 static_cast<int>(py), baseline);
                    }, slice.band);
            } else if (surface == ElevationSurface::Blended) {
                fill_polygon_shaded(image, outline,
                    [height, base](double px, double py) {
                        return base * height_tint_(height->at(static_cast<int>(px),
                                                              static_cast<int>(py)));
                    }, slice.band);
            } else {
                fill_polygon_shaded(image, outline,
                    [&graph, &center, &detail, &channels, base, surface,
                     inverse_scale](double px, double py) {
                        return base * elevation_shade_(graph, center, px * inverse_scale,
                                                       py * inverse_scale, detail, channels,
                                                       surface);
                    }, slice.band);
            }
        }

        for (const MapRiver& river : graph.rivers) {
            stroke_river_(image, graph, river, config,
                          [&palette](const MapCorner&) { return palette.river_color; },
                          slice.band);
        }
        draw_roads_(image, graph, config, palette, slice.band);
        draw_structures_(image, graph, config, palette, slice.band);
        draw_markers_(image, graph, config, palette, slice.band);
    }

    /** @brief Light direction for the hillshade: from the north-west, 45 degrees up. */
    static constexpr double k_light_x = -0.5773502692;
    static constexpr double k_light_y = -0.5773502692;
    static constexpr double k_light_z = 0.5773502692;
    /**
     * @brief How much the height field is exaggerated before its normal is taken.
     *
     * Elevation is normalised to `[0, 1]` across a 4.8 km world, so the true
     * gradients are numerically tiny -- a whole grid cell of the steepest ground
     * climbs a few thousandths -- and an unexaggerated normal points almost
     * straight up everywhere, giving a uniform grey wash. This is a drawing
     * choice, and deliberately does not feed back into the height data.
     */
    static constexpr double k_relief_exaggeration = 600.0;
    /**
     * @brief Gradient baseline for the hillshade, as a fraction of a cell.
     *
     * Wide enough to step over the per-cell interpolation and read the shape of
     * the land; narrow enough that a ridge still casts an edge.
     */
    static constexpr double k_shade_baseline_cells = 0.5;
    /** @brief Box-blur radius applied to the height field before shading, in cells. */
    static constexpr double k_shade_blur_cells = 0.45;
    /**
     * @brief Multiplier applied to a biome colour at sea level, and at the summit.
     *
     * A wide range on purpose. Elevation is rank-remapped by the elevation pass
     * so the land spans most of `[0, 1]`, and a timid range would leave a
     * continent looking uniformly flat -- the one thing this mode exists to
     * avoid. Values above 1 clamp per channel on the way to the buffer, so a
     * biome already near white simply saturates rather than wrapping.
     */
    static constexpr double k_elevation_shade_min = 0.68;
    static constexpr double k_elevation_shade_max = 1.24;
    /** @brief Darkest the hillshade may drive a biome colour. */
    static constexpr float k_shade_min = 0.58f;
    /** @brief Brightest the hillshade may drive a biome colour. */
    static constexpr float k_shade_max = 1.14f;
    /** @brief Contrast applied to the Lambertian term before clamping. */
    static constexpr float k_shade_gain = 1.28f;
    /** @brief How far a bridge parapet reaches either side of the road, in road widths. */
    static constexpr double k_bridge_span = 0.9;
    /** @brief Marker half-width in metres for a capital, a town and a village. */
    static constexpr double k_capital_marker_m = 12.0;
    static constexpr double k_town_marker_m = 8.0;
    static constexpr double k_village_marker_m = 5.0;
    /** @brief Marker half-width in metres for a wonder, and for every other landmark. */
    static constexpr double k_wonder_marker_m = 10.0;
    /**
     * @brief Outer radius of the ring drawn at a cave mouth, in metres.
     *
     * Sized against `k_landmark_marker_m` and `k_wonder_marker_m` rather than
     * against the overview's `k_cave_mouth_radius_m`, which is nearly three times
     * this: the overview has a whole image to itself and can afford a bold ring,
     * while this one has to sit among town and landmark markers without shouting
     * over them.
     */
    static constexpr double k_cave_mouth_marker_m = 8.0;
    static constexpr double k_landmark_marker_m = 6.0;

    /** @brief Pixels per grid unit; equals `meters_per_grid_unit / meters_per_pixel`. */
    static double pixels_per_grid_unit_(const MapConfig& config) {
        return static_cast<double>(config.image_size) / static_cast<double>(config.grid_size);
    }

    /**
     * @brief Converts a width in grid units to a stroke half-width in pixels.
     *
     * Kept fractional. `draw_line()` paints by distance to the segment, so it can
     * honour a half-width of 3.0 exactly and draw a 6 m road 6 pixels across --
     * where a pixel-centred brush could only manage the nearest odd number.
     */
    static double half_width_pixels_(double width_grid, double scale) {
        return std::max(0.0, width_grid * scale * 0.5);
    }

    /** @brief A normalised height as a greyscale colour. */
    static glm::vec3 height_grey_(double height) {
        const float grey = static_cast<float>(std::clamp(height, 0.0, 1.0) * 255.0);
        return glm::vec3(grey, grey, grey);
    }

    /**
     * @brief Converts a length in metres to whole pixels, at least 1.
     *
     * Routed through `pixels_per_grid_unit_()` -- the same scale every other
     * width in this renderer uses -- rather than dividing by
     * `MapConfig::meters_per_pixel` directly. The two are equal whenever
     * `image_size * meters_per_pixel == grid_size * meters_per_grid_unit`, which
     * is the invariant `MapConfig::image_size` documents and the config loader
     * enforces. They are *not* equal when a caller sets `image_size` by hand and
     * leaves the scale behind, and dividing by `meters_per_pixel` there would
     * draw markers oversized (7.5x on the test renders) while the roads and rivers
     * beside them came out right.
     *
     * Taking one scale for the whole renderer means a resolution set either way
     * round scales every feature together, and the invariant stops being load
     * bearing here.
     */
    static int meters_to_pixels_(double meters, const MapConfig& config) {
        return std::max(1, static_cast<int>(meters_to_grid(config, meters)
                                            * pixels_per_grid_unit_(config)));
    }

    /**
     * @class Outlines
     * @brief Hands out a cell's pixel-space outline, shared if the caller has one.
     *
     * Two jobs in one place. It hides whether the geometry was precomputed -- a
     * threaded render shares one `CellGeometry` across every band and every layer,
     * a lone call builds outlines as it goes -- and it performs the band check,
     * returning an empty outline for a cell that does not reach into the rows
     * being drawn.
     *
     * The band check needs the *real* outline extent, which is why it lives here
     * rather than being estimated from the corners: a subdivided boundary bulges
     * outside the straight corner polygon, and a cell wrongly skipped leaves a
     * hairline of background along a band seam.
     */
    class Outlines {
    public:
        Outlines(const MapGraph& graph, const MapConfig& config, const RenderSlice& slice)
            : scale_(pixels_per_grid_unit_(config)),
              shared_(slice.geometry != nullptr && slice.geometry->covers(graph.centers.size())
                          ? slice.geometry
                          : nullptr),
              graph_(graph) {}

        /**
         * @brief The cell's outline, or an empty one if it misses the band.
         * @param center The cell to outline.
         * @param slice The band being drawn.
         * @return A reference valid until the next call for a different cell.
         */
        const std::vector<MapPoint>& of(const MapCenter& center, const RenderSlice& slice) {
            const std::size_t index = static_cast<std::size_t>(center.index);
            if (shared_ != nullptr) {
                if (!slice.band.intersects(shared_->min_row[index], shared_->max_row[index])) {
                    return empty_;
                }
                return shared_->outlines[index];
            }

            scratch_ = graph_.cell_outline(center);
            int min_row = std::numeric_limits<int>::max();
            int max_row = std::numeric_limits<int>::min();
            for (MapPoint& point : scratch_) {
                point.x *= scale_;
                point.y *= scale_;
                min_row = std::min(min_row, static_cast<int>(std::floor(point.y)));
                max_row = std::max(max_row, static_cast<int>(std::ceil(point.y)));
            }
            if (scratch_.empty() || !slice.band.intersects(min_row, max_row)) {
                return empty_;
            }
            return scratch_;
        }

    private:
        double scale_ = 1.0;
        const CellGeometry* shared_ = nullptr;
        const MapGraph& graph_;
        std::vector<MapPoint> scratch_;
        std::vector<MapPoint> empty_;
    };

    /** @brief A cell's biome colour, tinted by its region when the config asks for it. */
    static glm::vec3 biome_color_(const MapGraph& graph, const MapCenter& center,
                                  const MapConfig& config, const BiomePalette& palette) {
        glm::vec3 color = palette.color_for(center.biome);
        if (config.show_regions && center.region != k_invalid_id
            && static_cast<std::size_t>(center.region) < graph.regions.size()) {
            // Tint rather than replace: a political map that hides the terrain
            // under flat colour stops being a map of the world.
            const glm::vec3& tint = graph.regions[static_cast<std::size_t>(center.region)].color;
            color = color * (1.0f - config.region_tint) + tint * config.region_tint;
        }
        return color;
    }

    /**
     * @brief Brightness factor from height alone: the higher the ground, the paler it is.
     *
     * Read straight from `MapGraph::elevation_at()`, which is interpolated from
     * the cell's own corners and agrees with its neighbour along a shared edge --
     * so the result is continuous across the map and needs none of the machinery
     * the slope mode does. The cusps that ruin a *gradient* taken from this
     * function are harmless to its *value*, which is all this reads.
     *
     * Being a function of elevation and nothing else, the same height comes out
     * the same brightness wherever it appears, which is what makes the shading
     * legible rather than merely decorative.
     *
     * @param graph The map being shaded.
     * @param center The cell the pixel falls in.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param detail The terrain detail field, so the shading matches the height layer.
     * @param channels The river channels, for the same reason -- a composite whose
     *        ground was not cut where the height layer's was would light the
     *        watercourses as if they were not there.
     * @return A multiplier for the surface colour, in
     *         `[k_elevation_shade_min, k_elevation_shade_max]`.
     */
    static float elevation_shade_(const MapGraph& graph, const MapCenter& center, double x,
                                  double y, const TerrainDetail& detail,
                                  const RiverChannels& channels, ElevationSurface surface) {
        return height_tint_(surface_height_(graph, center, x, y, detail, channels, surface));
    }

    /**
     * @brief The elevation tint for a height, whether sampled or read off a raster.
     *
     * Split out so the `Blended` composite, which has to read the smoothed raster
     * rather than call the sampler, produces the *identical* brightness for an
     * identical height. Two code paths computing the same ramp is how a composite
     * comes to disagree with the layer it is meant to match.
     *
     * @param height A normalised height; clamped, so a cut river bed is safe.
     * @return A multiplier in `[k_elevation_shade_min, k_elevation_shade_max]`.
     */
    static float height_tint_(double height) {
        const double clamped = std::clamp(height, 0.0, 1.0);
        return static_cast<float>(k_elevation_shade_min
                                  + (k_elevation_shade_max - k_elevation_shade_min) * clamped);
    }

    /**
     * @brief Extracts the height raster and box-blurs it, for shading only.
     *
     * The blur is what removes the cell faceting. Elevation inside a cell is
     * interpolated from that cell's own corners, so the surface is continuous
     * across a shared edge but its *slope* is not -- and a hillshade is a
     * function of slope, so every cell boundary shows up as a crease. Widening
     * the gradient stencil alone only softens it; smoothing the field first
     * removes it, and the terrain-scale relief survives because the blur is a
     * fraction of a cell while the landforms are many cells across.
     *
     * Separable, two passes with a running sum, so cost is linear in pixels
     * rather than in the square of the radius. The blurred field is never
     * written out -- the elevation layer on disk is the unsmoothed original.
     *
     * @param source The rendered elevation layer.
     * @param radius Box radius in pixels.
     * @return The smoothed single-channel field.
     */
    /**
     * @brief How wide a blur `elevation_blend = 1` asks for, in cells.
     *
     * One cell is the ceiling because that is where the mode stops being a blend
     * and starts being a different map: a radius wider than the tessellation
     * itself erases the landforms along with the facets, and the interpolated
     * surface is already there for anyone who wants smooth ground.
     */
    static constexpr double k_blend_max_cells = 1.0;

    /** @brief The blend radius in whole pixels; 0 when the knob is off. */
    static int blend_radius_(const MapConfig& config) {
        const double cells = std::clamp(config.elevation_blend, 0.0, 1.0) * k_blend_max_cells;
        return static_cast<int>(cells * pixels_per_grid_unit_(config));
    }

    /**
     * @brief Widest the per-cell factor may be feathered, as a fraction of a cell.
     *
     * The factor is drawn one flat value per cell, which would put a hard step in
     * the *weight* exactly where the weight matters most -- a cell boundary is
     * where the sharp and blurred rasters differ by the whole facet, so a jump in
     * the weight there redraws the edge this mode exists to remove.
     *
     * Feathering fixes that, and the width is a compromise with a floor and a
     * ceiling. Too narrow and the seam survives; too wide and neighbouring cells
     * average into each other and the variation is gone.
     *
     * The width itself comes from `blend_radius_()`, not from here: the blur radius
     * is the distance over which `blurred - sharp` is non-trivial near an edge, so
     * it is exactly how far the weight has to travel to hide the step. Sizing the
     * feather against the *cell* instead would be wrong -- at a 3000 px grid-80
     * render with `elevation_blend = 0.1` that gives a 7 px feather against a 3 px
     * blur, smearing each cell's value across its neighbours and averaging the
     * whole effect away.
     *
     * This caps it, for the other end of the range: at `elevation_blend = 1` the
     * radius is a whole cell, and feathering by a whole cell would homogenise
     * neighbours just as thoroughly.
     */
    static constexpr double k_factor_feather_max_cells = 0.25;

    /**
     * @struct BlendField
     * @brief The local blur factor over the image, one value per cell.
     *
     * Stored as a raster rather than a per-cell array because that is how it is
     * read -- per pixel, alongside the pyramid levels it weights -- and because
     * feathering it is then the same box blur everything else here uses.
     *
     * Values are packed into a byte over `[0, 2]`, so the factor resolves to about
     * 0.008. Against a knob whose whole range is 2 that is nothing, and it keeps
     * the field the same size as the three rasters it selects between.
     */
    struct BlendField {
        HeightField values; /**< @brief Factor at each pixel, scaled to `[0, 1]`. */

        /**
         * @brief The factor at a pixel.
         * @param x Horizontal pixel position.
         * @param y Vertical pixel position.
         * @return The local blur factor, 1 meaning exactly `elevation_blend`.
         */
        double at(int x, int y) const { return values.at(x, y) * 2.0; }
    };

    /**
     * @brief Builds the per-cell blur factor, from `noise_blend`.
     *
     * **Per cell, and that is the whole point.** The first version of this sampled
     * a low-frequency field -- a wavelength of some twenty cells -- on the theory
     * that smoothness should vary the way bedrock hardness does. It is a defensible
     * idea and it produced nothing anyone could see: across any handful of
     * neighbouring cells the factor was effectively constant, so the map came out
     * uniformly blurred with the variation only visible by flying across the whole
     * thing. What varies has to vary at the scale of the thing it varies.
     *
     * So each cell draws its own value, sampled at its site, and `noise_blend` runs
     * at a frequency high enough that neighbours are uncorrelated -- some cells
     * keep hard edges while the cell beside them is fully smoothed.
     *
     * The factor is `1 + variation * n` for a field value `n` in `[-1, 1]`,
     * clamped to `[0, 2]`. Symmetric about 1 on purpose: the mean radius stays
     * `elevation_blend`, so raising the variation makes a map *more varied* rather
     * than uniformly softer or sharper, which is what a knob named "variation" has
     * to mean.
     *
     * Sampled in grid units, not pixels, so the same config gives the same cells
     * the same character at any render size.
     *
     * @param config Supplies the variation amount, the field and the render size.
     * @param graph The map whose cells carry the factor.
     * @param palette Passed through to the scratch buffer's allocation.
     * @param geometry Shared cell outlines, or null to build them locally.
     * @return The feathered factor raster, ready to sample.
     */
    static BlendField make_blend_field_(const MapConfig& config, const MapGraph& graph,
                                        const BiomePalette& palette,
                                        const CellGeometry* geometry) {
        const double variation = std::clamp(config.elevation_blend_variation, 0.0, 1.0);
        const Noise noise(config.noise_blend);

        // Drawn through the ordinary polygon fill so the factor lands on exactly
        // the pixels the cell was drawn on -- a separate point-in-cell test here
        // could disagree with the rasteriser at a boundary, and a one-pixel
        // disagreement is a one-pixel seam.
        // Cleared to the neutral factor rather than to black: a pixel no cell
        // covers should fall back to the base blur, not to no blur at all. Cleared
        // by `reset()` rather than by a loop of `set_pixel()` -- at the default
        // 4800 px render that loop was 23 million calls to write a constant.
        (void)palette;
        Image field;
        field.reset(config.image_size, config.image_size, 3, height_grey_(0.5));
        const RenderSlice whole{RowBand{}, geometry};
        Outlines outlines(graph, config, whole);
        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, whole);
            if (outline.empty()) {
                continue;
            }
            const double sample = noise.sample(center.point.x, center.point.y);
            const double factor = std::clamp(1.0 + variation * sample, 0.0, 2.0);
            fill_polygon(field, outline, height_grey_(factor * 0.5), whole.band);
        }

        const double ceiling = pixels_per_grid_unit_(config) * k_factor_feather_max_cells;
        const int feather =
            std::clamp(blend_radius_(config), 1, std::max(1, static_cast<int>(ceiling)));

        BlendField built;
        built.values = smoothed_height_(field, feather);
        return built;
    }

    /**
     * @brief Smooths the finished flat raster, and cuts the rivers back into it.
     *
     * What `ElevationSurface::Blended` actually is. The first attempt decided a
     * blend *per pixel per cell*, pulling the flat height toward the interpolated
     * one near the cell's rim, and that cannot work however it is tuned: every cell
     * independently ramps its own edge, so the map comes out as a field of tiles
     * each wearing a bevel, with a soft halo tracing every outline. It drew the
     * tessellation more clearly than the hard edges it was meant to soften.
     *
     * A blend is not a property of a cell. It is a property of the *image*, so it
     * belongs here, after the rasteriser has put down one flat height per cell and
     * there is a grid to smooth.
     *
     * Three stages, and the order is what makes the rivers survive:
     *
     * 1. `surface_height_()` has already drawn the cells flat and, under this mode
     *    only, **uncut** -- no channel subtracted.
     * 2. Blur the whole raster, the same separable box pass the hillshade uses.
     * 3. Cut the channels in now. A half-cell blur costs an uncut river nothing and
     *    a pre-cut one almost half its contrast -- 15.0 grey levels down to 8.0 --
     *    because a channel a few pixels wide is exactly the feature a blur of that
     *    radius destroys. Cutting afterwards gives back all 15.
     *
     * A radius of 0 returns without touching a pixel, so `elevation_blend = 0` is
     * byte-identical to `ElevationSurface::Flat` rather than merely close to it.
     *
     * ### Varying the radius
     *
     * `elevation_blend_variation` makes stage 2 a *pyramid* instead of one blur:
     * the untouched raster, a blur at the base radius, and a blur at twice it, with
     * `make_blend_field_()` choosing **per cell** where between them to land -- so
     * one cell keeps its hard edges while the cell beside it is fully smoothed.
     *
     * A box blur whose radius genuinely changed per pixel is not one filter but a
     * different one at every pixel, and neighbouring pixels drawing from
     * differently-sized boxes have no reason to agree. Three globally consistent
     * blurs and a feathered weight cannot seam by construction, and cost two passes
     * over the image rather than a different convolution per pixel.
     *
     * Three levels rather than two so the variation is symmetric: a pixel can land
     * either side of the base radius, the mean stays where `elevation_blend` put
     * it, and the knob adds variety rather than sharpening the whole map. At
     * variation 0 the pyramid is not built at all and the single-blur path runs
     * unchanged, byte for byte.
     *
     * @param image The finished elevation raster, smoothed in place.
     * @param graph The map it was drawn from; supplies the cells and their channels.
     * @param config Supplies the surface mode, the blend radius, the variation and
     *        the scale.
     * @param geometry Shared cell outlines, or null to build them locally.
     */
    static void smooth_elevation_raster_(Image& image, const MapGraph& graph,
                                         const MapConfig& config, const BiomePalette& palette,
                                         const CellGeometry* geometry) {
        // `effective_surface()` has already turned a radius too small to see back
        // into `Flat`, and the cells were then drawn *with* their channels -- so
        // there is nothing here to do, and doing it would cost a grey level to
        // double quantisation.
        if (effective_surface(config) != ElevationSurface::Blended) {
            return;
        }

        const int radius = blend_radius_(config);
        const HeightField blurred = smoothed_height_(image, radius);
        const double variation = std::clamp(config.elevation_blend_variation, 0.0, 1.0);

        if (variation <= 0.0) {
            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    image.set_pixel(x, y, height_grey_(blurred.at(x, y)));
                }
            }
        } else {
            // The two ends of the pyramid. `sharp` is the raster as drawn, which is
            // why it is taken at radius 0 rather than kept as the image: the image
            // is about to be written over.
            const HeightField sharp = smoothed_height_(image, 0);
            const HeightField wide = smoothed_height_(image, radius * 2);
            const BlendField factor = make_blend_field_(config, graph, palette, geometry);

            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    const double f = factor.at(x, y);
                    const double height =
                        f <= 1.0
                            ? sharp.at(x, y) + (blurred.at(x, y) - sharp.at(x, y)) * f
                            : blurred.at(x, y) + (wide.at(x, y) - blurred.at(x, y)) * (f - 1.0);
                    image.set_pixel(x, y, height_grey_(height));
                }
            }
        }

        // Only the cells a river runs through are revisited. Rivers cover a
        // fraction of a percent of a map, so this is a rounding error against the
        // blur that precedes it -- and it is why the channel index is consulted
        // before the outline is ever asked for.
        const TerrainSurface ground(graph, config);
        const TerrainDetail& detail = ground.detail();
        const RiverChannels channels = make_river_channels(graph, config, detail);
        if (channels.empty()) {
            return;
        }

        const double inverse_scale = 1.0 / pixels_per_grid_unit_(config);
        const RenderSlice whole{RowBand{}, geometry};
        Outlines outlines(graph, config, whole);
        for (const MapCenter& center : graph.centers) {
            const std::size_t index = static_cast<std::size_t>(center.index);
            if (index >= channels.cells.size() || channels.cells[index].count == 0) {
                continue;
            }
            const std::vector<MapPoint>& outline = outlines.of(center, whole);
            if (outline.empty()) {
                continue;
            }
            fill_polygon_shaded(image, outline,
                [&graph, &center, &channels, &blurred, inverse_scale](double px, double py) {
                    const double cut = graph.channel_cut(center, px * inverse_scale,
                                                         py * inverse_scale, channels);
                    const double height =
                        blurred.at(static_cast<int>(px), static_cast<int>(py)) - cut;
                    return height_grey_(height);
                }, whole.band);
        }
    }

    static HeightField smoothed_height_(const Image& source, int radius) {
        HeightField field;
        field.width = source.width;
        field.height = source.height;
        const std::size_t count =
            static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height);
        field.values.resize(count);

        std::vector<unsigned char> scratch(count);
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                scratch[static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width)
                        + static_cast<std::size_t>(x)] =
                    static_cast<unsigned char>(source.color_at(x, y).r);
            }
        }

        const auto blur_axis = [radius](const std::vector<unsigned char>& in,
                                        std::vector<unsigned char>& out, int width, int height,
                                        bool horizontal) {
            const int along = horizontal ? width : height;
            const int across = horizontal ? height : width;
            for (int a = 0; a < across; ++a) {
                const auto read = [&in, width, a, horizontal](int b) {
                    const std::size_t index =
                        horizontal ? static_cast<std::size_t>(a) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(b)
                                   : static_cast<std::size_t>(b) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(a);
                    return static_cast<int>(in[index]);
                };
                int total = 0;
                for (int b = -radius; b <= radius; ++b) {
                    total += read(std::clamp(b, 0, along - 1));
                }
                const int window = 2 * radius + 1;
                for (int b = 0; b < along; ++b) {
                    const std::size_t index =
                        horizontal ? static_cast<std::size_t>(a) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(b)
                                   : static_cast<std::size_t>(b) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(a);
                    out[index] = static_cast<unsigned char>(total / window);
                    total -= read(std::clamp(b - radius, 0, along - 1));
                    total += read(std::clamp(b + radius + 1, 0, along - 1));
                }
            }
        };

        blur_axis(scratch, field.values, source.width, source.height, true);
        blur_axis(field.values, scratch, source.width, source.height, false);
        field.values.swap(scratch);
        return field;
    }

    /**
     * @brief Lambertian shading factor from the slope of the rasterised height field.
     *
     * The gradient is taken over a baseline of most of a cell, not over one
     * pixel, and over a field already smoothed by `smoothed_height_()`. A
     * one-pixel gradient on the raw raster picks up the inverse-distance
     * weighting inside each cell -- which is cusped at the corners -- and lights
     * every cell as its own little dome. Between them the blur and the wide
     * stencil step over that structure entirely and leave the slope of the
     * landscape, which is the thing worth drawing.
     *
     * Edges clamp rather than read off the raster, so the map border does not
     * light up as a cliff down to zero.
     *
     * @param height The smoothed height field.
     * @param x Column in pixels.
     * @param y Row in pixels.
     * @param baseline Half the gradient stencil, in pixels.
     * @return A multiplier for the surface colour, in `[k_shade_min, k_shade_max]`.
     */
    static float hillshade_(const HeightField& height, int x, int y, int baseline) {
        const double left = height.at(x - baseline, y);
        const double right = height.at(x + baseline, y);
        const double up = height.at(x, y - baseline);
        const double down = height.at(x, y + baseline);

        const double span = 2.0 * static_cast<double>(baseline);
        const double dx = (right - left) / span * k_relief_exaggeration;
        const double dy = (down - up) / span * k_relief_exaggeration;
        // Normal of the exaggerated height field is (-dx, -dy, 1); dotting it
        // with the light and dividing by its length normalises in one step.
        const double length = std::sqrt(dx * dx + dy * dy + 1.0);
        const double lambert = (-dx * k_light_x - dy * k_light_y + k_light_z) / length;

        return std::clamp(static_cast<float>(lambert) * k_shade_gain, k_shade_min, k_shade_max);
    }

    /**
     * @brief Strokes a river along its smoothed centreline.
     *
     * Width comes from the volume carried at each corner, so a channel widens
     * toward its mouth as tributaries join it. The shader is handed the nearest
     * corner so a caller can colour by the height of the ground there.
     *
     * @tparam Shader Callable of `(const MapCorner&) -> Color`.
     */
    template <typename Shader>
    static void stroke_river_(Image& image, const MapGraph& graph, const MapRiver& river,
                              const MapConfig& config, Shader&& shader,
                              const RowBand& band = RowBand{}) {
        if (river.points.size() < 2 || river.corners.empty()) {
            return;
        }
        const double scale = pixels_per_grid_unit_(config);
        const std::size_t segments = river.points.size() - 1;
        const std::size_t last_corner = river.corners.size() - 1;

        for (std::size_t i = 0; i < segments; ++i) {
            // Map a smoothed point back onto the corner chain it came from:
            // corner-cutting multiplies the point count, so the relationship is
            // proportional rather than one-to-one.
            const std::size_t corner_index =
                std::min(last_corner, river.corners.size() * i / segments);
            const MapCorner& corner =
                graph.corners[static_cast<std::size_t>(river.corners[corner_index])];

            draw_line(image, river.points[i].x * scale, river.points[i].y * scale,
                      river.points[i + 1].x * scale, river.points[i + 1].y * scale,
                      half_width_pixels_(river_width(config, corner.river), scale),
                      shader(corner), band);
        }
    }

    /** @brief Strokes every road run, least travelled first, then the bridges over them. */
    static void draw_roads_(Image& image, const MapGraph& graph, const MapConfig& config,
                            const BiomePalette& palette, const RowBand& band = RowBand{}) {
        const double scale = pixels_per_grid_unit_(config);
        static constexpr std::array<RoadClass, 3> k_order = {RoadClass::Trail, RoadClass::Road,
                                                             RoadClass::Highway};
        for (const RoadClass road_class : k_order) {
            const double half_width = half_width_pixels_(road_width_for(config, road_class), scale);
            const glm::vec3& color = palette.color_for(road_class);

            if (graph.roads.empty()) {
                // A map loaded from a document written before roads were traced
                // into runs still has its per-edge flags. Stroke those straight
                // rather than drawing nothing.
                for (const MapEdge& edge : graph.edges) {
                    if (edge.road_class != road_class || edge.d0 == k_invalid_id
                        || edge.d1 == k_invalid_id) {
                        continue;
                    }
                    draw_segment_(image, graph.centers[static_cast<std::size_t>(edge.d0)].point,
                                  graph.centers[static_cast<std::size_t>(edge.d1)].point, scale,
                                  half_width, color, band);
                }
                continue;
            }
            for (const MapRoad& run : graph.roads) {
                if (run.road_class != road_class) {
                    continue;
                }
                for (std::size_t i = 0; i + 1 < run.points.size(); ++i) {
                    draw_segment_(image, run.points[i], run.points[i + 1], scale, half_width,
                                  color, band);
                }
            }
        }

        for (const MapEdge& edge : graph.edges) {
            if (edge.bridge) {
                draw_bridge_(image, graph, edge, config, scale, palette, band);
            }
        }
    }

    /** @brief Fills every building footprint as the rotated quad it is. */
    static void draw_structures_(Image& image, const MapGraph& graph, const MapConfig& config,
                                 const BiomePalette& palette,
                                 const RowBand& band = RowBand{}) {
        const double scale = pixels_per_grid_unit_(config);
        const double street_width = meters_to_grid(config, config.towns.street_width_m);
        std::vector<MapPoint> footprint(4);
        for (const MapTown& town : graph.towns) {
            // Ground first, then what stands on it. The square and the streets are
            // what the buildings were laid out around, and drawing them is the
            // whole difference between a settlement and a scatter of specks --
            // buildings can only be seen to line up if there is a line to see.
            if (town.plaza.radius > 0.0) {
                fill_polygon(image, disc_outline_(town.plaza.centre, town.plaza.radius, scale),
                             palette.plaza_color, band);
            }
            for (const MapStreet& street : town.streets) {
                draw_line(image, street.from.x * scale, street.from.y * scale,
                          street.to.x * scale, street.to.y * scale,
                          half_width_pixels_(street_width, scale), palette.street_color, band);
            }

            // The rotated quad, not an axis-aligned blob: the yaw is part of what
            // the generator guarantees, and a square marker would hide whether
            // buildings actually line up along their street.
            for (const MapBuilding& building : town.buildings) {
                const std::array<MapPoint, 4> corners = building_corners(building);
                for (std::size_t i = 0; i < 4; ++i) {
                    footprint[i] = {corners[i].x * scale, corners[i].y * scale};
                }
                fill_polygon(image, footprint,
                             is_civic_role(building.role) ? palette.civic_color
                                                          : palette.building_color,
                             band);
            }
        }
    }

    /**
     * @brief Draws a marker per settlement, per landmark and per cave mouth.
     *
     * The one place surface markers are drawn, which is why both the composite and
     * the landmarks overlay call it: the two cannot disagree about what is on the
     * ground if they read the same function.
     *
     * A cave mouth belongs here even though the cave itself does not. A passage is
     * underground and has no business on a surface render -- that is why the
     * composite has never drawn one -- but a mouth is a hole in a hillside that
     * anyone standing there would see, and leaving it off made caves invisible on
     * the only layer most readers open.
     */
    static void draw_markers_(Image& image, const MapGraph& graph, const MapConfig& config,
                              const BiomePalette& palette, const RowBand& band = RowBand{}) {
        const double scale = pixels_per_grid_unit_(config);
        for (const MapTown& town : graph.towns) {
            draw_marker_(image, town.point, scale, marker_radius_(town.tier, config),
                         palette.town_color, band);
        }
        // Off by default: a landmark is a label rather than terrain, and is drawn as an
        // interactive marker by whatever is displaying the map. See
        // MapConfig::draw_landmark_marks -- and note it is render-only, where enable_landmarks
        // decides whether the map has any landmarks to draw in the first place.
        if (config.draw_landmark_marks) {
            for (const MapLandmark& landmark : graph.landmarks) {
                draw_landmark_(image, landmark, scale, config, palette, band);
            }
        }
        // A ring, where a town is a filled square and a natural landmark a diamond.
        // The shape has to carry the difference on a composite already crowded with
        // markers, and a ring reads as an opening where a disc reads as a pin. It
        // is also what the overview draws, so the two agree at a glance.
        const int mouth = meters_to_pixels_(k_cave_mouth_marker_m, config);
        for (const MapCave& cave : graph.caves) {
            draw_ring_(image, cave.mouth, scale, mouth,
                       static_cast<int>(mouth * k_cave_mouth_rim), palette.cave_mouth_color,
                       band);
        }
    }

    /**
     * @brief A circle as a pixel-space polygon, for anything round that must fill.
     *
     * Approximated rather than rasterised directly so it goes through
     * `fill_polygon()` and inherits its band clipping -- a banded render splits an
     * image across threads by rows, and a shape that drew its own scanlines would
     * have to re-derive that. Thirty-two sides is under a pixel of chord error at
     * the plaza radii this is used for.
     *
     * @param centre Middle of the circle, in grid units.
     * @param radius Radius in grid units.
     * @param scale Pixels per grid unit.
     * @return The outline, in pixel space, wound consistently.
     */
    static std::vector<MapPoint> disc_outline_(const MapPoint& centre, double radius,
                                               double scale) {
        static constexpr int k_sides = 32;
        static constexpr double k_turn = 6.283185307179586;
        std::vector<MapPoint> outline;
        outline.reserve(k_sides);
        for (int i = 0; i < k_sides; ++i) {
            const double angle = k_turn * static_cast<double>(i) / static_cast<double>(k_sides);
            outline.push_back({(centre.x + std::cos(angle) * radius) * scale,
                               (centre.y + std::sin(angle) * radius) * scale});
        }
        return outline;
    }

    /**
     * @brief One pixel's ground height, in whichever surface style the config asks for.
     *
     * The single place the choice is made, so the elevation layer and the
     * composite's elevation shading cannot drift into drawing different terrain.
     *
     * `Flat` returns the cell's own stored height with the river channel cut out of
     * it. The cut is a function of position, not of the interpolation, so nothing
     * stops it being subtracted from a flat base, and it costs the rivers nothing.
     *
     * `Blended` returns the same height *without* the cut, because it is only the
     * first of three stages -- the raster it produces is blurred and then re-cut by
     * `smooth_elevation_raster_()` once the whole image exists. A blend cannot be
     * decided per pixel per cell: doing that draws a bevelled rim around every
     * cell outline instead of smoothing anything.
     *
     * Terrain detail is left out of both, and that asymmetry is deliberate:
     * roughness is surface *texture*, which a fill constant across a cell has no
     * business carrying, while a river is a feature of the ground.
     *
     * @param graph The map being drawn.
     * @param center The cell the pixel falls in.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param detail The detail field, used by the interpolated style only.
     * @param channels The river channels, cut by every style.
     * @param surface The style to draw, already resolved by `effective_surface()`.
     * @return The height to shade this pixel from.
     */
    static double surface_height_(const MapGraph& graph, const MapCenter& center, double x,
                                  double y, const TerrainDetail& detail,
                                  const RiverChannels& channels, ElevationSurface surface) {
        if (surface == ElevationSurface::Blended) {
            // Flat *and uncut*. The channels are cut back in by
            // `smooth_elevation_raster_()` after the blur, because blurring a raster
            // that already carried them would smear the rivers and then cutting
            // again would subtract each one twice.
            return center.elevation;
        }
        if (surface == ElevationSurface::Flat) {
            const double cut = graph.channel_cut(center, x, y, channels);
            return cut > 0.0 ? std::clamp(center.elevation - cut, 0.0, 1.0) : center.elevation;
        }
        return graph.elevation_at(center, x, y, detail, channels);
    }

    /** @brief Strokes one grid-space segment, converting to pixels on the way. */
    template <typename Color>
    static void draw_segment_(Image& image, const MapPoint& from, const MapPoint& to, double scale,
                              double half_width, const Color& color,
                              const RowBand& band = RowBand{}) {
        draw_line(image, from.x * scale, from.y * scale, to.x * scale, to.y * scale, half_width,
                  color, band);
    }

    /**
     * @brief Marker half-width in pixels for each settlement size class.
     *
     * Sized in metres, so a marker means the same thing at any resolution: a
     * capital reads about 25 m across, which is a settlement-sized dot on a 1 m
     * map. A fixed pixel radius would instead shrink relative to the world every
     * time the render got finer.
     */
    static int marker_radius_(TownTier tier, const MapConfig& config) {
        switch (tier) {
            case TownTier::Capital: return meters_to_pixels_(k_capital_marker_m, config);
            case TownTier::Town:    return meters_to_pixels_(k_town_marker_m, config);
            case TownTier::Village: break;
        }
        return meters_to_pixels_(k_village_marker_m, config);
    }

    /**
     * @brief Draws a landmark, shaped so its kind is readable at a glance.
     *
     * Natural features get a diamond, works a square, a region's wonder a larger
     * diamond -- enough to tell them apart on a preview without a legend.
     */
    static void draw_landmark_(Image& image, const MapLandmark& landmark, double scale,
                               const MapConfig& config, const BiomePalette& palette,
                               const RowBand& band = RowBand{}) {
        const int cx = static_cast<int>(landmark.point.x * scale);
        const int cy = static_cast<int>(landmark.point.y * scale);
        const bool natural = landmark.kind != LandmarkKind::Ruins
                          && landmark.kind != LandmarkKind::StandingStones
                          && landmark.kind != LandmarkKind::Monolith
                          && landmark.kind != LandmarkKind::Wreck
                          && landmark.kind != LandmarkKind::Tower
                          && landmark.kind != LandmarkKind::Shrine;
        const int radius = meters_to_pixels_(
            landmark.kind == LandmarkKind::Wonder ? k_wonder_marker_m : k_landmark_marker_m,
            config);
        const glm::vec3& color =
            natural ? palette.landmark_natural_color : palette.landmark_built_color;

        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                // Diamond for natural, square for built.
                if (natural && std::abs(x) + std::abs(y) > radius) {
                    continue;
                }
                if (cy + y < band.begin || cy + y >= band.end) {
                    continue;
                }
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }

    /**
     * @brief Draws an unfilled ring centred on a grid-space point.
     *
     * The annulus is tested per pixel rather than stroked as a circle, for the
     * same reason `draw_landmark_()` tests its diamond that way: the row guard a
     * banded render needs is then one comparison in the outer loop, and a shape
     * that walked its own circumference would have to re-derive it.
     *
     * @param image The target buffer.
     * @param point Centre, in grid units.
     * @param scale Pixels per grid unit.
     * @param outer Outer radius, in pixels.
     * @param inner Inner radius, in pixels; pixels nearer than this are left alone.
     * @param color Colour to write.
     * @param band Rows this call may touch.
     */
    static void draw_ring_(Image& image, const MapPoint& point, double scale, int outer,
                           int inner, const glm::vec3& color, const RowBand& band = RowBand{}) {
        const int cx = static_cast<int>(point.x * scale);
        const int cy = static_cast<int>(point.y * scale);
        const int outer_squared = outer * outer;
        const int inner_squared = inner * inner;
        for (int y = -outer; y <= outer; ++y) {
            if (cy + y < band.begin || cy + y >= band.end) {
                continue;
            }
            for (int x = -outer; x <= outer; ++x) {
                const int distance_squared = x * x + y * y;
                if (distance_squared > outer_squared || distance_squared < inner_squared) {
                    continue;
                }
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }

    /** @brief Draws a filled square centred on a grid-space point. */
    static void draw_marker_(Image& image, const MapPoint& point, double scale, int radius,
                             const glm::vec3& color, const RowBand& band = RowBand{}) {
        const int cx = static_cast<int>(point.x * scale);
        const int cy = static_cast<int>(point.y * scale);
        for (int y = -radius; y <= radius; ++y) {
            if (cy + y < band.begin || cy + y >= band.end) {
                continue;
            }
            for (int x = -radius; x <= radius; ++x) {
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }

    /**
     * @brief Draws the parapet of a bridge or causeway, across the road.
     *
     * Placed at the midpoint of the Delaunay edge, which is exactly where the
     * road crosses the water: the dual Voronoi edge the river runs along lies on
     * the perpendicular bisector of `d0`-`d1`, so it meets the road there and
     * nowhere else. The tick runs along that bisector -- square across the
     * road -- so a crossing reads as a crossing at a glance, the same way
     * `draw_landmark_()` shapes a marker to its kind.
     */
    static void draw_bridge_(Image& image, const MapGraph& graph, const MapEdge& edge,
                             const MapConfig& config, double scale, const BiomePalette& palette,
                             const RowBand& band = RowBand{}) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
            return;
        }
        const MapPoint& d0 = graph.centers[static_cast<std::size_t>(edge.d0)].point;
        const MapPoint& d1 = graph.centers[static_cast<std::size_t>(edge.d1)].point;

        const double dx = d1.x - d0.x;
        const double dy = d1.y - d0.y;
        const double length = std::hypot(dx, dy);
        if (length == 0.0) {
            return;
        }
        const double width = road_width_for(config, edge.road_class);
        const double reach = width * k_bridge_span;
        const double offset_x = -dy / length * reach;
        const double offset_y = dx / length * reach;
        const MapPoint crossing{(d0.x + d1.x) * 0.5, (d0.y + d1.y) * 0.5};

        draw_segment_(image, {crossing.x + offset_x, crossing.y + offset_y},
                      {crossing.x - offset_x, crossing.y - offset_y}, scale,
                      half_width_pixels_(width * 0.5, scale), palette.bridge_color, band);
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_RENDERER_H
