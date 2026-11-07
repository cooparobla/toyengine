/**
 * @file terrain_noise.h
 * @brief A small 2D fractal noise, independent of any terrain storage -- what the editor's
 *        sculpt brush (editor/terrain/terrain_brush.cpp) uses for its Noise tool.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_NOISE_H
#define TOYENGINE_WORLD_TERRAIN_NOISE_H

#include <cstdint>

namespace toy {
namespace world {

/** @brief Fractal (multi-octave) gradient noise, roughly in [-1, 1]. `ridges` blends toward
 *         ridged noise (peaks along the zero crossings), 0 none .. 1 fully ridged. */
float heightmap_fbm(float x, float y, std::uint32_t seed, int octaves, float ridges);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_NOISE_H
