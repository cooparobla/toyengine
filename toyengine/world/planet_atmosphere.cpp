#include <toyengine/world/planet_atmosphere.h>

#include <algorithm>
#include <memory>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/components/volume.h>
#include <toyengine/world/terrain_component.h>

namespace toy {
namespace world {

void PlanetAtmosphere::apply() {
    if (owner == nullptr) return;
    // Resolved lazily rather than in start(): the component has to work the same in edit mode,
    // where the terrain it measures may only appear once the generator has run.
    if (terrain_ == nullptr) {
        for (coopa::scene::SceneObject* p = owner->parent(); p != nullptr; p = p->parent()) {
            if (auto* t = p->get_component<TerrainComponent>()) {
                terrain_ = t;
                break;
            }
        }
        if (terrain_ == nullptr) return;
    }
    if (terrain_->field.empty()) return;
    TerraData* data = terrain_->field.data();
    if (data == nullptr || data->height_revision() == applied_height_revision_) return;

    auto* volume = owner->get_component<coopa::gfx::engine::components::VolumeComponent>();
    if (volume == nullptr) return;

    // The terrain's own measured peak, not params.height_scale (a nominal amplitude the bake
    // may fall well short of, or -- with enough noise detail -- exceed).
    const float peak = std::max(data->height_range().y, 0.0f);
    const float planet_radius = terrain_->params.layout().radius();
    const float thickness = std::max(min_thickness, peak * thickness_scale);
    const float outer = planet_radius + peak + thickness;

    volume->extent         = glm::vec3(outer, outer, outer);
    volume->height_base    = planet_radius;   // VolumeShape::Shell's inner radius
    volume->height_falloff = std::max(thickness * scale_height_fraction, 0.1f);

    applied_height_revision_ = data->height_revision();
}

namespace {

/**
 * @class PlanetAtmosphereSystem
 * @brief Drives every PlanetAtmosphere; see install_planet_atmosphere_system().
 */
class PlanetAtmosphereSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "PlanetAtmosphere"; }
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) override {
        for (PlanetAtmosphere* a : scene.get_components<PlanetAtmosphere>()) a->apply();
    }
};

} // namespace

void install_planet_atmosphere_system(coopa::scene::Scene& scene, int order) {
    scene.add_system(std::make_unique<PlanetAtmosphereSystem>(), order);
}

} // namespace world
} // namespace toy
