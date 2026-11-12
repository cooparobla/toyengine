/**
 * @file map_export.h
 * @brief Writes a map's image layers to disk, in parallel and without blocking the caller.
 *
 * A separate header from `map_renderer.h` on purpose. Exporting needs
 * `image_writer.h`, which carries the stb *implementation* rather than just
 * declarations; `map_renderer.h` deliberately pulls in neither, so a consumer who
 * renders a layer into memory and uploads it to a texture never links an encoder
 * it will not use.
 */

#ifndef COOPA_MAPS_MAP_EXPORT_H
#define COOPA_MAPS_MAP_EXPORT_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <coopa/maps/image_writer.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_task.h>

namespace coopa {
namespace maps {

/**
 * @class MapExporter
 * @brief Renders every `MapLayer` of a map and writes each to its own PNG.
 *
 * ### Why this is the part worth threading
 *
 * A default map -- 4800 x 4800, nine layers -- spends about 135 ms generating
 * the world and the rest of its time here: roughly 3.9 s rasterising and 5.5 s
 * inside stb's deflate. Generation is a rounding error by comparison, which is
 * why it is not what this class parallelises.
 *
 * Two levels, both partitioned by *disjoint output*, which is what keeps the
 * result bit-identical to a serial run:
 *
 * - **Across layers.** The layers share nothing but the map they read;
 *   each owns its own `Image` and its own file. This is the level that
 *   parallelises the PNG encode, and nothing else can: stb's deflate is one
 *   opaque call per image.
 * - **Across row bands, within a layer.** Each band draws the same sequence
 *   clipped to its own rows. Splitting by *cell* instead would race, because
 *   adjacent cells deliberately share their boundary pixels.
 *
 * Nesting the two is safe: `JobEngine::wait_for()` called from a worker has that
 * worker participate rather than idle, so a layer job waiting on its own bands
 * cannot deadlock.
 *
 * With no engine injected everything runs inline, in layer order, with no bands.
 * That is the default, and it is what the tests compare against.
 */
class MapExporter {
public:
    /**
     * @brief Sets the engine to spread work across; null runs everything inline.
     * @param engine Not owned. The house pattern -- see `AnimationSystem` and
     *               `ToyRenderPipeline` -- is an injected, nullable engine
     *               pointer, so a subsystem never owns a thread pool a host is
     *               already running.
     */
    void set_job_engine(coopa::job::JobEngine* engine) { engine_ = engine; }

    /**
     * @brief Caps how many layers may be in flight at once, bounding peak memory.
     *
     * Uncapped by default, which means all nine, because capping costs far more
     * than it appears to. Measured on a 20-core machine at the default 4800 x 4800:
     *
     * | layers in flight | export |
     * |---|---|
     * | 1 | 8.3 s |
     * | 3 | 4.2 s |
     * | 7 (uncapped) | 1.6 s |
     *
     * The reason is that PNG encoding is over half the work and stb's deflate is
     * one indivisible call per image -- so the *only* way to overlap encodes is to
     * have several layers in flight. Band parallelism cannot help there, and
     * alone it buys just 1.4x.
     *
     * The cost is memory: roughly `7 * image_size^2 * channels`, about 550 MB at
     * the default render and four times that if `meters_per_pixel` is halved. Cap
     * it at 2 or 3 on a constrained machine and accept the slower export.
     *
     * @param layers At least 1; 0 means "no cap", which is the default.
     */
    void set_max_concurrent_layers(std::size_t layers) { max_concurrent_layers_ = layers; }

    /**
     * @brief Sets the row height of a render band; 0 picks one from the worker count.
     * @param rows Rows per band.
     */
    void set_band_rows(int rows) { band_rows_ = rows; }

    /**
     * @brief Renders and writes every layer, returning when the last file is closed.
     *
     * @param graph The map to draw.
     * @param config Supplies the render size, scale and compression level.
     * @param prefix Output path prefix; each layer becomes `<prefix>_<layer>.png`.
     * @param palette Colours for each biome and overlay.
     * @param logger Optional; receives one line per layer written.
     * @return True if every layer was written.
     */
    bool export_layers(const MapGraph& graph, const MapConfig& config, const std::string& prefix,
                       const BiomePalette& palette = BiomePalette{},
                       coopa::debug::Logger* logger = nullptr) const {
        auto state = std::make_shared<MapTaskState>();
        state->total.store(static_cast<int>(k_map_layer_count));
        std::atomic<bool> ok{true};
        run_(graph, config, prefix, palette, logger, state, ok, nullptr);
        return ok.load();
    }

