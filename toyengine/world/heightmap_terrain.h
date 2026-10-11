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

#include <toyengine/world/heightmap_data.h>
#include <toyengine/world/heightmap_store.h>

namespace coopa {
namespace asset {
class AssetManager;
}
} // namespace coopa

namespace toy {
namespace world {

struct HeightmapTerrainRuntime;

/**
 * @class HeightmapTerrain
 * @brief See the file comment. Configuration plus a handle on the shared data; the per-frame
 *        work (chunks, GPU uploads, collider) is HeightmapTerrainSystem's.
 */
class HeightmapTerrain : public coopa::scene::Component {
public:
    HeightmapTerrain();
    ~HeightmapTerrain() override;

    std::string type_name() const override { return "HeightmapTerrain"; }

    // --- Authored ---
    HeightmapGeometry geometry;
    HeightmapGenerate generate;
    std::string heightmap;   ///< As written in the scene (relative to it).
    std::string splat0;
    std::string splat1;
    std::vector<HeightmapLayer> layers;

    struct Tessellation {
        bool  enabled = true;
        float edge_pixels = 8.0f;
        float max_factor = 32.0f;
        float max_distance = 300.0f;
    } tessellation;

    int  patch_resolution = 16;     ///< Quads per chunk side before tessellation.
    int  layer_texture_size = 1024; ///< Texels per side every layer texture is resized to.
    bool collision = true;
    std::string physics_material;   ///< physics_materials/<name>.yaml for the collider (optional).
    bool deep_snow = false;
    bool puddles = true;
    bool cast_shadows = true;

    // --- Resolved by the parser ---
    HeightmapFiles files;                     ///< Where the data is read from / saved to.
    std::vector<std::string> layer_albedo_paths; ///< Resolved per layer (empty = none).
    std::vector<std::string> layer_normal_paths;

    /** @brief The live data (shared, see heightmap_store.h). Never null after parsing. */
    std::shared_ptr<HeightmapData> data;

    /** @brief World position of the grid's (0, 0) corner at height 0 (the owner's position). */
    glm::vec3 origin() const;
    /** @brief World-space query handle (heights, normals, layer weights). */
    HeightmapView view() const { return HeightmapView{data.get(), origin()}; }

    /** @brief Runtime state the system attaches; opaque here. */
    std::unique_ptr<HeightmapTerrainRuntime> runtime;
};

/**
 * @brief Registers the "HeightmapTerrain" scene-file parser. Called from
 *        toy::scene::register_scene_components().
 */
void register_heightmap_terrain_component(coopa::asset::AssetManager& assets);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_H
