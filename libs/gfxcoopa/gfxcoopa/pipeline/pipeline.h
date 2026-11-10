/**
 * @file pipeline.h
 * @brief Graphics pipeline creation with OpenGL-like defaults and dynamic state.
 *
 * PipelineConfig provides a builder-style struct for controlling all rasterization
 * state. Pipeline wraps VkPipeline and VkPipelineLayout. set_viewport() and
 * set_scissor() static helpers make dynamic state changes as terse as their
 * OpenGL equivalents.
 */

#ifndef COOPA_GFX_PIPELINE_PIPELINE_H
#define COOPA_GFX_PIPELINE_PIPELINE_H

#include <volk/volk.h>
#include <algorithm>
#include <vector>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/vertex_layout.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @enum BlendMode
 * @brief The blend equation a pipeline's color attachments use.
 */
enum class BlendMode {
    None,                ///< Blending disabled; the source fragment replaces the destination.
    Alpha,                ///< Straight alpha: src*srcA + dst*(1-srcA). Destination alpha is REPLACED by the source's.
    AlphaOver,            ///< Straight alpha like Alpha, but destination alpha ACCUMULATES coverage: dstA*(1-srcA) + srcA. For drawing into a transparent-cleared layer that is itself composited later; identical to Alpha over an opaque destination.
    PremultipliedAlpha,   ///< src*1 + dst*(1-srcA). For premultiplied-alpha source data (e.g. a sprite atlas composited with coverage baked in).
    Additive,             ///< src*srcA + dst*1. Glow/particle-style accumulation.
    Multiply,             ///< src*dst (color), dst unchanged (alpha). Tinting/shadow-style darkening.
    Max,                  ///< max(src, dst) per channel, alpha included; blend factors are ignored. For accumulating a COVERAGE or MASK buffer, where overlapping draws must union rather than sum. The alternatives both misbehave there: Additive scales rgb by the source alpha and then sums, so coverage saturates and any ramp carried in a channel double-counts, while None leaves the result dependent on draw order.
};

/**
 * @struct PipelineConfig
 * @brief Configurable rasterization state for Pipeline construction.
 *
 * Defaults match OpenGL's out-of-the-box behavior: triangle list, filled
 * polygons, back-face culling, CCW winding, depth test on, no blending.
 */
struct PipelineConfig {
    VkPrimitiveTopology    topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; /**< Input primitive topology. */
    VkPolygonMode          polygon_mode           = VK_POLYGON_MODE_FILL;                /**< Fill, line, or point rendering. */
    VkCullModeFlags        cull_mode              = VK_CULL_MODE_BACK_BIT;               /**< Face culling mode. */
    VkFrontFace            front_face             = VK_FRONT_FACE_COUNTER_CLOCKWISE;     /**< CCW winding is front-facing. */
    bool                   depth_test             = true;                                 /**< Enable depth testing. */
    bool                   depth_write            = true;                                 /**< Enable depth writing. */
    BlendMode              blend_mode             = BlendMode::None;                      /**< Blend equation; None disables blending. */
    float                  line_width             = 1.0f;                                 /**< Rasterized line width. */
    VkSampleCountFlagBits  samples                = VK_SAMPLE_COUNT_1_BIT;               /**< MSAA sample count. */
    VkCompareOp             depth_compare_op       = VK_COMPARE_OP_LESS;                  /**< Depth comparison function. */
    uint32_t                color_attachment_count = 1;                                   /**< Color attachments sharing this blend state. */
    uint32_t                patch_control_points   = 0;                                   /**< > 0: a tessellation pipeline (PATCH_LIST of this many control points). */
};

/// @brief A single push constant range: which stages read it, and its
/// byte offset/size within the push constant block. gfxcoopa's sealed
/// replacement for hand-building a VkPushConstantRange.
struct PushConstantRange {
    ShaderStage stages;
    uint32_t    offset;
    uint32_t    size;
};

