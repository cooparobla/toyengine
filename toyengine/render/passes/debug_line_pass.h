/**
 * @file debug_line_pass.h
 * @brief Draws physics collider wireframes and contact normals as a plain LineList overlay,
 *        depth-test off for the un-occluded X-ray gizmo look.
 *
 * A line flagged `occluded` is hidden behind scene geometry instead (the editor's ground grid):
 * post_target_ carries no scene depth, so the G-buffer depth arrives as a sampled texture at
 * set 0 and the fragment shader discards what lies behind it -- the same approach as uicoopa's
 * ui_world_occlude.glsl. X-ray lines draw first, then occluded ones, from one buffer.
 *
 * Built against ToyRenderPipeline's post_target_ render pass and drawn as a guest inside
 * its already-open bracket, right after stylize_pass_ -- NOT the swapchain pass a first
 * instinct might reach for. pipeline::RenderPass hardcodes LOAD_OP_CLEAR, so a genuinely
 * post-upscale overlay would need its own hand-built LOAD_OP_LOAD pass; the more important
 * reason is that nothing in this engine reads the swapchain image back, so a swapchain
 * overlay would be invisible to low_res_color_image()/final_color_image() and therefore to
 * every screenshot and headless capture. Landing pre-upscale costs a little resolution and
 * picks up AA and tilt shift, which for a wireframe gizmo is desirable.
 *
 * Deliberately physxcoopa-free: toy::render::DebugLine is a neutral {a, b, color} segment,
 * not coopa::physx::debug::DebugLine, so the render layer never depends on the physics
 * library. Engine::tick() bridges the two once per frame.
 *
 * Per-frame-in-flight vertex buffers are mandatory rather than stylistic -- this pipeline
 * never waits per frame, so a single shared buffer would race a still-in-flight read.
 */

#ifndef TOYENGINE_RENDER_PASSES_DEBUG_LINE_PASS_H
#define TOYENGINE_RENDER_PASSES_DEBUG_LINE_PASS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/presentation/renderer.h>

#include <toyengine/render/toy_render_math.h>

namespace toy {
namespace render {

/**
 * @struct DebugLine
 * @brief One world-space line segment. `color` must already be packed in vertex-buffer byte
 *        order (low byte = R, matching Format::RGBA8_Unorm) -- see pack_gpu_color() below,
 *        NOT physxcoopa's own 0xRRGGBBAA integer packing, which is big-endian-style and would
 *        read back with red and alpha swapped if uploaded directly.
 */
struct DebugLine {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    uint32_t color = 0xFFFFFFFFu;
    bool occluded = false;   ///< Hidden behind scene geometry (else drawn X-ray, always on top).
};

/**
 * @brief Converts physxcoopa::debug::DebugLine's 0xRRGGBBAA color (most significant byte = R)
 *        into the little-endian vertex-buffer byte order Format::RGBA8_Unorm expects (least
 *        significant byte = R). Pure integer arithmetic, so this is correct regardless of the
 *        host's own endianness -- it constructs the target byte order explicitly rather than
 *        reinterpreting one packing as the other.
 */
uint32_t pack_gpu_color(uint32_t rrggbbaa);

namespace passes {

class DebugLinePass {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    /** @brief Matches VertexLayout below exactly -- RGB32_Sfloat position + RGBA8_Unorm color. */
    struct Vertex {
        glm::vec3 pos;
        uint32_t color;
    };

    /** @brief Matches debug_line.vert/.frag's push block. */
    struct PushConstants {
        glm::mat4 view_proj;
        glm::vec4 params;   ///< x: occlude (0/1), yz: 1 / target size, w: relative depth slack.
    };

    DebugLinePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  coopa::gfx::pipeline::RenderPass& target_pass,
                  const std::string& vert_spv, const std::string& frag_spv,
                  uint32_t initial_capacity_vertices = 4096);

    /**
     * @brief The scene depth occluded lines compare against (bind once; the G-buffer is never
     *        recreated). Until bound, nothing draws occluded lines.
     */
    void set_scene_depth(coopa::gfx::TextureView depth, const coopa::gfx::engine::util::Sampler& nearest,
                         uint32_t width, uint32_t height);

    DebugLinePass(const DebugLinePass&) = delete;
    DebugLinePass& operator=(const DebugLinePass&) = delete;

    /**
     * @brief Uploads this frame's line list into `frame_index`'s buffer slot -- call once per
     *        frame, before Renderer::begin_frame() (matching InstanceStream::begin()+add()+
     *        upload()'s contract), then draw() with the same frame in mind.
     */
    void upload(uint32_t frame_index, const std::vector<DebugLine>& lines);

    /**
     * @brief Draws whatever upload() last recorded for this frame slot, letterboxed. No-op if
     *        upload() saw an empty line list this frame.
     *
     * Negative-height viewport, matching OffscreenTarget::begin()'s own Y-flip convention for
     * Vulkan NDC (VK_KHR_maintenance1) -- required here because, unlike StylizePass (a
     * screen-space fullscreen pass with no real vertex positions, so a viewport sign flip is
     * invisible to it), this pass's vertices come from an actual view_proj transform and a
     * positive-height viewport would show every line mirrored vertically about the target's
     * center. get_projection_matrix() itself has no Y-flip baked in -- see that comment.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, const glm::mat4& view_proj, const LetterboxRect& rect) const;

private:
    /** @brief Doubles (or matches the need, if larger) this frame slot's buffer -- callers
     *         always re-upload the full stream every frame, so growth never needs to preserve
     *         existing contents (same policy as TexturedQuad2DPass::ensure_capacity()). */
    void ensure_capacity_(uint32_t frame_index, uint32_t needed_verts);

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;

    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> depth_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> depth_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> depth_set_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;
    bool depth_bound_ = false;
    glm::vec2 inv_size_{1.0f};
    uint32_t xray_count_ = 0;

    std::array<std::unique_ptr<coopa::gfx::memory::Buffer>, kFrames> vbo_;
    std::array<uint32_t, kFrames> capacity_{};

    std::vector<Vertex> scratch_;
    uint32_t frame_index_ = 0;
    uint32_t vertex_count_ = 0;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_DEBUG_LINE_PASS_H
