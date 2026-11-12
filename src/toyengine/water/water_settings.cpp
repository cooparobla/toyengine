#include <toyengine/water/water_settings.h>

namespace toy {
namespace water {

WaterSettings WaterSettings::from_quality(WaterQuality q) {
    WaterSettings s;
    s.quality = q;
    switch (q) {
        case WaterQuality::Low:
            s.sim_radius = 40.0f;  s.wave_iterations = 1; s.max_box_subdivisions = 2;
            s.ripple_range = 30.0f;  s.max_ripples = 8;
            s.grid_density = 0.25f; s.lod_bias = 2.0f;
            s.detail_distance = 40.0f;  s.ripple_layers = 1;
            s.tessellate = false;
            break;
        case WaterQuality::Medium:
            s.sim_radius = 80.0f;  s.wave_iterations = 2; s.max_box_subdivisions = 2;
            s.ripple_range = 50.0f;  s.max_ripples = 16;
            s.grid_density = 0.5f;  s.lod_bias = 1.5f;
            s.detail_distance = 80.0f;  s.ripple_layers = 1;
            s.tessellate = false;
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

} // namespace water
} // namespace toy