/// @brief Rasterizer state for the sealed PipelineDesc, using gfxcoopa's
/// sealed Topology/PolygonMode/CullMode/FrontFace instead of Vk enums.
struct RasterState {
    Topology    topology    = Topology::TriangleList;
    PolygonMode polygon     = PolygonMode::Fill;
    CullMode    cull        = CullMode::Back;
    FrontFace   front       = FrontFace::CounterClockwise;
    float       line_width  = 1.0f;
};

/// @brief Depth test state for the sealed PipelineDesc.
struct DepthState {
    bool      test    = true;
    bool      write   = true;
    CompareOp compare = CompareOp::Less;
};

/// @brief Blend state for the sealed PipelineDesc.
struct BlendState {
    BlendMode mode = BlendMode::None;
    /// @brief Number of color attachments sharing this blend state. 0 means
    /// "ask the RenderPass" (via RenderPass::color_attachment_count()) --
    /// the sealed Pipeline constructor never lets this mismatch the render
    /// pass it's built against, which an independently-set count could.
    uint32_t  color_attachment_count = 0;
};

/**
 * @struct PipelineDesc
 * @brief A graphics pipeline's full description, in gfxcoopa's sealed
 * vocabulary -- the single argument to Pipeline's sealed constructor,
 * replacing its 7-positional-argument raw-typed constructor below.
 *
 * @code
 * Pipeline pipeline(device, render_pass, PipelineDesc{
 *     .shaders = {&vert_shader, &frag_shader},
 *     .vertex  = MyVertex::layout(),
 *     .descriptor_layouts = {&set0_layout},
 *     .push_constants = {{ShaderStage::Fragment, 0, sizeof(MyPushConstants)}},
 * });
 * @endcode
 */
struct PipelineDesc {
    std::vector<Shader*>                    shaders;
    VertexLayout                            vertex;
    std::vector<const DescriptorSetLayout*> descriptor_layouts;
    std::vector<PushConstantRange>          push_constants;
    RasterState raster;
    DepthState  depth;
    BlendState  blend;
    /// > 0: a tessellation pipeline whose patches have this many control points (3 for
    /// triangles). `shaders` must then hold a TessControl and a TessEval stage, and the
    /// device must report Device::supports_tessellation(). The topology becomes PatchList.
    uint32_t    patch_control_points = 0;
};

/**
 * @class Pipeline
 * @brief RAII wrapper around a Vulkan graphics pipeline and its layout.
 *
 * Builds the full VkGraphicsPipeline from shaders, vertex input layouts,
 * render pass, and a PipelineConfig. Dynamic viewport and scissor state
 * are always enabled so that window resizes do not require pipeline recreation.
 */
class Pipeline {
public:
    /**
     * @brief Creates a graphics pipeline.
     *
     * @param device             The logical device.
     * @param render_pass        The compatible render pass.
     * @param shaders            Pointers to Shader objects (vertex + fragment required).
     * @param vertex_bindings    VkVertexInputBindingDescription array (per-buffer stride).
     * @param vertex_attributes  VkVertexInputAttributeDescription array (per-attribute layout).
     * @param descriptor_layouts Descriptor set layouts used by the shaders.
     * @param config             Rasterization state overrides (defaults are GL-like).
     * @param push_constants     Push constant ranges (e.g. for per-object model matrices).
     */
    Pipeline(core::Device&                                           device,
             RenderPass&                                             render_pass,
             const std::vector<Shader*>&                            shaders,
             const std::vector<VkVertexInputBindingDescription>&    vertex_bindings,
             const std::vector<VkVertexInputAttributeDescription>&  vertex_attributes,
             const std::vector<VkDescriptorSetLayout>&              descriptor_layouts = {},
             const PipelineConfig&                                  config = {},
             const std::vector<VkPushConstantRange>&                push_constants = {})
        : device_(device)
    {
        create_layout(descriptor_layouts, push_constants);
        create_pipeline(render_pass.handle(), shaders, vertex_bindings, vertex_attributes, config);
    }

