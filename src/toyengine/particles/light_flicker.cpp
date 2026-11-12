#include <toyengine/particles/light_flicker.h>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <toyengine/particles/particle_math.h>

namespace toy {
namespace particles {

void LightFlicker::start() {
    if (!owner) return;
    if (auto* pl = owner->get_component<coopa::gfx::engine::components::PointLightComponent>()) {
        base_intensity_ = pl->intensity;
        base_color_ = pl->color;
    }
    if (auto* tc = owner->get_transform()) base_pos_ = tc->transform().position();
    if (seed == 0) {
        uint32_t h = 2166136261u;
        for (char c : owner->name()) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
        seed = h;
    }
    phase_ = hash01(seed) * 100.0f;
    started_ = true;
}

void LightFlicker::update(float dt) {
    if (!started_ || !owner) return;
    t_ += dt;
    auto* pl = owner->get_component<coopa::gfx::engine::components::PointLightComponent>();
    if (!pl) return;
    const float n = flicker_(t_ * speed + phase_);
    pl->intensity = base_intensity_ * std::max(0.0f, 1.0f + amount * n) * dimmer;
    pl->color = base_color_ + color_shift * std::max(0.0f, n);
    if (wobble > 0.0f) {
        if (auto* tc = owner->get_transform()) {
            const glm::vec3 j(flicker_(t_ * speed * 0.7f + phase_ + 13.0f), flicker_(t_ * speed * 0.6f + phase_ + 29.0f),
                              flicker_(t_ * speed * 0.5f + phase_ + 47.0f) * 0.5f);
            tc->transform().set_position(base_pos_ + j * wobble);
        }
    }
}

float LightFlicker::noise1_(float x) {
    const float i = std::floor(x);
    const float f = x - i;
    const float a = hash01(static_cast<uint32_t>(static_cast<int32_t>(i)) * 2654435761u) * 2.0f - 1.0f;
    const float b = hash01(static_cast<uint32_t>(static_cast<int32_t>(i) + 1) * 2654435761u) * 2.0f - 1.0f;
    const float s = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * s;
}

} // namespace particles
} // namespace toy
