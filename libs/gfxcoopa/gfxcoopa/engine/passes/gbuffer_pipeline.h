/**
 * @file gbuffer_pipeline.h
 * @brief Pipeline for rendering geometry into G-Buffer attachments.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_GBUFFER_PIPELINE_H
#define GFXCOOPA_ENGINE_PASSES_GBUFFER_PIPELINE_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/model_ubo.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class GBufferPipeline
 * @brief The pipelines that rasterize opaque and alpha-masked geometry into the
 *        G-buffer's five colour attachments.
 *
 * Holds the stock back-face-culled pipeline, a no-cull sibling, and one variant
 * per named SurfaceShaderDesc registered through add_variant(). bind() selects
 * between them by material shader name.
 */
class GBufferPipeline {
public:
    // Per-batch material state only. model/normal_matrix are streamed
    // per-instance via data::InstanceData instead, because this block is
    // pushed once per instanced draw batch, not per object.
    //
    // 80 bytes total. The first 32 bytes (through alpha_cutoff) are byte-identical to
    // the first 32 of TransparentPass::PushConstants / ProbeCapturePass::PushConstants --
    // emissive is deferred (opaque G-buffer) only, so those forward-path structs don't carry it.
    //
    // gfx_time/gfx_params are the standard trailing "surface" block every surface-shader
    // backbone appends (see gfx/surface/gbuffer_vs.glsl/gbuffer_fs.glsl) -- 32 bytes, fixed
    // across every pass so one surface file's hooks compile unchanged against the G-buffer,
    // shadow, and cube-shadow entry points. Left zero-initialized here (and by every call
    // site that doesn't set them) so a stock material's stock shader is unaffected; a
    // derived shader's record_*() call site fills them from the object's shader_params and
    // the frame's elapsed time.
    //
    // alignas(16): every field here happens to land on a 16-byte boundary already (32/48/64
    // are all multiples of 16), so no interior padding is needed today -- but see
    // ShadowPipeline's DirectionalShadowPushConstants/CubeShadowPushConstants for what
    // happens when that coincidence doesn't hold (glm::vec4 is NOT 16-byte aligned in this
    // build by default, while GLSL's push_constant blocks always align vec4 to 16). Marking
    // the struct alignas(16) up front, per data::CameraData's convention, means a future
    // field addition that breaks the coincidence fails loudly (wrong sizeof/alignment) at
    // the point it's added rather than at pipeline-creation-time validation.
    struct alignas(16) PushConstants {
        glm::vec4 albedo       = {0.8f, 0.8f, 0.8f, 1.0f}; // 16 bytes; .w = alpha
        float     metallic     = 0.0f;
        float     roughness    = 0.5f;
        float     ao           = 1.0f;
        float     alpha_cutoff = 0.0f;                     // 16 bytes; 0.0 = no alpha test
        glm::vec4 emissive     = {0.0f, 0.0f, 0.0f, 0.0f}; // 16 bytes; xyz = pre-multiplied emissive radiance, w reserved
        glm::vec4 gfx_time     = {0.0f, 0.0f, 0.0f, 0.0f}; // 16 bytes; x=time, y=delta_time, z=frame_index, w=spare
        glm::vec4 gfx_params   = {0.0f, 0.0f, 0.0f, 0.0f}; // 16 bytes; four author-defined floats
        // 16 bytes: x = packHalf2x16(tess edge_pixels, tess max_factor), y = packHalf2x16(tess
        // max_distance, displacement_scale) -- both 0 for an untessellated draw; z = surface
        // flags (bit 0: no snow cover); w = the material's packed toon code
        // (PBRMaterial::gpu_toon_code(), 0 for a plain PBR material), which the fragment
        // backbone forwards into the G-buffer's emissive alpha for the deferred lighting pass
        // to unpack. Read by every stage (gfx/surface/tess_common.glsl, gfx/surface/snow.glsl,
        // gfx/surface/gbuffer_fs.glsl).
        glm::uvec4 surface_ext = {0u, 0u, 0u, 0u};
    };
    static_assert(sizeof(PushConstants) <= 128,
                 "GBufferPipeline::PushConstants exceeds Vulkan's guaranteed "
                 "maxPushConstantsSize (128 bytes) -- see the layered-shaders plan's "
                 "push-constant budget table before growing this struct.");

