/**
 * @file motion_blur_pass.h
 * @brief Velocity-buffer motion blur (McGuire-style reconstruction) on the linear HDR scene colour.
 *
 * Three fullscreen stages, then an in-place write-back:
 *  - **TileMax** (`motion_blur_tile_max.frag`): the longest blur vector in each tile x tile block
 *    (tile = 20 px at 1080p, scaled with the render height), into an RG16F target at tile resolution.
 *  - **NeighborMax** (`motion_blur_neighbor_max.frag`): the longest TileMax within `reach` tiles,
 *    reach = ceil(max radius / tile) -- the dominant motion that can smear into the tile.
 *  - **Gather** (`motion_blur_gather.frag`): samples along that velocity with depth-aware foreground/
 *    background weights and per-pixel jitter (see the shader's header).
 *
 * The blur vectors come from the G-buffer's velocity attachment (G4), which measures camera AND
 * object motion between unjittered poses (gfx/surface/gbuffer_fs.glsl); the sky, which writes no
 * velocity, is reprojected through the camera alone (motion_blur_common.glsl). Skinned meshes carry
 * one velocity per object, so limbs blur with the body rather than on their own.
 *
 * **In place, so it can be switched at runtime.** Every pass downstream (DoF, bloom, auto-exposure,
 * stylize) bound its source image once at construction (ToyRenderPipeline's rule 1), so the blur
 * cannot become a new image for them to read. Instead it copies the scene colour into its own image
 * (`source_copy_`), and the gather draws the result back into the scene target itself -- whose render
 * pass clears on load, which the fullscreen draw overwrites. Off records nothing at all: no copy, no
 * draw, the image downstream is exactly the one the scene chain left.
 *
 * Everything (both tile targets, the copy, all three pipelines and descriptor sets) is allocated and
 * bound at construction.
 */

#ifndef TOYENGINE_RENDER_PASSES_MOTION_BLUR_PASS_H
#define TOYENGINE_RENDER_PASSES_MOTION_BLUR_PASS_H

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/pipeline/shader_library.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class MotionBlurPass
 * @brief See the file doc.
 */
class MotionBlurPass {
public:
    /// Tile edge at 1080p; scaled with the render height (McGuire uses the max radius, the
    /// neighbourhood reach below covers radii longer than one tile).
    static constexpr float kTileAt1080 = 20.0f;
    /// Neighbourhood reach ceiling, in tiles (17x17 TileMax reads at tile resolution).
    static constexpr int kMaxReach = 8;

    struct Params {
        /// Last frame's unjittered view-projection times the inverse of this frame's: the sky's motion.
        glm::mat4 sky_reproject{1.0f};
        bool      sky_valid    = false;
        float     shutter      = 0.5f;   ///< Fraction of the frame interval the shutter is open.
        float     max_radius   = 32.0f;  ///< Blur half-length ceiling, in render pixels.
        int       samples      = 16;
        uint32_t  noise_frame  = 0;      ///< Varies the per-pixel jitter frame to frame (0 = fixed).
    };

    MotionBlurPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                   uint32_t width, uint32_t height,
                   coopa::gfx::engine::targets::OffscreenTarget& scene_target,
                   coopa::gfx::TextureView velocity,
                   const coopa::gfx::pipeline::ShaderLibrary& shaders);

    MotionBlurPass(const MotionBlurPass&) = delete;
    MotionBlurPass& operator=(const MotionBlurPass&) = delete;

    uint32_t tile_size() const { return tile_; }

    /** @brief Records all three stages and the write-back into the scene target. */
    void execute(coopa::gfx::command::CommandBuffer& cmd, const Params& params);

private:
    /// Shared by the tile and gather stages (the gather reads info.z/w too).
    struct StagePush {
        glm::mat4  sky_reproject{1.0f};
        glm::vec4  extent_scale{0.0f};
        glm::ivec4 info{0};
    };
    static_assert(sizeof(StagePush) == 96, "StagePush must match the shaders' push blocks");
    struct NeighborPush {
        glm::ivec4 info{0};
    };

    static coopa::gfx::engine::passes::FullscreenStageDesc describe_(const std::string& vert, const std::string& frag,
                                                                     uint32_t images, size_t push_size);

    /// Scene colour -> source_copy_, leaving both SHADER_READ_ONLY for the gather (which reads the
    /// copy and writes the scene target through its clearing render pass).
    void copy_scene_(coopa::gfx::command::CommandBuffer& cmd);

    uint32_t width_, height_;
    uint32_t tile_, tiles_w_, tiles_h_;
    coopa::gfx::engine::targets::OffscreenTarget& scene_target_;
    coopa::gfx::engine::util::Sampler             nearest_;
    coopa::gfx::engine::targets::OffscreenTarget  tile_target_;
    coopa::gfx::engine::targets::OffscreenTarget  neighbor_target_;
    coopa::gfx::memory::Image                     source_copy_;
    coopa::gfx::engine::passes::FullscreenStage   tile_stage_;
    coopa::gfx::engine::passes::FullscreenStage   neighbor_stage_;
    coopa::gfx::engine::passes::FullscreenStage   gather_stage_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_MOTION_BLUR_PASS_H
