/**
 * @file transparent_pass.h
 * @brief Forward BLEND transparent geometry pass.
 *
 * Renders alpha-blended geometry after opaque lighting (and, when SSR is enabled, after
 * the SSR composite), into the live HDR colour image, depth-tested read-only against the
 * G-Buffer's opaque depth. Owns its own raw VkRenderPass rather than using
 * pipeline::RenderPass, because it needs LOAD_OP_LOAD on both attachments (the colour
 * image already holds the finished opaque result; the depth image is the G-Buffer's, not
 * a depth buffer this pass ever writes) -- pipeline::RenderPass hardcodes
 * VK_ATTACHMENT_LOAD_OP_CLEAR and does not support attaching an externally-owned depth
 * image.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_TRANSPARENT_PASS_H
#define GFXCOOPA_ENGINE_PASSES_TRANSPARENT_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/model_ubo.h>
#include <gfxcoopa/engine/passes/extra_sets.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class TransparentPass
 * @brief Alpha-blends BLEND-mode geometry into the live HDR frame, depth-tested
 *        against the G-buffer's opaque depth.
 *
 * Owns a raw VkRenderPass so it can load the existing colour and depth rather
 * than clearing them, and one pipeline variant per named surface shader.
 */
class TransparentPass {
public:
    /// 64 bytes -- the first 32 (through alpha_cutoff) are byte-identical to the first 32
    /// bytes of GBufferPipeline::PushConstants (which is 80 bytes total there: it carries a
    /// trailing `emissive` field this struct does not, since emissive is deferred/opaque-only
    /// -- this forward BLEND path doesn't carry it). The model matrix is streamed per
    /// instance (data::InstanceData), so this block is pushed once per instanced batch,
    /// not per object.
    ///
    /// gfx_time/gfx_params are the standard trailing "surface" block every surface-shader
    /// backbone appends (see gfx/surface/gbuffer_vs.glsl and toyengine's
    /// gfx/surface/transparent_vs.glsl) -- 32 bytes, so a derived transparent shader's
    /// vertex hook (e.g. water's wave displacement) reads the same gfx_time/gfx_params
    /// shape every other surface backbone does. alignas(16): every field here lands on a
    /// 16-byte boundary already (32/48 are multiples of 16), matching GBufferPipeline's
    /// PushConstants -- see that struct's doc for why this is marked explicitly rather than
    /// relied upon.
    struct alignas(16) PushConstants {
        glm::vec4 albedo       = {0.8f, 0.8f, 0.8f, 1.0f}; // 16 bytes; .w = alpha
        float     metallic     = 0.0f;
        float     roughness    = 0.5f;
        float     ao           = 1.0f;
        float     alpha_cutoff = 0.0f;                     // unused; BLEND never alpha-tests
        glm::vec4 gfx_time     = {0.0f, 0.0f, 0.0f, 0.0f}; // x=time, y=delta_time, z=frame_index, w=spare
        glm::vec4 gfx_params   = {0.0f, 0.0f, 0.0f, 0.0f}; // four author-defined floats
    };
    // <= 128 on its own (a caller's extra_pc_bytes adds on top -- see the ctor's own doc --
    // so this alone doesn't guarantee the combined size fits; a caller declaring
    // extra_pc_bytes should static_assert sizeof(PushConstants) + extra_pc_bytes <= 128
    // itself, e.g. toyengine's toy_render_pipeline.h does for TransparentRefractionPushConstants).
    static_assert(sizeof(PushConstants) <= 128,
                 "TransparentPass::PushConstants exceeds Vulkan's guaranteed "
                 "maxPushConstantsSize (128 bytes) -- see the layered-shaders plan's "
                 "push-constant budget table before growing this struct.");

