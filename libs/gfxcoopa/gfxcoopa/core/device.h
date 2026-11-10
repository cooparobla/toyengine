/**
 * @file device.h
 * @brief Physical and logical Vulkan device selection and queue management.
 *
 * Selects the best available GPU (discrete > integrated > virtual > CPU),
 * verifies swapchain extension support, finds graphics/present queue families,
 * and creates the VkDevice with those queues.
 *
 * The logical Device is the primary object passed to almost every other
 * gfxcoopa class. Analogous to the OpenGL context's underlying driver connection.
 */

#ifndef COOPA_GFX_CORE_DEVICE_H
#define COOPA_GFX_CORE_DEVICE_H

#include <volk/volk.h>
#include <vector>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <limits>
#include <cstring>
#include <cstdio>

#include <gfxcoopa/core/instance.h>
#include <gfxcoopa/core/surface.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace core {

/**
 * @struct QueueFamilyIndices
 * @brief Holds the queue family index for each required queue type.
 */
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics; /**< Queue family supporting VK_QUEUE_GRAPHICS_BIT. */
    std::optional<uint32_t> present;  /**< Queue family supporting surface presentation. */

    /**
     * @brief Returns true when all required queue families have been found.
     * @return True if graphics and present families are both valid.
     */
    bool is_complete() const { return graphics.has_value() && present.has_value(); }
};

/**
 * @struct ComputeCaps
 * @brief What the device offers compute work, queried once at device creation (see
 *        Device::compute_caps()). gfxcoopa records compute in-line on the graphics queue
 *        (there is no async compute queue), so `graphics_queue_compute` is the gate:
 *        Device::supports_compute() is exactly that bit.
 */
struct ComputeCaps {
    bool     graphics_queue_compute       = false; ///< The graphics queue family has VK_QUEUE_COMPUTE_BIT.
    uint32_t max_storage_buffer_range     = 0;     ///< Largest storage-buffer descriptor range, bytes.
    uint32_t max_per_stage_storage_buffers = 0;    ///< Storage buffers one shader stage may bind.
    uint32_t max_set_storage_buffers      = 0;     ///< Storage buffers across a whole pipeline layout.
    uint32_t max_work_group_invocations   = 0;     ///< local_size_x * y * z ceiling.
    uint32_t max_work_group_size[3]       = {};    ///< Per-axis local_size ceiling.
    uint32_t max_work_group_count[3]      = {};    ///< Per-axis dispatch() group-count ceiling.
    uint32_t max_shared_memory            = 0;     ///< `shared` variable bytes per work group.
    uint64_t min_storage_buffer_offset_alignment = 0; ///< Sub-range binding alignment.
    bool     multi_draw_indirect          = false; ///< draw_indirect() with draw_count > 1 (enabled when present).
    bool     draw_indirect_first_instance = false; ///< Indirect args may use a non-zero firstInstance (enabled when present).
};

/**
 * @class Device
 * @brief Manages the physical GPU selection and the logical Vulkan device.
 *
 * On construction:
 * 1. Enumerates all physical devices and scores them (discrete GPU preferred).
 * 2. Verifies VK_KHR_swapchain extension availability.
 * 3. Finds graphics + present queue family indices.
 * 4. Creates the logical VkDevice and retrieves queue handles.
 * 5. Loads all device-level function pointers via volkLoadDevice.
 */
class Device {
public:
    /**
     * @brief Selects the best physical device and creates the logical device.
     * @param instance The Vulkan instance.
     * @param surface  The window surface (used for present queue family detection).
     * @throws std::runtime_error if no suitable GPU is found or device creation fails.
     */
    Device(const Instance& instance, const Surface& surface) {
        pick_physical_device(instance.handle(), surface.handle());
        create_logical_device();
    }

