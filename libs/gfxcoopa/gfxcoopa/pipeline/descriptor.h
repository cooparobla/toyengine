/**
 * @file descriptor.h
 * @brief Vulkan descriptor pool, layout, and set management.
 *
 * Provides three RAII classes mirroring OpenGL's uniform/texture binding model:
 * - DescriptorPool: allocates descriptor sets.
 * - DescriptorSetLayout: describes binding points (like an interface block layout).
 * - DescriptorSet: a single set of bound resources (bind_buffer = glBindBufferBase,
 *   bind_image = glBindTexture).
 */

#ifndef COOPA_GFX_PIPELINE_DESCRIPTOR_H
#define COOPA_GFX_PIPELINE_DESCRIPTOR_H

#include <volk/volk.h>
#include <vector>
#include <map>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace pipeline {

class DescriptorSetLayout; // forward declaration for DescriptorPoolBuilder

/// @brief One binding point within a descriptor set: its resource type,
/// visibility, and (for arrays) element count. gfxcoopa's sealed
/// replacement for hand-building a VkDescriptorSetLayoutBinding.
struct DescriptorBinding {
    uint32_t       binding = 0;
    DescriptorType type    = DescriptorType::UniformBuffer;
    ShaderStage    stages  = ShaderStage::None;
    uint32_t       count   = 1;
};

// ---------------------------------------------------------
// DescriptorPool
// ---------------------------------------------------------

/**
 * @class DescriptorPool
 * @brief RAII wrapper around VkDescriptorPool.
 *
 * Allocates a pool with the given pool sizes. All DescriptorSet objects
 * allocated from this pool are freed when the pool is destroyed.
 */
class DescriptorPool {
public:
    /**
     * @brief Creates a VkDescriptorPool.
     * @param device     The logical device.
     * @param max_sets   Maximum number of descriptor sets that can be allocated.
     * @param pool_sizes Per-type descriptor counts for the pool.
     */
    DescriptorPool(core::Device&                            device,
                   uint32_t                                 max_sets,
                   const std::vector<VkDescriptorPoolSize>& pool_sizes)
        : device_(device)
    {
        VkDescriptorPoolCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.maxSets       = max_sets;
        info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        info.pPoolSizes    = pool_sizes.data();
        info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        GFX_VK_CHECK(vkCreateDescriptorPool(device_.handle(), &info, nullptr, &pool_));
    }

    /**
     * @brief Destroys the descriptor pool.
     */
    ~DescriptorPool() {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
        }
    }

    /// @brief Non-copyable.
    DescriptorPool(const DescriptorPool&) = delete;
    /// @brief Non-copyable.
    DescriptorPool& operator=(const DescriptorPool&) = delete;

    /**
     * @brief Move constructor: transfers VkDescriptorPool ownership.
     *
     * Needed so DescriptorPoolBuilder::build() can return a DescriptorPool
     * by value into e.g. `std::make_unique<DescriptorPool>(builder.build(device))`
     * -- make_unique's forwarding-reference argument passing does not
     * qualify for C++17's guaranteed prvalue elision the way a direct
     * `DescriptorPool p = builder.build(device);` does, so an actual move
     * constructor is required, not just reliance on elision.
     * @param other The DescriptorPool to move from (left in a null state).
     */
    DescriptorPool(DescriptorPool&& other) noexcept
        : device_(other.device_), pool_(other.pool_)
    {
        other.pool_ = VK_NULL_HANDLE;
    }

    /**
     * @brief Returns the underlying VkDescriptorPool handle.
     * @return Raw VkDescriptorPool.
     */
    VkDescriptorPool handle() const { return pool_; }

private:
    core::Device&    device_;              /**< Owning logical device (not owned). */
    VkDescriptorPool pool_ = VK_NULL_HANDLE;/**< The Vulkan descriptor pool. */
};

// ---------------------------------------------------------
// DescriptorSetLayout
// ---------------------------------------------------------

/**
 * @class DescriptorSetLayout
 * @brief RAII wrapper around VkDescriptorSetLayout.
 *
 * Describes the binding points used by a pipeline stage, analogous to
 * declaring a uniform block or sampler in GLSL.
 */
class DescriptorSetLayout {
public:
    /**
     * @brief Creates a VkDescriptorSetLayout from a list of binding descriptions.
     * @param device   The logical device.
     * @param bindings Vector of VkDescriptorSetLayoutBinding structs.
     */
    DescriptorSetLayout(core::Device&                                    device,
                        const std::vector<VkDescriptorSetLayoutBinding>& bindings)
        : device_(device)
    {
        create_from_vk_bindings(bindings);
    }

