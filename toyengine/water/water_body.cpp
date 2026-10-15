#include <toyengine/water/water_body.h>

#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

void WaterBody::set_geometry(std::vector<glm::vec3> positions, std::vector<glm::vec2> uvs, std::vector<uint32_t> indices) {
    set_geometry(std::move(positions), std::move(uvs), std::move(indices), {});
}

void WaterBody::set_geometry(std::vector<glm::vec3> positions, std::vector<glm::vec2> uvs,
                             std::vector<uint32_t> indices, std::vector<float> depths) {
    geometry_positions = std::move(positions);
    geometry_uvs       = std::move(uvs);
    geometry_indices   = std::move(indices);
    geometry_depths    = std::move(depths);
    mark_dirty();
}

void WaterBody::mark_dirty() {
    baked = false;
    bake_stage = 0;
}

const WaveSet& WaterBody::wave_set() const {
    if (!wave_set_.matches(live_waves())) wave_set_ = WaveSet::from(live_waves());
    return wave_set_;
}

void copy_water_settings(const WaterBody& from, WaterBody& to) {
    to.waves = from.waves;
    to.wind_response = from.wind_response;
    to.flow_speed = from.flow_speed;
    to.flow_min_speed = from.flow_min_speed;
    to.flow_slope_gain = from.flow_slope_gain;
    to.obstacle_radius = from.obstacle_radius;
    to.wake_length = from.wake_length;
    to.foam_color = from.foam_color;
    to.foam_amount = from.foam_amount;
    to.shore_foam_depth = from.shore_foam_depth;
    to.edge_fade_depth = from.edge_fade_depth;
    to.ripple_strength = from.ripple_strength;
    to.ripple_scale = from.ripple_scale;
    to.clarity = from.clarity;
    to.underwater_color = from.underwater_color;
    to.underwater_visibility = from.underwater_visibility;
    to.underwater_absorption = from.underwater_absorption;
    to.caustics = from.caustics;
    to.density = from.density;
    to.max_depth = from.max_depth;
}

} // namespace water
} // namespace toy
