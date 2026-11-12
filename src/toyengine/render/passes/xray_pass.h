/**
 * @file xray_pass.h
 * @brief See-through silhouettes: an XraySubject drawn as a silhouette or outline wherever an
 *        XrayOccluder is hiding it.
 *
 * A hidden object is not in the G-buffer at all -- it lost the depth test -- so no screen-space
 * pass over the G-buffer can recover its shape. This one re-draws the flagged geometry, which
 * is the only way the effect can exist, and it is why the per-object flags are marker
 * components (toyengine/scene/xray_markers.h) rather than material fields: the pass never
 * looks at a mesh's surface shader, so the effect works on any of them, and a scene opts an
 * object in with one component and no material edit.
 *
 * Two stages, the SsaoPass/ContactShadowPass shape of one class owning its own target:
 *
 *  - **Mask** (`xray_mask.frag`) into an RGBA8 coverage buffer of its own. One pipeline draws
 *    both roles, selected by a push constant: subjects write "hidden" (R), "covered" (B) and
 *    how far behind the covering surface they sit (A); permitted occluders write "may be seen
 *    through" (G) where they are the visible surface. Occlusion is a per-fragment compare
 *    against the sampled G-buffer depth -- this engine's house pattern (debug_line.frag,
 *    transparent_preview.frag, ui_world_occlude.glsl, particle.frag) -- rather than a hardware
 *    depth test, which would have forced this target to share the G-buffer's render pass and
 *    still could not have expressed the permission test.
 *  - **Composite** (`xray_composite.frag`) as a guest inside `post_target_`'s open bracket,
 *    after stylize, exactly like `DebugLinePass`. Post-tonemap, so the authored colour lands
 *    as authored and the silhouette never feeds bloom or the stylize outline's edge detector.
 *
 * Needing BOTH halves at a pixel is what makes occluder opt-in real: a subject hidden behind
 * an unflagged pillar has coverage but no permission, and stays hidden.
 *
 * **Overlapping subjects merge into one shape**, because every flagged mesh draws into the same
 * mask with `BlendMode::Max`. One tag on a prefab root therefore covers its whole subtree and a
 * rig's limbs read as a single silhouette with a single outline, never as stacked cut-outs.
 * Max rather than the alternatives for a reason: Additive scales rgb by the source alpha and
 * then sums, which saturates the coverage flags and double-counts the fade ramp in A, and None
 * would leave the result dependent on draw order.
 *
 * Built only when `xray_enabled` (startup-fixed), so a scene that does not want the effect
 * allocates no mask and records nothing. With it on but nothing flagged, both draw lists come
 * out empty and the mask stage early-outs before it clears -- which is why the caller tracks
 * whether it actually ran (`xray_drew_mask_`) before compositing a buffer that would otherwise
 * still hold the previous frame's coverage.
 */

#ifndef TOYENGINE_RENDER_PASSES_XRAY_PASS_H
#define TOYENGINE_RENDER_PASSES_XRAY_PASS_H

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/types/format.h>

#include <toyengine/render/render_types.h>

namespace coopa::gfx::core { class Device; }
namespace coopa::gfx::memory { class Allocator; }
namespace coopa::gfx::command { class CommandBuffer; }
namespace coopa::gfx::engine::util { class Sampler; }
namespace coopa::gfx::pipeline {
class DescriptorPool;
class DescriptorSet;
class DescriptorSetLayout;
class Pipeline;
class RenderPass;
class Shader;
}

namespace toy {
namespace render {
namespace passes {

/**
 * @class XrayPass
 * @brief The x-ray coverage mask and the composite that draws it over the finished frame.
 */
class XrayPass {
public:
    /**
     * Mask format. 8 bits per channel is plenty: R/G/B are coverage flags and A is a fade
     * ramp, so the quantization lands far below what an eye can see in a blended overlay,
     * and a wider format would cost bandwidth on a full-resolution target for nothing.
     */
    static constexpr coopa::gfx::Format kMaskFormat = coopa::gfx::Format::RGBA8_Unorm;

    /** @brief What a mask draw contributes. Mirrors toy::scene::XrayRole. */
    enum class Role : uint32_t { Subject = 1, Occluder = 2 };