    /**
     * @brief Creates a VkDescriptorSetLayout from gfxcoopa's sealed
     * DescriptorBinding list instead of raw VkDescriptorSetLayoutBinding
     * structs. See DescriptorLayoutBuilder for a fluent way to build the
     * `bindings` vector.
     *
     * Unlike the raw-typed constructor above, this ALSO retains the
     * sealed bindings (see bindings()), which is what lets
     * DescriptorPoolBuilder derive correct pool sizes from a layout
     * instead of requiring the caller to compute VkDescriptorPoolSize
     * counts by hand.
     *
     * @param device   The logical device.
     * @param bindings The set's binding points, in gfxcoopa's sealed vocabulary.
     */
    DescriptorSetLayout(core::Device& device, const std::vector<DescriptorBinding>& bindings)
        : device_(device), bindings_(bindings)
    {
        std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
        vk_bindings.reserve(bindings.size());
        for (const auto& b : bindings) {
            VkDescriptorSetLayoutBinding vb{};
            vb.binding         = b.binding;
            vb.descriptorType  = detail::to_vk(b.type);
            vb.descriptorCount = b.count;
            vb.stageFlags      = detail::to_vk(b.stages);
            vk_bindings.push_back(vb);
        }
        create_from_vk_bindings(vk_bindings);
    }

    /**
     * @brief Destroys the descriptor set layout.
     */
    ~DescriptorSetLayout() {
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_.handle(), layout_, nullptr);
        }
    }

    /// @brief Non-copyable.
    DescriptorSetLayout(const DescriptorSetLayout&) = delete;
    /// @brief Non-copyable.
    DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

    /**
     * @brief Move constructor: transfers VkDescriptorSetLayout ownership.
     * See DescriptorPool's move constructor docs for why this is needed
     * (DescriptorLayoutBuilder::build() returns by value).
     * @param other The DescriptorSetLayout to move from (left in a null state).
     */
    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
        : device_(other.device_), layout_(other.layout_), bindings_(std::move(other.bindings_))
    {
        other.layout_ = VK_NULL_HANDLE;
    }

    /**
     * @brief Returns the underlying VkDescriptorSetLayout handle.
     * @return Raw VkDescriptorSetLayout.
     */
    VkDescriptorSetLayout handle() const { return layout_; }

    /**
     * @brief Returns the sealed bindings this layout was built from.
     *
     * Only populated when constructed via the sealed
     * `std::vector<DescriptorBinding>` constructor (or via
     * DescriptorLayoutBuilder, which uses it) -- empty for layouts built
     * from raw VkDescriptorSetLayoutBinding structs, since there is no
     * DescriptorType/ShaderStage to recover from those. DescriptorPoolBuilder
     * relies on this being populated.
     */
    const std::vector<DescriptorBinding>& bindings() const { return bindings_; }

private:
    void create_from_vk_bindings(const std::vector<VkDescriptorSetLayoutBinding>& vk_bindings) {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(vk_bindings.size());
        info.pBindings    = vk_bindings.data();

        GFX_VK_CHECK(vkCreateDescriptorSetLayout(device_.handle(), &info, nullptr, &layout_));
    }

    core::Device&         device_;               /**< Owning logical device (not owned). */
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;/**< The Vulkan descriptor set layout. */
    std::vector<DescriptorBinding> bindings_;     /**< See bindings(). */
};

// ---------------------------------------------------------
// DescriptorSet
// ---------------------------------------------------------

/**
 * @class DescriptorSet
 * @brief A single allocated and updatable descriptor set.
 *
 * Wraps VkDescriptorSet allocation from a pool. Provides bind_buffer()
 * and bind_image() for updating bindings, mirroring glBindBufferBase and
 * glBindTexture in the OpenGL API.
 */
class DescriptorSet {
public:
    /**
     * @brief Allocates a VkDescriptorSet from the given pool and layout.
     * @param device The logical device.
     * @param pool   The descriptor pool to allocate from.
     * @param layout The layout describing the set's bindings.
     */
    DescriptorSet(core::Device&         device,
                  DescriptorPool&       pool,
                  DescriptorSetLayout&  layout)
        : device_(device), pool_(pool.handle())
    {
        VkDescriptorSetLayout raw_layout = layout.handle();

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &raw_layout;

        GFX_VK_CHECK(vkAllocateDescriptorSets(device_.handle(), &alloc_info, &set_));
    }

