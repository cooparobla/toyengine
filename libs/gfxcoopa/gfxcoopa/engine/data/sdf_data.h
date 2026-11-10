/**
 * @file sdf_data.h
 * @brief Per-frame-in-flight GPU buffers describing every SdfRenderer/SdfShape
 *        in the scene.
 *
 * Shapes and renderers live in storage buffers (SSBOs, via memory::Buffer::storage())
 * rather than a fixed-size UBO array, since their counts are open-ended. And because a
 * raymarching pass is recorded into the SAME overlapping command buffer InstanceStream
 * already has to defend against (see toyengine/render/instance_stream.h's file doc), the
 * buffers are duplicated per frame-in-flight slot, not shared.
 *
 * An SDF renderer's clip_rect and shape range feed a scissor rect and a loop bound that must
 * exactly match what CPU-side record_*() calls compute for THIS frame's draws -- reusing one
 * buffer across overlapping frames would let frame N's upload race frame N-1's still-in-flight
 * raymarch reading stale indices. Per-slot buffers plus binding one descriptor set per slot
 * ONCE at construction (see set()) keeps every write going to a slot the GPU is provably not
 * reading, with no per-frame device_.wait_idle() -- the same policy toyengine's InstanceStream
 * follows, and that ToyRenderPipeline applies to its camera and light uniforms by owning one
 * buffer plus one descriptor set per frame-in-flight slot. The CameraUBO/LightData *classes*
 * themselves are single-buffered: per-slot-ness is a property of a consumer's frame-overlap
 * model, not of the type -- GiSystem's own capture camera (gfxcoopa/engine/gi/gi_system.h)
 * submit-and-waits per cube face and genuinely wants one buffer.
 */

#ifndef GFXCOOPA_ENGINE_DATA_SDF_DATA_H
#define GFXCOOPA_ENGINE_DATA_SDF_DATA_H

#include <glm/glm.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/presentation/renderer.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/// Shape type encoding -- matches SdfShapeType (sdf_shape.h) and
/// gfx/sdf.glsl's GFX_SDF_TYPE_* defines.
enum class SdfGpuShapeType : uint32_t { Sphere = 0, Box = 1, Plane = 2 };

/// Boolean-op encoding -- matches SdfOperation (sdf_shape.h) and
/// gfx/sdf.glsl's GFX_SDF_OP_* defines.
enum class SdfGpuOp : uint32_t { Union = 0, Subtract = 1, Intersect = 2 };

/**
 * @struct SdfShapeGPU
 * @brief std140-aligned single SDF primitive, in gfx/sdf.glsl's vocabulary.
 */
struct alignas(16) SdfShapeGPU {
    glm::mat4 inv_world = glm::mat4(1.0f); /**< World -> shape local space. */
    glm::vec4 params_round = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f); /**< xyz = SdfShape::params, w = rounding. */
    glm::vec4 type_op_blend = glm::vec4(0.0f); /**< x = SdfGpuShapeType, y = SdfGpuOp, z = blend k,
                                                w = approximate uniform scale of the shape's world
                                                matrix (see ToyRenderPipeline's fill site), used
                                                to rescale the local-space distance back to world
                                                units -- true SDFs don't support non-uniform scale
                                                exactly, so this is the same approximation every
                                                raymarching engine makes. */
};

/**
 * @struct SdfRendererGPU
 * @brief std140-aligned single SdfRenderer draw record: its screen-space
 * rectangle, world bounds, material, and the range of SdfShapeGPU entries
 * that belong to it.
 */
struct alignas(16) SdfRendererGPU {
    glm::vec4  clip_rect  = glm::vec4(-1.0f, -1.0f, 1.0f, 1.0f); /**< xy = clip-space min, zw = clip-space max. */
    glm::vec4  bounds_min = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f); /**< World AABB min (xyz); w unused. */
    glm::vec4  bounds_max = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);    /**< World AABB max (xyz); w unused. */
    glm::vec4  albedo_alpha = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);  /**< rgb = albedo, a = alpha. */
    glm::vec4  mr_ao_cutoff = glm::vec4(0.0f, 0.5f, 1.0f, 0.0f);  /**< x = metallic, y = roughness, z = ao, w = alpha_cutoff. */
    glm::vec4  emissive     = glm::vec4(0.0f);                    /**< xyz = pre-multiplied emissive radiance,
                                                                       w = PBRMaterial::gpu_toon_code(), which
                                                                       sdf_gbuffer.frag forwards into the G-buffer's
                                                                       emissive alpha exactly as the mesh backbone
                                                                       does (0 for a plain PBR material). */
    glm::uvec4 range        = glm::uvec4(0, 0, 64, 0);            /**< x = first shape index, y = shape count, z = max_steps, w unused. */
    glm::vec4  march        = glm::vec4(0.001f, 0.002f, 0.0f, 0.0f); /**< x = surface_epsilon, y = normal_epsilon, zw unused. */
};

