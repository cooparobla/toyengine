/**
 * @file gi_data.h
 * @brief GPU buffers for Global Illumination (GI) probe volumes and reflection probes.
 */

#ifndef GFXCOOPA_ENGINE_GI_GI_DATA_H
#define GFXCOOPA_ENGINE_GI_GI_DATA_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace gi {

/// Maximum number of reflection probes blended together per pixel. Matches
/// this codebase's existing point-shadow-map cap (also 4) -- small enough
/// that shader-side consumers can address each one with a compile-time-
/// constant-indexed, separately-named sampler binding (see ibl.glsl) rather
/// than a dynamically-indexed sampler array, which the device does not
/// enable (VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing
/// is off -- only samplerAnisotropy is turned on in device.h).
static constexpr uint32_t MAX_REFLECTION_PROBES = 4;

/// Per-probe SH data: 9 vec4s = 144 bytes per probe.
/// Each vec4 stores one SH band's RGB coefficients + padding.
/// Bands are ordered: L0(1) + L1(3) + L2(5) = 9 total.
struct alignas(16) SHProbe {
    glm::vec4 bands[9];  // bands[i].xyz = RGB SH coefficient, .w = 0
};

/// Uniform block describing the probe volume grid geometry.
/// Bound as Set 3, Binding 0 (UBO).
struct alignas(16) GiUniforms {
    glm::vec4 grid_origin  = glm::vec4(0.0f); // xyz = world-space origin of grid corner, w = unused
    glm::vec4 grid_spacing = glm::vec4(1.0f); // xyz = cell size per axis, w = unused
    glm::ivec4 grid_counts = glm::ivec4(0);   // xyz = probe counts per axis (Nx, Ny, Nz), w = total
    glm::vec4 gi_params    = glm::vec4(1.0f, 1.0f, 5.0f, 0.0f); // x = gi_intensity, y = reflection_intensity, z = max_roughness_mip, w = num_reflection_probes
};

/// Uniform block for a single reflection probe. 4x vec4 = 64 bytes -- already
/// a multiple of 16, so an array of these under std140 has no inter-element
/// padding surprises (element stride == sizeof(ReflectionProbeUniforms)).
struct alignas(16) ReflectionProbeUniforms {
    glm::vec4 probe_position = glm::vec4(0.0f); // xyz = world position, w = unused
    glm::vec4 box_min        = glm::vec4(0.0f); // xyz = AABB min for parallax correction, w = unused
    glm::vec4 box_max        = glm::vec4(0.0f); // xyz = AABB max for parallax correction, w = unused
    glm::vec4 params         = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f); // x = blend_distance, y = importance, z = intensity, w = max_roughness_mip
};

/// Manages the UBO and SSBO for GI data on the GPU.
class GiData {
public:
    GiData(core::Device& device, memory::Allocator& allocator, uint32_t max_probes = 4096)
        : allocator_(allocator), max_probes_(max_probes)
    {
        uniforms_buf_ = std::make_unique<memory::Buffer>(
            memory::Buffer::uniform(device, allocator, sizeof(GiUniforms))
        );

        size_t ssbo_size = std::max(sizeof(SHProbe) * max_probes, sizeof(SHProbe));
        probes_buf_ = std::make_unique<memory::Buffer>(
            memory::Buffer::storage(device, allocator, ssbo_size)
        );

        reflection_buf_ = std::make_unique<memory::Buffer>(
            memory::Buffer::uniform(device, allocator, sizeof(ReflectionProbeUniforms) * MAX_REFLECTION_PROBES)
        );

        upload_uniforms();
    }

    ~GiData() = default;

    GiData(const GiData&) = delete;
    GiData& operator=(const GiData&) = delete;

    GiUniforms& uniforms() { return uniforms_; }
    const GiUniforms& uniforms() const { return uniforms_; }

    ReflectionProbeUniforms& reflection_uniforms() { return reflection_uniforms_; }
    const ReflectionProbeUniforms& reflection_uniforms() const { return reflection_uniforms_; }

    void upload_uniforms() {
        uniforms_buf_->upload(&uniforms_, sizeof(GiUniforms));
    }

    void upload_probes(const std::vector<SHProbe>& probes) {
        if (probes.empty()) return;
        size_t upload_size = std::min(probes.size(), static_cast<size_t>(max_probes_)) * sizeof(SHProbe);
        probes_buf_->upload(probes.data(), upload_size);
    }

    /// Uploads up to MAX_REFLECTION_PROBES entries (extras are silently
    /// dropped, same behavior as upload_probes()'s clamping). Always uploads
    /// the full fixed-size array so unused trailing slots are zeroed --
    /// harmless since shader consumers gate on the active count
    /// (GiUniforms.gi_params.w), but keeps every slot's descriptor read
    /// well-defined regardless.
    void upload_reflection_uniforms(const std::vector<ReflectionProbeUniforms>& probes) {
        std::array<ReflectionProbeUniforms, MAX_REFLECTION_PROBES> buf{};
        size_t n = std::min(probes.size(), static_cast<size_t>(MAX_REFLECTION_PROBES));
        for (size_t i = 0; i < n; ++i) {
            buf[i] = probes[i];
        }
        reflection_buf_->upload(buf.data(), sizeof(buf));
    }

    memory::Buffer& uniforms_buffer() { return *uniforms_buf_; }
    const memory::Buffer& uniforms_buffer() const { return *uniforms_buf_; }

    memory::Buffer& probes_buffer() { return *probes_buf_; }
    const memory::Buffer& probes_buffer() const { return *probes_buf_; }

    memory::Buffer& reflection_buffer() { return *reflection_buf_; }
    const memory::Buffer& reflection_buffer() const { return *reflection_buf_; }

private:
    memory::Allocator& allocator_;
    GiUniforms uniforms_{};
    ReflectionProbeUniforms reflection_uniforms_{};
    std::unique_ptr<memory::Buffer> uniforms_buf_;
    std::unique_ptr<memory::Buffer> probes_buf_;
    std::unique_ptr<memory::Buffer> reflection_buf_;
    uint32_t max_probes_;
};

} // namespace gi
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_GI_DATA_H