    /**
     * @brief Creates a graphics pipeline against a raw render pass handle.
     *
     * For callers (e.g. a pass with a hand-built VkRenderPass) that don't own a RenderPass
     * wrapper.
     *
     * @param device             The logical device.
     * @param render_pass        Handle of a compatible render pass.
     * @param shaders            Pointers to Shader objects (vertex + fragment required).
     * @param vertex_bindings    VkVertexInputBindingDescription array (per-buffer stride).
     * @param vertex_attributes  VkVertexInputAttributeDescription array (per-attribute layout).
     * @param descriptor_layouts Descriptor set layouts used by the shaders.
     * @param config             Rasterization state overrides (defaults are GL-like).
     * @param push_constants     Push constant ranges (e.g. for per-object model matrices).
     */
    Pipeline(core::Device&                                           device,
             VkRenderPass                                            render_pass,
             const std::vector<Shader*>&                            shaders,
             const std::vector<VkVertexInputBindingDescription>&    vertex_bindings,
             const std::vector<VkVertexInputAttributeDescription>&  vertex_attributes,
             const std::vector<VkDescriptorSetLayout>&              descriptor_layouts = {},
             const PipelineConfig&                                  config = {},
             const std::vector<VkPushConstantRange>&                push_constants = {})
        : device_(device)
    {
        create_layout(descriptor_layouts, push_constants);
        create_pipeline(render_pass, shaders, vertex_bindings, vertex_attributes, config);
    }

    /**
     * @brief Creates a graphics pipeline from a single sealed PipelineDesc,
     * instead of seven positional raw-typed arguments.
     *
     * Sample count and (unless PipelineDesc::blend::color_attachment_count
     * is explicitly set) color attachment count are read from `render_pass`
     * automatically -- see RenderPass::samples()'s docs for why a pipeline
     * is never allowed to specify these independently of the render pass
     * it targets.
     *
     * @param device      The logical device.
     * @param render_pass The compatible render pass.
     * @param desc        The pipeline's full description.
     */
    Pipeline(core::Device& device, const RenderPass& render_pass, const PipelineDesc& desc)
        : device_(device)
    {
        TranslatedDesc t = translate(desc);
        create_layout(t.layouts, t.push_constants);

        // Sample count and attachment count come from the render pass, which cannot
        // disagree with itself. An explicit blend.color_attachment_count still wins.
        t.config.samples = detail::to_vk(render_pass.samples());
        t.config.color_attachment_count = desc.blend.color_attachment_count != 0
                                             ? desc.blend.color_attachment_count
                                             : render_pass.color_attachment_count();

        create_pipeline(render_pass.handle(), desc.shaders, t.bindings, t.attributes, t.config);
    }

    /**
     * @brief Creates a graphics pipeline against a raw render pass handle
     * wrapped in detail::RawRenderPass, using a sealed PipelineDesc.
     *
     * For gfxcoopa-internal callers only (e.g. a pass with a hand-built
     * multi-render-target VkRenderPass render_pass.h's RenderPass can't yet
     * express) -- see detail::RawRenderPass's docs for why the wrapper
     * exists instead of a bare VkRenderPass parameter here. Since
     * `render_pass.samples()`/`color_attachment_count()` aren't available
     * without a real RenderPass, `desc.blend.color_attachment_count` must
     * be set explicitly (0 falls back to 1), and MSAA is not supported
     * through this overload.
     *
     * @param device      The logical device.
     * @param render_pass Handle of a compatible render pass, wrapped.
     * @param desc        The pipeline's full description.
     */
    Pipeline(core::Device& device, detail::RawRenderPass render_pass, const PipelineDesc& desc)
        : device_(device)
    {
        TranslatedDesc t = translate(desc);
        create_layout(t.layouts, t.push_constants);

        // No RenderPass to interrogate, so the caller must state the attachment count
        // (0 falls back to PipelineConfig's default of 1) and MSAA stays at 1 sample.
        t.config.color_attachment_count = desc.blend.color_attachment_count;

        create_pipeline(render_pass.handle, desc.shaders, t.bindings, t.attributes, t.config);
    }

