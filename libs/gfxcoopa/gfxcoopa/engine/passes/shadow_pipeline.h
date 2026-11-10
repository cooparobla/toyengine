/**
 * @file shadow_pipeline.h
 * @brief Vulkan pipelines for directional and cubemap shadow map depth passes.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SHADOW_PIPELINE_H
#define GFXCOOPA_ENGINE_PASSES_SHADOW_PIPELINE_H

#include <volk/volk.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/mesh.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @struct DirectionalShadowPushConstants
 * @brief Push constant block for directional shadow depth pass (112 bytes).
 *
 * model is streamed per-instance via data::InstanceData rather than pushed
 * here. light_space_matrix is shared across the whole pass call. alpha is a
 * per-batch caster opacity slot (1.0 = fully opaque, the default); the
 * shadow fragment shader declares it but need not read it -- toyengine's
 * gfx/surface/shadow_fs.glsl only performs the CUTOUT mask test.
 */
struct alignas(16) DirectionalShadowPushConstants {
    glm::mat4 light_space_matrix;
    float     alpha = 1.0f;

    /// CUTOUT (AlphaMode::Mask) support: 0.0 disables the alpha-mask discard in
    /// the shadow fragment shader entirely (the default, and what OPAQUE/BLEND casters get -- see
    /// PBRMaterial::gpu_alpha_cutoff()); a masked caster's own material.gpu_alpha_cutoff()
    /// otherwise. Only meaningful when this ShadowPipeline was built with a non-null
    /// material_layout (see the ctor) -- the shader has no mask sampler to test against
    /// otherwise, so a caller with no CUTOUT support leaves this at its 0.0 default.
    float     alpha_cutoff = 0.0f;

    /// Explicit std430 padding: glm::vec4 is NOT guaranteed 16-byte aligned in this build
    /// (alignof(glm::vec4) == 4 unless GLM's SIMD/aligned-gentype flags are on), but GLSL's
    /// push_constant blocks always align vec4 to 16 bytes. Without this pad, gfx_time below
    /// would land at C++ offset 72 while the GLSL side puts it at 80 -- exactly the
    /// mismatch Vulkan's validation layer catches as "block range outside push constant
    /// range". Same convention as data::CameraData's _pad0 (see camera_ubo.h).
    uint32_t  tess_a = 0u;   // packHalf2x16(edge_pixels, max_factor); 0 = untessellated (gfx/surface/tess_common.glsl)
    uint32_t  tess_b = 0u;   // packHalf2x16(max_distance, displacement_scale)

    /// Standard trailing "surface" block (see gfx/surface/shadow_vs.glsl) -- 32 bytes,
    /// zero-initialized by default so a stock caster is unaffected. A derived shader's
    /// caller fills these from the same object's shader_params/elapsed time it already
    /// pushes to GBufferPipeline, so displacement stays in sync between the G-buffer and
    /// this shadow pass. This is the tightest-fitting pass: with these fields,
    /// CubeShadowPushConstants below (not this one) lands at exactly 128 bytes, Vulkan's
    /// guaranteed minimum maxPushConstantsSize -- see the layered-shaders plan's budget
    /// table before growing either struct.
    glm::vec4 gfx_time   = {0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 gfx_params = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(DirectionalShadowPushConstants) <= 128,
             "DirectionalShadowPushConstants exceeds Vulkan's guaranteed "
             "maxPushConstantsSize (128 bytes) -- see the layered-shaders plan's "
             "push-constant budget table before growing this struct.");

/**
 * @struct CubeShadowPushConstants
 * @brief Push constant block for point light cubemap shadow pass (128 bytes).
 *
 * Same as DirectionalShadowPushConstants above (model streamed per instance,
 * alpha a per-batch opacity slot), plus the light's position and range.
 */
struct alignas(16) CubeShadowPushConstants {
    glm::mat4 light_space_matrix;
    glm::vec4 light_pos_range; // xyz = light pos, w = range
    float     alpha = 1.0f;

