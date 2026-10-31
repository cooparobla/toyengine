#include "toyengine/hair/hair_params.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace toy {
namespace hair {

namespace {

/// FNV-1a over raw bytes. Only ever fed scalars, so endianness is the only portability concern
/// and this hash never leaves the process (it compares a frame against the previous one).
inline void mix_bytes(std::uint64_t& h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
}

template <typename T>
inline void mix(std::uint64_t& h, const T& value) {
    mix_bytes(h, &value, sizeof(T));
}

inline void mix(std::uint64_t& h, const std::string& value) {
    mix_bytes(h, value.data(), value.size());
    mix(h, value.size());
}

} // namespace

const char* hair_shape_name(HairShape shape) {
    return shape == HairShape::Tube ? "tube" : "ribbon";
}

HairShape parse_hair_shape(const std::string& name) {
    return name == "tube" ? HairShape::Tube : HairShape::Ribbon;
}

const char* hair_regenerate_name(HairRegenerate mode) {
    switch (mode) {
        case HairRegenerate::OnLoad: return "on_load";
        case HairRegenerate::Manual: return "manual";
        case HairRegenerate::OnChange: break;
    }
    return "on_change";
}

HairRegenerate parse_hair_regenerate(const std::string& name) {
    if (name == "on_load") return HairRegenerate::OnLoad;
    if (name == "manual") return HairRegenerate::Manual;
    return HairRegenerate::OnChange;
}

void HairParams::sanitize() {
    cross_section = std::clamp(cross_section, 1, 3);
    sides         = std::clamp(sides, 3, 16);
    segments      = std::clamp(segments, 1, 64);

    density     = std::max(density, 0.0f);
    max_strands = std::clamp(max_strands, 0, 1 << 20);
    length      = std::max(length, 1e-4f);
    // A strand that jittered to exactly zero length has no tangent to build a frame from.
    length_jitter = std::clamp(length_jitter, 0.0f, 0.95f);
    thickness     = std::max(thickness, 1e-5f);

    droop         = std::max(droop, 0.0f);
    clump         = std::clamp(clump, 0.0f, 1.0f);
    clump_density = std::clamp(clump_density, 1e-4f, 1.0f);
    curl          = std::max(curl, 0.0f);
    curl_radius   = std::max(curl_radius, 0.0f);
    frizz_degrees = std::clamp(frizz_degrees, 0.0f, 89.0f);

    guide_influence = std::clamp(guide_influence, 0.0f, 1.0f);
    guide_count     = std::clamp(guide_count, 1, 8);

    wind_flexibility = std::max(wind_flexibility, 0.0f);
    wind_frequency   = std::max(wind_frequency, 0.0f);

    sim_guides = std::clamp(sim_guides, 1, 4096);
    sim_stiffness           = std::clamp(sim_stiffness, 0.0f, 1.0f);
    sim_stiffness_falloff   = std::clamp(sim_stiffness_falloff, 0.0f, 16.0f);
    sim_curl_hold           = std::clamp(sim_curl_hold, 0.0f, 1.0f);
    sim_damping             = std::clamp(sim_damping, 0.0f, 10.0f);
    sim_drag                = std::clamp(sim_drag, 0.0f, 20.0f);
    sim_collision_thickness = std::clamp(sim_collision_thickness, 0.0f, 1.0f);
    sim_friction            = std::clamp(sim_friction, 0.0f, 1.0f);
    sim_gravity             = std::clamp(sim_gravity, -10.0f, 10.0f);

    // Thresholds must descend: level 1 is drawn before level 2 as the groom shrinks on screen.
    lod_screen_sizes.x = std::max(lod_screen_sizes.x, 0.0f);
    lod_screen_sizes.y = std::clamp(lod_screen_sizes.y, 0.0f, lod_screen_sizes.x);
    cull_screen_size   = std::max(cull_screen_size, 0.0f);

    if (thickness_over_length.empty()) thickness_over_length.set_fallback(1.0f);
}

std::uint64_t HairParams::hash() const {
    std::uint64_t h = 14695981039346656037ull;
    mix(h, shape);
    mix(h, cross_section);
    mix(h, sides);
    mix(h, segments);
    mix(h, density);
    mix(h, max_strands);
    mix(h, length);
    mix(h, length_jitter);
    mix(h, thickness);
    for (const glm::vec2& key : thickness_over_length.keys()) mix(h, key);
    mix(h, droop);
    mix(h, clump);
    mix(h, clump_density);
    mix(h, curl);
    mix(h, curl_radius);
    mix(h, frizz_degrees);
    mix(h, root_mask);
    mix(h, guides);
    mix(h, guide_influence);
    mix(h, guide_count);
    mix(h, simulate);
    mix(h, sim_guides);
    mix(h, seed);
    // lod_screen_sizes, cull_screen_size and the wind response are deliberately absent: they
    // change which level is DRAWN or how the shader moves it, never what was generated, so tuning
    // them must not trigger a re-scatter.
    return h;
}

} // namespace hair
} // namespace toy
