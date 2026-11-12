/**
 * @file ui_world_pass.h
 * @brief Draws a WorldSpace CanvasComponent's DrawList as real 3D geometry, inside a render
 *        pass the host already opened.
 *
 * The world-space sibling of ui_pass.h, and deliberately almost the same file: both wrap
 * gfxcoopa's TexturedQuad2DPass (engine/passes/textured_quad_2d_pass.h) for the descriptor
 * layout/pool, the TextureView->DescriptorSet cache, the per-frame-in-flight streaming
 * vertex/index buffers, and the pipeline-variant machinery. Only three things genuinely
 * differ, and they are all this file owns:
 *
 *  1. The push block is a full `proj * view * CanvasComponent::model()` matrix instead of a
 *     2D scale/offset pair, so it needs its own vertex shader (ui_world.vert -- see that
 *     file for why gfx_quad_2d_transform() cannot be reused).
 *  2. Clip rects become scissors by PROJECTING the rect's corners, not by scaling them.
 *  3. Occlusion: a host-supplied scene depth texture at set 1, compared per fragment.
 *
 * Nothing about the UI itself changes. UiVertex is still 2D canvas pixels, DrawList still
 * batches by (texture, clip, z_order), and every widget still emits exactly what it always
 * did -- which is the whole point: a world canvas hosts the entire widget library for free.
 *
 * One rule UiPass does NOT have: begin_frame() must precede the frame's draw() calls.
 * TexturedQuad2DPass owns ONE streaming vertex/index buffer pair per frame-in-flight, and a
 * scene can hold many world canvases -- so, unlike the single screen-space canvas UiPass
 * usually serves, draw() cannot simply overwrite that buffer. It APPENDS at a running cursor
 * that begin_frame() resets, and begin_frame() is also where the buffers are grown to the
 * whole frame's total (growing mid-frame would reallocate out from under geometry already
 * uploaded and already referenced by recorded draw commands).
 *
 * Same two ordering rules as UiPass, for the same reasons:
 *  - register_textures() must run BEFORE the host's Renderer::begin_frame(), because
 *    DescriptorSet::bind_image() updates descriptor sets immediately, which is unsafe once
 *    a render pass has begun. draw() only ever looks up already-registered sets.
 *  - draw() is a GUEST inside a render pass the host already opened. gfxcoopa's
 *    pipeline::RenderPass always clears its colour attachment on load, so opening a second
 *    pass over the same image would erase the 3D scene underneath the UI.
 */

#ifndef UICOOPA_RENDER_UI_WORLD_PASS_H
#define UICOOPA_RENDER_UI_WORLD_PASS_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>

#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/extra_sets.h>
#include <gfxcoopa/engine/passes/textured_quad_2d_pass.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/texture_view.h>

#include <uicoopa/layout/canvas.h>
#include <uicoopa/render/draw_list.h>
#include <uicoopa/render/ui_vertex.h>