    /// See DirectionalShadowPushConstants::alpha_cutoff -- same CUTOUT meaning, same
    /// 0.0 (disabled) default.
    float     alpha_cutoff = 0.0f;

    /// Explicit std430 padding -- see DirectionalShadowPushConstants::_pad0/_pad1 for why
    /// this is required rather than relying on glm::vec4's C++ alignment.
    uint32_t  tess_a = 0u;   // packHalf2x16(edge_pixels, max_factor); 0 = untessellated (gfx/surface/tess_common.glsl)
    uint32_t  tess_b = 0u;   // packHalf2x16(max_distance, displacement_scale)

    /// See DirectionalShadowPushConstants::gfx_time/gfx_params -- same surface-block
    /// meaning. With mat4 + vec4 + 2 floats + 2 pad floats already at 96 bytes, these
    /// 32 bytes land this struct at exactly 128 -- Vulkan's guaranteed minimum
    /// maxPushConstantsSize. Do not add fields to this struct without shrinking something
    /// else first (the documented growth path is moving light_space_matrix into a small
    /// per-face UBO); a size above 128 is a portability fault, not just a bigger push.
    glm::vec4 gfx_time   = {0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 gfx_params = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(CubeShadowPushConstants) <= 128,
             "CubeShadowPushConstants exceeds Vulkan's guaranteed maxPushConstantsSize "
             "(128 bytes) -- this is the tightest-fitting surface-shader pass, so this is "
             "the first struct to break; see the layered-shaders plan's push-constant "
             "budget table for the documented growth path (shrink light_space_matrix into "
             "a per-face UBO) before adding fields here.");

/**
 * @class ShadowPipeline
 * @brief Manages graphics pipelines for directional & point light shadow map rendering.
 */
class ShadowPipeline {
public:
    ShadowPipeline(core::Device&         device,
                   pipeline::RenderPass& dir_pass,
                   pipeline::RenderPass& cube_pass,
                   const std::string&    dir_vert_spv,
                   const std::string&    dir_frag_spv,
                   const std::string&    cube_vert_spv,
                   const std::string&    cube_frag_spv,
                   const pipeline::DescriptorSetLayout* material_layout = nullptr,
                   std::vector<const pipeline::DescriptorSetLayout*> extra_layouts = {})
        : device_(device), dir_pass_(dir_pass), cube_pass_(cube_pass), extra_layouts_(std::move(extra_layouts))
    {
        pc_stages_ = coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment;
        if (device.supports_tessellation()) {
            pc_stages_ = pc_stages_ | coopa::gfx::ShaderStage::TessControl | coopa::gfx::ShaderStage::TessEval;
        }
        // A tessellated caster reads the whole vertex (the pass-through stage forwards normal /
        // uv / tangent to the evaluation stage, where displacement needs them).
        tess_vertex_layout_ = data::Vertex::layout();
        tess_vertex_layout_.append(data::InstanceData::layout());
        // Shadow depth shaders only consume position (location 0) from the per-vertex stream,
        // plus the per-instance model matrix (locations 4-7) -- a position-only binding-0 layout
        // (built here, not via data::Vertex::layout(), which declares all four of its
        // position/normal/uv/tangent attributes) eliminates validation warnings about unconsumed
        // locations 1/2/3 for normal, uv, and tangent. When material_layout is non-null (CUTOUT
        // support requested), location 2 (uv) is added too, so the shadow fragment shaders
        // can alpha-test against the same mask texture the G-buffer pass uses; location 1
        // (normal) and 3 (tangent) stay unconsumed either way -- depth-only shading needs neither.
        //
        // Cached as members (not locals): add_variant() below builds additional dir/cube
        // pipelines against this exact vertex layout and descriptor set -- a derived
        // shader's shadow entry points read the same attributes and mask sampler as the
        // stock ones, only the shader modules (and hence displacement) differ.
        vertex_layout_.binding(0, sizeof(data::Vertex));
        vertex_layout_.attribute(0, coopa::gfx::Format::RGB32_Sfloat,
                                 static_cast<uint32_t>(offsetof(data::Vertex, position)));
        if (material_layout != nullptr) {
            vertex_layout_.attribute(2, coopa::gfx::Format::RG32_Sfloat,
                                     static_cast<uint32_t>(offsetof(data::Vertex, uv)));
        }
        vertex_layout_.append(data::InstanceData::layout());
        material_layout_ = material_layout;

        // 1. Directional Shadow Pipeline
        dir_vert_ = std::make_unique<pipeline::Shader>(device, dir_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        dir_frag_ = std::make_unique<pipeline::Shader>(device, dir_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        dir_pipeline_        = create_dir_pipeline_(*dir_vert_, *dir_frag_, false);
        dir_pipeline_culled_ = create_dir_pipeline_(*dir_vert_, *dir_frag_, true);

        // 2. Cube Shadow Pipeline
        cube_vert_ = std::make_unique<pipeline::Shader>(device, cube_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        cube_frag_ = std::make_unique<pipeline::Shader>(device, cube_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        cube_pipeline_        = create_cube_pipeline_(*cube_vert_, *cube_frag_, false);
        cube_pipeline_culled_ = create_cube_pipeline_(*cube_vert_, *cube_frag_, true);
    }

    /**
     * @brief Registers a derived shader's directional + cube shadow entry points as a
     *        named variant, reusing this ShadowPipeline's vertex layout and (if CUTOUT
     *        support was requested) material descriptor set layout.
     *
     * Unlike GBufferPipeline::add_variant(), each variant here gets its own
     * pipeline::Pipeline (and hence its own VkPipelineLayout) rather than sharing one --
     * ShadowPipeline was already built on the sealed Pipeline/PipelineDesc API, which owns
     * its layout internally, so duplicating a functionally-identical layout per variant is
     * the lower-friction choice here; the extra VkPipelineLayout objects are negligible
     * next to N pipelines already being created.
     *
     * @param name           The SurfaceShaderDesc's name.
     * @param dir_vert_spv   Resolved .spv path for this shader's directional shadow vertex entry point.
     * @param dir_frag_spv   Resolved .spv path for this shader's directional shadow fragment entry point.
     * @param cube_vert_spv  Resolved .spv path for this shader's cube shadow vertex entry point.
     * @param cube_frag_spv  Resolved .spv path for this shader's cube shadow fragment entry point.
     * @param cull           The shader's own SurfaceShaderDesc::cull. None (a two-sided card,
     *                       say) means the variant never back-face culls its casters either,
     *                       whatever the material asks; Back builds a culled pipeline too.
     */
    /**
     * @brief Builds the stock tessellated shadow pipelines and records the shared pass-through
     *        vertex / control stages variants' tessellated twins use. Call before add_variant();
     *        a no-op on a device without tessellation.
     */
    void enable_tessellation(const std::string& tess_vert_spv,
                             const std::string& dir_tesc_spv, const std::string& dir_tese_spv,
                             const std::string& cube_tesc_spv, const std::string& cube_tese_spv) {
        if (!device_.supports_tessellation()) return;
        tess_vert_ = std::make_unique<pipeline::Shader>(device_, tess_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        dir_tesc_  = std::make_unique<pipeline::Shader>(device_, dir_tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
        cube_tesc_ = std::make_unique<pipeline::Shader>(device_, cube_tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
        dir_tese_  = std::make_unique<pipeline::Shader>(device_, dir_tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        cube_tese_ = std::make_unique<pipeline::Shader>(device_, cube_tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        dir_tess_pipeline_         = create_dir_pipeline_(*tess_vert_, *dir_frag_, false, dir_tesc_.get(), dir_tese_.get());
        dir_tess_pipeline_culled_  = create_dir_pipeline_(*tess_vert_, *dir_frag_, true, dir_tesc_.get(), dir_tese_.get());
        cube_tess_pipeline_        = create_cube_pipeline_(*tess_vert_, *cube_frag_, false, cube_tesc_.get(), cube_tese_.get());
        cube_tess_pipeline_culled_ = create_cube_pipeline_(*tess_vert_, *cube_frag_, true, cube_tesc_.get(), cube_tese_.get());
    }

    void add_variant(const std::string& name,
                     const std::string& dir_vert_spv, const std::string& dir_frag_spv,
                     const std::string& cube_vert_spv, const std::string& cube_frag_spv,
                     coopa::gfx::CullMode cull = coopa::gfx::CullMode::None,
                     const std::string& dir_tese_spv = {}, const std::string& cube_tese_spv = {},
                     const std::string& dir_tesc_spv = {}, const std::string& cube_tesc_spv = {}) {
        Variant v;
        v.dir_vert  = std::make_unique<pipeline::Shader>(device_, dir_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        v.dir_frag  = std::make_unique<pipeline::Shader>(device_, dir_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        v.dir_pipeline = create_dir_pipeline_(*v.dir_vert, *v.dir_frag, false);

        v.cube_vert = std::make_unique<pipeline::Shader>(device_, cube_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        v.cube_frag = std::make_unique<pipeline::Shader>(device_, cube_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        v.cube_pipeline = create_cube_pipeline_(*v.cube_vert, *v.cube_frag, false);

        if (cull != coopa::gfx::CullMode::None) {
            v.dir_pipeline_culled  = create_dir_pipeline_(*v.dir_vert, *v.dir_frag, true);
            v.cube_pipeline_culled = create_cube_pipeline_(*v.cube_vert, *v.cube_frag, true);
        }
        // Tessellated twins: shared pass-through vertex + control stages, this variant's
        // evaluation stages (its displacement hook) and fragment stages. Two-sided only, like
        // the plain variant's default.
        if (tess_vert_ && !dir_tese_spv.empty() && !cube_tese_spv.empty()) {
            v.dir_tese  = std::make_unique<pipeline::Shader>(device_, dir_tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
            v.cube_tese = std::make_unique<pipeline::Shader>(device_, cube_tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
            // A variant may bring its own control stages (edge factors from displaced endpoints).
            if (!dir_tesc_spv.empty()) {
                v.dir_tesc = std::make_unique<pipeline::Shader>(device_, dir_tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            }
            if (!cube_tesc_spv.empty()) {
                v.cube_tesc = std::make_unique<pipeline::Shader>(device_, cube_tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            }
            v.dir_tess_pipeline  = create_dir_pipeline_(*tess_vert_, *v.dir_frag, false,
                                                        v.dir_tesc ? v.dir_tesc.get() : dir_tesc_.get(), v.dir_tese.get());
            v.cube_tess_pipeline = create_cube_pipeline_(*tess_vert_, *v.cube_frag, false,
                                                         v.cube_tesc ? v.cube_tesc.get() : cube_tesc_.get(), v.cube_tese.get());
        }

        variants_.emplace(name, std::move(v));
    }

    /** @brief True if a variant named `name` was registered via add_variant(). */
    bool has_variant(const std::string& name) const {
        return variants_.find(name) != variants_.end();
    }

    void bind_directional(command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*dir_pipeline_);
    }

    /// Binds a named variant's directional pipeline, or the stock one if `name` is empty
    /// or unregistered (see PBRMaterial::shader's doc: empty means "stock").
    ///
    /// `cull_backfaces` (PBRMaterial::cull_backfaces) selects the back-face-culled pipeline
    /// -- a closed caster's back faces are hidden behind its front faces from the light
    /// too, so culling them halves the caster's raster work without changing the map. A
    /// variant registered with CullMode::None ignores it and always draws both sides.
    void bind_directional(command::CommandBuffer& cmd, const std::string& name,
                          bool cull_backfaces = false, bool tessellated = false) const {
        auto it = variants_.find(name);
        if (it != variants_.end()) {
            const Variant& v = it->second;
            if (tessellated && v.dir_tess_pipeline) { cmd.bind_pipeline(*v.dir_tess_pipeline); return; }
            cmd.bind_pipeline((cull_backfaces && v.dir_pipeline_culled) ? *v.dir_pipeline_culled
                                                                         : *v.dir_pipeline);
            return;
        }
        if (tessellated && dir_tess_pipeline_) {
            cmd.bind_pipeline(cull_backfaces ? *dir_tess_pipeline_culled_ : *dir_tess_pipeline_);
            return;
        }
        cmd.bind_pipeline(cull_backfaces ? *dir_pipeline_culled_ : *dir_pipeline_);
    }

    /** @brief True if `name` (empty = stock) has tessellated shadow pipelines. */
    bool has_tessellated(const std::string& name) const {
        auto it = variants_.find(name);
        if (it != variants_.end()) return it->second.dir_tess_pipeline != nullptr;
        return dir_tess_pipeline_ != nullptr;
    }

    void push_directional(command::CommandBuffer& cmd, const DirectionalShadowPushConstants& pc) const {
        cmd.push_constants(pc_stages_, pc);
    }

    void bind_cube(command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*cube_pipeline_);
    }

    /// Binds a named variant's cube pipeline, or the stock one if `name` is empty or
    /// unregistered. `cull_backfaces` as for bind_directional().
    void bind_cube(command::CommandBuffer& cmd, const std::string& name,
                   bool cull_backfaces = false, bool tessellated = false) const {
        auto it = variants_.find(name);
        if (it != variants_.end()) {
            const Variant& v = it->second;
            if (tessellated && v.cube_tess_pipeline) { cmd.bind_pipeline(*v.cube_tess_pipeline); return; }
            cmd.bind_pipeline((cull_backfaces && v.cube_pipeline_culled) ? *v.cube_pipeline_culled
                                                                          : *v.cube_pipeline);
            return;
        }
        if (tessellated && cube_tess_pipeline_) {
            cmd.bind_pipeline(cull_backfaces ? *cube_tess_pipeline_culled_ : *cube_tess_pipeline_);
            return;
        }
        cmd.bind_pipeline(cull_backfaces ? *cube_pipeline_culled_ : *cube_pipeline_);
    }

    void push_cube(command::CommandBuffer& cmd, const CubeShadowPushConstants& pc) const {
        cmd.push_constants(pc_stages_, pc);
    }

    /** @brief The stages the push range covers (Vertex|Fragment, + tessellation when supported). */
    coopa::gfx::ShaderStage push_stages() const { return pc_stages_; }

private:
    struct Variant {
        std::unique_ptr<pipeline::Shader>   dir_vert, dir_frag;
        std::unique_ptr<pipeline::Pipeline> dir_pipeline;
        std::unique_ptr<pipeline::Pipeline> dir_pipeline_culled;    // null: variant is two-sided
        std::unique_ptr<pipeline::Shader>   cube_vert, cube_frag;
        std::unique_ptr<pipeline::Pipeline> cube_pipeline;
        std::unique_ptr<pipeline::Pipeline> cube_pipeline_culled;   // null: variant is two-sided
        std::unique_ptr<pipeline::Shader>   dir_tese, cube_tese;
        std::unique_ptr<pipeline::Shader>   dir_tesc, cube_tesc;    // null: the shared control stages
        std::unique_ptr<pipeline::Pipeline> dir_tess_pipeline;      // null: no tessellated twin
        std::unique_ptr<pipeline::Pipeline> cube_tess_pipeline;
    };

    /// Raster state for a shadow pipeline. Shadow targets render with a POSITIVE-height
    /// viewport and unflipped light matrices (ShadowMapTarget; toy_render_math.h's
    /// compute_dir_shadow_fit_slice explains why), unlike the main passes' negative-height
    /// viewport -- so a triangle that is counter-clockwise (front-facing) in the main view
    /// lands CLOCKWISE in a shadow map's framebuffer. The culled variant therefore names
    /// Clockwise as its front face; with CounterClockwise it would cull the FRONT faces.
    static void set_shadow_raster_(pipeline::PipelineDesc& desc, bool cull_backfaces) {
        desc.raster.cull  = cull_backfaces ? coopa::gfx::CullMode::Back : coopa::gfx::CullMode::None;
        desc.raster.front = coopa::gfx::FrontFace::Clockwise;
    }

    /// The layouts every shadow pipeline shares: the material set (when there is one) and the
    /// host's extra sets after it (toyengine's surface-world set at 1).
    std::vector<const pipeline::DescriptorSetLayout*> layouts_() const {
        std::vector<const pipeline::DescriptorSetLayout*> l;
        if (material_layout_ != nullptr) l.push_back(material_layout_);
        l.insert(l.end(), extra_layouts_.begin(), extra_layouts_.end());
        return l;
    }

    std::unique_ptr<pipeline::Pipeline> create_dir_pipeline_(pipeline::Shader& vert, pipeline::Shader& frag,
                                                             bool cull_backfaces,
                                                             pipeline::Shader* tesc = nullptr, pipeline::Shader* tese = nullptr) {
        pipeline::PipelineDesc desc;
        desc.vertex       = tese ? tess_vertex_layout_ : vertex_layout_;
        set_shadow_raster_(desc, cull_backfaces);
        desc.depth.test   = true;
        desc.depth.write  = true;
        desc.descriptor_layouts = layouts_();
        desc.shaders = {&vert, &frag};
        if (tesc && tese) {
            desc.shaders = {&vert, tesc, tese, &frag};
            desc.patch_control_points = 3;
        }
        desc.push_constants = {{pc_stages_, 0, sizeof(DirectionalShadowPushConstants)}};
        return std::make_unique<pipeline::Pipeline>(device_, dir_pass_, desc);
    }

    std::unique_ptr<pipeline::Pipeline> create_cube_pipeline_(pipeline::Shader& vert, pipeline::Shader& frag,
                                                              bool cull_backfaces,
                                                              pipeline::Shader* tesc = nullptr, pipeline::Shader* tese = nullptr) {
        pipeline::PipelineDesc desc;
        desc.vertex       = tese ? tess_vertex_layout_ : vertex_layout_;
        set_shadow_raster_(desc, cull_backfaces);
        desc.depth.test   = true;
        desc.depth.write  = true;
        desc.descriptor_layouts = layouts_();
        desc.shaders = {&vert, &frag};
        if (tesc && tese) {
            desc.shaders = {&vert, tesc, tese, &frag};
            desc.patch_control_points = 3;
        }
        desc.push_constants = {{pc_stages_, 0, sizeof(CubeShadowPushConstants)}};
        return std::make_unique<pipeline::Pipeline>(device_, cube_pass_, desc);
    }

    core::Device&                       device_;
    pipeline::RenderPass&               dir_pass_;
    pipeline::RenderPass&               cube_pass_;
    coopa::gfx::VertexLayout            vertex_layout_;
    const pipeline::DescriptorSetLayout* material_layout_ = nullptr;
    std::vector<const pipeline::DescriptorSetLayout*> extra_layouts_;
    coopa::gfx::ShaderStage             pc_stages_ = coopa::gfx::ShaderStage::Vertex;
    coopa::gfx::VertexLayout            tess_vertex_layout_;
    std::unique_ptr<pipeline::Shader>   tess_vert_, dir_tesc_, cube_tesc_, dir_tese_, cube_tese_;
    std::unique_ptr<pipeline::Pipeline> dir_tess_pipeline_, dir_tess_pipeline_culled_;
    std::unique_ptr<pipeline::Pipeline> cube_tess_pipeline_, cube_tess_pipeline_culled_;

    std::unique_ptr<pipeline::Shader>   dir_vert_;
    std::unique_ptr<pipeline::Shader>   dir_frag_;
    std::unique_ptr<pipeline::Pipeline> dir_pipeline_;
    std::unique_ptr<pipeline::Pipeline> dir_pipeline_culled_;

    std::unique_ptr<pipeline::Shader>   cube_vert_;
    std::unique_ptr<pipeline::Shader>   cube_frag_;
    std::unique_ptr<pipeline::Pipeline> cube_pipeline_;
    std::unique_ptr<pipeline::Pipeline> cube_pipeline_culled_;

    std::map<std::string, Variant> variants_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_SHADOW_PIPELINE_H