    GBufferPipeline(coopa::gfx::core::Device& device,
                    VkRenderPass render_pass,
                    VkDescriptorSetLayout camera_layout,
                    VkDescriptorSetLayout material_layout,
                    const std::string& vert_spv,
                    const std::string& frag_spv,
                    const std::vector<VkDescriptorSetLayout>& extra_layouts = {})
        : device_(device), render_pass_(render_pass)
    {
        // Descriptor set layouts: camera, material, then any extra sets the host appends (toyengine's
        // surface-world set at 2 -- bound once per pass with this pipeline's layout()).
        std::vector<VkDescriptorSetLayout> layouts = { camera_layout };
        if (material_layout != VK_NULL_HANDLE) {
            layouts.push_back(material_layout);
        }
        layouts.insert(layouts.end(), extra_layouts.begin(), extra_layouts.end());
        // Tessellation stages read the push block too (tess params, gfx_time/gfx_params for the
        // displacement hook) -- only named when the device has them.
        pc_stages_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        if (device_.supports_tessellation()) {
            pc_stages_ |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        }

        // Push constant range -- VERTEX|FRAGMENT: the fragment stage still owns
        // albedo/metallic/.../emissive, but the trailing gfx_time/gfx_params surface block
        // (see PushConstants' doc above) must also reach the vertex stage, since a derived
        // shader's displacement hook (gfx_surface_vertex()) is what reads gfx_params. GLSL
        // requires the same block declared byte-for-byte in both stages when they share one
        // VkPushConstantRange (see gfx/surface/gbuffer_vs.glsl and gbuffer_fs.glsl).
        VkPushConstantRange pc_range{};
        pc_range.stageFlags = pc_stages_;
        pc_range.offset     = 0;
        pc_range.size       = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount         = static_cast<uint32_t>(layouts.size());
        layout_info.pSetLayouts            = layouts.data();
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges    = &pc_range;

        GFX_VK_CHECK(vkCreatePipelineLayout(device_.handle(), &layout_info, nullptr, &pipeline_layout_));

        // Vertex Input -- binding 0 (per-vertex) + binding 1 (per-instance model matrix).
        // Cached as members (not locals) since add_variant() below reuses this exact
        // description for every additional shader -- only the shader modules and cull mode
        // differ between the stock pipeline and a derived shader's variant.
        auto binding          = coopa::gfx::engine::data::Vertex::binding_description();
        auto instance_binding = coopa::gfx::engine::data::InstanceData::binding_description();
        binding_vec_ = {binding, instance_binding};

        auto attrs = coopa::gfx::engine::data::Vertex::attribute_descriptions();
        attr_vec_.assign(attrs.begin(), attrs.end());
        auto instance_attrs = coopa::gfx::engine::data::InstanceData::attribute_descriptions();
        attr_vec_.insert(attr_vec_.end(), instance_attrs.begin(), instance_attrs.end());

        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        pipeline_ = create_pipeline_(*vert_shader_, *frag_shader_, coopa::gfx::CullMode::Back);
        // Second stock pipeline, same shader modules, CullMode::None -- selected per-material at
        // bind time via PBRMaterial::cull_backfaces (see bind()'s doc below). Built eagerly
        // alongside pipeline_ rather than lazily on first use: this is the only pipeline object
        // the engine builds mid-frame (add_variant() is always a startup-time call), so a lazy
        // build would risk creating a VkPipeline while a command buffer is mid-recording.
        pipeline_no_cull_ = create_pipeline_(*vert_shader_, *frag_shader_, coopa::gfx::CullMode::None);
    }

