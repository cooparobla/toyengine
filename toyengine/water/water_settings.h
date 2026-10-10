/**
 * @file water_settings.h
 * @brief Water quality tiers: what WaterSystem simulates, how far from the camera, and how much
 *        detail the water mesh and shader carry.
 *
 * One `water_quality` key (config.yaml, render section) picks a tier; Engine maps it here and
 * hands the expanded WaterSettings to WaterSystem::set_settings(), live. High is the shipped
 * default.
 *
 * Range. Everything is measured from WaterSystem's focus -- the main camera:
 *  - `sim_radius`: floating bodies inside it get full pontoon buoyancy. Outside (with
 *    hysteresis) they are frozen: put to sleep where they float, at zero cost. One that is woken
 *    out there anyway (a collision) gets cheap calm-water buoyancy until it settles, so it never
 *    sinks. The physics-aware (stage 2) bake of a water body also waits until it is in range.
 *  - `ripple_range`: only bodies this close ring the water, and only rings this close are drawn.
 *  - `detail_distance`: past it the shader drops ripples and foam noise for a rougher surface.
 *
 * Tessellation. A body tessellates when its MeshRenderer enables `tessellation:` (with that
 * renderer's values), so near the camera the Gerstner waves keep their shape where the baked grid
 * is too coarse to carry them. The tier only gates it: Low and Medium draw the baked grid
 * whatever the renderer asks (`tessellate`). Devices without tessellation draw it as baked too.
 * Wave LOD needs no setting: every derived wave fades with distance on its own (water_waves.h).
 */

#ifndef TOYENGINE_WATER_WATER_SETTINGS_H
#define TOYENGINE_WATER_WATER_SETTINGS_H

#include <cstddef>

namespace toy {
namespace water {

/** @brief Water quality tier. Mirrors render::RenderQuality without depending on render/. */
enum class WaterQuality { Low, Medium, High, Ultra };

/**
 * @struct WaterSettings
 * @brief A tier expanded into concrete values. See the file doc; from_quality() is the table.
 */
struct WaterSettings {
    WaterQuality quality = WaterQuality::High;

    // --- Simulation range (m from the focus) ---
    float sim_radius = 150.0f;         ///< Full buoyancy inside; frozen floaters outside.
    float sim_hysteresis = 1.15f;      ///< A floater leaves the simulated set at sim_radius * this.
    int   wave_iterations = 3;         ///< Wave-height inversion steps per buoyancy sample.
    int   max_box_subdivisions = 3;    ///< Cap on Buoyancy::subdivisions (box pontoon lattice).

    // --- Ripples ---
    float       ripple_range = 80.0f;  ///< Emitters and drawn rings within this of the focus.
    std::size_t max_ripples = 32;      ///< Live rings kept (<= render::kMaxWaterRipples).

    // --- Mesh ---
    float grid_density = 1.0f;         ///< Multiplier on a procedural WaterBody::resolution.
    float lod_bias = 1.0f;             ///< Scales tile LOD switch sizes: > 1 coarsens sooner.
    int   bakes_per_frame = 1;         ///< Stage-2 (physics-aware) bakes per frame, nearest first.

    // --- Shader ---
    float detail_distance = 150.0f;    ///< Ripples/foam noise fade out toward this distance.
    int   ripple_layers = 2;           ///< Flow-ripple layers in the fragment shader (1 or 2).

    // --- Tessellation (see the file doc) ---
    bool  tessellate = true;           ///< Allow bodies whose MeshRenderer asks for it to tessellate.

    static WaterSettings from_quality(WaterQuality q);
};

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_SETTINGS_H
