/**
 * @file instance.h
 * @brief Vulkan instance creation and lifetime management.
 *
 * The Instance is the entry point for all Vulkan API calls. It initializes
 * volk, creates VkInstance, and (when validation is enabled and the layer is
 * installed) installs a validation-layer debug messenger. Analogous to creating an OpenGL context.
 */

#ifndef COOPA_GFX_CORE_INSTANCE_H
#define COOPA_GFX_CORE_INSTANCE_H

#include <volk/volk.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <memory>

// GLFW for extension enumeration (no OpenGL)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <gfxcoopa/util/error.h>
#include <gfxcoopa/util/volk_init.h>
#include <gfxcoopa/util/debug_messenger.h>

namespace coopa {
namespace gfx {
namespace core {

/**
 * @class Instance
 * @brief RAII wrapper around VkInstance.
 *
 * Initializes the Vulkan loader via volk, queries and enables all GLFW-
 * required extensions plus VK_EXT_debug_utils when validation is on.
 * Installs a DebugMessenger that routes validation output to stderr.
 *
 * Must be the first gfxcoopa object created and the last destroyed.
 */
class Instance {
public:
    /**
     * @brief Creates a VkInstance and optionally enables validation layers.
     *
     * - Calls volkInitialize() once via the static volk guard.
     * - Queries GLFW for required surface extensions.
     * - Enables VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils when
     *   enable_validation is true and the layer is available.
     *
     * @param app_name Application display name embedded in the instance.
     * @param enable_validation Enable Khronos validation layer + debug messenger.
     * @throws std::runtime_error if instance creation fails. A requested but
     *         missing validation layer only logs a warning; the instance is
     *         created without it.
     */
    Instance(const std::string& app_name, bool enable_validation = true) {
        // Initialize volk (loads vulkan-1.so / vulkan-1.dll at runtime).
        util::ensure_volk_initialized();

        // --- Application info ---
        VkApplicationInfo app_info{};
        app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName   = app_name.c_str();
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName        = "gfxcoopa";
        app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion         = VK_API_VERSION_1_3;

        // --- Extensions ---
        uint32_t glfw_ext_count = 0;
        const char** glfw_exts  = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
#ifdef __APPLE__
        if (!glfw_exts) {
            throw std::runtime_error(
                "[gfxcoopa] glfwGetRequiredInstanceExtensions() returned none -- "
                "GLFW could not find a Vulkan loader.");
        }
#endif
        std::vector<const char*> extensions(glfw_exts, glfw_exts + glfw_ext_count);

        if (enable_validation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        // Portability drivers (MoltenVK on macOS) are hidden by loaders >= 1.3.216
        // unless the instance opts in. Apple-only: the loader advertises this
        // extension on every platform, and native drivers don't need it.
        VkInstanceCreateFlags instance_flags = 0;
#ifdef __APPLE__
        if (instance_extension_available(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            if (instance_extension_available(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            }
            instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // --- Validation layers ---
        const char* k_validation_layer = "VK_LAYER_KHRONOS_validation";
        std::vector<const char*> layers;

        if (enable_validation) {
            // A debug build run on a machine without the Vulkan SDK (e.g. a development package
            // handed to a tester) must still start: warn and run without the layer.
            if (layer_available(k_validation_layer)) {
                layers.push_back(k_validation_layer);
            } else {
                std::cerr << "[gfxcoopa] VK_LAYER_KHRONOS_validation requested but not installed; "
                             "continuing without validation.\n";
            }
        }

        // --- Instance create info ---
        VkDebugUtilsMessengerCreateInfoEXT debug_info =
            util::DebugMessenger::make_create_info();

        VkInstanceCreateInfo create_info{};
        create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo        = &app_info;
        create_info.flags                   = instance_flags;
        create_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        create_info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
        create_info.ppEnabledLayerNames     = layers.data();
        // Chain the debug info so validation catches errors during instance creation.
        if (enable_validation) {
            create_info.pNext = &debug_info;
        }

        GFX_VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance_));

        // Load all instance-level function pointers via volk.
        volkLoadInstance(instance_);

        // Install the permanent debug messenger now that the instance exists.
        if (enable_validation) {
            debug_messenger_ = std::make_unique<util::DebugMessenger>(instance_);
        }
    }

    /**
     * @brief Destroys the debug messenger (if any) and the VkInstance.
     */
    ~Instance() {
        debug_messenger_.reset(); // Must be destroyed before the instance.
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    /// @brief Non-copyable.
    Instance(const Instance&) = delete;
    /// @brief Non-copyable.
    Instance& operator=(const Instance&) = delete;

    /**
     * @brief Returns the underlying VkInstance handle.
     * @return Raw VkInstance.
     */
    VkInstance handle() const { return instance_; }

private:
    /**
     * @brief Checks whether a given Vulkan layer name is available on this system.
     * @param name Layer name string (e.g. "VK_LAYER_KHRONOS_validation").
     * @return True if the layer exists in the enumerated instance layer list.
     */
    static bool layer_available(const char* name) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> props(count);
        vkEnumerateInstanceLayerProperties(&count, props.data());

        for (const auto& p : props) {
            if (std::strcmp(p.layerName, name) == 0) return true;
        }
        return false;
    }

    /**
     * @brief Checks whether a given instance extension is available on this system.
     * @param name Extension name string (e.g. "VK_KHR_portability_enumeration").
     * @return True if the extension exists in the enumerated instance extension list.
     */
    static bool instance_extension_available(const char* name) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());

        for (const auto& p : props) {
            if (std::strcmp(p.extensionName, name) == 0) return true;
        }
        return false;
    }

    VkInstance                              instance_        = VK_NULL_HANDLE; /**< The Vulkan instance handle. */
    std::unique_ptr<util::DebugMessenger>   debug_messenger_;                  /**< Validation debug messenger (null when validation is off). */
};

} // namespace core
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_CORE_INSTANCE_H