namespace coopa {
namespace ui {

/**
 * @struct UiWorldPushConstants
 * @brief Matches the `Push` block in ui_world.vert / ui_world_quad.frag /
 *        ui_world_text.frag exactly.
 *
 * 80 bytes, well inside the 128-byte range Vulkan guarantees on every device: a mat4 at
 * offset 0, then vec2/float/float packed at 64/72/76 with no hidden padding under std430's
 * push-constant rules.
 */
struct UiWorldPushConstants {
    glm::mat4 clip_from_canvas; ///< proj * view * CanvasComponent::model().
    glm::vec2 inv_target_size;  ///< 1 / render-target extent, for the occlusion depth fetch.
    float     occlude = 0.0f;   ///< > 0.5 enables the depth compare; see CanvasComponent::occlude.
    float     depth_bias = 0.0f;///< Slack on that compare, in NDC depth units.
};

/**
 * @class UiWorldPass
 * @brief Owns the world-space UI pipeline (via TexturedQuad2DPass), its per-frame streaming
 *        geometry, and projected clip-rect scissoring.
 *
 * Usage, inside a host renderer that already has a colour target open:
 * @code
 * // once, at construction -- the host owns the scene depth set (see the ctor's scene_depth)
 * UiWorldPass ui_world(device, allocator, cmd_pool, post_target.render_pass_object(),
 *                      shaders("ui_world.vert"), shaders("ui_world_quad.frag"),
 *                      shaders("ui_world_text.frag"), scene_depth_sets);
 *
 * // per frame, BEFORE begin_frame:
 * for (CanvasComponent* c : world_canvases) ui_world.register_textures(c->draw_list());
 *
 * // per frame, inside the host's already-open render pass:
 * for (CanvasComponent* c : world_canvases)
 *     ui_world.draw(cmd, frame_slot, target_w, target_h, proj * view, *c);
 * @endcode
 */
class UiWorldPass {
public:
    /**
     * @param device         Logical device.
     * @param allocator      VMA allocator.
     * @param cmd_pool       Command pool for the default white texture's one-shot upload.
     * @param target_pass    The SAME render pass the host opened for its 3D scene colour.
     * @param vert_spv       Path to ui_world.vert.spv -- shared by both variants below.
     * @param quad_frag_spv  Path to ui_world_quad.frag.spv -- the RGBA-sampling variant.
     * @param text_frag_spv  Path to ui_world_text.frag.spv -- the R8-coverage variant, bound
     *                       for batches whose texture was mark_as_text_atlas()'d.
     * @param scene_depth    REQUIRED, unlike UiPass: the host's scene depth texture as a
     *                       one-combined-sampler set bound at set 1. Both fragment shaders
     *                       declare it unconditionally and branch the compare on a push
     *                       constant -- see ui_world_occlude.glsl for why that beats a pair
     *                       of shader variants. A host with no scene depth to offer binds a
     *                       1x1 texel of 1.0, which never occludes anything.
     * @param max_textures   Upper bound on distinct textures drawn in a single frame
     *                       (sprites + font atlases); sizes the descriptor pool.
     * @throws std::logic_error if scene_depth's layouts and bind callback disagree (see
     *         gfxcoopa's ExtraSets::validate()).
     */
    UiWorldPass(coopa::gfx::core::Device& device,
                coopa::gfx::memory::Allocator& allocator,
                coopa::gfx::command::CommandPool& cmd_pool,
                coopa::gfx::pipeline::RenderPass& target_pass,
                const std::string& vert_spv,
                const std::string& quad_frag_spv,
                const std::string& text_frag_spv,
                coopa::gfx::engine::passes::ExtraSets scene_depth,
                uint32_t max_textures = 256)
    {
        using namespace coopa::gfx;
        using namespace coopa::gfx::engine::passes;

        TexturedQuad2DDesc desc;
        desc.vertex             = UiVertex::layout();
        desc.vertex_stride      = sizeof(UiVertex);
        // AlphaOver, not Alpha: this pass draws into a transparent-cleared layer in hosts that
        // composite the UI separately (see toyengine's ui_world_target_), where Alpha's
        // dstAlpha=ZERO would leave the layer's alpha equal to the LAST fragment's rather than
        // accumulated coverage -- an opaque glyph under a translucent panel would end up
        // stamped with the panel's alpha and let the background through. Identical to Alpha
        // over an opaque target, so no existing host changes behaviour.
        desc.blend_mode         = pipeline::BlendMode::AlphaOver;
        desc.push_constant_size = sizeof(UiWorldPushConstants);
        // Identical sampler policy to UiPass: bilinear, but CLAMP_TO_EDGE rather than
        // REPEAT, because this one sampler is shared by every texture the pass binds --
        // including sprite-sheet sub-rects whose UVs never reach 0/1, where REPEAT would
        // bleed a neighbouring packed icon or glyph in at the seam.
        desc.sampler_desc = coopa::gfx::SamplerDesc::linear_repeat();
        desc.sampler_desc.address = coopa::gfx::AddressMode::ClampToEdge;
        desc.fallback_pixel = {255, 255, 255, 255}; // white -- an untextured Graphic reads as "no image"
        desc.initial_max_verts   = kInitialMaxVerts;
        desc.initial_max_indices = kInitialMaxIndices;
        desc.max_textures        = max_textures;
        desc.extra               = std::move(scene_depth);

        pass_ = std::make_unique<TexturedQuad2DPass>(
            device, allocator, cmd_pool, target_pass, vert_spv, quad_frag_spv, desc);
        pass_->add_variant("text", vert_spv, text_frag_spv);
    }

    /** @brief Returns the default white texture's view, for seeding DrawList::set_default_texture(). */
    coopa::gfx::TextureView white_view() const { return pass_->fallback_view(); }

