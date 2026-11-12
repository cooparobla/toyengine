/**
 * @file volk_init.h
 * @brief One-time volk initialization guard for gfxcoopa.
 *
 * volk must be initialized exactly once per process before any Vulkan
 * function is called. This header provides a static guard that is called
 * by Instance on construction. All library headers include volk/volk.h
 * rather than vulkan/vulkan.h directly.
 */

#ifndef COOPA_GFX_UTIL_VOLK_INIT_H
#define COOPA_GFX_UTIL_VOLK_INIT_H

#include <volk/volk.h>
#include <stdexcept>

#ifdef __APPLE__
#include <cstdlib>
#include <string>
#include <dlfcn.h>
// Declared by <GLFW/glfw3.h> (GLFW >= 3.4) only when Vulkan types were already
// defined at its first inclusion, which include order elsewhere doesn't
// guarantee -- so declare it directly. Same signature, C linkage.
extern "C" void glfwInitVulkanLoader(PFN_vkGetInstanceProcAddr loader);
#endif

namespace coopa {
namespace gfx {
namespace util {

/**
 * @brief Initializes volk (and therefore the Vulkan loader) exactly once.
 *
 * Subsequent calls are no-ops. Must be called before any vkXxx function
 * is invoked. Automatically called by Instance's constructor.
 *
 * @throws std::runtime_error if volkInitialize() fails (Vulkan not present).
 */
inline void ensure_volk_initialized() {
    static bool initialized = false;
    if (initialized) return;

    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
#ifdef __APPLE__
    // A packaged app bundles its own loader in Contents/Frameworks; it must win over any
    // system/Homebrew loader volkInitialize() would find first, so a shipped build loads the
    // exact loader (and, through its ICD, the MoltenVK) it was tested with.
    // COOPA_VULKAN_LOADER names an explicit loader path to try before everything else.
    {
        std::string bundled[2];
        if (const char* env = std::getenv("COOPA_VULKAN_LOADER"); env && *env) bundled[0] = env;
        bundled[1] = "@executable_path/../Frameworks/libvulkan.1.dylib";
        for (const std::string& path : bundled) {
            if (path.empty()) continue;
            void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!module) continue;
            auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(module, "vkGetInstanceProcAddr"));
            if (!gipa) { dlclose(module); continue; }
            volkInitializeCustom(gipa);
            result = VK_SUCCESS;
            break;
        }
    }
    if (result != VK_SUCCESS) result = volkInitialize();
#else
    result = volkInitialize();
#endif
#ifdef __APPLE__
    // volk's own dlopen() search (volk.c) only tries bare leaf names plus
    // /usr/local/lib -- it never finds a Homebrew loader in /opt/homebrew/lib
    // on Apple Silicon. Fall back to the known install locations explicitly.
    if (result != VK_SUCCESS) {
        std::string candidates[3];
        if (const char* sdk = std::getenv("VULKAN_SDK")) {
            candidates[0] = std::string(sdk) + "/lib/libvulkan.1.dylib";
        }
        candidates[1] = "/opt/homebrew/lib/libvulkan.1.dylib";
        candidates[2] = "/usr/local/lib/libvulkan.1.dylib";
        for (const std::string& path : candidates) {
            if (path.empty()) continue;
            void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!module) continue;
            auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(module, "vkGetInstanceProcAddr"));
            if (!gipa) { dlclose(module); continue; }
            volkInitializeCustom(gipa);
            result = VK_SUCCESS;
            break;
        }
    }
    // GLFW does its own, separate loader lookup on macOS; hand it the loader
    // volk resolved so both agree (and so GLFW finds a Homebrew loader at
    // all). Must happen before glfwInit(), which is why Window's constructor
    // calls this function on Apple. Harmless if GLFW is already initialized.
    if (result == VK_SUCCESS) {
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
    }
#endif
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            "[gfxcoopa] volkInitialize() failed — is the Vulkan loader installed?");
    }

    initialized = true;
}

} // namespace util
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_UTIL_VOLK_INIT_H
