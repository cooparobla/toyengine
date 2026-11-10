/**
 * @file banded_render_test.cpp
 * @brief Rendering a layer as row bands (render_into per band, then finish once) is
 *        byte-identical to rendering it whole, for every layer, shading mode and surface style.
 *
 * Its own suite because it is the most expensive check in the repo -- down to one-row bands,
 * every layer, four render configurations -- and ctest runs suites in parallel. The
 * blend-variation pyramid's band independence is elevation_layer_test.
 */

#include <coopa/testing/test.h>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("banded_render");

/**
 * @brief Splitting a layer into row bands changes nothing about the result.
 *
 * The test that would catch an off-by-one at a band seam, which is the bug this
 * design most invites. Run down to one-row bands, where every seam there could
 * be is exercised at once.
 *
 * Swept over `ElevationSurface::Blended` as well, because that mode is the one
 * with a whole-image pass behind it: the blur reads well outside whatever band is
 * being drawn, so it lives in `MapLayers::finish()` rather than in `render_into()`,
 * and the banded arm here calls it exactly where a real caller has to -- once,
 * after the last band. Run per band instead it would blur each band from its own
 * rows; run twice it would blur twice; and either way this comparison fails.
 */
COOPA_TEST(band_rendering_matches_whole_image) {
    struct Case { ElevationSurface surface; CompositeShading shading; };
    const Case cases[] = {
        {ElevationSurface::Interpolated, CompositeShading::Elevation},
        {ElevationSurface::Interpolated, CompositeShading::Hillshade},
        {ElevationSurface::Blended, CompositeShading::Elevation},
        {ElevationSurface::Blended, CompositeShading::Hillshade},
    };
    for (const Case& c : cases) {
        MapConfig config = small_config(21);
        set_render_size(config, 96);
        config.elevation_surface = c.surface;
        config.elevation_blend = 0.5;
        config.composite_shading = c.shading;
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        const CellGeometry geometry = MapLayers::build_cell_geometry(graph, config);
        HeightField height;
        const HeightField* height_ptr = nullptr;
        if (c.shading == CompositeShading::Hillshade) {
            height = MapLayers::build_height_field(graph, config, BiomePalette{}, &geometry);
            height_ptr = &height;
        } else if (c.surface == ElevationSurface::Blended) {
            height = MapLayers::build_height_field(graph, config, BiomePalette{}, &geometry, 0.0);
            height_ptr = &height;
        }

        for (std::size_t i = 0; i < k_map_layer_count; ++i) {
            const MapLayer layer = static_cast<MapLayer>(i);
            const Image whole = MapLayers::render(layer, graph, config, BiomePalette{},
                                                  RenderSlice{RowBand{}, &geometry, height_ptr});
            for (const int bands : {2, 5, 96}) {
                Image banded = MapLayers::allocate(layer, config, BiomePalette{});
                for (int b = 0; b < bands; ++b) {
                    const int from = config.image_size * b / bands;
                    const int to = config.image_size * (b + 1) / bands;
                    MapLayers::render_into(banded, layer, graph, config, BiomePalette{},
                                           RenderSlice{RowBand{from, to}, &geometry, height_ptr});
                }
                MapLayers::finish(banded, layer, graph, config, BiomePalette{},
                                  RenderSlice{RowBand{}, &geometry, height_ptr});
                ASSERT_EQ(whole.pixels.size(), banded.pixels.size());
                ASSERT_TRUE(whole.pixels == banded.pixels);
            }
        }
    }
}
