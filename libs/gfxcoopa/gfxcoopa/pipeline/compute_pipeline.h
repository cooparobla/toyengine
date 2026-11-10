/**
 * @file compute_pipeline.h
 * @brief Compute pipeline creation: one compute Shader, its descriptor set layouts and
 *        push-constant ranges.
 *
 * The compute sibling of Pipeline (pipeline.h): the same sealed description style
 * (DescriptorSetLayout pointers, PushConstantRange), the same RAII/move semantics, and the
 * same CommandBuffer integration -- CommandBuffer::bind_pipeline(const ComputePipeline&)
 * makes the sealed bind_descriptor_set()/push_constants() overloads target the compute bind
 * point, then dispatch()/dispatch_indirect() record the work.
 *
 * Compute runs in-line on the graphics queue (Device::supports_compute()); there is no
 * async compute queue. Synchronise its results with CommandBuffer::buffer_barrier() or the
 * named shortcuts next to it.
 */

#ifndef COOPA_GFX_PIPELINE_COMPUTE_PIPELINE_H
#define COOPA_GFX_PIPELINE_COMPUTE_PIPELINE_H

#include <volk/volk.h>
#include <stdexcept>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @struct ComputePipelineDesc
 * @brief A compute pipeline's full description -- the single argument to ComputePipeline's
 *        constructor.
 *
 * @code
 * Shader skin(device, library.path("skin.comp"), ShaderStage::Compute);
 * ComputePipeline pipe(device, ComputePipelineDesc{
 *     .shader             = &skin,
 *     .descriptor_layouts = {&set0_layout},
 *     .push_constants     = {{ShaderStage::Compute, 0, sizeof(SkinPush)}},
 * });
 * @endcode
 */
struct ComputePipelineDesc {
    Shader*                                 shader = nullptr;  ///< Must be a ShaderStage::Compute module.
    std::vector<const DescriptorSetLayout*> descriptor_layouts;
    std::vector<PushConstantRange>          push_constants;
};

/**
 * @class ComputePipeline
 * @brief RAII wrapper around a Vulkan compute pipeline and its layout.
 */
class ComputePipeline {
public:
    /**
     * @brief Creates the pipeline layout and the compute pipeline.
     * @param device The logical device.
     * @param desc   Shader, set layouts and push-constant ranges.
     * @throws std::runtime_error if the device lacks compute on its graphics queue, the shader
     *         is missing or not a compute stage, or pipeline creation fails.
     */
    ComputePipeline(core::Device& device, const ComputePipelineDesc& desc) : device_(device) {
        create_(desc);
    }

    /**
     * @brief Convenience: loads the compute module from `spv_path` and builds the pipeline.
     *        The Shader is only needed during creation, so it does not outlive this call.
     */
    ComputePipeline(core::Device& device, const std::string& spv_path,
                    std::vector<const DescriptorSetLayout*> descriptor_layouts,
                    std::vector<PushConstantRange> push_constants = {})
        : device_(device)
    {
        Shader shader(device, spv_path, ShaderStage::Compute);
        ComputePipelineDesc desc;
        desc.shader             = &shader;
        desc.descriptor_layouts = std::move(descriptor_layouts);
        desc.push_constants     = std::move(push_constants);
        create_(desc);
    }

    /** @brief Destroys the pipeline and its layout. */
    ~ComputePipeline() {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_.handle(), pipeline_, nullptr);
        if (layout_   != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_.handle(), layout_, nullptr);
    }

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    /** @brief Move constructor: transfers ownership of the pipeline and its layout. */
    ComputePipeline(ComputePipeline&& other) noexcept
        : device_(other.device_), pipeline_(other.pipeline_), layout_(other.layout_)
    {
        other.pipeline_ = VK_NULL_HANDLE;
        other.layout_   = VK_NULL_HANDLE;
    }

    /** @brief The underlying VkPipeline. */
    VkPipeline handle() const { return pipeline_; }
    /** @brief The VkPipelineLayout (descriptor sets and push constants bind against it). */
    VkPipelineLayout layout() const { return layout_; }

    /**
     * @brief Work groups needed to cover `count` items with `local_size` items per group
     *        (a ceiling divide) -- the usual dispatch() argument for a 1D kernel.
     */
    static uint32_t groups_for(uint32_t count, uint32_t local_size) {
        return local_size == 0 ? 0u : (count + local_size - 1u) / local_size;
    }

private:
    void create_(const ComputePipelineDesc& desc) {
        if (!device_.supports_compute()) {
            throw std::runtime_error("[gfxcoopa] ComputePipeline: the device's graphics queue has no compute support.");
        }
        if (!desc.shader || desc.shader->stage() != VK_SHADER_STAGE_COMPUTE_BIT) {
            throw std::runtime_error("[gfxcoopa] ComputePipeline: desc.shader must be a ShaderStage::Compute module.");
        }

        std::vector<VkDescriptorSetLayout> layouts;
        layouts.reserve(desc.descriptor_layouts.size());
        for (const DescriptorSetLayout* l : desc.descriptor_layouts) layouts.push_back(l->handle());
        std::vector<VkPushConstantRange> ranges;
        ranges.reserve(desc.push_constants.size());
        for (const PushConstantRange& pc : desc.push_constants) {
            ranges.push_back(VkPushConstantRange{ detail::to_vk(pc.stages), pc.offset, pc.size });
        }

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount         = static_cast<uint32_t>(layouts.size());
        layout_info.pSetLayouts            = layouts.empty() ? nullptr : layouts.data();
        layout_info.pushConstantRangeCount = static_cast<uint32_t>(ranges.size());
        layout_info.pPushConstantRanges    = ranges.empty() ? nullptr : ranges.data();
        GFX_VK_CHECK(vkCreatePipelineLayout(device_.handle(), &layout_info, nullptr, &layout_));

        VkComputePipelineCreateInfo info{};
        info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage  = desc.shader->stage_info();
        info.layout = layout_;
        const VkResult r = vkCreateComputePipelines(device_.handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
        if (r != VK_SUCCESS) {
            vkDestroyPipelineLayout(device_.handle(), layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            GFX_VK_CHECK(r);
        }
    }

    core::Device&    device_;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_COMPUTE_PIPELINE_H
