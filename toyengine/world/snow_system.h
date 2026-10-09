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
 * The SnowSystem (order 370, after transforms resolve) keeps the field window on what the main
 * camera looks at (where its view ray meets the ground, kept within the window of the camera
 * itself) -- so zooming an orbit camera out does not scroll the tracks under its target out of the
 * window and clear them. It stamps every deformer, and refills trenches: back to level over the
 * weather's snow_trench_recover_time, plus faster while it snows (at the rate the cover builds). Engine::sync_surface_state_() hands the field to the renderer; the `snow` surface
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
        style_ = live ? SnowStyle{w->settings().snow_patch_hard, w->settings().snow_patch_size} : SnowStyle{};

        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        if (cam && cam->owner && cam->owner->get_transform()) {
            const glm::mat4& m = cam->owner->get_transform()->transform().get_world_matrix();
            field_.set_focus(focus_point(glm::vec3(m[3]), -glm::normalize(glm::vec3(m[2])), ground_at_(glm::vec2(m[3])),
                                         0.5f * static_cast<float>(field_.n()) * field_.cell() - 4.0f));
        }
        if (!live || cover_ <= 0.0f) {
            // No snow lying: nothing to keep.
            if (had_trenches_) { field_.clear(); had_trenches_ = false; }
            return;
        }
        if (!scene.is_simulating()) return;

        // Tracks settle back to level over snow_trench_recover_time; fresh snow fills them faster
        // still, at the rate the cover builds. Applied in steps of at least kRefillStep seconds:
        // every refill re-uploads the whole field, so per-frame steps would cost an upload a frame.
        const weather::WeatherState& ws = w->state();
        if (ctx.delta_time > 0.0f) {
            float rate = 0.0f;   // metres per second
            const float recover = w->settings().snow_trench_recover_time;
            if (recover > 0.0f) rate += max_depth_ / recover;
            if (weather::WeatherSystem::snows(ws.precipitation, ws.temperature)) {
                rate += max_depth_ * ws.precipitation / std::max(w->settings().snow_accumulate_time, 1.0f);
            }
            refill_pending_ += rate * ctx.delta_time;
            refill_clock_ += ctx.delta_time;
            if (refill_clock_ >= kRefillStep) {
                field_.refill(refill_pending_);
                refill_pending_ = 0.0f;
                refill_clock_ = 0.0f;
            }
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

    /** @brief Seconds between trench refill steps (each re-uploads the field). */
    static constexpr float kRefillStep = 0.05f;

    /**
     * @brief Where the trench window centres: the ground point the camera looks at (its view ray
     *        meeting the plane z = `ground_z`), kept within `max_offset` metres of the camera's
     *        own xy; the camera's xy when it looks level or up.
     */
    static glm::vec2 focus_point(const glm::vec3& cam_pos, const glm::vec3& forward, float ground_z, float max_offset) {
        const glm::vec2 cam_xy(cam_pos);
        if (forward.z >= -1e-3f || cam_pos.z <= ground_z) return cam_xy;
        const float t = (ground_z - cam_pos.z) / forward.z;
        glm::vec2 off = glm::vec2(cam_pos + forward * t) - cam_xy;
        const float len = glm::length(off), lim = std::max(max_offset, 0.0f);
        if (len > lim) off *= lim / len;
        return cam_xy + off;
    }

    /**
     * @brief How deep the visible deep snow is at world xy (m): what a `snow`-shaded surface at
     *        ground height `ground_z` is raised by there, trenches included.
     */
    float depth_at(const glm::vec2& xy, float ground_z) const {
        return deep_snow_depth(glm::vec3(xy, ground_z), max_depth_, cover_, occlusion_.get(), &field_, style_);
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
        const float snow_top = ground + deep_snow_depth(glm::vec3(c, ground), max_depth_, cover_, occlusion_.get(), nullptr, style_);
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
    SnowStyle style_;
    float refill_pending_ = 0.0f;   ///< Metres of refill not yet applied (see kRefillStep).
    float refill_clock_ = 0.0f;
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
