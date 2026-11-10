/**
 * @file map_viewer_export.h
 * @brief The viewer's PNG-writing seam, kept in its own translation unit.
 *
 * ## Why this file exists
 *
 * `coopa/maps/image_writer.h` compiles stb_image_write's implementation block, with
 * `STB_IMAGE_WRITE_STATIC`, into every TU that includes it -- directly, or via
 * `coopa/maps/map_export.h`. gfxcoopa ships its own copy of the same upstream header whose
 * body lives in `gfxcoopa_impl` and whose symbols are therefore `extern`. The two copies
 * are different files with different include guards, so a TU that pulls in both compiles
 * stb's body twice and collides on every symbol in it -- and no include ordering fixes
 * that, because reordering only swaps which linkage conflicts with which.
 *
 * The viewer needs both halves: mapcoopa to write map PNGs, uicoopa/gfxcoopa to draw the
 * window. So the two never meet. This header declares the handful of things the GUI needs,
 * `map_viewer_export.cpp` includes only mapcoopa and provides them, and `map_viewer.cpp`
 * includes no mapcoopa header that reaches stb. It is the same single-TU isolation
 * `uicoopa/src/ui_impl.cpp` and `gfxcoopa_impl` already use for their own stb bodies.
 *
 * Nothing in these declarations mentions stb, `MapExporter` or `Image`, so this header is
 * safe to include from the GUI side.
 */

#ifndef MAPCOOPA_EXAMPLES_MAP_VIEWER_EXPORT_H
#define MAPCOOPA_EXAMPLES_MAP_VIEWER_EXPORT_H

#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_task.h>
#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <string>

namespace viewer {

/**
 * @brief Sets stb's deflate level for every later write.
 *
 * Call once before any threaded export: stb keeps the level in a mutable global, so
 * changing it while writes are in flight is not safe.
 *
 * @param level 1-9; out-of-range values are ignored by the library.
 */
void set_png_level(int level);

/**
 * @brief Renders one layer at the config's full image_size and writes it to a PNG.
 *
 * Synchronous and slow by design -- seconds, at the default 3000px. Callers run it on a
 * job engine; see map_viewer.cpp's "Export current layer" action.
 *
 * @param layer   Which view to draw.
 * @param graph   The map to draw.
 * @param config  Supplies the image size, world scale and feature widths.
 * @param palette Colours for each biome and overlay.
 * @param path    Where to write the PNG.
 * @return True if the file was written.
 */
bool write_layer_png(coopa::maps::MapLayer layer,
                     const coopa::maps::MapGraph& graph,
                     const coopa::maps::MapConfig& config,
                     const coopa::maps::BiomePalette& palette,
                     const std::string& path);

/**
 * @brief Starts a MapExporter run over every layer and returns immediately.
 *
 * Writes `<prefix>_<layer>.png` for all nine enum layers plus two files per cave level.
 *
 * @warning MapExporter holds `graph`, `config`, `prefix` and `palette` **by reference** for
 *          the returned task's whole lifetime. Every one of them must outlive the task.
 *
 * @param jobs    The engine to spread the work across.
 * @param graph   The map to draw.
 * @param config  Supplies the image size and feature widths.
 * @param prefix  Output path prefix.
 * @param palette Colours for each biome and overlay.
 * @param logger  Progress sink; may be null.
 * @return A task to poll for progress and completion.
 */
coopa::maps::MapTask export_all_layers(coopa::job::JobEngine& jobs,
                                       const coopa::maps::MapGraph& graph,
                                       const coopa::maps::MapConfig& config,
                                       const std::string& prefix,
                                       const coopa::maps::BiomePalette& palette,
                                       coopa::debug::Logger* logger);

}  // namespace viewer

#endif  // MAPCOOPA_EXAMPLES_MAP_VIEWER_EXPORT_H