/**
 * @struct SdfGlobals
 * @brief std140-aligned per-frame globals -- camera ray reconstruction plus
 * the lighting/indirect/SSR tuning the forward SDF pass needs.
 *
 * Lives in a UBO rather than piggybacking on CameraUBO/LightData: those are
 * shared by every pass in the pipeline and adding SDF-only fields to them
 * would force every OTHER consumer's shader to declare bindings it never
 * reads.
 */
struct alignas(16) SdfGlobals {
    glm::mat4 inv_view_proj = glm::mat4(1.0f); /**< Clip -> world; drives gfx_sdf_ray_from_clip().
                                                NDC.xy itself comes from an interpolated per-vertex
                                                varying (see toyengine's sdf_quad.vert), not
                                                gl_FragCoord, so no viewport-size field is needed
                                                here to reconstruct it. */
    glm::vec4 camera_pos    = glm::vec4(0.0f);
    // Forward (BLEND) pass lighting/indirect/SSR tuning -- byte-for-byte the same fields
    // toyengine's ForwardGlobals carries (forward_globals.h, the mesh forward path's own
    // analogous UBO), carried in a UBO rather than a push constant because the SDF forward
    // shader's per-object range/march fields (read from the renderer SSBO, not pushed)
    // already leave no push-constant room for this block too -- see this file's doc and
    // the plan's "why not push constants" note.
    glm::vec4 lighting0 = glm::vec4(4.0f, 0.55f, 0.0f, 0.0f);  /**< x=light_bands, y=spec_threshold, z=soft_lighting, w=rim_strength. */
    glm::vec4 lighting1 = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);   /**< x=ambient_intensity, y=sky_intensity, z=ssr_enabled, w=ssgi_intensity. */
    glm::vec4 ssr0 = glm::vec4(0.5f, 15.0f, 3.5f, 0.05f);      /**< x=ssgi_distance, y=ssr_max_distance, z=ssr_bias_texels, w=ssr_thickness_min. */
    glm::vec4 ssr1 = glm::vec4(0.01f, 1.0f, 0.0f, 0.0f);       /**< x=ssr_thickness_scale, y=ssr_roughness_cutoff, zw unused. */
    glm::ivec4 ssr_steps = glm::ivec4(64, 0, 0, 1);            /**< x=ssr_max_iterations, y=ssr_max_hiz_mip, z=ssr_start_mip, w=ssr_min_mip0_steps. */
    glm::ivec4 ssr_mip   = glm::ivec4(0, 0, 0, 0);             /**< x=ssr_max_color_mip, yzw unused. */
};

/**
 * @class SdfData
 * @brief Owns, per frame-in-flight slot, the SdfGlobals UBO + renderer/shape
 * SSBOs and their bound-once descriptor set.
 *
 * Usage mirrors InstanceStream: call begin(frame_index) once per frame,
 * add_renderer()/add_shape() for every SdfRenderer/SdfShape gathered that
 * frame (renderers and their shapes must be added in matching order -- see
 * add_renderer()'s `first_shape`/`shape_count` contract), then upload().
 * set(frame_index) returns the descriptor set to bind for that frame's draws.
 */
