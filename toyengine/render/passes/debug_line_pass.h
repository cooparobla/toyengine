/**
 * @file debug_line_pass.h
 * @brief Draws physics collider wireframes / contact normals as a plain LineList overlay, an
 *        un-occluded X-ray style (depth-test off) -- the standard gizmo look.
 *
 * Built against PixelRenderPipeline's post_target_ render pass and drawn as a guest inside its
 * already-open begin/end bracket, right after pixel_stylize_pass_ (see PixelRenderPipeline::
 * render()) -- NOT the swapchain pass a first instinct might reach for. pipeline::RenderPass
 * hardcodes LOAD_OP_CLEAR, so a genuinely post-upscale overlay would need its own hand-built
 * LOAD_OP_LOAD render pass (as transparent_pass.h does); the more important reason is that
 * nothing in this engine reads the swapchain image back, so a swapchain overlay would be
 * invisible to low_res_color_image()/final_color_image() and therefore to every screenshot/
 * headless-test capture. Landing pre-upscale costs a little resolution and picks up the AA
 * pass and tilt_shift_pass_'s blur, but it's what makes the overlay both correct on real
 * display output and verifiable the same way everything else in this pipeline already is.
 *
 * Note this is where the world-space UI pass USED to live too, for the same reasons -- it does
 * not any more. A wireframe gizmo wants to be anti-aliased and belongs to the image; UI does
 * not, so UI moved to its own layer composited after every post effect (see
 * PixelRenderPipeline::overlay_target_). Debug lines stay here deliberately, which is also why
 * they remain the one overlay visible in a low_res_color_image() capture.
 *
 * Deliberately physxcoopa-free: toy::render::DebugLine is a neutral {a, b, color} segment, not
 * coopa::physx::debug::DebugLine, so the render layer never depends on the physics library.
 * Engine::tick() is what bridges the two, converting PhysicsWorld::debug_draw()'s output into
 * this vector once per frame (see engine.h). Follows the house pass convention documented in
 * toyengine/render/passes/README.md: ctor takes (Device&, RenderPass&, shader paths...), a
 * `draw(cmd, ...)` method, no virtuals. Per-frame-in-flight vertex buffers (modeled on
 * toy::render::InstanceStream) are mandatory here, not stylistic -- this pipeline never
 * vkQueueWaitIdle's per frame, so a single shared buffer would race a still-in-flight read.
 *
 * gfxcoopa's Topology::LineList already exists in pipeline::RasterState with zero other callers
 * in this workspace -- no gfxcoopa change was needed to add this pass (see the ctor's own note
 * on why PolygonMode stays Fill, not Line, despite the name suggesting otherwise).
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

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/vertex_layout.h>

#include <toyengine/render/pixel_math.h>

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
};

/**
 * @brief Converts physxcoopa::debug::DebugLine's 0xRRGGBBAA color (most significant byte = R)
 *        into the little-endian vertex-buffer byte order Format::RGBA8_Unorm expects (least
 *        significant byte = R). Pure integer arithmetic, so this is correct regardless of the
 *        host's own endianness -- it constructs the target byte order explicitly rather than
 *        reinterpreting one packing as the other.
 */
inline uint32_t pack_gpu_color(uint32_t rrggbbaa) {
    uint32_t r = (rrggbbaa >> 24) & 0xFFu;
    uint32_t g = (rrggbbaa >> 16) & 0xFFu;
    uint32_t b = (rrggbbaa >> 8) & 0xFFu;
    uint32_t a = rrggbbaa & 0xFFu;
    return r | (g << 8) | (b << 16) | (a << 24);
}

namespace passes {

class DebugLinePass {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    /** @brief Matches VertexLayout below exactly -- RGB32_Sfloat position + RGBA8_Unorm color. */
    struct Vertex {
        glm::vec3 pos;
        uint32_t color;
    };

    DebugLinePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  coopa::gfx::pipeline::RenderPass& swapchain_pass,
                  const std::string& vert_spv, const std::string& frag_spv,
                  uint32_t initial_capacity_vertices = 4096)
        : device_(device), allocator_(allocator)
    {
        using namespace coopa::gfx;

        vert_shader_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
        frag_shader_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

        pipeline::PipelineDesc desc;
        desc.shaders = {vert_shader_.get(), frag_shader_.get()};
        desc.vertex = VertexLayout{}
            .binding(0, sizeof(Vertex))
            .attribute(0, Format::RGB32_Sfloat, static_cast<uint32_t>(offsetof(Vertex, pos)))
            .attribute(1, Format::RGBA8_Unorm, static_cast<uint32_t>(offsetof(Vertex, color)));
        desc.raster.topology = Topology::LineList;
        // polygon stays the default PolygonMode::Fill deliberately: Line (wireframe) mode
        // rasterizes the EDGES of a filled polygon and requires the fillModeNonSolid device
        // feature, which this app doesn't request -- pipeline creation fails validation
        // outright (VUID-VkPipelineRasterizationStateCreateInfo-polygonMode-01507) if set,
        // regardless of whether the pass ever draws. Neither applies to LineList topology in
        // the first place: there's no polygon to fill or outline, only Fill is meaningful/
        // universally supported, and Vulkan renders LineList primitives as thin lines under it.
        desc.raster.cull = CullMode::None;
        desc.raster.line_width = 1.0f; // >1.0 needs the unenabled wideLines device feature
        desc.depth.test = false;       // post-upscale X-ray overlay -- see file doc
        desc.depth.write = false;
        desc.push_constants = {{ShaderStage::Vertex, 0, sizeof(glm::mat4)}};

        pipeline_ = std::make_unique<pipeline::Pipeline>(device, swapchain_pass, desc);

        for (uint32_t i = 0; i < kFrames; ++i) {
            capacity_[i] = initial_capacity_vertices;
            vbo_[i] = std::make_unique<memory::Buffer>(
                memory::Buffer::vertex(device_, allocator_, capacity_[i] * sizeof(Vertex)));
        }
    }

    DebugLinePass(const DebugLinePass&) = delete;
    DebugLinePass& operator=(const DebugLinePass&) = delete;

    /**
     * @brief Uploads this frame's line list into `frame_index`'s buffer slot -- call once per
     *        frame, before Renderer::begin_frame() (matching InstanceStream::begin()+add()+
     *        upload()'s contract), then draw() with the same frame in mind.
     */
    void upload(uint32_t frame_index, const std::vector<DebugLine>& lines) {
        frame_index_ = frame_index;
        vertex_count_ = static_cast<uint32_t>(lines.size()) * 2;
        if (lines.empty()) return;

        ensure_capacity_(frame_index, vertex_count_);

        scratch_.clear();
        scratch_.reserve(vertex_count_);
        for (const DebugLine& line : lines) {
            scratch_.push_back(Vertex{line.a, line.color});
            scratch_.push_back(Vertex{line.b, line.color});
        }
        vbo_[frame_index]->upload(scratch_.data(), sizeof(Vertex) * scratch_.size());
    }

    /**
     * @brief Draws whatever upload() last recorded for this frame slot, letterboxed. No-op if
     *        upload() saw an empty line list this frame.
     *
     * Negative-height viewport, matching OffscreenTarget::begin()'s own Y-flip convention for
     * Vulkan NDC (VK_KHR_maintenance1) -- required here because, unlike PixelStylizePass (a
     * screen-space fullscreen pass with no real vertex positions, so a viewport sign flip is
     * invisible to it), this pass's vertices come from an actual view_proj transform and a
     * positive-height viewport would show every line mirrored vertically about the target's
     * center. get_projection_matrix() itself has no Y-flip baked in -- see that comment.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, const glm::mat4& view_proj, const LetterboxRect& rect) const {
        if (vertex_count_ == 0) return;
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y + static_cast<int32_t>(rect.h)),
                         static_cast<float>(rect.w), -static_cast<float>(rect.h));
        cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex, view_proj);
        cmd.bind_vertex_buffer(*vbo_[frame_index_]);
        cmd.draw(vertex_count_);
    }

private:
    /** @brief Doubles (or matches the need, if larger) this frame slot's buffer -- callers
     *         always re-upload the full stream every frame, so growth never needs to preserve
     *         existing contents (same policy as TexturedQuad2DPass::ensure_capacity()). */
    void ensure_capacity_(uint32_t frame_index, uint32_t needed_verts) {
        if (needed_verts <= capacity_[frame_index]) return;
        uint32_t new_capacity = std::max(needed_verts, capacity_[frame_index] * 2);
        vbo_[frame_index].reset(); // must be destroyed before the allocator creates the replacement
        vbo_[frame_index] = std::make_unique<coopa::gfx::memory::Buffer>(
            coopa::gfx::memory::Buffer::vertex(device_, allocator_, new_capacity * sizeof(Vertex)));
        capacity_[frame_index] = new_capacity;
    }

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;

    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;

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
