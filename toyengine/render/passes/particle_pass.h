/**
 * @file particle_pass.h
 * @brief Draws particle quads (toy::render::ParticleDrawBatch) as instanced, camera-expanded
 *        sprites inside the forward transparent pass.
 *
 * Built against TransparentPass's own render pass (HDR colour loaded in place, G-buffer depth
 * attached read-only) and drawn INSIDE its begin()/end() bracket: PixelRenderPipeline::
 * record_transparent_() merges particle batches into the same back-to-front list as BLEND
 * meshes and SDFs, switching pipelines per item, so a fire behind a window, or steam over
 * water, composites in the right order -- the same reason SdfForwardPass shares that pass.
 *
 * - **One pipeline, every blend.** Premultiplied-alpha blending (src + dst * (1 - srcA)) with
 *   the fragment writing `rgb * a` and an alpha scaled by (1 - additive) covers alpha blending
 *   (additive 0), pure additive glow (additive 1, alpha 0) and everything between -- the
 *   classic trick that lets fire (mostly additive) and smoke (alpha) share a pipeline and sort
 *   together.
 * - **No vertex buffer.** Each instance is five vec4s at instance rate (ParticleInstance);
 *   particle.vert builds the quad's six corners from gl_VertexIndex, oriented per render mode.
 * - **Descriptor sets** (bound by the caller after bind()): 0 camera UBO, 1 lights, 2 the Hi-Z
 *   pyramid (mip 0 is a copy of the opaque depth -- soft particles read it, never the depth
 *   attachment itself, which is attached to this very render pass), 3 the material set
 *   (albedo slot = the sprite texture; white when untextured), 4 the shadow maps (sun cascade
 *   atlas and local-light atlas -- lit particles receive shadows).
 * - **TAA reactive mask** (optional, build_reactive()): a second pipeline over the same shaders
 *   and sets, into an R8 colour-only target, additive (UNORM clamps the sum at 1), no depth
 *   attachment -- the fragment depth-tests itself against the Hi-Z copy. Each `reactive` batch is
 *   drawn again into it (PushConstants::extra.x = 1), and taa.frag trusts the current frame where
 *   it is set, so fast thin particles never smear into streaks.
 * - **Per-frame-in-flight instance buffer**, grown by doubling and refilled whole each frame,
 *   for the same reason as DebugLinePass's: the pipeline never waits per frame.
 */

#ifndef TOYENGINE_RENDER_PASSES_PARTICLE_PASS_H
#define TOYENGINE_RENDER_PASSES_PARTICLE_PASS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/vertex_layout.h>

#include <toyengine/render/particle_types.h>

namespace toy {
namespace render {
namespace passes {

class ParticlePass {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    /** @brief Matches particle.vert / particle.frag's push block (112 bytes, under the 128 floor). */
    struct PushConstants {
        glm::vec4 mode{0.0f};     ///< x render mode, y sprite, z flipbook cols, w flipbook rows
        glm::vec4 shading{0.0f};  ///< x lit, y toon bands, z emissive, w additive
        glm::vec4 shape{0.0f};    ///< x softness, y soft-particle distance, z camera fade, w aspect
        glm::vec4 stretch{0.0f};  ///< x stretch speed, y stretch length, z distortion, w time (s)
        glm::vec4 misc{0.0f};     ///< x opacity, y has texture, z pivot (sizes), w is perspective
        glm::vec4 depth{0.0f};    ///< x near, y far, zw 1 / render extent
        glm::vec4 ambient{0.0f};  ///< x ambient scale (sky intensity), y receive shadows
        glm::vec4 extra{0.0f};    ///< x pass (0 colour, 1 reactive mask), y reactive, z scatter, w scatter anisotropy
    };
    static_assert(sizeof(PushConstants) <= 128, "particle push block must fit Vulkan's guaranteed 128 bytes");

    ParticlePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                 VkRenderPass shared_render_pass,
                 const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                 const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                 const coopa::gfx::pipeline::DescriptorSetLayout& hiz_layout,
                 const coopa::gfx::pipeline::DescriptorSetLayout& material_layout,
                 const coopa::gfx::pipeline::DescriptorSetLayout& shadow_layout,
                 const std::string& vert_spv, const std::string& frag_spv,
                 uint32_t initial_capacity = 1024)
        : device_(device), allocator_(allocator)
    {
        using namespace coopa::gfx;
        vert_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
        frag_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

        pipeline::PipelineDesc desc;
        desc.shaders = {vert_.get(), frag_.get()};
        desc.vertex = VertexLayout{}
            .binding(0, sizeof(ParticleInstance), VertexRate::Instance)
            .attribute(0, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, pos_size)))
            .attribute(1, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, color)))
            .attribute(2, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, velocity_rot)))
            .attribute(3, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, orient)))
            .attribute(4, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, misc)));
        desc.raster.cull = CullMode::None;      // quads are seen from both sides (aligned / horizontal)
        desc.depth.test = true;                 // occluded by opaque geometry...
        desc.depth.write = false;               // ...but never by each other (they are sorted)
        desc.depth.compare = CompareOp::Less;
        desc.blend.mode = pipeline::BlendMode::PremultipliedAlpha;
        desc.blend.color_attachment_count = 1;
        desc.descriptor_layouts = {&camera_layout, &light_layout, &hiz_layout, &material_layout, &shadow_layout};
        desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        pipeline_ = std::make_unique<pipeline::Pipeline>(device, detail::RawRenderPass{shared_render_pass}, desc);
        desc_ = desc;

        for (uint32_t i = 0; i < kFrames; ++i) {
            capacity_[i] = std::max(1u, initial_capacity);
            buffers_[i] = make_buffer_(capacity_[i]);
        }
    }

    ParticlePass(const ParticlePass&) = delete;
    ParticlePass& operator=(const ParticlePass&) = delete;

    /**
     * @brief Copies every batch's instances, back to back, into `frame_slot`'s buffer. Call once
     *        per frame before recording; first_instance(i) then names batch i's start.
     */
    void upload(uint32_t frame_slot, const std::vector<ParticleDrawBatch>& batches) {
        frame_slot_ = frame_slot;
        firsts_.assign(batches.size(), 0);
        uint32_t total = 0;
        for (size_t i = 0; i < batches.size(); ++i) {
            firsts_[i] = total;
            total += batches[i].count;
        }
        total_ = total;
        if (total == 0) return;
        if (total > capacity_[frame_slot]) {
            uint32_t cap = capacity_[frame_slot];
            while (cap < total) cap *= 2;
            buffers_[frame_slot] = make_buffer_(cap);
            capacity_[frame_slot] = cap;
        }
        for (size_t i = 0; i < batches.size(); ++i) {
            if (batches[i].count == 0) continue;
            buffers_[frame_slot]->upload(batches[i].instances, sizeof(ParticleInstance) * batches[i].count,
                                         sizeof(ParticleInstance) * firsts_[i]);
        }
    }

    uint32_t total_instances() const { return total_; }

    /** @brief Binds the pipeline -- before the caller binds sets 0-4. */
    void bind(coopa::gfx::command::CommandBuffer& cmd) const { cmd.bind_pipeline(*pipeline_); }

    /**
     * @brief Builds the reactive-mask pipeline against `mask_render_pass` (an R8 colour-only
     *        target): the same shaders, layouts and instance buffers; additive; no depth.
     */
    void build_reactive(VkRenderPass mask_render_pass) {
        coopa::gfx::pipeline::PipelineDesc d = desc_;
        d.depth.test = false;
        d.depth.write = false;
        d.blend.mode = coopa::gfx::pipeline::BlendMode::Additive;
        reactive_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device_, coopa::gfx::detail::RawRenderPass{mask_render_pass}, d);
    }
    bool has_reactive() const { return reactive_pipeline_ != nullptr; }
    /** @brief Binds the reactive-mask pipeline -- before the caller binds sets 0-4. */
    void bind_reactive(coopa::gfx::command::CommandBuffer& cmd) const { cmd.bind_pipeline(*reactive_pipeline_); }

    /** @brief One batch: its slice of the instance buffer, its look, six vertices per instance. */
    void draw(coopa::gfx::command::CommandBuffer& cmd, size_t batch, uint32_t count, const PushConstants& pc) const {
        if (count == 0 || batch >= firsts_.size()) return;
        cmd.bind_vertex_buffer(*buffers_[frame_slot_], sizeof(ParticleInstance) * static_cast<VkDeviceSize>(firsts_[batch]), 0);
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
        cmd.draw(6, 0, count);
    }

private:
    std::unique_ptr<coopa::gfx::memory::Buffer> make_buffer_(uint32_t capacity) {
        return std::make_unique<coopa::gfx::memory::Buffer>(
            device_, allocator_, sizeof(ParticleInstance) * static_cast<VkDeviceSize>(std::max(capacity, 1u)),
            coopa::gfx::BufferUsage::Vertex, coopa::gfx::MemoryResidency::CpuToGpu);
    }

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> frag_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> reactive_pipeline_;
    coopa::gfx::pipeline::PipelineDesc desc_;
    std::array<std::unique_ptr<coopa::gfx::memory::Buffer>, kFrames> buffers_;
    std::array<uint32_t, kFrames> capacity_{};
    std::vector<uint32_t> firsts_;
    uint32_t frame_slot_ = 0;
    uint32_t total_ = 0;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_PARTICLE_PASS_H
