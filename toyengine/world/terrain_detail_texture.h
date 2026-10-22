#pragma once

/**
 * @file terrain_detail_texture.h
 * @brief The styled terrain's surface detail, baked once into a tiling mipmapped texture.
 *
 * terrain_styled.glsl used to evaluate its detail noise analytically per pixel and fade each
 * octave out with a hand-placed `octave_weight` band. An analytic fade is a single-sample
 * approximation of prefiltering: it decides how much of an octave to keep from the pixel's
 * footprint, but it still evaluates that octave at ONE point, so the value a pixel gets keeps
 * changing as the sampling point drifts. Under camera motion that is per-pixel flicker on every
 * surface -- bisected on cubeworld (greenhollow) to the fine octave, and tunable only into a
 * trade between flicker and a washed-out look.
 *
 * Baking the same noise into a texture hands the problem to the hardware that exists for it: a
 * mip chain is a real prefilter (every level is the average of the one above, not a guess from
 * a derivative), and trilinear + anisotropic sampling blends between levels continuously, per
 * pixel, per frame -- including on the oblique walls a single footprint number cannot describe.
 * The same mechanism every engine uses for texture detail, now used for this.
 *
 * The bake is deterministic and CPU-side: the same value-noise field and the same per-kind
 * character table the shader used, evaluated on a regular grid instead of at shading points.
 */

#include <cstdint>
#include <memory>
#include <vector>

#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/memory/allocator.h>

namespace toy {
namespace world {

/**
 * @brief Bakes (once) the tiling detail texture the styled terrain samples, and registers it
 *        with the asset manager under a synthetic id so every chunk material shares one upload.
 *
 * The texture is RGBA8, `resolution` square, and tiles seamlessly over `world_period` world
 * units: the noise is evaluated on a torus so the left edge continues into the right and the
 * top into the bottom, which is what lets a repeating sampler cover unbounded terrain without
 * a visible seam.
 *
 * Channels, all in [0,1] with 0.5 as "no change", so the shader's use of them is a signed
 * offset around the kind's flat colour:
 *   - R: the broad + mid octaves (large blotches, the slow variation across a tile)
 *   - G: the fine octave (the close-range grain)
 *   - B: the speck field, already thresholded into rounded flecks at bake time -- a hard
 *        threshold is exactly the kind of sharp edge that cannot be filtered after the fact,
 *        and baking it means the mip chain averages the flecks into coverage instead
 *   - A: the strata field (noise squashed vertically, for the horizontal banding on walls)
 *
 * @param device     Logical device.
 * @param allocator  VMA allocator.
 * @param cmd_pool   Command pool for the one-shot upload.
 * @param assets     Asset manager the baked texture is registered with.
 * @param resolution Edge length in texels; 512 gives ~8 texels per 10 cm at the default period.
 * @param world_period World-space size of one tile of the texture, in metres.
 * @param jobs       Job engine the per-row noise evaluation is spread over; null bakes serially.
 *                   The field is a pure function of the texel, so rows are independent and the
 *                   result is identical either way. Worth passing: the serial bake is ~1.3 M
 *                   noise evaluations on whatever thread calls this, which is a visible stall on
 *                   the first frame a terrain exists.
 * @return A handle to the baked texture, valid for the lifetime of the asset manager.
 */
coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> bake_terrain_detail_texture(
    coopa::gfx::core::Device& device,
    coopa::gfx::memory::Allocator& allocator,
    coopa::gfx::command::CommandPool& cmd_pool,
    coopa::asset::AssetManager& assets,
    uint32_t resolution = 512,
    float world_period = 16.0f,
    coopa::job::JobEngine* jobs = nullptr);

/** @brief The synthetic asset id bake_terrain_detail_texture() registers its texture under. */
const char* terrain_detail_texture_id();

} // namespace world
} // namespace toy