    /**
     * @brief Creates the pass's render pass and pipeline.
     *
     * @param device        Logical device.
     * @param color_format  Format of the HDR colour image this pass will be pointed at
     *                      (offscreen_target_ / the SSR composite target -- both share it).
     * @param camera_layout Set 0.
     * @param light_layout  Set 1.
     * @param shadow_layout Set 2.
     * @param vert_spv      Vertex shader (e.g. toyengine's transparent.vert.spv).
     * @param frag_spv      Fragment shader (e.g. toyengine's transparent.frag.spv).
     * @param extra         Optional trailing sets (e.g. GI), appended after set 2.
     * @param extra_pc_bytes Additional bytes to reserve past PushConstants' own
     *                      [0, sizeof(PushConstants)) -- for a consumer whose transparent.frag
     *                      needs frame-level data (e.g. toyengine's
     *                      soft_lighting/light_bands/spec_threshold toggle) beyond the
     *                      per-object material block this pass owns. Folded into ONE fragment-
     *                      stage VkPushConstantRange, not a second one: the Vulkan spec forbids
     *                      two ranges in the same pipeline layout sharing a stage, even with
     *                      disjoint byte ranges. GLSL likewise permits only one push_constant
     *                      block per stage, so such a consumer's shader declares ONE block
     *                      spanning [0, sizeof(PushConstants) + extra_pc_bytes) and pushes into
     *                      each region independently (this pass's own push() call handles
     *                      [0, sizeof(PushConstants)); the caller issues its own
     *                      cmd.push_constants() at [sizeof(PushConstants), ...) for the rest).
     * @param material_layout Optional material set (see engine::util::MaterialTextureCache),
     *                      appended as the LAST set -- after camera/light/shadow AND extra --
     *                      so passing one never shifts extra's own set indices. Null (the
     *                      default) omits the set entirely, same "declared iff bound" contract
     *                      GBufferPipeline's material_layout already follows. See
     *                      bind_material()/gfx/surface/transparent_fs.glsl's set 7.
     */
    TransparentPass(coopa::gfx::core::Device& device,
                    VkFormat color_format,
                    const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                    const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                    const coopa::gfx::pipeline::DescriptorSetLayout& shadow_layout,
                    const std::string& vert_spv,
                    const std::string& frag_spv,
                    ExtraSets extra = {},
                    uint32_t extra_pc_bytes = 0,
                    const coopa::gfx::pipeline::DescriptorSetLayout* material_layout = nullptr)
        : device_(device), color_format_(color_format), extra_(std::move(extra)),
          extra_pc_bytes_(extra_pc_bytes)
    {
        extra_.validate("TransparentPass");
        pc_stages_ = coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment;
        if (device.supports_tessellation()) {
            pc_stages_ = pc_stages_ | coopa::gfx::ShaderStage::TessControl | coopa::gfx::ShaderStage::TessEval;
        }
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        create_render_pass_();

        // Cached as a member: add_variant() below builds additional pipelines against this
        // exact descriptor set layout list -- a derived shader's own vertex/fragment pair
        // still reads the same camera/light/shadow/extra/material sets, only the shader
        // modules (hence displacement/shading) differ.
        layouts_ = { &camera_layout, &light_layout, &shadow_layout };
        extra_first_set_ = static_cast<uint32_t>(layouts_.size());
        layouts_.insert(layouts_.end(), extra_.layouts.begin(), extra_.layouts.end());

        // Deliberately AFTER extra: with up to 7 sets ahead of it (0-6) this makes 8. Vulkan's
        // guaranteed maxBoundDescriptorSets minimum is only 4, so a device that can't bind 8
        // couldn't bind the 7 without it either.
        material_set_index_ = static_cast<uint32_t>(layouts_.size());
        if (material_layout != nullptr) {
            layouts_.push_back(material_layout);
        }

        pipeline_ = create_pipeline_(*vert_shader_, *frag_shader_);
    }

