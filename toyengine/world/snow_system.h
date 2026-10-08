/**
 * @file snow_system.h
 * @brief Snow trails: the system that presses things into the lying snow (SnowField) and the
 *        component that marks what presses.
 *
 *  - `SnowDeformer` on an object presses a disc into the snow under it while it is in the snow
 *    (its base no higher than the snow surface): footprints, wheel tracks, a sled.
 *    @code
 *    - type: SnowDeformer
 *      radius: 0.35     # m
 *      depth: -1        # m pressed in; < 0: down to the object's base (its lowest point)
 *      falloff: 0.5     # 0 hard-edged .. 1 a soft bowl
 *    @endcode
 *  - With the weather's `snow_auto_deformers: true`, every Rigidbody (kinematic controllers
 *    included) presses its footprint in too, without a component.
 *
 * The SnowSystem (order 370, after transforms resolve) follows the main camera with the field
 * window, stamps every deformer, and refills trenches while it snows (at the rate the cover
 * builds). Engine::sync_surface_state_() hands the field to the renderer; the `snow` surface
 * shader (and nothing else) draws the trenches. Gameplay reads the same snow through
 * depth_at(): how deep the visible snow is at a point.
 *
 * A deformer is "in the snow" when its base is below the snow top: the ground under it (the
 * weather's precipitation map -- its sky layer, which looks through moving bodies -- lowest of
 * the 3x3 cells around, or the weather's ground_height) plus the deep-snow depth there.
 */

#ifndef TOYENGINE_WORLD_SNOW_SYSTEM_H
#define TOYENGINE_WORLD_SNOW_SYSTEM_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <fkYAML/node.hpp>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <physxcoopa/components/rigidbody.h>

#include <toyengine/weather/weather_system.h>
#include <toyengine/world/snow_field.h>

namespace toy {
namespace world {

/** @brief Presses a disc into the lying snow under its object. See the file doc. */
class SnowDeformer : public coopa::scene::Component {
public:
    float radius = 0.35f;   ///< m.
    float depth = -1.0f;    ///< m pressed in; < 0: down to the object's base.
    float falloff = 0.5f;   ///< 0 hard-edged .. 1 a soft bowl.
    std::string type_name() const override { return "SnowDeformer"; }
};

/** @brief Registers "SnowDeformer". */
inline void register_snow_components() {
    using coopa::scene::SceneLoader;
    SceneLoader::register_component_parser("SnowDeformer",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<SnowDeformer>();
            auto num = [&](const char* k, float& out) {
                if (!node.contains(k)) return;
                const fkyaml::node& v = node.at(k);
                if (v.is_float_number()) out = static_cast<float>(v.get_value<double>());
                else if (v.is_integer()) out = static_cast<float>(v.get_value<int64_t>());
            };
            num("radius", d->radius);
            num("depth", d->depth);
            num("falloff", d->falloff);
        });
}

inline constexpr int k_snow_system_order = 370;

class SnowSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "Snow"; }
    /// Runs in edit mode only to keep the window on the camera; nothing presses in until the
    /// scene simulates.
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        weather::WeatherSystem* w = weather::find(scene);
        const bool live = w && w->enabled() && w->state().enabled;
        cover_ = live ? w->state().snow_cover : 0.0f;
        max_depth_ = live ? w->settings().snow_max_depth : 0.0f;
        occlusion_ = live ? w->ground_probe().field() : nullptr;
        fallback_ground_ = live ? w->settings().ground_height : 0.0f;

        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        if (cam && cam->owner && cam->owner->get_transform()) {
            field_.set_focus(glm::vec2(cam->owner->get_transform()->transform().get_world_matrix()[3]));
        }
        if (!live || cover_ <= 0.0f) {
            // No snow lying: nothing to keep.
            if (had_trenches_) { field_.clear(); had_trenches_ = false; }
            return;
        }
        if (!scene.is_simulating()) return;

        // Fresh snow fills the trenches at the rate the cover builds.
        const weather::WeatherState& ws = w->state();
        if (weather::WeatherSystem::snows(ws.precipitation, ws.temperature) && ctx.delta_time > 0.0f) {
            field_.refill(max_depth_ * ws.precipitation * ctx.delta_time / std::max(w->settings().snow_accumulate_time, 1.0f));
        }

        for (SnowDeformer* d : scene.get_components<SnowDeformer>()) {
            if (!d->owner || !d->owner->active()) continue;
            press_(*d->owner, d->radius, d->depth, d->falloff);
        }
        if (w->settings().snow_auto_deformers) {
            for (auto* rb : scene.get_components<coopa::physx::components::RigidbodyComponent>()) {
                if (!rb->owner || !rb->owner->active() || rb->owner->get_component<SnowDeformer>()) continue;
                press_(*rb->owner, -1.0f, -1.0f, 0.4f);
            }
        }
    }

    const SnowField& field() const { return field_; }
    SnowField& field() { return field_; }

    /**
     * @brief How deep the visible deep snow is at world xy (m): what a `snow`-shaded surface at
     *        ground height `ground_z` is raised by there, trenches included.
     */
    float depth_at(const glm::vec2& xy, float ground_z) const {
        return deep_snow_depth(glm::vec3(xy, ground_z), max_depth_, cover_, occlusion_.get(), &field_);
    }
    /** @brief The trench pressed in at world xy (m). */
    float trench_at(const glm::vec2& xy) const { return field_.trench_at(xy); }