    /**
     * @brief Destroys the pipeline and its layout.
     */
    ~Pipeline() {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
        if (layout_   != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_.handle(), layout_, nullptr);
    }

    /// @brief Non-copyable.
    Pipeline(const Pipeline&) = delete;
    /// @brief Non-copyable.
    Pipeline& operator=(const Pipeline&) = delete;

    /**
     * @brief Move constructor: transfers ownership of the pipeline and its layout.
     * @param other The Pipeline to move from (left holding null handles).
     */
    Pipeline(Pipeline&& other) noexcept
        : device_(other.device_), pipeline_(other.pipeline_), layout_(other.layout_)
    {
        other.pipeline_ = VK_NULL_HANDLE;
        other.layout_   = VK_NULL_HANDLE;
    }

    /**
     * @brief Returns the underlying VkPipeline handle.
     * @return Raw VkPipeline.
     */
    VkPipeline handle() const { return pipeline_; }

    /**
     * @brief Returns the VkPipelineLayout (needed for vkCmdBindDescriptorSets).
     * @return Raw VkPipelineLayout.
     */
    VkPipelineLayout layout() const { return layout_; }

    // --- Dynamic state helpers (analogous to glViewport / glScissor) ---

    /**
     * @brief Records a dynamic viewport command into the command buffer.
     *
     * @param cmd Command buffer to record into.
     * @param x   Viewport left edge.
     * @param y   Viewport top edge.
     * @param w   Viewport width.
     * @param h   Viewport height.
     */
    static void set_viewport(VkCommandBuffer cmd, float x, float y, float w, float h) {
        VkViewport vp{};
        vp.x        = x;
        vp.y        = y;
        vp.width    = w;
        vp.height   = h;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
    }

