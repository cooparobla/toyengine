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

    static WaterSettings from_quality(WaterQuality q) {
        WaterSettings s;
        s.quality = q;
        switch (q) {
            case WaterQuality::Low:
                s.sim_radius = 40.0f;  s.wave_iterations = 1; s.max_box_subdivisions = 2;
                s.ripple_range = 30.0f;  s.max_ripples = 8;
                s.grid_density = 0.25f; s.lod_bias = 2.0f;
                s.detail_distance = 40.0f;  s.ripple_layers = 1;
                break;
            case WaterQuality::Medium:
                s.sim_radius = 80.0f;  s.wave_iterations = 2; s.max_box_subdivisions = 2;
                s.ripple_range = 50.0f;  s.max_ripples = 16;
                s.grid_density = 0.5f;  s.lod_bias = 1.5f;
                s.detail_distance = 80.0f;  s.ripple_layers = 1;
                break;
            case WaterQuality::High:
                break; // the defaults above
            case WaterQuality::Ultra:
                s.sim_radius = 250.0f; s.wave_iterations = 3; s.max_box_subdivisions = 4;
                s.ripple_range = 120.0f; s.max_ripples = 64;
                s.grid_density = 1.0f;  s.lod_bias = 0.5f;
                s.detail_distance = 300.0f; s.ripple_layers = 2;
                break;
        }
        return s;
    }
};

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_SETTINGS_H
