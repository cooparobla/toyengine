#include <toyengine/water/water_body.h>

#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

void WaterBody::set_geometry(std::vector<glm::vec3> positions, std::vector<glm::vec2> uvs, std::vector<uint32_t> indices) {
    geometry_positions = std::move(positions);
    geometry_uvs       = std::move(uvs);
    geometry_indices   = std::move(indices);
    mark_dirty();
}

void WaterBody::mark_dirty() {
    baked = false;
    bake_stage = 0;
}

const WaveSet& WaterBody::wave_set() const {
    if (!wave_set_.matches(waves)) wave_set_ = WaveSet::from(waves);
    return wave_set_;
}

} // namespace water
} // namespace toy