    /**
     * @brief `xray_mask.frag`'s push block. The first 64 bytes are `transparent.vert`'s, which
     *        this pass reuses as its vertex stage.
     */
    struct alignas(16) MaskPushConstants {
        glm::vec4 albedo{1.0f};       ///< transparent.vert's block; unread by the mask shader.
        float     metallic     = 0.0f;
        float     roughness    = 0.0f;
        float     ao           = 0.0f;
        float     alpha_cutoff = 0.0f;
        glm::vec4 gfx_time{0.0f};
        glm::vec4 gfx_params{0.0f};
        /// x: Role, yz: 1 / mask size, w: 1 / the metres over which `A` fades to 0.
        glm::vec4 params{0.0f};
        /// x: camera near, y: camera far, z: 1 for perspective, w: spare.
        glm::vec4 depth{0.0f};
    };
    static_assert(sizeof(MaskPushConstants) == 96, "must match xray_mask.frag");

    /** @brief `xray_composite.frag`'s push block. */
    struct alignas(16) CompositePushConstants {
        glm::vec4 color{1.0f};        ///< rgb = colour, a = how far it blends over the scene.
        glm::vec2 inv_size{1.0f};     ///< 1 / mask size.
        float     thickness  = 2.0f;  ///< Outline half-width in texels; <= 0 draws no outline.
        float     depth_fade = 0.0f;  ///< 0..1: how much deeper-hidden parts dim.
        float     fill       = 1.0f;  ///< 1 fills the hidden area, 0 is outline-only.
        float     outline    = 0.0f;  ///< 1 draws the outline.
        float     pad0       = 0.0f;
        float     pad1       = 0.0f;
    };
    static_assert(sizeof(CompositePushConstants) == 48, "must match xray_composite.frag");

    /**
     * @param device          Vulkan logical device.
     * @param allocator       VMA allocator for the mask target.
     * @param composite_pass  The render pass the composite draws into as a guest (post_target_).
     * @param camera_layout   The camera UBO set layout (set 0), owned by the caller.
     * @param width           Render width in pixels.
     * @param height          Render height in pixels.
     * @param mesh_vert_spv   transparent.vert.spv -- the mask stage's vertex shader.
     * @param mask_frag_spv   xray_mask.frag.spv.
     * @param fullscreen_vert_spv fullscreen.vert.spv.
     * @param composite_frag_spv  xray_composite.frag.spv.
     */
    XrayPass(coopa::gfx::core::Device& device,
             coopa::gfx::memory::Allocator& allocator,
             coopa::gfx::pipeline::RenderPass& composite_pass,
             const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
             uint32_t width, uint32_t height,
             const std::string& mesh_vert_spv,
             const std::string& mask_frag_spv,
             const std::string& fullscreen_vert_spv,
             const std::string& composite_frag_spv);

    ~XrayPass();

    XrayPass(const XrayPass&) = delete;
    XrayPass& operator=(const XrayPass&) = delete;

    /**
     * @brief Binds the scene depth the mask stage compares against, and the mask the composite
     *        reads. Called once, at construction time, by the caller.
     *
     * Both descriptors are bound immediately and never rebound -- the G-buffer and this pass's
     * own mask are startup-sized and never recreated. See the passes/README.md note on why
     * rebinding from inside the frame loop is not allowed.
     *
     * @param scene_depth The G-buffer depth view.
     * @param nearest     A NEAREST sampler (a depth compare must not interpolate).
     */
    void set_scene_depth(coopa::gfx::TextureView scene_depth,
                         const coopa::gfx::engine::util::Sampler& nearest);

    /** @brief Opens the mask target and binds the mask pipeline, viewport and camera set. */
    void begin_mask(coopa::gfx::command::CommandBuffer& cmd,
                    const coopa::gfx::pipeline::DescriptorSet& camera_set,
                    const LetterboxRect& rect) const;

    /** @brief Per-object push constants for one mask draw (between begin_mask() and end_mask()). */
    void push_mask(coopa::gfx::command::CommandBuffer& cmd, MaskPushConstants pc) const;

    /** @brief Closes the mask target. */
    void end_mask(coopa::gfx::command::CommandBuffer& cmd) const;

    /** @brief Draws the composite as a guest in the render pass this was built against. */
    void draw_composite(coopa::gfx::command::CommandBuffer& cmd, CompositePushConstants pc,
                        uint32_t width, uint32_t height) const;

    /** @brief 1 / mask size, for filling a push block's `inv_size`. */
    glm::vec2 inv_size() const { return inv_size_; }

private:
    coopa::gfx::engine::targets::OffscreenTarget mask_target_;

    std::unique_ptr<coopa::gfx::pipeline::Shader> mesh_vert_, mask_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> fullscreen_vert_, composite_frag_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> depth_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      depth_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       depth_set_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> mask_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      mask_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       mask_set_;

    std::unique_ptr<coopa::gfx::pipeline::Pipeline> mask_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> composite_pipeline_;

    glm::vec2 inv_size_{1.0f};
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_XRAY_PASS_H
