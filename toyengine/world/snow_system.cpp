#include <toyengine/world/snow_system.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <physxcoopa/components/rigidbody.h>
#include <toyengine/weather/weather_system.h>

namespace toy {
namespace world {

void register_snow_components() {
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

void SnowSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
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

glm::vec2 SnowSystem::focus_point(const glm::vec3& cam_pos, const glm::vec3& forward, float ground_z, float max_offset) {
    const glm::vec2 cam_xy(cam_pos);
    if (forward.z >= -1e-3f || cam_pos.z <= ground_z) return cam_xy;
    const float t = (ground_z - cam_pos.z) / forward.z;
    glm::vec2 off = glm::vec2(cam_pos + forward * t) - cam_xy;
    const float len = glm::length(off), lim = std::max(max_offset, 0.0f);
    if (len > lim) off *= lim / len;
    return cam_xy + off;
}

float SnowSystem::depth_at(const glm::vec2& xy, float ground_z) const {
    return deep_snow_depth(glm::vec3(xy, ground_z), max_depth_, cover_, occlusion_.get(), &field_, style_);
}

float SnowSystem::ground_at_(const glm::vec2& xy) const {
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

void SnowSystem::press_(coopa::scene::SceneObject& obj, float radius, float depth, float falloff) {
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

SnowSystem* install_snow_system(coopa::scene::Scene& scene, int order) {
    auto sys = std::make_unique<SnowSystem>();
    SnowSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace world
} // namespace toy