    /**
     * @brief Resolves every texture referenced by draw_list's batches into a descriptor set.
     *
     * Must be called before the host's Renderer::begin_frame() -- never while a render pass
     * is open. Already-registered textures (by TextureView) are skipped, so this is cheap to
     * call every frame even when the texture set is mostly stable.
     */
    void register_textures(const DrawList& draw_list) {
        for (const DrawBatch& batch : draw_list.batches()) {
            pass_->register_view(batch.texture_view);
        }
    }

    /**
     * @brief Marks a texture view as an R8 coverage atlas (a glyph atlas), so draw() binds
     *        the "text" variant for batches using it instead of sampling it as RGBA.
     *
     * Same contract as UiPass::mark_as_text_atlas(); a canvas drawn by both passes must be
     * registered with both.
     */
    void mark_as_text_atlas(coopa::gfx::TextureView view) { text_views_.insert(view); }

    /**
     * @brief Starts a frame's geometry accumulation: sizes this frame slot's buffers for
     *        everything the frame will upload, and resets the append cursor.
     *
     * Must be called once per frame, before that frame's draw() calls, and -- like
     * register_textures() -- before the host's Renderer::begin_frame(), since growing a buffer
     * destroys and recreates it.
     *
     * The totals are the SUM over every canvas this frame will draw. Passing a short total is
     * not a soft failure: draw() would grow the buffer mid-frame, invalidating geometry an
     * earlier canvas already uploaded and that recorded draw commands still point at.
     *
     * @param frame_index     Host's current frame-in-flight index.
     * @param total_vertices  Sum of draw_list().vertices().size() over this frame's canvases.
     * @param total_indices   Sum of draw_list().indices().size() over this frame's canvases.
     */
    void begin_frame(uint32_t frame_index, size_t total_vertices, size_t total_indices) {
        pass_->ensure_capacity(frame_index, total_vertices, total_indices);
        vertex_cursor_ = 0;
        index_cursor_  = 0;
    }