    ~TransparentPass() {
        destroy_framebuffer_();
        if (render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_.handle(), render_pass_, nullptr);
        }
    }

    TransparentPass(const TransparentPass&) = delete;
    TransparentPass& operator=(const TransparentPass&) = delete;

    /**
     * @brief Points the pass at the current frame's targets, rebuilding the framebuffer
     * only when the view/extent triple actually changed.
     *
     * @param color_view        The live HDR colour image view to blend into (the raw
     *                          offscreen target, or the SSR composite output).
     * @param gbuffer_depth_view The G-Buffer's depth view, bound read-only.
     * @param width  Render width.
     * @param height Render height.
     */
    void set_targets(VkImageView color_view, VkImageView gbuffer_depth_view,
                     uint32_t width, uint32_t height) {
        if (framebuffer_ != VK_NULL_HANDLE &&
            color_view == color_view_ && gbuffer_depth_view == depth_view_ &&
            width == width_ && height == height_) {
            return;
        }
        destroy_framebuffer_();
        color_view_ = color_view;
        depth_view_ = gbuffer_depth_view;
        width_      = width;
        height_     = height;

        VkImageView views[2] = { color_view_, depth_view_ };

        VkFramebufferCreateInfo fb_info{};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = render_pass_;
        fb_info.attachmentCount = 2;
        fb_info.pAttachments    = views;
        fb_info.width           = width_;
        fb_info.height          = height_;
        fb_info.layers          = 1;

        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr, &framebuffer_));
    }

    /**
     * @brief Transitions the G-Buffer depth image to read-only and begins the render pass.
     *
     * The depth image's layout on entry depends on whether HiZPass ran this frame (it
     * transitions DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL and only
     * runs when SSR is enabled), so the caller must pass the layout it actually left the
     * image in.
     *
     * @param cmd                 Command buffer to record into.
     * @param gbuffer_depth       The G-Buffer depth VkImage (for the barrier).
     * @param depth_current_layout The image's layout on entry.
     */
    void begin(coopa::gfx::command::CommandBuffer& cmd, VkImage gbuffer_depth,
              VkImageLayout depth_current_layout) {
        const bool from_shader_read = (depth_current_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = depth_current_layout;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = gbuffer_depth;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barrier.srcAccessMask = from_shader_read
            ? VK_ACCESS_SHADER_READ_BIT
            : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        vkCmdPipelineBarrier(cmd.handle(),
                             from_shader_read ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                              : VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        cmd.begin_render_pass(render_pass_, framebuffer_, {width_, height_});

        // Negative-height viewport for the Vulkan Y-flip, matching every other geometry
        // target in this engine (gbuffer_target.h, offscreen_target.h).
        cmd.set_viewport(0.0f, static_cast<float>(height_),
                         static_cast<float>(width_),
                         -static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);
    }

    /** @brief Binds the stock pipeline. Call after begin(), before push()/draws. */
    void bind(coopa::gfx::command::CommandBuffer& cmd) {
        cmd.bind_pipeline(*pipeline_);
    }

    /**
     * @brief Registers a derived shader's own vertex/fragment pair as a named variant,
     *        reusing this pass's descriptor set layouts, render pass, and blend/depth
     *        state -- only the shader modules and, optionally, the cull mode differ (see
     *        GBufferPipeline::add_variant() for the general shape). Back-face culling is the
     *        default every BLEND material shares; a surface meant to be seen from both sides
     *        (water, viewed from under its surface) passes CullMode::None.
     *
     * @param name     The SurfaceShaderDesc's name.
     * @param vert_spv Resolved .spv path for this shader's transparent vertex entry point.
     * @param frag_spv Resolved .spv path for this shader's transparent fragment entry point.
     * @param cull     Face culling for this variant (Back unless the surface is two-sided).
     */
    void add_variant(const std::string& name, const std::string& vert_spv, const std::string& frag_spv,
                     coopa::gfx::CullMode cull = coopa::gfx::CullMode::Back,
                     const std::string& tesc_spv = {}, const std::string& tese_spv = {}) {
        Variant v;
        v.vert_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        v.frag_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        v.pipeline    = create_pipeline_(*v.vert_shader, *v.frag_shader, cull);
        // Tessellated twin: shared pass-through vertex stage, this variant's (or the default)
        // control stage, its evaluation stage (the hook) and fragment stage.
        if (tess_vert_ && !tese_spv.empty()) {
            v.tesc_shader = std::make_unique<coopa::gfx::pipeline::Shader>(
                device_, tesc_spv.empty() ? tesc_path_ : tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            v.tese_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
            v.tess_pipeline = create_pipeline_(*tess_vert_, *v.frag_shader, cull, v.tesc_shader.get(), v.tese_shader.get());
        }
        variants_.emplace(name, std::move(v));
    }

    /**
     * @brief Builds the stock tessellated pipeline and records the shared pass-through vertex /
     *        default control stages for variants' twins. Call before add_variant(); a no-op on a
     *        device without tessellation.
     */
    void enable_tessellation(const std::string& tess_vert_spv, const std::string& tesc_spv, const std::string& tese_spv) {
        if (!device_.supports_tessellation()) return;
        tesc_path_ = tesc_spv;
        tess_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tess_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        tess_tesc_ = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tesc_spv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
        tess_tese_ = std::make_unique<coopa::gfx::pipeline::Shader>(device_, tese_spv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        tess_pipeline_ = create_pipeline_(*tess_vert_, *frag_shader_, coopa::gfx::CullMode::Back, tess_tesc_.get(), tess_tese_.get());
    }

    /** @brief True if `name` (empty = stock) has a tessellated pipeline. */
    bool has_tessellated(const std::string& name) const {
        auto it = variants_.find(name);
        if (it != variants_.end()) return it->second.tess_pipeline != nullptr;
        return tess_pipeline_ != nullptr;
    }

    /** @brief True if a variant named `name` was registered via add_variant(). */
    bool has_variant(const std::string& name) const {
        return variants_.find(name) != variants_.end();
    }

    /**
     * @brief Binds a named variant's pipeline, or the stock pipeline if `name` is empty or
     *        unregistered (see PBRMaterial::shader's doc: empty means "stock").
     */
    void bind(coopa::gfx::command::CommandBuffer& cmd, const std::string& name, bool tessellated = false) {
        auto it = variants_.find(name);
        if (it != variants_.end()) {
            cmd.bind_pipeline(tessellated && it->second.tess_pipeline ? *it->second.tess_pipeline : *it->second.pipeline);
            return;
        }
        cmd.bind_pipeline(tessellated && tess_pipeline_ ? *tess_pipeline_ : *pipeline_);
    }

    /** @brief The push range's stages (Vertex|Fragment, + tessellation when supported). */
    coopa::gfx::ShaderStage push_stages() const { return pc_stages_; }

    /// Binds the caller's extra sets (e.g. GI), if any were provided at construction. Call
    /// after bind(), alongside binding sets 0-2, before draws. A no-op when `extra` was empty.
    void bind_extra(coopa::gfx::command::CommandBuffer& cmd) {
        if (extra_.bind) {
            extra_.bind(cmd, extra_first_set_);
        }
    }

    /// Binds `set` at the material set index (see ctor's material_layout doc) -- call after
    /// bind()/bind_extra(), before draws. Only meaningful when a non-null material_layout was
    /// passed to the ctor; calling this otherwise binds a set the pipeline layout never
    /// declared, which the validation layer will flag.
    void bind_material(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& set) {
        cmd.bind_descriptor_set(set, material_set_index_);
    }

    /// Uploads per-batch material push constants. VERTEX|FRAGMENT: the fragment stage still
    /// owns albedo/metallic/.../alpha_cutoff, but the trailing gfx_time/gfx_params surface
    /// block must also reach the vertex stage for a derived shader's displacement hook --
    /// same reasoning as GBufferPipeline::push(). A caller pushing trailing extra_pc_bytes
    /// (e.g. toyengine's TransparentRefractionPushConstants) at
    /// [sizeof(PushConstants), ...) must use the same VERTEX|FRAGMENT stage mask: Vulkan
    /// requires a push call's stageFlags to match the declared range's stageFlags for every
    /// byte it touches, not just the stages that actually read that particular sub-range.
    void push(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& pc) {
        cmd.push_constants(pc_stages_, pc);
    }

    /** @brief Ends the render pass. */
    void end(coopa::gfx::command::CommandBuffer& cmd) {
        cmd.end_render_pass();
    }

    VkPipelineLayout layout() const { return pipeline_->layout(); }

    /// @brief The hand-built render pass this pass owns -- for a sibling pass
    /// (e.g. SdfForwardPass) that needs to build its own Pipeline against the
    /// SAME render pass instance, so both can draw into the same open
    /// begin()/end() bracket (see ToyRenderPipeline's merged back-to-front
    /// BLEND draw list, which switches between this pass and SdfForwardPass
    /// per item without ever closing and reopening the render pass).
    VkRenderPass render_pass() const { return render_pass_; }

private:
    struct Variant {
        std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader;
        std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader;
        std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline;
        std::unique_ptr<coopa::gfx::pipeline::Shader> tesc_shader, tese_shader;
        std::unique_ptr<coopa::gfx::pipeline::Pipeline> tess_pipeline;   ///< null: no tessellated twin
    };

    /// Builds one Pipeline against this pass's shared render pass, descriptor layouts,
    /// blend/depth state, and push-constant size -- the stock pipeline and every
    /// add_variant() call route through here so the only thing that can differ between
    /// them is the two shader modules.
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> create_pipeline_(
        coopa::gfx::pipeline::Shader& vert, coopa::gfx::pipeline::Shader& frag,
        coopa::gfx::CullMode cull = coopa::gfx::CullMode::Back,
        coopa::gfx::pipeline::Shader* tesc = nullptr, coopa::gfx::pipeline::Shader* tese = nullptr) {
        coopa::gfx::pipeline::PipelineDesc desc;
        desc.shaders = {&vert, &frag};
        if (tesc && tese) {
            desc.shaders = {&vert, tesc, tese, &frag};
            desc.patch_control_points = 3;
        }
        desc.vertex  = coopa::gfx::engine::data::Vertex::layout().append(coopa::gfx::engine::data::InstanceData::layout());
        desc.descriptor_layouts = layouts_;
        desc.push_constants = {{pc_stages_, 0,
                                static_cast<uint32_t>(sizeof(PushConstants) + extra_pc_bytes_)}};
        desc.raster.cull  = cull;
        desc.raster.front = coopa::gfx::FrontFace::CounterClockwise;
        desc.depth.test    = true;                                    // test against the opaque G-Buffer depth
        desc.depth.write   = false;                                   // never occlude other transparents
        desc.depth.compare = coopa::gfx::CompareOp::Less;              // a coplanar decal fails cleanly, no flicker
        desc.blend.mode    = coopa::gfx::pipeline::BlendMode::Alpha;   // src-alpha-over (pipeline.h)
        // detail::RawRenderPass's Pipeline ctor has no RenderPass to read samples()/
        // color_attachment_count() from (see pipeline.h) -- this pass is always single-sampled,
        // single-color-attachment, so BlendState's defaults (0 => derive from RenderPass) don't
        // apply; set it explicitly.
        desc.blend.color_attachment_count = 1;

        // render_pass_ is a hand-built raw VkRenderPass (LOAD_OP_LOAD on both attachments,
        // externally-owned depth -- see this class's file doc for why pipeline::RenderPass can't
        // express it), so the sealed Pipeline ctor's detail::RawRenderPass escape hatch is used
        // here instead of the normal pipeline::RenderPass overload.
        return std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device_, coopa::gfx::detail::RawRenderPass{render_pass_}, desc);
    }

    void create_render_pass_() {
        VkAttachmentDescription attachments[2]{};

        // 0: the live HDR colour image. Both OffscreenTarget instances this pass can be
        // pointed at (the raw offscreen target and the SSR composite target) leave their
        // colour image in SHADER_READ_ONLY_OPTIMAL when their own render pass ends.
        attachments[0].format         = color_format_;
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // 1: the G-Buffer's own depth image, read-only -- this pass tests against it but
        // never writes it.
        //
        // storeOp MUST be STORE, even though nothing here modifies depth. DONT_CARE does
        // NOT mean "nothing was written, so there is nothing to preserve": per the spec the
        // attachment's contents become
        // UNDEFINED after the render pass whether or not this pass wrote them, and the driver is
        // free to act on that. On a tiled/depth-compressed implementation it does -- the
        // G-Buffer depth came back as mostly zeroes (with scattered blocks of surviving real
        // depth, at compression-block granularity) for every LATER pass in the frame that
        // samples it. Those consumers exist and are the whole reason this attachment is
        // preserved across the pass: toyengine's StylizePass outline edge detector and
        // DofPass, which re-linearizes it into a thin-lens circle of confusion. With the depth
        // reading ~0, DofPass computed a large negative (near-field) CoC across the entire
        // frame, so the focus subject was blurred and the actual background was not -- the exact
        // inversion of what depth of field is supposed to do. The bug stayed latent until a
        // consumer was switched on: outline_enabled defaults to false.
        //
        // The sync-validation false positive that motivated DONT_CARE is real but is the lesser
        // problem, and belongs to the barrier rather than the store op: sync validation
        // (VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT) models a depth/stencil
        // attachment with storeOp=STORE as a WRITE at vkCmdEndRenderPass regardless of the
        // actual layout or pipeline state, so a caller reading this image later in the frame can
        // see a spurious WRITE_AFTER_WRITE. toyengine's
        // transition_gbuffer_depth_to_shader_read_() already covers that with a deliberately
        // conservative ALL_COMMANDS_BIT source stage -- see its own comment.
        attachments[1].format         = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        // Two dependencies: the tonemapper reads the colour image after we're done with
        // it (out), and we read it (having been written by deferred lighting / skybox /
        // SSR composite) before we're allowed to write it (in).
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp_info{};
        rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_info.attachmentCount = 2;
        rp_info.pAttachments    = attachments;
        rp_info.subpassCount    = 1;
        rp_info.pSubpasses      = &subpass;
        rp_info.dependencyCount = 2;
        rp_info.pDependencies   = deps;

        GFX_VK_CHECK(vkCreateRenderPass(device_.handle(), &rp_info, nullptr, &render_pass_));
    }

    void destroy_framebuffer_() {
        if (framebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_.handle(), framebuffer_, nullptr);
            framebuffer_ = VK_NULL_HANDLE;
        }
    }

    coopa::gfx::core::Device& device_;
    VkFormat                  color_format_;

    VkRenderPass  render_pass_  = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_  = VK_NULL_HANDLE;
    VkImageView   color_view_   = VK_NULL_HANDLE;
    VkImageView   depth_view_   = VK_NULL_HANDLE;
    uint32_t      width_        = 0;
    uint32_t      height_       = 0;

    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;

    ExtraSets extra_;
    uint32_t  extra_first_set_    = 0;
    uint32_t  extra_pc_bytes_     = 0;
    uint32_t  material_set_index_ = 0;
    std::vector<const coopa::gfx::pipeline::DescriptorSetLayout*> layouts_;

    std::map<std::string, Variant> variants_;

    coopa::gfx::ShaderStage pc_stages_ = coopa::gfx::ShaderStage::Vertex;
    std::string tesc_path_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> tess_vert_, tess_tesc_, tess_tese_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> tess_pipeline_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_TRANSPARENT_PASS_H