    /**
     * @brief Destroys the logical VkDevice.
     */
    ~Device() {
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }
    }

    /// @brief Non-copyable.
    Device(const Device&) = delete;
    /// @brief Non-copyable.
    Device& operator=(const Device&) = delete;

    // --- Accessors ---

    /**
     * @brief Returns the logical device handle.
     * @return Raw VkDevice.
     */
    VkDevice handle() const { return device_; }

    /**
     * @brief Returns the selected physical device handle.
     * @return Raw VkPhysicalDevice.
     */
    VkPhysicalDevice physical() const { return physical_device_; }

    /** @brief True when the tessellation stages are enabled (the device reports tessellationShader). */
    bool supports_tessellation() const { return tessellation_supported_; }
    /** @brief The device's maxTessellationGenerationLevel (64 on Apple GPUs); 0 without tessellation. */
    uint32_t max_tessellation_level() const { return tessellation_supported_ ? max_tessellation_level_ : 0u; }

    /** @brief True when compute dispatches can be recorded into graphics-queue command
     *         buffers (the graphics family reports compute). Gate every compute path on it. */
    bool supports_compute() const { return compute_caps_.graphics_queue_compute; }
    /** @brief The device's compute and storage-buffer limits; see ComputeCaps. */
    const ComputeCaps& compute_caps() const { return compute_caps_; }

    /** @brief The selected GPU's name, as the driver reports it. */
    std::string gpu_name() const {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        return props.deviceName;
    }

    /** @brief The Vulkan version the selected GPU's driver supports ("1.3.290"). */
    std::string api_version_string() const {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        return std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." + std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + "." +
               std::to_string(VK_API_VERSION_PATCH(props.apiVersion));
    }

    /**
     * @brief Returns the graphics queue handle.
     * @return Raw VkQueue for graphics submission.
     */
    VkQueue graphics_queue() const { return graphics_queue_; }

    /**
     * @brief Returns the present queue handle.
     * @return Raw VkQueue for presentation submission.
     */
    VkQueue present_queue()  const { return present_queue_; }

    /**
     * @brief Returns the graphics queue family index.
     * @return Graphics queue family index.
     */
    uint32_t graphics_family() const { return queue_families_.graphics.value(); }

    /**
     * @brief Returns the present queue family index.
     * @return Present queue family index.
     */
    uint32_t present_family()  const { return queue_families_.present.value(); }

    /**
     * @brief Stalls until all device queues have finished executing.
     *
     * Equivalent to glFinish() in OpenGL — use before destroying resources
     * that may still be in use by the GPU.
     */
    void wait_idle() const { vkDeviceWaitIdle(device_); }

    /**
     * @brief Returns the cached queue family indices for this device.
     * @return QueueFamilyIndices struct.
     */
    QueueFamilyIndices queue_family_indices() const { return queue_families_; }

    /**
     * @brief Queries and returns the memory properties of the physical device.
     * @return VkPhysicalDeviceMemoryProperties struct.
     */
    VkPhysicalDeviceMemoryProperties memory_properties() const {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &props);
        return props;
    }

