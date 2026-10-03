/**
 * @file water_body.h
 * @brief WaterBody component: marks an object as a body of water -- a planar lake/ocean or a
 *        flowing river -- and owns its CPU surface query.
 *
 * The component is configuration plus baked state; every decision (baking the mesh, publishing
 * it to the sibling MeshRenderer, driving buoyancy) lives in toy::water::WaterSystem.
 *
 * Geometry comes from one of three places (first match wins):
 *  - set_geometry(), for water built by code;
 *  - `mesh_path`: a mesh YAML (any shape -- a lake outline, a river strip). Loaded CPU-side as a
 *    SkinnedMeshSource, since the bake needs the vertices and a GPU Mesh keeps none. The sibling
 *    MeshRenderer must NOT also name it as its own mesh_path (AssetManager caches one asset type
 *    per resolved file); WaterSystem publishes the baked GPU mesh to it instead.
 *  - otherwise a procedural flat grid, `size` x `resolution`, centred on the object -- enough for
 *    any planar body.
 *
 * Coordinates are the engine's: Z up, the surface lies in the object's local XY plane.
 */

#ifndef TOYENGINE_WATER_WATER_BODY_H
#define TOYENGINE_WATER_WATER_BODY_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/asset/asset_handle.h>
#include <coopa/scene/component.h>

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/water/water_surface_query.h>
#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

/** @brief Static (planar) water or flowing water whose current is baked from the mesh slope. */
enum class WaterMode { Planar, Flowing };

/**
 * @class WaterBody
 * @brief A body of water. See the file doc.
 */
class WaterBody : public coopa::scene::Component {
public:
    std::string type_name() const override { return "WaterBody"; }

    WaterMode mode = WaterMode::Planar;

    // --- Geometry ---
    std::string mesh_path;                ///< Logical mesh name (scene meshes/ dir); empty = grid.
    glm::vec2   size{20.0f, 20.0f};       ///< Procedural grid extent (local units).
    int         resolution = 48;          ///< Procedural grid quads per side.

    // --- Waves (see water_waves.h) ---
    WaveParams waves;

    // --- Flow (Flowing mode; see water_flow_bake.h) ---
    float flow_speed      = 1.0f;
    float flow_min_speed  = 0.6f;
    float flow_slope_gain = 4.0f;
    float obstacle_radius = 1.5f;
    float wake_length     = 4.0f;

    // --- Look (packed into PBRMaterial::shader_params_ext; see water_surface.glsl) ---
    glm::vec3 foam_color{0.92f, 0.96f, 1.0f};
    float     foam_amount      = 1.0f;   ///< Overall foam multiplier.
    float     shore_foam_depth = 0.6f;   ///< Water depth (m) under which shore/contact foam forms.
    float     edge_fade_depth  = 0.15f;  ///< Depth (m) over which the surface fades in at contacts.
    float     ripple_strength  = 0.35f;  ///< Fine flow-advected ripple normal strength.
    float     ripple_scale     = 1.2f;   ///< Ripple frequency (1/m).
    float     clarity          = 3.0f;   ///< Depth (m) at which the water reads ~63% opaque.

    // --- Underwater (camera below this body's surface; see UnderwaterPass) ---
    glm::vec3 underwater_color{0.05f, 0.24f, 0.28f}; ///< In-scattered colour of the water volume.
    float     underwater_visibility = 14.0f;         ///< Metres until ~95% fogged.
    glm::vec3 underwater_absorption{0.35f, 0.09f, 0.06f}; ///< Per-metre extinction, r/g/b (red goes first).
    float     caustics = 1.0f;                       ///< Caustic brightness on submerged surfaces.

    // --- Physics ---
    float density   = 1000.0f;           ///< kg/m^3 -- fresh water.
    float max_depth = 60.0f;             ///< Depth probe length / depth assigned over open water.

    /**
     * @brief Supplies the surface geometry directly (local space; triangle triples into
     *        `positions`), taking priority over `mesh_path` and the procedural grid -- for water
     *        built by code (gameplay, tools, tests). `uvs` may be empty; for flowing water, UV u
     *        should increase downstream (see water_flow_bake.h). Re-bakes on the next frame.
     */
    void set_geometry(std::vector<glm::vec3> positions, std::vector<glm::vec2> uvs, std::vector<uint32_t> indices) {
        geometry_positions = std::move(positions);
        geometry_uvs       = std::move(uvs);
        geometry_indices   = std::move(indices);
        mark_dirty();
    }
    std::vector<glm::vec3> geometry_positions;
    std::vector<glm::vec2> geometry_uvs;
    std::vector<uint32_t>  geometry_indices;

    /// Set by the parser from `mesh_path`; consumed by WaterSystem's bake.
    coopa::asset::AssetHandle<coopa::gfx::engine::data::SkinnedMeshSource> source;

    // --- Baked state (WaterSystem only) ---
    WaterSurfaceQuery query;
    std::shared_ptr<coopa::gfx::engine::data::Mesh> gpu_mesh;
    glm::mat4 baked_world{0.0f};
    bool      baked = false;
    int       bake_stage = 0;  ///< 1 = baked before physics existed, 2 = with depth/obstacle raycasts.

    /** @brief Marks the bake stale (e.g. after editing a field at runtime). */
    void mark_dirty() {
        baked = false;
        bake_stage = 0;
    }

    /**
     * @brief Samples this body's surface above world XY `p` at water time `t`.
     * @return false if `p` is not over this body, or it has not been baked yet.
     */
    bool sample(const glm::vec2& p, float t, WaterSample& out) const {
        return baked && query.sample(p, waves, t, out);
    }
};

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_BODY_H
