/**
 * @file fullscreen_stage.h
 * @brief One fullscreen-triangle render stage: its two shaders, its own
 *        descriptor set(s), and the pipeline that draws it.
 *
 * Nearly every post-processing pass in `engine/passes/` is built from the
 * same parts -- a vertex shader that generates a fullscreen triangle from
 * `gl_VertexIndex`, a fragment shader reading one or more sampled images,
 * a descriptor set holding those images, and a pipeline with no vertex
 * input, no culling and no depth test. FullscreenStage is that assembly,
 * so a pass supplies only what makes it different.
 *
 * A pass OWNS one or more stages; it is not a base class. Passes keep their
 * own public constructors, `set_source_image()`-style binders and `draw()`/
 * `execute()` entry points, so a stage is never visible to a consumer.
 *
 * Render targets, per-frame barriers and the order stages run in stay with
 * the pass: those are the decisions that genuinely differ between passes,
 * and they should stay readable at the call site.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_FULLSCREEN_STAGE_H
#define GFXCOOPA_ENGINE_PASSES_FULLSCREEN_STAGE_H

#include <volk/volk.h>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/vertex_layout.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @struct FullscreenStageDesc
 * @brief Everything that distinguishes one fullscreen stage from another.
 */
struct FullscreenStageDesc {
    std::string vert_spv;  ///< Fullscreen-triangle vertex shader.
    std::string frag_spv;  ///< The stage's fragment shader.

    /// @brief Layouts owned elsewhere that are declared BEFORE the stage's own
    /// sets -- a camera UBO the app binds at set 0, say. The stage's own sets
    /// then start at index leading_layouts.size(). The pass binds these.
    std::vector<const pipeline::DescriptorSetLayout*> leading_layouts;

    /// @brief The descriptor sets the stage OWNS, declared after
    /// `leading_layouts`.
    /// Most stages own exactly one (the images they sample); a stage that also
    /// owns, say, a UBO set declares it as a second entry. Empty means the
    /// stage owns no set at all and reads only through `extra_layouts`.
    std::vector<std::vector<pipeline::DescriptorBinding>> owned_sets;

    /// @brief Layouts owned elsewhere, declared AFTER the stage's own sets
    /// (a G-buffer set, a GI system). The pass binds these.
    std::vector<const pipeline::DescriptorSetLayout*> extra_layouts;

    std::vector<pipeline::PushConstantRange> push_constants;

    /// @brief Blend equation. None (the default) writes the fragment straight
    /// through, which is what a post-processing stage usually wants.
    pipeline::BlendMode blend = pipeline::BlendMode::None;

    /// @brief How many independent copies of `owned_sets` to allocate. More
    /// than one is for a stage reused across pyramid levels or frames in
    /// flight, where each instance binds its own images. Select with the
    /// `instance` argument to set()/bind().
    uint32_t instances = 1;
};

/**
 * @class FullscreenStage
 * @brief Owns the shaders, descriptor set(s) and pipeline for one
 *        fullscreen-triangle draw.
 */
class FullscreenStage {
public:
    /**
     * @brief Builds the stage against a gfxcoopa RenderPass.
     *
     * Sample count and color attachment count come from `render_pass`.
     *
     * @param device      The logical device.
     * @param render_pass The render pass this stage draws into.
     * @param desc        What distinguishes this stage.
     */
    FullscreenStage(core::Device& device,
                    const pipeline::RenderPass& render_pass,
                    const FullscreenStageDesc& desc)
        : device_(device)
    {
        build_shaders_and_sets(device, desc);
        pipeline_ = std::make_unique<pipeline::Pipeline>(
            device, render_pass, make_pipeline_desc(desc));
    }

