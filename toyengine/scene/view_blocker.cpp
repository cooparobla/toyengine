#include <toyengine/scene/view_blocker.h>

#include <algorithm>
#include <cmath>

#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>

namespace toy {
namespace scene {

float advance_blocker_cutoff(float current, bool blocking, float fade_target,
                             float fade_in_time, float fade_out_time, float dt) {
    const float target = blocking ? std::max(fade_target, 0.0f) : 0.0f;
    const float time   = blocking ? fade_in_time : fade_out_time;
    const float limit  = std::max(fade_target, 0.0f);
    // A paused frame must not advance the dissolve, so dt and time are NOT the same case: dt 0
    // holds, while time 0 means the author asked for an instant fade.
    if (dt <= 0.0f) return std::clamp(current, 0.0f, limit);
    if (time <= 0.0f) return std::clamp(target, 0.0f, limit);
    // Rate is per full 0 -> fade_target sweep, so halving fade_target halves the distance AND
    // the time, leaving the dissolve looking the same speed rather than twice as fast.
    const float step = (limit > 0.0f ? limit : 1.0f) * dt / time;
    const float next = current + std::clamp(target - current, -step, step);
    return std::clamp(next, 0.0f, limit);
}

float quantize_cutoff(float value, int steps) {
    if (steps <= 1) return value;
    const float n = static_cast<float>(steps);
    return std::round(value * n) / n;
}

void ViewBlockerFade::advance(float delta_time) {
    if (!owner) return;
    cutoff_ = advance_blocker_cutoff(cutoff_, blocking_, fade_target, fade_in_time, fade_out_time, delta_time);

    const float quantized = quantize_cutoff(cutoff_, cutoff_steps);
    if (written_ == quantized) return;   // the point of quantizing: most frames write nothing
    written_ = quantized;

    auto* mr = owner->get_component<coopa::gfx::engine::components::MeshRenderer>();
    if (!mr) return;
    // Every slot, not just slot 0: a wall built as one mesh with a trim submesh has to erode
    // as one object or the trim is left floating.
    const uint32_t slots = static_cast<uint32_t>(mr->slot_materials.size()) + 1u;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        mr->material_for_mut(slot).alpha_cutoff = quantized;
    }
}

} // namespace scene
} // namespace toy
