/**
 * @file camera_ubo.h
 * @brief Per-frame camera matrices UBO (set 0 of every scene pass).
 *
 * Manages a host-visible uniform buffer containing view matrix, projection matrix,
 * and camera world position. Updated once per frame from the active CameraComponent.
 *
 * Pixel-snapping: the camera position is rounded to the nearest world-space pixel
 * boundary to prevent sub-pixel jitter at low render resolutions.
 */

#ifndef GFXCOOPA_ENGINE_DATA_CAMERA_UBO_H
#define GFXCOOPA_ENGINE_DATA_CAMERA_UBO_H

#include <glm/glm.hpp>
#include <cmath>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/**
 * @struct CameraData
 * @brief std140-aligned camera matrices and view position.
 *
 * Layout: view (64 bytes) + proj (64 bytes) + view_pos (12 bytes) + padding (4 bytes) = 144 bytes,
 * then the reprojection block appended below: prev_view (64) + prev_proj (64) + jitter_ndc (16)
 * = 288 bytes. A shader declares only the leading members it needs (most keep the original
 * three-member block; a Vulkan UBO may be larger than the block bound to it), so the
 * trailing fields cost existing shaders nothing.
 */
struct alignas(16) CameraData {
    glm::mat4 view     = glm::mat4(1.0f); /**< View matrix (world-to-camera). */
    glm::mat4 proj     = glm::mat4(1.0f); /**< Projection matrix (camera-to-clip); TAA-jittered when TAA is on. */
    glm::vec3 view_pos = glm::vec3(0.0f); /**< Camera world-space position (for specular). */
    float     _pad0    = 0.0f;            /**< std140 padding. */
    // --- Reprojection, for the G-buffer's velocity attachment (gfx/surface/gbuffer_fs.glsl).
    // Set by set_reprojection(); left at identity/zero by callers that never need motion
    // vectors (GiSystem's probe captures), which makes every velocity read as zero there.
    glm::mat4 prev_view  = glm::mat4(1.0f); /**< Previous frame's view matrix, exactly as uploaded then (pixel-snapped if snapping was on). */
    glm::mat4 prev_proj  = glm::mat4(1.0f); /**< Previous frame's UNJITTERED projection. */
    glm::vec4 jitter_ndc = glm::vec4(0.0f); /**< xy = this frame's TAA jitter as the NDC displacement it applied to `proj`; zw unused. */
};
static_assert(sizeof(CameraData) == 288, "CameraData must stay std140-compatible: shaders declare it member-for-member");

/**
 * @class CameraUBO
 * @brief Manages the per-frame camera uniform buffer.
 *
 * Call update() once per frame with the view and projection matrices.
 * The buffer is host-visible and persistently mapped via VMA.
 */
class CameraUBO {
public:
    /**
     * @brief Creates the camera uniform buffer.
     * @param device    Logical device.
     * @param allocator VMA allocator.
     */
    CameraUBO(core::Device& device, memory::Allocator& allocator)
        : buffer_(memory::Buffer::uniform(device, allocator, sizeof(CameraData)))
    {
        buffer_.upload(&data_, sizeof(CameraData));
    }

    CameraUBO(const CameraUBO&) = delete;
    CameraUBO& operator=(const CameraUBO&) = delete;

    /**
     * @brief Updates the camera UBO from view/projection matrices and uploads to GPU.
     *
     * Applies pixel-snapping to the view position to prevent sub-pixel jitter.
     *
     * @param view          View matrix (world-to-camera).
     * @param proj          Projection matrix (camera-to-clip, Vulkan Y-flipped).
     * @param camera_pos    Camera world-space position.
     * @param pixel_density World-space units per render pixel (for snapping). 0 = disable snapping.
     */
    void update(const glm::mat4& view,
                const glm::mat4& proj,
                const glm::vec3& camera_pos,
                float            pixel_density = 0.0f)
    {
        data_.view = view;
        data_.proj = proj;

        // Pixel-snap: round camera translation to world-space pixel grid.
        if (pixel_density > 0.0f) {
            glm::vec3 snapped = glm::round(camera_pos / pixel_density) * pixel_density;
            data_.view_pos = snapped;
            // Also snap the view translation component.
            data_.view[3][0] = std::round(data_.view[3][0] / pixel_density) * pixel_density;
            data_.view[3][1] = std::round(data_.view[3][1] / pixel_density) * pixel_density;
            data_.view[3][2] = std::round(data_.view[3][2] / pixel_density) * pixel_density;
        } else {
            data_.view_pos = camera_pos;
        }

        buffer_.upload(&data_, sizeof(CameraData));
    }

    /**
     * @brief Sets the previous-frame matrices and this frame's jitter that the G-buffer's
     *        velocity attachment reprojects with. Call BEFORE update(), which uploads the
     *        whole struct; the values persist until the next call.
     *
     * @param prev_view  The view matrix the PREVIOUS frame rendered with -- read it back from
     *                   data().view at the end of that frame so it is the pixel-snapped one
     *                   the shaders actually saw, never a re-derived copy.
     * @param prev_proj  The previous frame's projection WITHOUT its TAA jitter.
     * @param jitter_ndc This frame's jitter as the NDC displacement applied to `proj`
     *                   (zero when TAA is off); the velocity is measured unjittered-to-unjittered.
     */
    void set_reprojection(const glm::mat4& prev_view, const glm::mat4& prev_proj, glm::vec2 jitter_ndc) {
        data_.prev_view  = prev_view;
        data_.prev_proj  = prev_proj;
        data_.jitter_ndc = glm::vec4(jitter_ndc, data_.jitter_ndc.z, 0.0f);
    }

    /**
     * @brief Render pixels per metre at 1 m from the eye (0.5 * render height * proj[1][1]),
     *        carried in jitter_ndc.z -- what tessellation stages size their edge factors by.
     */
    void set_pixel_scale(float px_scale) { data_.jitter_ndc.z = px_scale; }

    /** @brief The host-side copy of what update() last uploaded (view is the snapped one). */
    const CameraData& data() const { return data_; }

    /** @brief Returns the underlying uniform buffer for descriptor binding. */
    const memory::Buffer& buffer() const { return buffer_; }

private:
    CameraData     data_;   /**< Host-side camera matrices. */
    memory::Buffer buffer_; /**< GPU-side uniform buffer. */
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_CAMERA_UBO_H