    /**
     * @brief Records one world-space canvas's draw calls into an already-open render pass.
     *
     * No-op when the canvas emitted nothing, is not a WorldSpace canvas, or lies entirely
     * behind the camera.
     *
     * @param cmd        Command buffer, mid-recording inside the host's render pass.
     * @param frame_index  Host's current frame-in-flight index; selects this frame's buffers.
     * @param target_w   Render target width in pixels.
     * @param target_h   Render target height in pixels.
     * @param view_proj  The host's authoritative `proj * view`. Passed per draw rather than
     *                   stored, so the UI is transformed by exactly the same matrix the
     *                   depth buffer it is compared against was rasterized with -- including
     *                   any TAA jitter the host applies, which a separately-derived
     *                   projection would miss by up to half a pixel.
     * @param canvas     The canvas to draw; supplies model(), occlude, and its DrawList.
     * @param depth_bias Slack on the occlusion compare, in NDC depth units. Only read when
     *                   canvas.occludes(); raise it if a canvas nearly coplanar with a
     *                   surface z-fights against it.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd,
              uint32_t frame_index,
              uint32_t target_w, uint32_t target_h,
              const glm::mat4& view_proj,
              const CanvasComponent& canvas,
              float depth_bias = 0.0f) {
        if (!canvas.is_world_space()) return;
        const DrawList& draw_list = canvas.draw_list();
        if (draw_list.indices().empty()) return;
        if (target_w == 0 || target_h == 0) return;

        const glm::mat4 clip_from_canvas = view_proj * canvas.model();

        // Whole canvas behind the eye: every corner has w <= 0, so nothing it could emit is
        // visible and every projected scissor would be garbage. Bail before touching state.
        if (entirely_behind_camera_(clip_from_canvas, canvas.root_rect())) return;

        // Append at this frame's running cursor -- see begin_frame(). Every canvas in the
        // scene shares one buffer pair per frame slot, so uploading at offset 0 here would
        // make the LAST canvas's geometry the only geometry, and every earlier canvas's
        // already-recorded draw commands would read it by mistake.
        const uint32_t first_vertex = vertex_cursor_;
        const uint32_t first_index  = index_cursor_;
        pass_->vertex_buffer(frame_index).upload(
            draw_list.vertices().data(), draw_list.vertices().size() * sizeof(UiVertex),
            static_cast<size_t>(first_vertex) * sizeof(UiVertex));
        pass_->index_buffer(frame_index).upload(
            draw_list.indices().data(), draw_list.indices().size() * sizeof(uint32_t),
            static_cast<size_t>(first_index) * sizeof(uint32_t));
        vertex_cursor_ += static_cast<uint32_t>(draw_list.vertices().size());
        index_cursor_  += static_cast<uint32_t>(draw_list.indices().size());

        pass_->bind(cmd); // stock ("quad") -- rebound to "text" per batch below as needed

        // The negative-height viewport is set HERE, not inherited. Two reasons it has to be:
        // this pass's vertices go through a real view_proj, so a positive-height viewport
        // would mirror the whole canvas vertically about the target's centre (exactly as
        // documented on DebugLinePass::draw()); and the viewport state at this point in a
        // host's frame is not reliably negative anyway -- gfxcoopa's StylizePass, which
        // typically runs immediately before, sets a POSITIVE one. Depending on the host's
        // state here would make correctness hinge on unrelated feature flags.
        cmd.set_viewport(0.0f, static_cast<float>(target_h),
                         static_cast<float>(target_w), -static_cast<float>(target_h));
        cmd.bind_vertex_buffer(pass_->vertex_buffer(frame_index));
        cmd.bind_index_buffer(pass_->index_buffer(frame_index));

        UiWorldPushConstants push{};
        push.clip_from_canvas = clip_from_canvas;
        push.inv_target_size  = glm::vec2(1.0f / static_cast<float>(target_w),
                                          1.0f / static_cast<float>(target_h));
        push.occlude          = canvas.occludes() ? 1.0f : 0.0f;
        push.depth_bias       = depth_bias;
        // Pushed once for the whole canvas: everything in the block is per-canvas, and the
        // per-batch variation below is a pipeline rebind. That is only safe because every
        // variant of a TexturedQuad2DPass shares one pipeline layout -- see
        // TexturedQuad2DDesc::extra's doc.
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, push);

        bool last_is_text = false;
        bool have_bound = true; // "quad" bound just above

        for (const DrawBatch& batch : draw_list.batches()) {
            if (batch.index_count == 0) continue;

            bool is_text = is_text_view_(batch.texture_view);
            if (!have_bound || is_text != last_is_text) {
                pass_->bind(cmd, is_text ? "text" : "");
                last_is_text = is_text;
                have_bound = true;
            }

            cmd.bind_descriptor_set(pass_->descriptor_set_for(batch.texture_view));
            pass_->bind_extra(cmd); // scene depth at set 1; must follow the pipeline bind

            ScreenScissor scissor = project_clip_(clip_from_canvas, batch.clip, target_w, target_h);
            if (scissor.empty) continue;
            cmd.set_scissor(scissor.x, scissor.y, scissor.w, scissor.h);

            // first_index shifts by this canvas's slice start; vertex_offset re-bases the
            // indices, which DrawList wrote relative to this canvas's own vertex array.
            cmd.draw_indexed(batch.index_count,
                             first_index + batch.first_index,
                             static_cast<int32_t>(first_vertex),
                             1);
        }
    }

private:
    static constexpr uint32_t kInitialMaxVerts   = 8192;
    static constexpr uint32_t kInitialMaxIndices = 12288;
    /// Below this, a corner's clip-space w is at or behind the eye and dividing by it would
    /// produce a meaningless (or infinite) screen position.
    static constexpr float kMinW = 1.0e-6f;

    bool is_text_view_(coopa::gfx::TextureView view) const {
        return view != pass_->fallback_view() && text_views_.count(view) != 0;
    }

    /** @brief A clamped, framebuffer-pixel scissor rect. `empty` means "skip this batch". */
    struct ScreenScissor { int32_t x = 0, y = 0; uint32_t w = 0, h = 0; bool empty = false; };