    ~GBufferPipeline() {
        for (auto& [name, v] : variants_) {
            (void)name;
            if (v.pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_.handle(), v.pipeline, nullptr);
            }
            if (v.tess_pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_.handle(), v.tess_pipeline, nullptr);
            }
        }
        for (VkPipeline p : {tess_pipeline_, tess_pipeline_no_cull_}) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_.handle(), p, nullptr);
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
        }
        if (pipeline_no_cull_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.handle(), pipeline_no_cull_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_.handle(), pipeline_layout_, nullptr);
        }
    }

    GBufferPipeline(const GBufferPipeline&) = delete;
    GBufferPipeline& operator=(const GBufferPipeline&) = delete;

    /**
     * @brief Registers a derived shader's own vertex/fragment pair as a named variant,
     *        reusing this pipeline's layout (same descriptor sets, same push-constant
     *        range) -- only the shader modules and rasterization cull mode differ.
     *
     * Called once per SurfaceShaderDesc of SurfaceShaderDomain::Opaque at construction
     * time (see ToyRenderPipeline's ctor), never mid-frame -- an unresolvable or
     * duplicate name is a startup error via the Shader ctor / std::map's behaviour, not a
     * runtime one, matching this codebase's startup-vs-runtime tier policy
     * (render_features.h).
     *
     * @param name     The SurfaceShaderDesc's name; must be non-empty and not already
     *                 registered (asserted via GFX_VK_CHECK-style hard failure is
     *                 unnecessary here -- a duplicate simply overwrites, which can't
     *                 happen because SurfaceShaderRegistry::add() already rejects
     *                 duplicates before this is ever called).
     * @param vert_spv Resolved .spv path for this shader's G-buffer vertex entry point.
     * @param frag_spv Resolved .spv path for this shader's G-buffer fragment entry point.
     * @param cull     Rasterization cull mode override (e.g. CullMode::None for
     *                 two-sided foliage cards).
     */
    void add_variant(const std::string& name, const std::string& vert_spv,
                     const std::string& frag_spv, coopa::gfx::CullMode cull,
                     const std::string& tesc_spv = {}, const std::string& tese_spv = {}) {
        Variant v;
        v.vert_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        v.frag_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        v.pipeline    = create_pipeline_(*v.vert_shader, *v.frag_shader, cull);
        // A tessellated twin: the shared pass-through vertex stage (enable_tessellation()), this
        // variant's control / evaluation stages (its hook runs in the evaluation stage), its
        // own fragment stage.
        if (tess_vert_shader_ && !tese_spv.empty()) {
            v.tesc_shader = std::make_unique<coopa::gfx::pipeline::Shader>(
                device_, tesc_spv.empty() ? tesc_path_ : tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            v.tese_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
            v.tess_pipeline = create_pipeline_(*tess_vert_shader_, *v.frag_shader, cull, v.tesc_shader.get(), v.tese_shader.get());
        }
        variants_.emplace(name, std::move(v));
    }

    /**
     * @brief Builds the stock tessellated pipelines (both cull modes) and records the shared
     *        pass-through vertex / default control stages variants' tessellated twins use.
     *        Call before add_variant(); a no-op on a device without tessellation.
     */
    void enable_tessellation(const std::string& tess_vert_spv, const std::string& tesc_spv,
                             const std::string& tese_spv) {
        if (!device_.supports_tessellation()) return;
        tesc_path_ = tesc_spv;
        tess_vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tess_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        tess_tesc_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
        tess_tese_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        tess_pipeline_ = create_pipeline_(*tess_vert_shader_, *frag_shader_, coopa::gfx::CullMode::Back,
                                          tess_tesc_shader_.get(), tess_tese_shader_.get());
        tess_pipeline_no_cull_ = create_pipeline_(*tess_vert_shader_, *frag_shader_, coopa::gfx::CullMode::None,
                                                  tess_tesc_shader_.get(), tess_tese_shader_.get());
    }

    /** @brief True if `name` (empty = stock) has a tessellated pipeline. */
    bool has_tessellated(const std::string& name) const {
        auto it = variants_.find(name);
        if (it != variants_.end()) return it->second.tess_pipeline != VK_NULL_HANDLE;
        return tess_pipeline_ != VK_NULL_HANDLE;
    }

    /** @brief True if a variant named `name` was registered via add_variant(). */
    bool has_variant(const std::string& name) const {
        return variants_.find(name) != variants_.end();
    }

    /// Binds the stock pipeline (identity displacement/shading, CullMode::Back).
    void bind(coopa::gfx::command::CommandBuffer& cmd) const {
        vkCmdBindPipeline(cmd.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    }

    /**
     * @brief Binds a named variant's pipeline, or one of the two stock pipelines if `name`
     *        is empty or unregistered (see PBRMaterial::shader's doc: empty means "stock").
     *
     * @param cull_backfaces Selects between the stock pipelines when `name` doesn't resolve
     *                       to a variant -- see PBRMaterial::cull_backfaces's doc. Ignored
     *                       (a variant's own SurfaceShaderDesc::cull always wins) when `name`
     *                       does resolve. Defaults to true, matching the stock
     *                       back-face-culled pipeline.
     */
    void bind(coopa::gfx::command::CommandBuffer& cmd, const std::string& name,
             bool cull_backfaces = true, bool tessellated = false) const {
        auto it = variants_.find(name);
        VkPipeline p = VK_NULL_HANDLE;
        if (it != variants_.end()) {
            p = (tessellated && it->second.tess_pipeline != VK_NULL_HANDLE) ? it->second.tess_pipeline : it->second.pipeline;
        } else if (tessellated && tess_pipeline_ != VK_NULL_HANDLE) {
            p = cull_backfaces ? tess_pipeline_ : tess_pipeline_no_cull_;
        } else {
            p = cull_backfaces ? pipeline_ : pipeline_no_cull_;
        }
        vkCmdBindPipeline(cmd.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    }

    void push(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& pc) const {
        cmd.push_constants(pipeline_layout_, pc_stages_, 0, sizeof(PushConstants), &pc);
    }

    VkPipelineLayout layout() const {
        return pipeline_layout_;
    }

private:
    struct Variant {
        std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader;
        std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader;
        std::unique_ptr<coopa::gfx::pipeline::Shader> tesc_shader;
        std::unique_ptr<coopa::gfx::pipeline::Shader> tese_shader;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline tess_pipeline = VK_NULL_HANDLE;   ///< null: no tessellated twin
    };

    /// Builds one VkPipeline against this object's shared pipeline_layout_/render_pass_/
    /// vertex-input description -- the stock pipeline and every add_variant() call route
    /// through here so the only things that can differ between them are the two shader
    /// modules and the cull mode.
    VkPipeline create_pipeline_(const coopa::gfx::pipeline::Shader& vert_shader,
                               const coopa::gfx::pipeline::Shader& frag_shader,
                               coopa::gfx::CullMode cull,
                               const coopa::gfx::pipeline::Shader* tesc_shader = nullptr,
                               const coopa::gfx::pipeline::Shader* tese_shader = nullptr) {
        const bool tess = tesc_shader && tese_shader;
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount   = static_cast<uint32_t>(binding_vec_.size());
        vertex_input.pVertexBindingDescriptions      = binding_vec_.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attr_vec_.size());
        vertex_input.pVertexAttributeDescriptions    = attr_vec_.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology               = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineTessellationStateCreateInfo tessellation{};
        tessellation.sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
        tessellation.patchControlPoints = 3;

        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates    = dynamic_states;

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;

        static const VkCullModeFlagBits kCullLut[] = {
            VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_AND_BACK,
        };
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable        = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode                = kCullLut[static_cast<size_t>(cull)];
        rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable         = VK_FALSE;
        rasterizer.lineWidth               = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling.sampleShadingEnable  = VK_FALSE;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable  = VK_TRUE;
        depth_stencil.depthWriteEnable = VK_TRUE;
        depth_stencil.depthCompareOp   = VK_COMPARE_OP_LESS;
        depth_stencil.stencilTestEnable= VK_FALSE;

        // One per GBufferTarget colour attachment: G0-G3 plus the G4 velocity attachment.
        VkPipelineColorBlendAttachmentState blend_attachments[5]{};
        for (int i = 0; i < 5; ++i) {
            blend_attachments[i].blendEnable = VK_FALSE;
            blend_attachments[i].colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.logicOpEnable   = VK_FALSE;
        color_blending.attachmentCount = 5;
        color_blending.pAttachments    = blend_attachments;

        std::vector<VkPipelineShaderStageCreateInfo> stages = { vert_shader.stage_info() };
        if (tess) {
            stages.push_back(tesc_shader->stage_info());
            stages.push_back(tese_shader->stage_info());
        }
        stages.push_back(frag_shader.stage_info());

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount          = static_cast<uint32_t>(stages.size());
        pipeline_info.pStages             = stages.data();
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pTessellationState  = tess ? &tessellation : nullptr;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &rasterizer;
        pipeline_info.pMultisampleState   = &multisampling;
        pipeline_info.pDepthStencilState  = &depth_stencil;
        pipeline_info.pColorBlendState    = &color_blending;
        pipeline_info.pDynamicState       = &dynamic_state;
        pipeline_info.layout              = pipeline_layout_;
        pipeline_info.renderPass          = render_pass_;
        pipeline_info.subpass             = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        GFX_VK_CHECK(vkCreateGraphicsPipelines(device_.handle(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
        return pipeline;
    }

    coopa::gfx::core::Device& device_;
    VkRenderPass render_pass_;
    std::vector<VkVertexInputBindingDescription>   binding_vec_;
    std::vector<VkVertexInputAttributeDescription> attr_vec_;

    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader_;
    VkPipelineLayout pipeline_layout_   = VK_NULL_HANDLE;
    VkPipeline       pipeline_          = VK_NULL_HANDLE; // stock, CullMode::Back
    VkPipeline       pipeline_no_cull_  = VK_NULL_HANDLE; // stock, CullMode::None -- see bind()
    VkShaderStageFlags pc_stages_ = 0;                    // the push range's stages (tess when supported)

    // Tessellation (enable_tessellation()): the shared pass-through vertex / default control
    // stages and the stock tessellated pipelines.
    std::string tesc_path_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> tess_vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> tess_tesc_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> tess_tese_shader_;
    VkPipeline tess_pipeline_         = VK_NULL_HANDLE;
    VkPipeline tess_pipeline_no_cull_ = VK_NULL_HANDLE;

    std::map<std::string, Variant> variants_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_GBUFFER_PIPELINE_H