private:
    // --- Physical device selection ---

    /**
     * @brief Enumerates physical devices and picks the highest-scoring one.
     *
     * Score is based on device type: discrete GPU > integrated > virtual > CPU.
     * Devices that lack swapchain support or valid queue families are skipped.
     *
     * @param instance The VkInstance to enumerate from.
     * @param surface  The surface used for present-queue detection.
     */
    void pick_physical_device(VkInstance instance, VkSurfaceKHR surface) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) {
            throw std::runtime_error("[gfxcoopa] No Vulkan-capable GPU found.");
        }

        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        VkPhysicalDevice best = VK_NULL_HANDLE;
        int best_score = -1;

        for (VkPhysicalDevice dev : devices) {
            if (!is_device_suitable(dev, surface)) continue;

            int score = rate_device(dev);
            if (score > best_score) {
                best_score     = score;
                best           = dev;
                queue_families_ = find_queue_families(dev, surface);
            }
        }

        if (best == VK_NULL_HANDLE) {
            throw std::runtime_error("[gfxcoopa] No suitable GPU found. "
                                     "(Missing swapchain support or queue families.)");
        }

        physical_device_ = best;

        // Name what was picked -- when the lag question comes, the log should answer it.
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(best, &props);
        static const char* const type_names[] = {"other", "integrated GPU", "discrete GPU",
                                                 "virtual GPU", "CPU (software rasterizer)"};
        const unsigned type = static_cast<unsigned>(props.deviceType);
        std::printf("[gfxcoopa] device: %s (%s)\n", props.deviceName,
                    type < 5u ? type_names[type] : "unknown");
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            // Hardware always outscores a CPU rasterizer (rate_device), so landing here means
            // no hardware device enumerated at all -- on a machine that has one, its Vulkan
            // driver is down (commonly after a suspend/resume or a driver update under a
            // still-loaded module). Every frame will be rasterized on the CPU.
            std::printf("[gfxcoopa] WARNING: software Vulkan rasterizer '%s' selected -- no "
                        "hardware GPU enumerated. Rendering will be extremely slow. If this "
                        "machine has a GPU, its driver failed to load (check vulkaninfo, "
                        "reboot or reload the driver).\n",
                        props.deviceName);
        }
    }

    /**
     * @brief Assigns a numeric score to a physical device based on its type.
     * @param device The physical device to score.
     * @return Integer score (higher is better).
     */
    static int rate_device(VkPhysicalDevice device) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 1000;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 100;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 10;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
            default:                                     return 0;
        }
    }

    /**
     * @brief Returns true if the device supports all required extensions and
     *        has at least one valid format and present mode for the surface.
     * @param device  The physical device to test.
     * @param surface The target surface.
     * @return True if the device is suitable.
     */
    static bool is_device_suitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
        // Check extension support.
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, exts.data());

        bool has_swapchain = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swapchain = true; break;
            }
        }
        if (!has_swapchain) return false;

        // Check queue families.
        QueueFamilyIndices indices = find_queue_families(device, surface);
        if (!indices.is_complete()) return false;

        // Check swapchain adequacy.
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
        uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, nullptr);
        return format_count > 0 && mode_count > 0;
    }

    /**
     * @brief Finds graphics and present queue family indices for a device/surface pair.
     * @param device  The physical device to query.
     * @param surface The target surface for present support.
     * @return QueueFamilyIndices with graphics and present optionals filled where found.
     */
    static QueueFamilyIndices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
        QueueFamilyIndices indices;

        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphics = i;
            }

            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
            if (present_support) {
                indices.present = i;
            }

            if (indices.is_complete()) break;
        }

        return indices;
    }

    // --- Logical device creation ---

    /**
     * @brief Creates the logical VkDevice with graphics and present queues.
     *
     * If graphics and present share the same queue family, only one queue is
     * created (common on most desktop GPUs). Enables VK_KHR_swapchain.
     */
    void create_logical_device() {
        std::set<uint32_t> unique_families = {
            queue_families_.graphics.value(),
            queue_families_.present.value()
        };

        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        for (uint32_t family : unique_families) {
            VkDeviceQueueCreateInfo qi{};
            qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qi.queueFamilyIndex = family;
            qi.queueCount       = 1;
            qi.pQueuePriorities = &priority;
            queue_infos.push_back(qi);
        }

        VkPhysicalDeviceFeatures features{};
        features.samplerAnisotropy = VK_TRUE; // Enable anisotropic filtering
        // Tessellation when the device has it (MoltenVK does, through Metal's tessellator):
        // surface shaders then get camera-adaptive tessellated variants; without it they draw
        // untessellated. See supports_tessellation().
        {
            VkPhysicalDeviceFeatures available{};
            vkGetPhysicalDeviceFeatures(physical_device_, &available);
            features.tessellationShader = available.tessellationShader;
            tessellation_supported_ = available.tessellationShader == VK_TRUE;
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical_device_, &props);
            max_tessellation_level_ = props.limits.maxTessellationGenerationLevel;
            // Indirect-draw conveniences for compute-fed draws: enabled when present (MoltenVK
            // has both), never required -- a single draw with firstInstance 0 needs neither.
            features.multiDrawIndirect         = available.multiDrawIndirect;
            features.drawIndirectFirstInstance = available.drawIndirectFirstInstance;
            query_compute_caps_(available, props);
        }

        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        // The spec requires enabling VK_KHR_portability_subset whenever a device
        // advertises it (MoltenVK on macOS does; native drivers never do). Named
        // by string: its macro lives in vulkan_beta.h behind VK_ENABLE_BETA_EXTENSIONS.
        //
        // Its feature struct is likewise beta-only, so it is mirrored here (layout
        // identical to VkPhysicalDevicePortabilitySubsetFeaturesKHR). Every feature
        // the device reports is enabled -- notably mutableComparisonSamplers, which
        // the shadow passes' compare samplers require.
        struct PortabilitySubsetFeatures {
            // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR (also beta-guarded).
            VkStructureType sType = static_cast<VkStructureType>(1000163000);
            void*           pNext = nullptr;
            VkBool32        flags[15] = {}; // constantAlphaColorBlendFactors .. vertexAttributeAccessBeyondStride
        } portability_features;
        bool has_portability_subset = false;
        {
            const char* k_portability_subset = "VK_KHR_portability_subset";
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> available(count);
            vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &count, available.data());
            for (const auto& ext : available) {
                if (std::strcmp(ext.extensionName, k_portability_subset) == 0) {
                    extensions.push_back(k_portability_subset);
                    has_portability_subset = true;
                    break;
                }
            }
        }
        if (has_portability_subset) {
            PFN_vkGetPhysicalDeviceFeatures2 get_features2 = vkGetPhysicalDeviceFeatures2
                ? vkGetPhysicalDeviceFeatures2 : vkGetPhysicalDeviceFeatures2KHR;
            if (get_features2) {
                VkPhysicalDeviceFeatures2 query{};
                query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                query.pNext = &portability_features;
                get_features2(physical_device_, &query);
            }
        }

        VkDeviceCreateInfo create_info{};
        create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount    = static_cast<uint32_t>(queue_infos.size());
        create_info.pQueueCreateInfos       = queue_infos.data();
        create_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        create_info.pEnabledFeatures        = &features;
        if (has_portability_subset) {
            create_info.pNext = &portability_features;
        }

        GFX_VK_CHECK(vkCreateDevice(physical_device_, &create_info, nullptr, &device_));

        // Load all device-level function pointers via volk.
        volkLoadDevice(device_);

        vkGetDeviceQueue(device_, queue_families_.graphics.value(), 0, &graphics_queue_);
        vkGetDeviceQueue(device_, queue_families_.present.value(),  0, &present_queue_);
    }

    /**
     * @brief Fills compute_caps_ from the selected GPU's limits and its graphics queue family,
     *        and logs them once per process (an editor or test that brings up several devices
     *        on the same GPU prints them once).
     */
    void query_compute_caps_(const VkPhysicalDeviceFeatures& available, const VkPhysicalDeviceProperties& props) {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, families.data());
        const uint32_t gfx = queue_families_.graphics.value();

        ComputeCaps& c = compute_caps_;
        const VkPhysicalDeviceLimits& l = props.limits;
        c.graphics_queue_compute        = gfx < count && (families[gfx].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        c.max_storage_buffer_range      = l.maxStorageBufferRange;
        c.max_per_stage_storage_buffers = l.maxPerStageDescriptorStorageBuffers;
        c.max_set_storage_buffers       = l.maxDescriptorSetStorageBuffers;
        c.max_work_group_invocations    = l.maxComputeWorkGroupInvocations;
        c.max_shared_memory             = l.maxComputeSharedMemorySize;
        c.min_storage_buffer_offset_alignment = l.minStorageBufferOffsetAlignment;
        for (int i = 0; i < 3; ++i) {
            c.max_work_group_size[i]  = l.maxComputeWorkGroupSize[i];
            c.max_work_group_count[i] = l.maxComputeWorkGroupCount[i];
        }
        c.multi_draw_indirect          = available.multiDrawIndirect == VK_TRUE;
        c.draw_indirect_first_instance = available.drawIndirectFirstInstance == VK_TRUE;

        static bool logged = false;
        if (logged) return;
        logged = true;
        std::printf("[gfxcoopa] compute: %s on graphics queue family %u | storage buffer range %u MiB, "
                    "%u per stage, %u per layout | work group <= %u invocations (%u, %u, %u), "
                    "shared %u KiB | multiDrawIndirect %d, drawIndirectFirstInstance %d\n",
                    c.graphics_queue_compute ? "supported" : "UNSUPPORTED", gfx,
                    c.max_storage_buffer_range >> 20, c.max_per_stage_storage_buffers, c.max_set_storage_buffers,
                    c.max_work_group_invocations, c.max_work_group_size[0], c.max_work_group_size[1],
                    c.max_work_group_size[2], c.max_shared_memory >> 10,
                    c.multi_draw_indirect ? 1 : 0, c.draw_indirect_first_instance ? 1 : 0);
    }

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE; /**< Selected GPU. */
    bool             tessellation_supported_ = false;   /**< tessellationShader enabled (see supports_tessellation()). */
    uint32_t         max_tessellation_level_ = 0;
    ComputeCaps      compute_caps_;                     /**< See compute_caps(). */
    VkDevice         device_          = VK_NULL_HANDLE; /**< Logical device. */
    VkQueue          graphics_queue_  = VK_NULL_HANDLE; /**< Graphics submission queue. */
    VkQueue          present_queue_   = VK_NULL_HANDLE; /**< Presentation queue. */
    QueueFamilyIndices queue_families_;                  /**< Cached queue family indices. */
};

} // namespace core
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_CORE_DEVICE_H