    /** @brief Projects one canvas-space point to framebuffer pixels. Caller checks w first. */
    static glm::vec2 to_framebuffer_(const glm::mat4& m, glm::vec2 canvas_pt,
                                     uint32_t target_w, uint32_t target_h, float& out_w) {
        glm::vec4 h = m * glm::vec4(canvas_pt, 0.0f, 1.0f);
        out_w = h.w;
        if (h.w <= kMinW) return glm::vec2(0.0f);
        glm::vec2 ndc = glm::vec2(h) / h.w;
        // Matches the negative-height viewport draw() sets: fb_y = target_h * (1 - ndc_y)/2,
        // the opposite sign of the usual Vulkan NDC-to-framebuffer relation.
        return glm::vec2((ndc.x * 0.5f + 0.5f) * static_cast<float>(target_w),
                         (0.5f - ndc.y * 0.5f) * static_cast<float>(target_h));
    }

    /** @brief True when all four corners of `rect` are at or behind the eye. */
    static bool entirely_behind_camera_(const glm::mat4& m, const Rect& rect) {
        const glm::vec2 corners[4] = {
            {rect.min.x, rect.min.y}, {rect.max.x, rect.min.y},
            {rect.max.x, rect.max.y}, {rect.min.x, rect.max.y}};
        for (const glm::vec2& c : corners) {
            if ((m * glm::vec4(c, 0.0f, 1.0f)).w > kMinW) return false;
        }
        return true;
    }

    /**
     * @brief Turns a canvas-space clip rect into a framebuffer scissor by projecting its four
     *        corners and taking their axis-aligned bounding box.
     *
     * EXACT for CanvasBillboard::CameraFacing: that quad sits at a constant view-space depth,
     * so the projection restricted to its plane is a uniform scale plus a translate and an
     * axis-aligned canvas rect stays axis-aligned on screen (TAA jitter is a pure NDC
     * translation and does not break this). CONSERVATIVE for CanvasBillboard::Transform,
     * where a rotated canvas's Mask clips a little loosely rather than incorrectly -- which
     * is sound, because a scissor only ever has to be a SUPERSET of the pixels a batch
     * covers; the rasterizer does the real trimming.
     *
     * This is load-bearing, not a nicety: ProgressBar::start() adds a Mask to its own object
     * if one is absent, so a canvas with a health bar on it always emits a clipped batch.
     */
    static ScreenScissor project_clip_(const glm::mat4& m, const Rect& clip,
                                       uint32_t target_w, uint32_t target_h) {
        const glm::vec2 corners[4] = {
            {clip.min.x, clip.min.y}, {clip.max.x, clip.min.y},
            {clip.max.x, clip.max.y}, {clip.min.x, clip.max.y}};

        glm::vec2 lo(std::numeric_limits<float>::max());
        glm::vec2 hi(std::numeric_limits<float>::lowest());
        int behind = 0;
        for (const glm::vec2& c : corners) {
            float w = 0.0f;
            glm::vec2 fb = to_framebuffer_(m, c, target_w, target_h, w);
            if (w <= kMinW) { ++behind; continue; }
            lo = glm::min(lo, fb);
            hi = glm::max(hi, fb);
        }

        if (behind == 4) return ScreenScissor{0, 0, 0, 0, true};
        if (behind > 0) {
            // The clip rect straddles the camera plane, so its projected extent is unbounded
            // in at least one direction. Fall back to the whole target: conservative, but a
            // real rect, rather than whatever a divide by a near-zero w would have produced.
            return ScreenScissor{0, 0, target_w, target_h, false};
        }

        const int32_t tw = static_cast<int32_t>(target_w);
        const int32_t th = static_cast<int32_t>(target_h);
        int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(lo.x)), 0, tw);
        int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(lo.y)), 0, th);
        int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(hi.x)),  0, tw);
        int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(hi.y)),  0, th);
        // Clamp to >= 0 BEFORE the unsigned cast. An inverted or off-screen rect would
        // otherwise wrap to a ~4-billion-pixel scissor, the classic form of this bug.
        return ScreenScissor{x0, y0,
                             static_cast<uint32_t>(std::max(0, x1 - x0)),
                             static_cast<uint32_t>(std::max(0, y1 - y0)),
                             x1 <= x0 || y1 <= y0};
    }

    std::unique_ptr<coopa::gfx::engine::passes::TexturedQuad2DPass> pass_;
    std::unordered_set<coopa::gfx::TextureView> text_views_;
    /// Append cursors into this frame's shared geometry buffers; reset by begin_frame().
    uint32_t vertex_cursor_ = 0;
    uint32_t index_cursor_  = 0;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_UI_WORLD_PASS_H
