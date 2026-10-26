/**
 * @file heightmap_terrain.h
 * @brief The scene-authored `HeightmapTerrain` component: a sculptable, paintable height-grid
 *        terrain for open worlds, drawn as GPU-displaced, tessellated chunks.
 *
 * The second kind of terrain toyengine has (the first, `TileTerrain`, generates a stylised tile
 * world from mapcoopa). This one is a plain heightmap: you give it a size in metres, a height,
 * a chunk size and a resolution; its shape lives in a 16-bit PNG (load one, or sculpt it in the
 * editor's Terrain mode) and its look in eight paint layers blended by two splat maps.
 *
 * ### How it is drawn
 *
 * Nothing is meshed on the CPU. Every chunk is one instance of the same small flat patch; the
 * `heightmap_terrain` surface shader places each vertex on the height grid (bound as the chunks'
 * displacement map) and tessellation adds detail near the camera, so an edit is a texture upload,
 * not a re-mesh, and every chunk batches into one instanced draw. See
 * assets/shaders/heightmap_terrain_surface.glsl.
 *
 * ### Where its data lives
 *
 * In a HeightmapData shared through the HeightmapStore (heightmap_store.h), keyed by the
 * heightmap file -- so the editor re-parsing this component keeps unsaved strokes -- not in the
 * scene file. Collision reads the same grid through a heightfield collider; GroundProbe, the
 * foliage scatter and gameplay read it through view().
 *
 * Example YAML:
 * @code
 * - type: HeightmapTerrain
 *   size: { x: 1024, y: 1024 }    # metres (x = width, y = depth)
 *   height: 160                   # metres at a full-white heightmap
 *   chunk_size: 64                # metres per chunk (render / collision / scatter unit)
 *   resolution: 1025              # height samples per side
 *   splat_resolution: 1024
 *   heightmap: terrain/height.png # 16-bit greyscale; generated (see `generate`) when missing
 *   splat0: terrain/splat0.png    # layers 0-3 in RGBA
 *   splat1: terrain/splat1.png    # layers 4-7
 *   generate: { seed: 7, feature_size: 220, ridges: 0.4 }
 *   layers:
 *     - { name: grass, albedo: textures/terrain/grass.png, normal: textures/terrain/grass_n.png, tiling: 6 }
 *     - { name: rock, albedo: textures/terrain/rock.png, tiling: 12, triplanar: true }
 *   tessellation: { edge_pixels: 8, max_factor: 32, max_distance: 300 }
 *   deep_snow: true               # deep snow (and trenches) on flat ground when it snows
 *   puddles: true                 # rain gathers in hollows
 *   collision: true
 * @endcode
 *
 * The terrain sits at its object's world position (the grid's (0, 0) corner at height 0) and is
 * axis-aligned: object rotation and scale are ignored.
 */

#ifndef TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_H
#define TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_H

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/scene/component.h>

#include <fkYAML/node.hpp>

#include <coopa/scene/scene_loader.h>

#include <toyengine/world/heightmap_data.h>
#include <toyengine/world/terrain_component.h>

namespace coopa {
namespace asset {
class AssetManager;
}
} // namespace coopa

namespace toy {
namespace world {

/**
 * @brief The `HeightmapTerrain` component.
 *
 * One C++ type serves both terrain modes (toyengine/world/terrain_component.h); this is the name
 * the heightmap half is authored and inspected under. A component whose `mode` is
 * `TerrainMode::Heightmap` reads `geometry`, `layers` and `data`, and reports
 * `type_name() == "HeightmapTerrain"`.
 */
using HeightmapTerrain = TerrainComponent;

/**
 * @brief Authors the heightmap half of a terrain from a scene block.
 *
 * The entry point is in toy::scene::register_scene_components(), which registers one parser under
 * all three names and dispatches on infer_terrain_mode() (terrain_yaml.h); this is the body it
 * calls for `TerrainMode::Heightmap`.
 */
void parse_heightmap_terrain(const fkyaml::node& node, coopa::scene::SceneObject& obj,
                             const coopa::scene::SceneLoader::ParseContext& ctx,
                             coopa::asset::AssetManager& assets);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_H
