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
 * Built against PixelRenderPipeline's post_target_ render pass and drawn as a guest inside
 * its already-open bracket, right after pixel_stylize_pass_ -- NOT the swapchain pass a first
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

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
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
    bool occluded = false;   ///< Hidden behind scene geometry (else drawn X-ray, always on top).
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

    /** @brief Matches debug_line.vert/.frag's push block. */
    struct PushConstants {
        glm::mat4 view_proj;
        glm::vec4 params;   ///< x: occlude (0/1), yz: 1 / target size, w: relative depth slack.
    };

    DebugLinePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  coopa::gfx::pipeline::RenderPass& target_pass,
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
        desc.depth.test = false;       // X-ray gizmo look; occluded lines test in the shader
        desc.depth.write = false;
        desc.blend.mode = pipeline::BlendMode::Alpha;   // faint lines (grid) fade
        depth_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder().combined_sampler(0, ShaderStage::Fragment).build(device));
        depth_pool_ = std::make_unique<pipeline::DescriptorPool>(
            pipeline::DescriptorPoolBuilder().add_sets(*depth_layout_, 1).build(device));
        depth_set_ = std::make_unique<pipeline::DescriptorSet>(device, *depth_pool_, *depth_layout_);
        desc.descriptor_layouts = {depth_layout_.get()};
        desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(PushConstants)}};

        pipeline_ = std::make_unique<pipeline::Pipeline>(device, target_pass, desc);

        for (uint32_t i = 0; i < kFrames; ++i) {
            capacity_[i] = initial_capacity_vertices;
            vbo_[i] = std::make_unique<memory::Buffer>(
                memory::Buffer::vertex(device_, allocator_, capacity_[i] * sizeof(Vertex)));
        }
    }

    /**
     * @brief The scene depth occluded lines compare against (bind once; the G-buffer is never
     *        recreated). Until bound, nothing draws occluded lines.
     */
    void set_scene_depth(coopa::gfx::TextureView depth, const coopa::gfx::engine::util::Sampler& nearest,
                         uint32_t width, uint32_t height) {
        depth_set_->bind_image(0, depth, nearest);
        depth_bound_ = true;
        inv_size_ = glm::vec2(1.0f / std::max(1u, width), 1.0f / std::max(1u, height));
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
        xray_count_ = 0;
        if (lines.empty()) return;

        ensure_capacity_(frame_index, vertex_count_);

        scratch_.clear();
        scratch_.reserve(vertex_count_);
        for (int pass = 0; pass < 2; ++pass) {
            for (const DebugLine& line : lines) {
                if (line.occluded != (pass == 1)) continue;
                scratch_.push_back(Vertex{line.a, line.color});
                scratch_.push_back(Vertex{line.b, line.color});
            }
            if (pass == 0) xray_count_ = static_cast<uint32_t>(scratch_.size());
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
        cmd.bind_descriptor_set(*depth_set_, 0);
        cmd.bind_vertex_buffer(*vbo_[frame_index_]);
        const auto stages = coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment;
        // Occluded lines first, so X-ray gizmos and wireframes blend on top of the grid.
        const uint32_t occluded = vertex_count_ - xray_count_;
        if (occluded > 0 && depth_bound_) {
            cmd.push_constants(stages, PushConstants{view_proj, glm::vec4(1.0f, inv_size_, 0.003f)});
            cmd.draw(occluded, xray_count_);
        }
        if (xray_count_ > 0) {
            cmd.push_constants(stages, PushConstants{view_proj, glm::vec4(0.0f)});
            cmd.draw(xray_count_, 0);
        }
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
