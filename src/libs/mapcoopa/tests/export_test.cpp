/**
 * @file export_test.cpp
 * @brief MapExporter: writing every layer across threads, and under every concurrency cap and
 *        band height, produces byte-identical PNGs to a serial export.
 *
 * Exact comparisons, because the seams between concurrent layers are where this design invites
 * bugs. Row-band rendering itself (render_into + finish) is banded_render_test; what each layer
 * *contains* is map_layers_test and the feature suites.
 */

#include <coopa/testing/test.h>

#include <string>

#include <coopa/maps/map_export.h>
#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("export");

/**
 * @brief Every layer renders identically whether split across threads or not.
 *
 * Checked through `MapExporter`'s own rendering path rather than by calling
 * `render()` twice, so what is compared is what actually gets written.
 */
COOPA_TEST(parallel_export_matches_serial) {
    for (const CompositeShading shading : {CompositeShading::Elevation,
                                           CompositeShading::Hillshade}) {
        MapConfig config = small_config(5);
        config.composite_shading = shading;
        MapGenerator generator(config, maps_logger());
        generator.generate();

        const auto dir = coopa::test::scratch_dir(composite_shading_name(shading));
        const std::string serial_prefix = (dir / "serial").string();
        const std::string parallel_prefix = (dir / "parallel").string();

        MapExporter serial;
        ASSERT_TRUE(serial.export_layers(generator.graph(), config, serial_prefix));

        MapExporter threaded;
        threaded.set_job_engine(&maps_engine());
        ASSERT_TRUE(threaded.export_layers(generator.graph(), config, parallel_prefix));

        for (std::size_t i = 0; i < k_map_layer_count; ++i) {
            const std::string name(map_layer_name(static_cast<MapLayer>(i)));
            const std::string a = read_file(serial_prefix + "_" + name + ".png");
            const std::string b = read_file(parallel_prefix + "_" + name + ".png");
            ASSERT_TRUE(!a.empty());
            ASSERT_TRUE(a == b);
        }
    }
}

/**
 * @brief The concurrency and band knobs are performance dials, not output ones.
 *
 * Every combination of layer concurrency cap (including 0, "unlimited") and band height
 * (including one-row bands) writes the same composite PNG, byte for byte, as a plain serial
 * export -- and none of them crashes or deadlocks getting there.
 */
COOPA_TEST(export_tuning_does_not_change_output) {
    MapConfig config = small_config(33);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const std::string reference_prefix = (coopa::test::scratch_dir("serial") / "map").string();
    MapExporter serial;
    ASSERT_TRUE(serial.export_layers(generator.graph(), config, reference_prefix));
    const std::string reference = read_file(reference_prefix + "_composite.png");
    ASSERT_TRUE(!reference.empty());

    for (const std::size_t cap : {std::size_t{1}, std::size_t{3}, std::size_t{0}}) {
        for (const int band_rows : {1, 7, config.image_size}) {
            MapExporter exporter;
            exporter.set_job_engine(&maps_engine());
            exporter.set_max_concurrent_layers(cap);
            exporter.set_band_rows(band_rows);

            const std::string prefix =
                (coopa::test::scratch_dir("cap" + std::to_string(cap) + "_rows"
                                          + std::to_string(band_rows)) / "map").string();
            ASSERT_TRUE(exporter.export_layers(generator.graph(), config, prefix));
            ASSERT_TRUE(read_file(prefix + "_composite.png") == reference);
        }
    }
}
