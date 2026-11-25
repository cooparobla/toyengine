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
    set_geometry(std::move(positions), std::move(uvs), std::move(indices), std::move(depths), {});
}

void WaterBody::set_geometry(std::vector<glm::vec3> positions, std::vector<glm::vec2> uvs,
                             std::vector<uint32_t> indices, std::vector<float> depths,
                             std::vector<float> wave_scales) {
    geometry_positions    = std::move(positions);
    geometry_uvs          = std::move(uvs);
    geometry_indices      = std::move(indices);
    geometry_depths       = std::move(depths);
    geometry_wave_scales  = std::move(wave_scales);
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
    to.tile_size = from.tile_size;
    to.waves = from.waves;
    to.wind_response = from.wind_response;
    to.flow_speed = from.flow_speed;
    to.flow_min_speed = from.flow_min_speed;
    to.flow_slope_gain = from.flow_slope_gain;
    to.obstacle_radius = from.obstacle_radius;
    to.wake_length = from.wake_length;
    to.deep_color = from.deep_color;
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
    to.fluid = from.fluid;
    to.emissive_color = from.emissive_color;
    to.emissive_strength = from.emissive_strength;
    to.viscosity = from.viscosity;
    to.temperature_c = from.temperature_c;
}

void apply_fluid_preset(WaterBody& body, FluidKind kind) {
    body.fluid = kind;
    if (kind == FluidKind::Water) {
        const WaterBody fresh;
        body.emissive_color = fresh.emissive_color;
        body.emissive_strength = fresh.emissive_strength;
        body.viscosity = fresh.viscosity;
        body.temperature_c = fresh.temperature_c;
        body.deep_color = fresh.deep_color;
        body.foam_color = fresh.foam_color;
        body.foam_amount = fresh.foam_amount;
        body.clarity = fresh.clarity;
        body.underwater_color = fresh.underwater_color;
        body.underwater_visibility = fresh.underwater_visibility;
        body.underwater_absorption = fresh.underwater_absorption;
        body.caustics = fresh.caustics;
        body.density = fresh.density;
        body.waves = fresh.waves;
        body.wind_response = fresh.wind_response;
        body.ripple_strength = fresh.ripple_strength;
        body.ripple_scale = fresh.ripple_scale;
        return;
    }

    // Lava. The albedo is the COOLED CRUST and the heat arrives as emission, for the same
    // reason the lava tile surface works that way: a body whose own colour was already orange
    // reads as orange water the moment it is in shadow.
    body.deep_color = {0.05f, 0.02f, 0.015f};
    body.emissive_color = {1.0f, 0.32f, 0.06f};
    // Low for the same reason the lava GROUND's is (see terrain_styled.glsl): a lake of this
    // fills a large part of the frame, and at a fire's brightness it saturates to white.
    body.emissive_strength = 1.1f;
    // Opaque within centimetres, so nothing of the bed shows through and the surface carries
    // the whole look. `clarity` is the depth at which it reads ~63% opaque.
    body.clarity = 0.05f;
    // "Foam" on lava is the pale crust that gathers where a flow meets its edge.
    body.foam_color = {0.28f, 0.11f, 0.07f};
    body.foam_amount = 0.55f;
    // Basaltic melt, a little under three times water.
    body.density = 2800.0f;
    body.temperature_c = 1100.0f;
    body.viscosity = 0.88f;
    // Long, low, slow swells rather than chop -- and deaf to the weather, since wind does not
    // raise waves on something this viscous.
    body.waves.amplitude = 0.06f;
    body.waves.wavelength = 14.0f;
    body.waves.steepness = 0.15f;
    body.wind_response = 0.0f;
    body.ripple_strength = 0.08f;
    body.ripple_scale = 0.35f;
    // Submerged in lava nothing is visible at all, and the little that is read is red.
    body.underwater_color = {0.35f, 0.06f, 0.01f};
    body.underwater_visibility = 0.6f;
    body.underwater_absorption = {0.2f, 3.0f, 6.0f};
    body.caustics = 0.0f;
}

FluidKind fluid_kind_from_name(const std::string& name) {
    return name == "lava" ? FluidKind::Lava : FluidKind::Water;
}

const char* fluid_kind_name(FluidKind kind) {
    return kind == FluidKind::Lava ? "lava" : "water";
}

} // namespace water
} // namespace toy