    /**
     * @brief Frees the descriptor set back into the pool.
     */
    ~DescriptorSet() {
        if (set_ != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(device_.handle(), pool_, 1, &set_);
        }
    }

    /// @brief Non-copyable.
    DescriptorSet(const DescriptorSet&) = delete;
    /// @brief Non-copyable.
    DescriptorSet& operator=(const DescriptorSet&) = delete;

    /**
     * @brief Move constructor: transfers VkDescriptorSet ownership.
     * @param other The DescriptorSet to move from (left in a null state, so
     *   its destructor frees nothing).
     */
    DescriptorSet(DescriptorSet&& other) noexcept
        : device_(other.device_), pool_(other.pool_), set_(other.set_)
    {
        other.set_ = VK_NULL_HANDLE;
    }

    /**
     * @brief Binds a uniform buffer to the given binding point (like glBindBufferBase).
     * @param binding The binding index within the set.
     * @param buffer  The Buffer containing uniform data.
     */
    void bind_buffer(uint32_t binding, const memory::Buffer& buffer) {
        write_buffer(binding, buffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }

    /**
     * @brief Binds a storage buffer (SSBO) to the given binding point.
     * @param binding The binding index within the set.
     * @param buffer  The Buffer containing storage data.
     */
    void bind_storage_buffer(uint32_t binding, const memory::Buffer& buffer) {
        write_buffer(binding, buffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    /**
     * @brief Binds a combined image sampler to the given binding point (like glBindTexture).
     * @param binding    The binding index within the set.
     * @param image_view The VkImageView to expose to the shader.
     * @param sampler    The VkSampler controlling filtering and addressing.
     */
    void bind_image(uint32_t binding, VkImageView image_view, VkSampler sampler) {
        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView   = image_view;
        image_info.sampler     = sampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set_;
        write.dstBinding      = binding;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &image_info;

        vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    }

    /**
     * @brief Binds a storage image (compute read/write, like glBindImageTexture). The image
     * must be in VK_IMAGE_LAYOUT_GENERAL whenever a dispatch reads or writes it.
     * @param binding    The binding index within the set.
     * @param image_view The VkImageView to expose (a single mip level).
     */
    void bind_storage_image(uint32_t binding, VkImageView image_view) {
        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_info.imageView   = image_view;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set_;
        write.dstBinding      = binding;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo      = &image_info;

        vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    }

    /**
     * @brief Binds a combined image sampler, using the sealed TextureView
     * instead of a raw VkImageView.
     * @param binding    The binding index within the set.
     * @param view       The texture view to expose to the shader.
     * @param sampler    The sampler controlling filtering and addressing.
     */
    void bind_image(uint32_t binding, TextureView view, const engine::util::Sampler& sampler) {
        bind_image(binding, detail::unwrap(view), sampler.handle());
    }

    /**
     * @brief Returns the underlying VkDescriptorSet handle.
     * @return Raw VkDescriptorSet.
     */
    VkDescriptorSet handle() const { return set_; }

private:
    /**
     * @brief Writes `buffer`'s whole range into `binding` as `type`.
     * @param binding The binding index within the set.
     * @param buffer  The buffer to bind.
     * @param type    Uniform or storage buffer descriptor type.
     */
    void write_buffer(uint32_t binding, const memory::Buffer& buffer, VkDescriptorType type) {
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = buffer.handle();
        buffer_info.offset = 0;
        buffer_info.range  = buffer.size();

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set_;
        write.dstBinding      = binding;
        write.dstArrayElement = 0;
        write.descriptorType  = type;
        write.descriptorCount = 1;
        write.pBufferInfo     = &buffer_info;

        vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
    }

    core::Device&    device_;               /**< Owning logical device (not owned). */
    VkDescriptorPool pool_  = VK_NULL_HANDLE;/**< Pool this set was allocated from (not owned). */
    VkDescriptorSet  set_   = VK_NULL_HANDLE;/**< The allocated descriptor set. */
};

// ---------------------------------------------------------
// DescriptorLayoutBuilder
// ---------------------------------------------------------

/**
 * @class DescriptorLayoutBuilder
 * @brief Fluent construction of a DescriptorSetLayout's bindings, so a
 * caller never hand-builds a std::vector<VkDescriptorSetLayoutBinding>.
 *
 * @code
 * auto layout = DescriptorLayoutBuilder()
 *     .uniform_buffer(0, ShaderStage::Vertex | ShaderStage::Fragment)
 *     .combined_sampler(1, ShaderStage::Fragment)
 *     .build(device);
 * @endcode
 */
class DescriptorLayoutBuilder {
public:
    /// @brief Adds a uniform buffer binding.
    DescriptorLayoutBuilder& uniform_buffer(uint32_t binding, ShaderStage stages, uint32_t count = 1) {
        bindings_.push_back(DescriptorBinding{binding, DescriptorType::UniformBuffer, stages, count});
        return *this;
    }

    /// @brief Adds a storage buffer (SSBO) binding.
    DescriptorLayoutBuilder& storage_buffer(uint32_t binding, ShaderStage stages, uint32_t count = 1) {
        bindings_.push_back(DescriptorBinding{binding, DescriptorType::StorageBuffer, stages, count});
        return *this;
    }

    /// @brief Adds a combined image+sampler binding.
    DescriptorLayoutBuilder& combined_sampler(uint32_t binding, ShaderStage stages, uint32_t count = 1) {
        bindings_.push_back(DescriptorBinding{binding, DescriptorType::CombinedImageSampler, stages, count});
        return *this;
    }

    /// @brief Adds a storage image binding (compute read/write).
    DescriptorLayoutBuilder& storage_image(uint32_t binding, ShaderStage stages, uint32_t count = 1) {
        bindings_.push_back(DescriptorBinding{binding, DescriptorType::StorageImage, stages, count});
        return *this;
    }

    /// @brief Builds the DescriptorSetLayout from the bindings added so far.
    DescriptorSetLayout build(core::Device& device) const {
        return DescriptorSetLayout(device, bindings_);
    }

private:
    std::vector<DescriptorBinding> bindings_;
};

// ---------------------------------------------------------
// DescriptorPoolBuilder
// ---------------------------------------------------------

/**
 * @class DescriptorPoolBuilder
 * @brief Derives a DescriptorPool's per-type sizes from the
 * DescriptorSetLayouts it will allocate sets from, instead of requiring
 * the caller to compute VkDescriptorPoolSize counts by hand and keep them
 * in sync with the layouts as bindings change.
 *
 * @code
 * auto pool = DescriptorPoolBuilder()
 *     .add_sets(camera_layout, 1)
 *     .add_sets(material_layout, max_materials)
 *     .build(device);
 * @endcode
 */
class DescriptorPoolBuilder {
public:
    /**
     * @brief Reserves pool capacity for `count` sets allocated from `layout`.
     *
     * `layout` must have been built via the sealed DescriptorSetLayout
     * constructor (or DescriptorLayoutBuilder) -- its bindings() must be
     * populated, since that is the only source of type/count information
     * this builder has to work from.
     *
     * @param layout The layout that will be used to allocate `count` sets.
     * @param count  How many sets will be allocated from this layout.
     * @return *this, for chaining.
     * @throws std::runtime_error if `layout` has no sealed bindings to size from.
     */
    DescriptorPoolBuilder& add_sets(const DescriptorSetLayout& layout, uint32_t count) {
        // A raw-constructed layout has no bindings() to read, so it would silently
        // contribute zero pool sizes and surface much later, at allocation time, as
        // VK_ERROR_OUT_OF_POOL_MEMORY. Fail here instead, where the cause is visible.
        if (layout.bindings().empty()) {
            throw std::runtime_error(
                "[gfxcoopa] DescriptorPoolBuilder::add_sets: layout has no sealed bindings to "
                "size the pool from. Build it from std::vector<DescriptorBinding> (or via "
                "DescriptorLayoutBuilder), or size this pool with add_type() instead.");
        }
        max_sets_ += count;
        for (const DescriptorBinding& b : layout.bindings()) {
            type_counts_[b.type] += b.count * count;
        }
        return *this;
    }

    /// @brief Escape hatch: adds `count` descriptors of a given type
    /// directly, for pools that allocate sets DescriptorPoolBuilder can't
    /// see the layout of (e.g. one shared across multiple layout variants).
    DescriptorPoolBuilder& add_type(DescriptorType type, uint32_t count) {
        type_counts_[type] += count;
        return *this;
    }

    /// @brief Builds the DescriptorPool sized for everything added so far.
    DescriptorPool build(core::Device& device) const {
        std::vector<VkDescriptorPoolSize> sizes;
        sizes.reserve(type_counts_.size());
        for (const auto& [type, count] : type_counts_) {
            sizes.push_back(VkDescriptorPoolSize{ detail::to_vk(type), count });
        }
        return DescriptorPool(device, max_sets_, sizes);
    }

private:
    uint32_t max_sets_ = 0;
    std::map<DescriptorType, uint32_t> type_counts_;
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_DESCRIPTOR_H