class SdfData {
public:
    /// @brief References gfxcoopa's own constant directly (see
    /// InstanceStream's identical reasoning) rather than a hardcoded
    /// duplicate that could drift out of sync with it.
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    /**
     * @brief Creates the per-slot buffers, descriptor layout/pool/sets.
     * @param device         Logical device.
     * @param allocator      VMA allocator.
     * @param max_renderers  SSBO capacity for SdfRendererGPU records (startup-fixed).
     * @param max_shapes     SSBO capacity for SdfShapeGPU records (startup-fixed).
     */
    SdfData(core::Device& device, memory::Allocator& allocator,
           uint32_t max_renderers = 64, uint32_t max_shapes = 512)
        : max_renderers_(max_renderers), max_shapes_(max_shapes)
    {
        globals_buffers_.reserve(kFrames);
        renderer_buffers_.reserve(kFrames);
        shape_buffers_.reserve(kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            globals_buffers_.push_back(memory::Buffer::uniform(device, allocator, sizeof(SdfGlobals)));
            renderer_buffers_.push_back(memory::Buffer::storage(device, allocator,
                sizeof(SdfRendererGPU) * max_renderers_));
            shape_buffers_.push_back(memory::Buffer::storage(device, allocator,
                sizeof(SdfShapeGPU) * max_shapes_));
        }

        layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, ShaderStage::Vertex | ShaderStage::Fragment)
                .storage_buffer(1, ShaderStage::Vertex | ShaderStage::Fragment)
                .storage_buffer(2, ShaderStage::Vertex | ShaderStage::Fragment)
                .build(device));

        pool_ = std::make_unique<pipeline::DescriptorPool>(
            pipeline::DescriptorPoolBuilder().add_sets(*layout_, kFrames).build(device));

        sets_.reserve(kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            sets_.push_back(std::make_unique<pipeline::DescriptorSet>(device, *pool_, *layout_));
            sets_[i]->bind_buffer(0, globals_buffers_[i]);
            sets_[i]->bind_storage_buffer(1, renderer_buffers_[i]);
            sets_[i]->bind_storage_buffer(2, shape_buffers_[i]);
        }
    }

    SdfData(const SdfData&) = delete;
    SdfData& operator=(const SdfData&) = delete;

    /** @brief Drops last frame's pending renderers/shapes. Call once per frame before any add(). */
    void begin(uint32_t frame_index) {
        frame_index_ = frame_index;
        pending_renderers_.clear();
        pending_shapes_.clear();
    }

    /** @brief Mutable access to this frame's globals -- fill then upload(). */
    SdfGlobals& globals() { return globals_; }

    /** @brief Appends one shape record. @return Its index within this frame's shape list. */
    uint32_t add_shape(const SdfShapeGPU& shape) {
        if (pending_shapes_.size() >= max_shapes_) {
            std::cerr << "[gfxcoopa] SdfData shape capacity (" << max_shapes_
                     << ") exceeded; dropping shape.\n";
            return static_cast<uint32_t>(pending_shapes_.size() > 0 ? pending_shapes_.size() - 1 : 0);
        }
        uint32_t idx = static_cast<uint32_t>(pending_shapes_.size());
        pending_shapes_.push_back(shape);
        return idx;
    }

    /**
     * @brief Appends one renderer record.
     *
     * Caller sets `record.range` = {first_shape, shape_count, max_steps, 0}
     * from the indices add_shape() returned for this renderer's own shapes
     * BEFORE calling this -- shapes for a given renderer must be added
     * contiguously, immediately before that renderer's own add_renderer()
     * call, so `range` describes a contiguous run.
     *
     * @return Its index within this frame's renderer list.
     */
    uint32_t add_renderer(const SdfRendererGPU& record) {
        if (pending_renderers_.size() >= max_renderers_) {
            std::cerr << "[gfxcoopa] SdfData renderer capacity (" << max_renderers_
                     << ") exceeded; dropping renderer.\n";
            return static_cast<uint32_t>(pending_renderers_.size() > 0 ? pending_renderers_.size() - 1 : 0);
        }
        uint32_t idx = static_cast<uint32_t>(pending_renderers_.size());
        pending_renderers_.push_back(record);
        return idx;
    }

    /** @brief Number of renderers added so far this frame. */
    uint32_t renderer_count() const { return static_cast<uint32_t>(pending_renderers_.size()); }

    /** @brief Uploads this frame's globals + renderer + shape records to the current frame slot. */
    void upload() {
        globals_buffers_[frame_index_].upload(&globals_, sizeof(SdfGlobals));
        if (!pending_renderers_.empty()) {
            renderer_buffers_[frame_index_].upload(pending_renderers_.data(),
                sizeof(SdfRendererGPU) * pending_renderers_.size());
        }
        if (!pending_shapes_.empty()) {
            shape_buffers_[frame_index_].upload(pending_shapes_.data(),
                sizeof(SdfShapeGPU) * pending_shapes_.size());
        }
    }

    /** @brief The descriptor set bound once at construction for the given frame slot. */
    pipeline::DescriptorSet& set(uint32_t frame_index) const { return *sets_[frame_index]; }

    /** @brief set() for the slot most recently begin()'d -- the common case at every SDF draw call site. */
    pipeline::DescriptorSet& current_set() const { return *sets_[frame_index_]; }

    /** @brief The layout every set() shares -- for building a pipeline's descriptor_layouts list. */
    const pipeline::DescriptorSetLayout& layout() const { return *layout_; }

private:
    uint32_t max_renderers_;
    uint32_t max_shapes_;
    uint32_t frame_index_ = 0;

    SdfGlobals globals_;

    std::vector<memory::Buffer> globals_buffers_;
    std::vector<memory::Buffer> renderer_buffers_;
    std::vector<memory::Buffer> shape_buffers_;

    std::unique_ptr<pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<pipeline::DescriptorPool>      pool_;
    std::vector<std::unique_ptr<pipeline::DescriptorSet>> sets_;

    std::vector<SdfRendererGPU> pending_renderers_;
    std::vector<SdfShapeGPU>    pending_shapes_;
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_DATA_SDF_DATA_H