    /**
     * @brief Records a dynamic scissor rectangle command into the command buffer.
     *
     * @param cmd Command buffer to record into.
     * @param x   Scissor left edge.
     * @param y   Scissor top edge.
     * @param w   Scissor width.
     * @param h   Scissor height.
     */
    static void set_scissor(VkCommandBuffer cmd,
                            int32_t  x, int32_t  y,
                            uint32_t w, uint32_t h)
    {
        VkRect2D scissor{};
        scissor.offset = {x, y};
        scissor.extent = {w, h};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

private:
    /**
     * @brief A PipelineDesc converted to the Vulkan structs create_layout() and
     * create_pipeline() take, owning the vectors those structs point into.
     */
    struct TranslatedDesc {
        std::vector<VkDescriptorSetLayout>             layouts;
        std::vector<VkPushConstantRange>               push_constants;
        std::vector<VkVertexInputBindingDescription>   bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        PipelineConfig                                 config;
    };

    /**
     * @brief Converts the render-pass-independent half of a PipelineDesc.
     *
     * Everything that depends on the target render pass (sample count, color
     * attachment count) is left at PipelineConfig's default for the calling
     * constructor to fill in -- that is the only thing the two sealed
     * constructors do differently.
     *
     * @param desc The pipeline description to convert.
     * @return The converted descriptor set layouts, push constant ranges,
     *   vertex bindings/attributes, and rasterization config.
     */
    static TranslatedDesc translate(const PipelineDesc& desc) {
        TranslatedDesc t;

        t.layouts.reserve(desc.descriptor_layouts.size());
        for (const DescriptorSetLayout* l : desc.descriptor_layouts) {
            t.layouts.push_back(l->handle());
        }

        t.push_constants.reserve(desc.push_constants.size());
        for (const PushConstantRange& pc : desc.push_constants) {
            VkPushConstantRange r{};
            r.stageFlags = detail::to_vk(pc.stages);
            r.offset     = pc.offset;
            r.size       = pc.size;
            t.push_constants.push_back(r);
        }

        t.bindings.reserve(desc.vertex.bindings.size());
        for (const VertexBinding& b : desc.vertex.bindings) {
            VkVertexInputBindingDescription vb{};
            vb.binding   = b.binding;
            vb.stride    = b.stride;
            vb.inputRate = b.rate == VertexRate::Instance
                              ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            t.bindings.push_back(vb);
        }

        t.attributes.reserve(desc.vertex.attributes.size());
        for (const VertexAttribute& a : desc.vertex.attributes) {
            VkVertexInputAttributeDescription va{};
            va.location = a.location;
            va.binding  = a.binding;
            va.format   = detail::to_vk(a.format);
            va.offset   = a.offset;
            t.attributes.push_back(va);
        }

        t.config.topology         = detail::to_vk(desc.raster.topology);
        t.config.polygon_mode     = detail::to_vk(desc.raster.polygon);
        t.config.cull_mode        = detail::to_vk(desc.raster.cull);
        t.config.front_face       = detail::to_vk(desc.raster.front);
        t.config.line_width       = desc.raster.line_width;
        t.config.depth_test       = desc.depth.test;
        t.config.depth_write      = desc.depth.write;
        t.config.depth_compare_op = detail::to_vk(desc.depth.compare);
        t.config.blend_mode       = desc.blend.mode;
        t.config.patch_control_points = desc.patch_control_points;
        return t;
    }

    /**
     * @brief Creates the VkPipelineLayout from the given descriptor set layouts and push constants.
     * @param descriptor_layouts Layouts to include in the pipeline layout.
     * @param push_constants     Push constant ranges.
     */
    void create_layout(const std::vector<VkDescriptorSetLayout>& descriptor_layouts,
                       const std::vector<VkPushConstantRange>&   push_constants = {}) {
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount         = static_cast<uint32_t>(descriptor_layouts.size());
        layout_info.pSetLayouts            = descriptor_layouts.empty() ? nullptr
                                                                        : descriptor_layouts.data();
        layout_info.pushConstantRangeCount = static_cast<uint32_t>(push_constants.size());
        layout_info.pPushConstantRanges    = push_constants.empty() ? nullptr : push_constants.data();

        GFX_VK_CHECK(vkCreatePipelineLayout(device_.handle(), &layout_info, nullptr, &layout_));
    }

    /**
     * @brief Creates the VkPipeline from all pipeline state.
     * @param render_pass       Handle of a compatible render pass.
     * @param shaders           Shader stages.
     * @param vertex_bindings   Vertex buffer binding descriptions.
     * @param vertex_attributes Vertex attribute descriptions.
     * @param config            Rasterization overrides.
     */
    void create_pipeline(VkRenderPass                                            render_pass,
                         const std::vector<Shader*>&                            shaders,
                         const std::vector<VkVertexInputBindingDescription>&    vertex_bindings,
                         const std::vector<VkVertexInputAttributeDescription>&  vertex_attributes,
                         const PipelineConfig&                                  config)
    {
        // Shader stages.
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.reserve(shaders.size());
        for (auto* s : shaders) {
            stages.push_back(s->stage_info());
        }

        // Vertex input.
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount   = static_cast<uint32_t>(vertex_bindings.size());
        vertex_input.pVertexBindingDescriptions      = vertex_bindings.empty() ? nullptr : vertex_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_attributes.size());
        vertex_input.pVertexAttributeDescriptions    = vertex_attributes.empty() ? nullptr : vertex_attributes.data();

        // Input assembly.
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology               = config.patch_control_points > 0 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                                                                : config.topology;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineTessellationStateCreateInfo tessellation{};
        tessellation.sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
        tessellation.patchControlPoints = config.patch_control_points;

        // Dynamic viewport/scissor — always enabled so resize is handled without rebuild.
        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates    = dynamic_states;

        // Viewport state (actual values set dynamically).
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;

        // Rasterizer.
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable        = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode             = config.polygon_mode;
        rasterizer.cullMode                = config.cull_mode;
        rasterizer.frontFace               = config.front_face;
        rasterizer.depthBiasEnable         = VK_FALSE;
        rasterizer.lineWidth               = config.line_width;

        // Multisampling.
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = config.samples;
        multisampling.sampleShadingEnable  = VK_FALSE;

        // Depth/stencil.
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable  = config.depth_test  ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = config.depth_write ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp   = config.depth_compare_op;
        depth_stencil.stencilTestEnable= VK_FALSE;

        // Color blending. The same attachment state is replicated across
        // config.color_attachment_count attachments (defaults to 1).
        VkPipelineColorBlendAttachmentState blend_attachment{};
        switch (config.blend_mode) {
        case BlendMode::Alpha:
            blend_attachment.blendEnable         = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
            break;
        case BlendMode::AlphaOver:
            // Same colour equation as Alpha above -- the only difference is dstAlpha. Alpha's
            // VK_BLEND_FACTOR_ZERO leaves the attachment's alpha equal to the LAST fragment's,
            // which is meaningless as a coverage mask: an opaque glyph drawn under a translucent
            // panel would end up stamping the panel's alpha over it. ONE_MINUS_SRC_ALPHA makes
            // alpha accumulate the same way colour does, so the layer can be composited with a
            // premultiplied "over" afterwards. Over an OPAQUE destination the two are identical
            // (1*srcA + 1*(1-srcA) == 1), which is why switching an existing pass to this mode
            // is a no-op unless it draws into a transparent-cleared target.
            blend_attachment.blendEnable         = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
            break;
        case BlendMode::PremultipliedAlpha:
            blend_attachment.blendEnable         = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Additive:
            blend_attachment.blendEnable         = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Max:
            // Factors are not consulted by VK_BLEND_OP_MAX, but leave them at ONE rather than
            // uninitialized: a later edit switching this case's op back to ADD would otherwise
            // silently inherit whatever was here.
            blend_attachment.blendEnable         = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.colorBlendOp        = VK_BLEND_OP_MAX;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.alphaBlendOp        = VK_BLEND_OP_MAX;
            break;
        case BlendMode::Multiply:
            blend_attachment.blendEnable         = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
            break;
        case BlendMode::None:
        default:
            blend_attachment.blendEnable = VK_FALSE;
            break;
        }
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(
            std::max(1u, config.color_attachment_count), blend_attachment);

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.logicOpEnable   = VK_FALSE;
        color_blending.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
        color_blending.pAttachments    = blend_attachments.data();

        // Final pipeline create info.
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount          = static_cast<uint32_t>(stages.size());
        pipeline_info.pStages             = stages.data();
        pipeline_info.pVertexInputState   = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pTessellationState  = config.patch_control_points > 0 ? &tessellation : nullptr;
        pipeline_info.pViewportState      = &viewport_state;
        pipeline_info.pRasterizationState = &rasterizer;
        pipeline_info.pMultisampleState   = &multisampling;
        pipeline_info.pDepthStencilState  = &depth_stencil;
        pipeline_info.pColorBlendState    = &color_blending;
        pipeline_info.pDynamicState       = &dynamic_state;
        pipeline_info.layout              = layout_;
        pipeline_info.renderPass          = render_pass;
        pipeline_info.subpass             = 0;

        GFX_VK_CHECK(vkCreateGraphicsPipelines(device_.handle(), VK_NULL_HANDLE, 1,
                                               &pipeline_info, nullptr, &pipeline_));
    }

    core::Device&    device_;                      /**< Owning logical device (not owned). */
    VkPipeline       pipeline_ = VK_NULL_HANDLE;   /**< The Vulkan graphics pipeline. */
    VkPipelineLayout layout_   = VK_NULL_HANDLE;   /**< The pipeline layout (descriptor bindings + push constants). */
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_PIPELINE_H