    /**
     * @brief Builds the stage against a hand-built VkRenderPass.
     *
     * For gfxcoopa-internal passes whose render pass has multiple color
     * attachments, which pipeline::RenderPass cannot express. The attachment
     * count must be supplied here, since there is no RenderPass to ask.
     *
     * @param device                 The logical device.
     * @param render_pass            Handle of the target render pass, wrapped.
     * @param desc                   What distinguishes this stage.
     * @param color_attachment_count Color attachments sharing the blend state.
     */
    FullscreenStage(core::Device& device,
                    detail::RawRenderPass render_pass,
                    const FullscreenStageDesc& desc,
                    uint32_t color_attachment_count = 1)
        : device_(device)
    {
        build_shaders_and_sets(device, desc);
        pipeline::PipelineDesc pd = make_pipeline_desc(desc);
        pd.blend.color_attachment_count = color_attachment_count;
        pipeline_ = std::make_unique<pipeline::Pipeline>(device, render_pass, pd);
    }

    /// @brief Non-copyable.
    FullscreenStage(const FullscreenStage&) = delete;
    /// @brief Non-copyable.
    FullscreenStage& operator=(const FullscreenStage&) = delete;
    /// @brief Movable, so a pass can hold a std::vector of pyramid levels.
    FullscreenStage(FullscreenStage&&) = default;

    /**
     * @brief One of the stage's own descriptor sets, for binding into.
     * @param set_index Which owned set (0 is the first declared).
     * @param instance  Which copy, when `instances` > 1.
     * @return The requested descriptor set.
     */
    pipeline::DescriptorSet& set(uint32_t set_index = 0, uint32_t instance = 0) {
        return *sets_.at(instance * set_widths_ + set_index);
    }

    /**
     * @brief The layout of one of the stage's own descriptor sets.
     * @param set_index Which owned set.
     */
    const pipeline::DescriptorSetLayout& layout(uint32_t set_index = 0) const {
        return *layouts_.at(set_index);
    }

    /// @brief The stage's pipeline, for a pass that records its own bind.
    const pipeline::Pipeline& pipeline() const { return *pipeline_; }

    /**
     * @brief Reallocates the owned descriptor sets for a new instance count,
     * keeping the pipeline and shaders.
     *
     * For a pass whose instance count depends on something that changes at
     * runtime -- a mip chain whose length follows the render resolution --
     * where rebuilding the pipeline too would be wasted work. The layouts are
     * unchanged, so the pipeline stays compatible.
     *
     * Every set is freshly allocated and unbound afterward; the caller must
     * rebind each one. Safe only between frames, after a device wait_idle().
     *
     * @param instances New number of independent copies of the owned sets.
     */
    void rebuild_sets(uint32_t instances) {
        if (layouts_.empty()) return;
        sets_.clear();
        pipeline::DescriptorPoolBuilder pool_builder;
        for (const auto& layout : layouts_) pool_builder.add_sets(*layout, instances);
        pool_ = std::make_unique<pipeline::DescriptorPool>(pool_builder.build(device_));
        sets_.reserve(static_cast<size_t>(set_widths_) * instances);
        for (uint32_t inst = 0; inst < instances; ++inst) {
            for (const auto& layout : layouts_) {
                sets_.push_back(std::make_unique<pipeline::DescriptorSet>(device_, *pool_, *layout));
            }
        }
    }

    /**
     * @brief Binds the pipeline, sets viewport and scissor to the full target,
     * and binds the stage's own descriptor set at set 0.
     *
     * @param cmd   Command buffer to record into.
     * @param width Target width in pixels.
     * @param height Target height in pixels.
     * @param instance Which copy of the owned sets to bind, when `instances` > 1.
     */
    void bind(command::CommandBuffer& cmd, uint32_t width, uint32_t height,
              uint32_t instance = 0) const
    {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        cmd.set_scissor(0, 0, width, height);
        for (uint32_t i = 0; i < set_widths_; ++i) {
            cmd.bind_descriptor_set(*sets_.at(instance * set_widths_ + i), first_owned_set_ + i);
        }
    }

    /// @brief Index of the stage's first owned set in the pipeline layout --
    /// leading_layouts.size(). The pass binds its leading layouts below this.
    uint32_t first_owned_set() const { return first_owned_set_; }

    /// @brief Records the fullscreen triangle draw. The vertex shader builds
    /// its three clip-space positions from gl_VertexIndex; no vertex buffer
    /// is bound, which is why PipelineDesc uses VertexLayout::none().
    void draw(command::CommandBuffer& cmd) const { cmd.draw(3); }