private:
    /** @brief The ground under xy: the lowest cell of the precipitation map's sky layer (moving
     *         bodies looked through) in the 3x3 around it. */
    float ground_at_(const glm::vec2& xy) const {
        const particles::GroundField* f = occlusion_.get();
        if (!f || f->nx <= 0) return fallback_ground_;
        const int ci = static_cast<int>(std::floor((xy.x - f->origin.x) / f->cell));
        const int cj = static_cast<int>(std::floor((xy.y - f->origin.y) / f->cell));
        float g = std::numeric_limits<float>::max();
        for (int j = cj - 1; j <= cj + 1; ++j) {
            for (int i = ci - 1; i <= ci + 1; ++i) {
                float h = f->fallback;
                if (i >= 0 && j >= 0 && i < f->nx && j < f->ny) {
                    const float v = snow_sky_heights(*f)[static_cast<size_t>(j) * static_cast<size_t>(f->nx) + static_cast<size_t>(i)];
                    if (!std::isnan(v)) h = v;
                }
                g = std::min(g, h);
            }
        }
        return g;
    }

    /** @brief Stamps `obj`'s footprint if its base is in the snow. radius < 0: from its bounds. */
    void press_(coopa::scene::SceneObject& obj, float radius, float depth, float falloff) {
        auto* tc = obj.get_transform();
        if (!tc) return;
        const glm::mat4& m = tc->transform().get_world_matrix();
        glm::vec3 lo(m[3]), hi(m[3]);
        bool have_bounds = false;
        if (auto* mr = obj.get_component<coopa::gfx::engine::components::MeshRenderer>()) {
            if (auto mesh = mr->get_mesh()) {
                const glm::vec3 bmin = mesh->bounds_min(), bmax = mesh->bounds_max();
                if (bmin.x <= bmax.x) {
                    lo = glm::vec3(std::numeric_limits<float>::max());
                    hi = glm::vec3(std::numeric_limits<float>::lowest());
                    for (int k = 0; k < 8; ++k) {
                        const glm::vec3 c((k & 1) ? bmax.x : bmin.x, (k & 2) ? bmax.y : bmin.y, (k & 4) ? bmax.z : bmin.z);
                        const glm::vec3 p = glm::vec3(m * glm::vec4(c, 1.0f));
                        lo = glm::min(lo, p);
                        hi = glm::max(hi, p);
                    }
                    have_bounds = true;
                }
            }
        }
        const glm::vec2 c = have_bounds ? glm::vec2((lo + hi) * 0.5f) : glm::vec2(m[3]);
        if (radius < 0.0f) radius = have_bounds ? std::max(0.1f, 0.5f * std::min(hi.x - lo.x, hi.y - lo.y)) : 0.35f;
        const float base = lo.z;
        const float ground = ground_at_(c);
        const float snow_top = ground + deep_snow_depth(glm::vec3(c, ground), max_depth_, cover_, occlusion_.get(), nullptr);
        if (base > snow_top + 0.02f) return;   // above the snow
        const float press = depth >= 0.0f ? depth : std::clamp(snow_top - std::max(base, ground), 0.0f, max_depth_);
        if (press <= 0.0f) return;
        field_.stamp(c, radius, press, falloff);
        had_trenches_ = true;
    }

    SnowField field_;
    float cover_ = 0.0f;
    float max_depth_ = 0.0f;
    float fallback_ground_ = 0.0f;
    std::shared_ptr<const particles::GroundField> occlusion_;
    bool had_trenches_ = false;
};

/** @brief `scene`'s snow system, or null. */
inline SnowSystem* find_snow(const coopa::scene::Scene& scene) {
    return dynamic_cast<SnowSystem*>(scene.find_system("Snow"));
}

inline SnowSystem* install_snow_system(coopa::scene::Scene& scene, int order = k_snow_system_order) {
    auto sys = std::make_unique<SnowSystem>();
    SnowSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_SNOW_SYSTEM_H