    /**
     * @brief Starts the export and returns immediately.
     *
     * The returned task is the only thing keeping the work addressable: its
     * destructor cancels and waits, so it must outlive nothing and nothing needs
     * to outlive it. **But `graph`, `config`, `prefix` and `palette` are held by
     * reference for the duration** -- they must stay alive until the task reports
     * `done()`, which in practice means declaring them no later than the task.
     *
     * Cancellation is checked before each layer and between bands, so the
     * worst-case latency is one band of one layer's rasterisation, plus -- if a
     * layer has already reached its encode -- that encode, which stb gives no way
     * to interrupt.
     *
     * @param graph The map to draw; must outlive the task.
     * @param config Render settings; must outlive the task.
     * @param prefix Output path prefix; must outlive the task.
     * @param palette Colours; must outlive the task.
     * @param logger Optional; receives one line per layer written.
     * @return A task to poll, measure and cancel.
     */
    MapTask export_layers_async(const MapGraph& graph, const MapConfig& config,
                                const std::string& prefix,
                                const BiomePalette& palette = BiomePalette{},
                                coopa::debug::Logger* logger = nullptr) const {
        auto state = std::make_shared<MapTaskState>();
        state->total.store(static_cast<int>(k_map_layer_count));

        if (engine_ == nullptr) {
            std::atomic<bool> ok{true};
            run_(graph, config, prefix, palette, logger, state, ok, nullptr);
            return MapTask(state);
        }

        const coopa::job::JobHandle handle = engine_->create_handle();
        // `ok` has to live as long as the job, and the caller has nowhere to put
        // it -- export_layers_async() returns before the work starts. The shared
        // flag rides along with the progress state for the same reason.
        auto ok = std::make_shared<std::atomic<bool>>(true);
        engine_->submit(
            [this, &graph, &config, &prefix, &palette, logger, state, ok](
                const coopa::job::JobContext& ctx) {
                run_(graph, config, prefix, palette, logger, state, *ok, &ctx);
                state->finish();
            },
            k_map_job_type, handle);
        return MapTask(engine_, handle, state);
    }

private:
    /**
     * @brief Renders and writes every layer, serially or across the engine.
     *
     * One body for both paths, so the parallel and serial results cannot drift
     * apart -- the same idiom `AnimationSystem::evaluate_parallel_()` uses.
     */
    void run_(const MapGraph& graph, const MapConfig& config, const std::string& prefix,
              const BiomePalette& palette, coopa::debug::Logger* logger,
              const std::shared_ptr<MapTaskState>& state, std::atomic<bool>& ok,
              const coopa::job::JobContext* ctx) const {
        // Built once, serially, and shared by every layer and every band. Each
        // layer would otherwise rebuild all ~6,900 cell outlines from scratch,
        // and a band needs the row extents to know which cells reach into it.
        // Once, here, before anything fans out. stb keeps the compression level in
        // a global its encoder reads while running, so setting it per write would
        // race with every concurrent write -- which thread sanitizer duly caught.
        set_png_compression_level(config.png_compression_level);

        const CellGeometry geometry = MapLayers::build_cell_geometry(graph, config);

        // Built once here, for the same reason as the geometry: the hillshaded
        // composite reads it across the whole image, so a band cannot build its
        // own -- and left to, every band would rebuild the entire raster. Only
        // that one mode needs it, so only that mode pays for it.
        //
        // `ElevationSurface::Blended` needs one for a different reason -- the
        // composite's elevation shading has to read the smoothed raster rather than
        // sample the flat surface the blend is applied *after* -- and asks for no
        // extra blur on top of it.
        HeightField height;
        const HeightField* height_ptr = nullptr;
        const bool hillshade = config.composite_shading == CompositeShading::Hillshade;
        if (hillshade) {
            height = MapLayers::build_height_field(graph, config, palette, &geometry);
            height_ptr = &height;
        } else if (MapLayers::effective_surface(config) == ElevationSurface::Blended) {
            height = MapLayers::build_height_field(graph, config, palette, &geometry, 0.0);
            height_ptr = &height;
        }

        if (engine_ == nullptr) {
            for (std::size_t i = 0; i < k_map_layer_count; ++i) {
                if (ctx != nullptr && ctx->is_cancelled()) {
                    return;
                }
                one_layer_(graph, config, prefix, palette, logger, geometry, height_ptr, i, ok,
                           ctx);
                state->step();
            }
            return;
        }

        const std::size_t cap = max_concurrent_layers_ == 0
                                    ? k_map_layer_count
                                    : std::min(max_concurrent_layers_, k_map_layer_count);
        // Grain is how concurrency is capped: fewer chunks, fewer layers in
        // flight, less memory held at once.
        const std::size_t grain = (k_map_layer_count + cap - 1) / cap;
        engine_->parallel_for_blocking(
            k_map_layer_count, grain,
            [&](std::size_t begin, std::size_t end, const coopa::job::JobContext& inner) {
                for (std::size_t i = begin; i < end; ++i) {
                    if (inner.is_cancelled()) {
                        return;
                    }
                    one_layer_(graph, config, prefix, palette, logger, geometry, height_ptr, i, ok,
                               &inner);
                    state->step();
                }
            },
            k_map_job_type);
    }