    /**
     * @brief bind() + push constants + draw(), the whole stage in one call.
     * @tparam T The push constant struct, matching the fragment shader's block.
     * @param cmd    Command buffer to record into.
     * @param width  Target width in pixels.
     * @param height Target height in pixels.
     * @param stages Shader stages that read the push constants.
     * @param push   The push constant value.
     * @param instance Which copy of the owned sets to bind.
     */
    template <typename T>
    void draw(command::CommandBuffer& cmd, uint32_t width, uint32_t height,
              ShaderStage stages, const T& push, uint32_t instance = 0) const
    {
        bind(cmd, width, height, instance);
        cmd.push_constants(stages, push);
        cmd.draw(3);
    }

private:
    /// @brief Loads both shaders and allocates the stage's own set(s), if any.
    void build_shaders_and_sets(core::Device& device, const FullscreenStageDesc& desc) {
        vert_ = std::make_unique<pipeline::Shader>(device, desc.vert_spv,
                                                   VK_SHADER_STAGE_VERTEX_BIT);
        frag_ = std::make_unique<pipeline::Shader>(device, desc.frag_spv,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT);
        first_owned_set_ = static_cast<uint32_t>(desc.leading_layouts.size());
        if (desc.owned_sets.empty()) return;

        set_widths_ = static_cast<uint32_t>(desc.owned_sets.size());
        pipeline::DescriptorPoolBuilder pool_builder;
        layouts_.reserve(set_widths_);
        for (const auto& bindings : desc.owned_sets) {
            layouts_.push_back(std::make_unique<pipeline::DescriptorSetLayout>(device, bindings));
            pool_builder.add_sets(*layouts_.back(), desc.instances);
        }
        pool_ = std::make_unique<pipeline::DescriptorPool>(pool_builder.build(device));

        // Instance-major, so instance i's sets are contiguous: see set()'s indexing.
        sets_.reserve(static_cast<size_t>(set_widths_) * desc.instances);
        for (uint32_t inst = 0; inst < desc.instances; ++inst) {
            for (const auto& layout : layouts_) {
                sets_.push_back(std::make_unique<pipeline::DescriptorSet>(device, *pool_, *layout));
            }
        }
    }

    /// @brief The PipelineDesc every fullscreen stage shares: no vertex input,
    /// no culling, no depth test or write.
    pipeline::PipelineDesc make_pipeline_desc(const FullscreenStageDesc& desc) const {
        pipeline::PipelineDesc pd;
        pd.shaders       = {vert_.get(), frag_.get()};
        pd.vertex        = VertexLayout::none();
        pd.raster.cull   = CullMode::None;
        pd.depth.test    = false;
        pd.depth.write   = false;
        pd.blend.mode    = desc.blend;
        pd.push_constants = desc.push_constants;
        pd.descriptor_layouts = desc.leading_layouts;
        for (const auto& layout : layouts_) pd.descriptor_layouts.push_back(layout.get());
        pd.descriptor_layouts.insert(pd.descriptor_layouts.end(),
                                     desc.extra_layouts.begin(), desc.extra_layouts.end());
        return pd;
    }

    core::Device& device_;  /**< The logical device (not owned). */
    std::unique_ptr<pipeline::Shader>              vert_;
    std::unique_ptr<pipeline::Shader>              frag_;
    std::vector<std::unique_ptr<pipeline::DescriptorSetLayout>> layouts_;  /**< One per owned set; empty if none. */
    std::unique_ptr<pipeline::DescriptorPool>      pool_;
    std::vector<std::unique_ptr<pipeline::DescriptorSet>> sets_;  /**< instance-major, set_widths_ per instance. */
    uint32_t set_widths_ = 0;      /**< Owned sets per instance. */
    uint32_t first_owned_set_ = 0;  /**< See first_owned_set(). */
    std::unique_ptr<pipeline::Pipeline>            pipeline_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_FULLSCREEN_STAGE_H
