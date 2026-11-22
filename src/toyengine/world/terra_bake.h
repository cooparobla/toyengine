/**
 * @file terra_bake.h
 * @brief Baking a `.terra` file offline: a map and a terrain's shape in, a stored world out.
 *
 * The same work `MapGeneratorComponent` does the first time a terrain with no stored tiles loads,
 * lifted out of the scene so that a command-line tool can do it. **Nothing here touches the
 * GPU**: a bake is a `MapGenerator`, a `TerrainSampler` and a `TerraData`, so it runs with no
 * window, no device, no shaders and no `Engine` -- which is what makes it usable from a build
 * step rather than only from a running game.
 *
 * Why bake at all: a terrain whose `.terra` is missing generates its world at load, and for a
 * large one that is seconds of a player's time, every run, for a result that never varies. Baking
 * moves it to a tool, and the stored file then loads in a fraction of the time.
 *
 * A world whose grid is not one flat square bakes through [`world_bake.h`](../mapgen/world_bake.h)'s
 * hook exactly as it does at runtime: install the hook first and the same code shapes the file.
 * A project with no hook gets the flat walk, which is the only shape this engine has.
 */

#ifndef TOYENGINE_WORLD_TERRA_BAKE_H
#define TOYENGINE_WORLD_TERRA_BAKE_H

#include <cstddef>
#include <string>
#include <vector>

#include <coopa/maps/map_config.h>

#include <toyengine/world/terrain_params.h>

namespace coopa {
namespace debug {
class Logger;
}
}  // namespace coopa

namespace toy {
namespace world {

/**
 * @struct TerraBakeRequest
 * @brief One world to bake, and where to put it.
 */
struct TerraBakeRequest {
    /**
     * @brief The map to generate.
     *
     * Passed through `coopa::maps::derived_config()` before use, exactly as
     * `MapGeneratorComponent` does, so the per-pass noise seeds and frequencies come out the same
     * as they would at runtime. Hand it the authored config, not an already-derived one.
     */
    coopa::maps::MapConfig config;

    /**
     * @brief The terrain's shape: `tile_size`, `chunk_size`, `height_step`, `height_scale` and
     *        anything a project's own hook reads off it (`face_tiles`, `surface_shader`).
     *
     * This is what the scene's terrain would carry. It decides the geometry the file is written
     * at, so it has to match the terrain that will later read it -- see `TerraBakeResult::tiles_x`.
     */
    TerrainParams params;

    /**
     * @brief Surface layer names, in the terrain's own order, for the splat weights.
     *
     * Empty uses the built-in tile layers, which is what a terrain that authors no `layers:` has.
     * Only read on a field with a height plane (`height_sub > 1`); a tile world stores its surface
     * in the tiles themselves and never looks at these.
     */
    std::vector<std::string> layers;

    /** @brief Where the `.terra` goes. Parent directories are created. */
    std::string out_path;
};

/**
 * @struct TerraBakeResult
 * @brief What a bake produced, or why it did not.
 */
struct TerraBakeResult {
    bool ok = false;            ///< False leaves no file written.
    std::string message;        ///< What happened, ready to print; the failure reason when `!ok`.
    int tiles_x = 0;            ///< The baked grid's width. mapcoopa (or the hook) decides it.
    int tiles_y = 0;            ///< And its height.
    std::size_t bytes = 0;      ///< Size of the file written.
};

/**
 * @brief Generates a world and writes it to a `.terra` file.
 *
 * Synchronous and single-threaded: a 6-million-tile globe is seconds of work, which is the point
 * of doing it here rather than at load. Overwrites `out_path` if it exists.
 *
 * @param request What to bake and where.
 * @param log Where the map generator's own pass log goes.
 * @return The result; check `ok` and print `message`.
 */
TerraBakeResult bake_terra(const TerraBakeRequest& request, coopa::debug::Logger& log);

}   // namespace world
}   // namespace toy

#endif   // TOYENGINE_WORLD_TERRA_BAKE_H