    /** @brief Renders one layer -- banded if there is an engine -- then writes it. */
    void one_layer_(const MapGraph& graph, const MapConfig& config, const std::string& prefix,
                    const BiomePalette& palette, coopa::debug::Logger* logger,
                    const CellGeometry& geometry, const HeightField* height, std::size_t index,
                    std::atomic<bool>& ok, const coopa::job::JobContext* ctx) const {
        const MapLayer layer = static_cast<MapLayer>(index);
        Image image = render_layer_(graph, config, palette, geometry, height, layer, ctx);
        if (ctx != nullptr && ctx->is_cancelled()) {
            return;
        }

        const std::string path =
            prefix + "_" + std::string(map_layer_name(layer)) + ".png";
        if (!write_png(path, image)) {
            ok.store(false);
            if (logger != nullptr) {
                logger->warn("map export: failed to write " + path);
            }
            return;
        }
        if (logger != nullptr) {
            // Pass granularity, not per-band: the logger serialises on one global
            // mutex, so chatter inside a parallel_for would throttle the workers.
            logger->info("map export: wrote " + path);
        }
    }

    /**
     * @brief Renders one layer, splitting it across row bands when an engine is set.
     *
     * The bands write into one `Image`, which is safe because each owns a
     * disjoint run of rows -- and reproducible, because each replays the same
     * draw sequence clipped to those rows.
     *
     * One route, whatever the thread count: allocate, draw, finish. The engineless
     * case deliberately does not hand off to `MapLayers::render()`: a layer with a
     * whole-image pass would then run it on only one of the two paths, and an
     * unthreaded render and a threaded one would produce different pixels.
     * `finish()` is called exactly once here, after every band, which is the only
     * place it can be correct.
     */
    Image render_layer_(const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette, const CellGeometry& geometry,
                        const HeightField* height, MapLayer layer,
                        const coopa::job::JobContext* ctx) const {
        const RenderSlice whole{RowBand{}, &geometry, height};

        // Cleared once, here, and then only drawn into. This is why `allocate()`
        // and `render_into()` are separate entry points: a band that cleared the
        // buffer itself would wipe whatever its neighbours had already drawn.
        Image image = MapLayers::allocate(layer, config, palette);
        const int rows = config.image_size;
        const std::size_t bands = engine_ == nullptr ? 1 : band_count_(rows);
        if (bands <= 1) {
            MapLayers::render_into(image, layer, graph, config, palette, whole);
            MapLayers::finish(image, layer, graph, config, palette, whole);
            return image;
        }

        (void)ctx;
        engine_->parallel_for_blocking(
            bands, 1,
            [&](std::size_t begin, std::size_t end, const coopa::job::JobContext& inner) {
                for (std::size_t b = begin; b < end; ++b) {
                    if (inner.is_cancelled()) {
                        return;
                    }
                    // Split by exact division rather than by band_rows, so the
                    // bands tile the image with no gap and no overlap whatever
                    // the row count divides into.
                    const int from =
                        static_cast<int>(static_cast<std::size_t>(rows) * b / bands);
                    const int to =
                        static_cast<int>(static_cast<std::size_t>(rows) * (b + 1) / bands);
                    MapLayers::render_into(image, layer, graph, config, palette,
                                           RenderSlice{RowBand{from, to}, &geometry, height});
                }
            },
            k_map_job_type);
        MapLayers::finish(image, layer, graph, config, palette, whole);
        return image;
    }

    /**
     * @brief How many row bands to split a layer into.
     * @param rows Image height in pixels.
     * @return At least one band; one means draw it in a single call.
     */
    std::size_t band_count_(int rows) const {
        const int band_rows =
            band_rows_ > 0
                ? band_rows_
                : std::max(1, rows / static_cast<int>(
                                         std::max(1u, engine_->worker_count()) * 4));
        return static_cast<std::size_t>((rows + band_rows - 1) / std::max(1, band_rows));
    }

    /** @brief The engine work is spread across, or null for inline. */
    coopa::job::JobEngine* engine_ = nullptr;
    /** @brief Layers allowed in flight at once; 0 = all of them. See the setter. */
    std::size_t max_concurrent_layers_ = 0;
    /** @brief Rows per render band; 0 derives one from the worker count. */
    int band_rows_ = 0;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_EXPORT_H
