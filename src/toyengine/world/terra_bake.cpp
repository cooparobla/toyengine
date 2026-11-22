/**
 * @file terra_bake.cpp
 * @brief See terra_bake.h.
 */

#include <toyengine/world/terra_bake.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>

#include <toyengine/mapgen/terrain_sampler.h>
#include <toyengine/world/terra_codec.h>
#include <toyengine/world/terra_data.h>
#include <toyengine/world/terra_geometry.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {
namespace {

/**
 * @brief The geometry a tiles terrain asks `TerraStore` for.
 *
 * `TerrainComponent::sync_field()`'s tiles branch, with the same `tiles_x = 0`: the tile COUNT is
 * mapcoopa's to decide at bake time (or the world-bake hook's), and saying 0 is how a caller
 * says it has no opinion yet.
 */
TerraGeometry geometry_for(const TerrainParams& params) {
    TerraGeometry g;
    g.tiles_x = 0;
    g.tiles_y = 0;
    g.tile_size = params.tile_size;
    g.height_sub = 1;
    g.layer_sub = 1;
    g.height = params.height_scale;
    g.height_step = params.height_step;
    g.chunk_tiles = params.chunk_size;
    return g.sanitized();
}

/**
 * @brief Fills the splat weights from the baked tiles, one dominant surface per texel.
 *
 * `TerrainSampler::bake()` fills the tile plane and (on a field with a height plane) the heights
 * and water, but not the layer weights: those need the terrain's own `layers:` name mapping,
 * which the sampler has no idea exists. The same pass `MapGeneratorComponent::apply_to()` runs.
 *
 * A field with no height plane has no weights to blend, so this does nothing for a tile world.
 */
void fill_layers(const TerrainSampler& sampler, const std::vector<std::string>& layers,
                 TerraData& out) {
    if (!out.has_heights()) return;
    const int lnx = out.layer_nx();
    const int lny = out.layer_ny();
    const double n = static_cast<double>(out.geometry().tiles_x);
    if (lnx <= 0 || lny <= 0 || n <= 0.0) return;

    // Resolve the map's surface kinds onto the terrain's layer list BY NAME -- a terrain's list
    // is its own, and only the built-in set happens to be in TileKind order.
    std::array<int, k_tile_kind_count> layer_for_kind{};
    for (std::size_t k = 0; k < k_tile_kind_count; ++k) {
        layer_for_kind[k] = 0;
        const char* name = tile_kind_names()[k];
        for (std::size_t i = 0; i < layers.size() && i < k_heightmap_layer_count; ++i) {
            if (layers[i] == name) { layer_for_kind[k] = static_cast<int>(i); break; }
        }
    }

    for (int j = 0; j < lny; ++j) {
        for (int i = 0; i < lnx; ++i) {
            const double u = (static_cast<double>(i) + 0.5) / lnx;
            const double v = (static_cast<double>(j) + 0.5) / lny;
            const auto s = sampler.sample_surface(u * n, v * n);
            const std::size_t kind = static_cast<std::size_t>(s.top);
            const int layer = kind < layer_for_kind.size() ? layer_for_kind[kind] : 0;
            for (int l = 0; l < static_cast<int>(k_heightmap_layer_count); ++l) out.layer_at(l, i, j) = 0;
            out.layer_at(layer, i, j) = 255;
        }
    }
    out.mark_layer_dirty(IRect{0, 0, lnx, lny});
}

}   // namespace

TerraBakeResult bake_terra(const TerraBakeRequest& request, coopa::debug::Logger& log) {
    TerraBakeResult result;
    if (request.out_path.empty()) {
        result.message = "no output path";
        return result;
    }

    // Derived first, as the component does: the per-pass noise seeds and the frequency scaling
    // against the grid size live there, so an authored config baked without it is a different
    // world from the one the same config would generate at runtime.
    const coopa::maps::MapConfig config = coopa::maps::derived_config(request.config);
    coopa::maps::MapGenerator generator(config, log);
    generator.generate();

    TerrainSampler sampler;
    sampler.build(config, coopa::maps::MapGraph(generator.graph()), request.params);
    if (!sampler.is_built()) {
        result.message = "the sampler did not build -- check the map config";
        return result;
    }

    TerraData out(geometry_for(request.params));
    // Reshapes `out` to whatever grid the world really has: one flat square here, or whatever a
    // project's world-bake hook decides (the six faces of a closed solid, say).
    sampler.bake(out);
    fill_layers(sampler, request.layers, out);

    result.tiles_x = out.geometry().tiles_x;
    result.tiles_y = out.geometry().tiles_y;
    if (result.tiles_x <= 0 || result.tiles_y <= 0) {
        result.message = "the bake produced no tiles";
        return result;
    }

    if (!save_terra(request.out_path, out)) {
        result.message = "could not write " + request.out_path;
        return result;
    }

    std::error_code ec;
    result.bytes = static_cast<std::size_t>(std::filesystem::file_size(request.out_path, ec));
    if (ec) result.bytes = 0;
    result.ok = true;
    result.message = std::to_string(result.tiles_x) + "x" + std::to_string(result.tiles_y) +
                     " tiles, " + std::to_string(result.bytes / 1024) + " KB -> " + request.out_path;
    return result;
}

}   // namespace world
}   // namespace toy
