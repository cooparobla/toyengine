#include <toyengine/world/terrain_noise.h>

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace toy {
namespace world {

namespace {

std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/** @brief Pseudo-random gradient direction at an integer lattice point. */
glm::vec2 lattice_gradient(int ix, int iy, std::uint32_t seed) {
    const std::uint32_t h = hash_u32(static_cast<std::uint32_t>(ix) * 0x8da6b343U ^
                                     static_cast<std::uint32_t>(iy) * 0xd8163841U ^ seed * 0xcb1ab31fU);
    const float a = static_cast<float>(h) * (6.28318530718f / 4294967296.0f);
    return glm::vec2(std::cos(a), std::sin(a));
}

/** @brief 2D gradient noise in about [-0.7, 0.7]. */
float gradient_noise(float x, float y, std::uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const float tx = x - fx, ty = y - fy;
    auto dot_at = [&](int ox, int oy) {
        const glm::vec2 g = lattice_gradient(ix + ox, iy + oy, seed);
        return g.x * (tx - static_cast<float>(ox)) + g.y * (ty - static_cast<float>(oy));
    };
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(tx), v = fade(ty);
    const float a = dot_at(0, 0) + (dot_at(1, 0) - dot_at(0, 0)) * u;
    const float b = dot_at(0, 1) + (dot_at(1, 1) - dot_at(0, 1)) * u;
    return a + (b - a) * v;
}

} // namespace

float heightmap_fbm(float x, float y, std::uint32_t seed, int octaves, float ridges) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    float fx = x, fy = y;
    for (int o = 0; o < std::max(1, octaves); ++o) {
        const float n = gradient_noise(fx, fy, seed + static_cast<std::uint32_t>(o) * 1013u) * 1.4f;
        const float ridged = 1.0f - 2.0f * std::abs(n); // peaks along the zero crossings
        sum += amp * (n + (ridged - n) * ridges);
        norm += amp;
        amp *= 0.5f;
        // Rotate a little each octave so lattice artefacts don't line up.
        const float rx = fx * 1.6f - fy * 1.2f, ry = fx * 1.2f + fy * 1.6f;
        fx = rx + 17.3f; fy = ry - 9.1f;
    }
    return sum / norm;
}

} // namespace world
} // namespace toy
