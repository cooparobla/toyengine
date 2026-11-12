/**
 * @file shader.h
 * @brief SPIR-V shader module loading and pipeline stage info builder.
 *
 * Reads a compiled .spv file from disk, creates a VkShaderModule, and
 * provides stage_info() for use with VkGraphicsPipelineCreateInfo.
 * Analogous to glCreateShader + glShaderSource + glCompileShader.
 */

#ifndef COOPA_GFX_PIPELINE_SHADER_H
#define COOPA_GFX_PIPELINE_SHADER_H

#include <volk/volk.h>
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @class Shader
 * @brief RAII wrapper around a VkShaderModule loaded from a SPIR-V binary.
 *
 * Reads a .spv file, creates the shader module, and holds the stage
 * flag so that stage_info() can be directly used in a pipeline create
 * info struct. Multiple Shader objects are combined via Pipeline.
 */
class Shader {
public:
    /**
     * @brief Loads a SPIR-V binary and creates a VkShaderModule.
     *
     * @param device   The logical device.
     * @param filepath Absolute or relative path to the compiled .spv file.
     * @param stage    The shader stage this module implements
     *                 (e.g. VK_SHADER_STAGE_VERTEX_BIT).
     * @throws std::runtime_error if the file cannot be read or module creation fails.
     */
    Shader(core::Device&       device,
           const std::string&  filepath,
           VkShaderStageFlagBits stage)
        : device_(device), stage_(stage)
    {
        std::vector<char> code = read_spirv(filepath);

        VkShaderModuleCreateInfo create_info{};
        create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = code.size();
        create_info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

        GFX_VK_CHECK(vkCreateShaderModule(device_.handle(), &create_info, nullptr, &module_));
    }

    /**
     * @brief Loads a SPIR-V binary, using the sealed ShaderStage instead of
     * VkShaderStageFlagBits.
     * @param device   The logical device.
     * @param filepath Absolute or relative path to the compiled .spv file.
     * @param stage    The shader stage this module implements.
     * @throws std::runtime_error if the file cannot be read or module creation fails.
     */
    Shader(core::Device& device, const std::string& filepath, ShaderStage stage)
        : Shader(device, filepath, detail::to_vk_bit(stage))
    {}

    /**
     * @brief Destroys the VkShaderModule.
     */
    ~Shader() {
        if (module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_.handle(), module_, nullptr);
        }
    }

    /// @brief Non-copyable.
    Shader(const Shader&) = delete;
    /// @brief Non-copyable.
    Shader& operator=(const Shader&) = delete;

    /**
     * @brief Returns a VkPipelineShaderStageCreateInfo ready for pipeline creation.
     *
     * The entry point is always "main" per GLSL/HLSL convention.
     *
     * @return Populated VkPipelineShaderStageCreateInfo.
     */
    VkPipelineShaderStageCreateInfo stage_info() const {
        VkPipelineShaderStageCreateInfo info{};
        info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage  = stage_;
        info.module = module_;
        info.pName  = "main";
        return info;
    }

    /**
     * @brief Returns the shader stage flag for this module.
     * @return VkShaderStageFlagBits set on construction.
     */
    VkShaderStageFlagBits stage() const { return stage_; }

    /**
     * @brief Returns the underlying VkShaderModule handle.
     * @return Raw VkShaderModule.
     */
    VkShaderModule handle() const { return module_; }

private:
    /**
     * @brief Reads a SPIR-V file into a byte buffer.
     *
     * Opens the file in binary mode at the end to determine size, then
     * reads the entire contents into a std::vector<char>.
     *
     * @param filepath Path to the .spv file.
     * @return Byte buffer containing the SPIR-V binary.
     * @throws std::runtime_error if the file cannot be opened.
     */
    static std::vector<char> read_spirv(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("[gfxcoopa] Failed to open shader file: " + filepath);
        }

        size_t file_size = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(file_size);
        file.seekg(0);
        file.read(buffer.data(), static_cast<std::streamsize>(file_size));
        return buffer;
    }

    core::Device&         device_;               /**< Owning logical device (not owned). */
    VkShaderStageFlagBits stage_;                /**< Shader pipeline stage. */
    VkShaderModule        module_ = VK_NULL_HANDLE;/**< The Vulkan shader module. */
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_SHADER_H
