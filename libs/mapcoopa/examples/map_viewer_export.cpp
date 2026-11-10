/**
 * @file map_viewer_export.cpp
 * @brief The only translation unit in the viewer that touches mapcoopa's stb_image_write.
 *
 * See map_viewer_export.h for why this separation exists. Nothing from gfxcoopa, uicoopa or
 * Vulkan may be included here, and nothing from `coopa/maps/image_writer.h` or
 * `coopa/maps/map_export.h` may be included by map_viewer.cpp -- that is the whole contract.
 */

#include "map_viewer_export.h"

#include <coopa/maps/image_writer.h>
#include <coopa/maps/map_export.h>

namespace viewer {

void set_png_level(int level) {
    coopa::maps::set_png_compression_level(level);
}

bool write_layer_png(coopa::maps::MapLayer layer,
                     const coopa::maps::MapGraph& graph,
                     const coopa::maps::MapConfig& config,
                     const coopa::maps::BiomePalette& palette,
                     const std::string& path) {
    const coopa::maps::Image img = coopa::maps::MapLayers::render(layer, graph, config, palette);
    return coopa::maps::write_png(path, img);
}

coopa::maps::MapTask export_all_layers(coopa::job::JobEngine& jobs,
                                       const coopa::maps::MapGraph& graph,
                                       const coopa::maps::MapConfig& config,
                                       const std::string& prefix,
                                       const coopa::maps::BiomePalette& palette,
                                       coopa::debug::Logger* logger) {
    // Static so it outlives the returned task -- MapExporter holds no state that a second
    // export would corrupt, but the task does reference it for its lifetime.
    static coopa::maps::MapExporter exporter;
    exporter.set_job_engine(&jobs);
    return exporter.export_layers_async(graph, config, prefix, palette, logger);
}

}  // namespace viewer
